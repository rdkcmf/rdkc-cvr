/**
##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
**/
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib/gstrfuncs.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#if !defined ( CVR_PLATFORM_RPI )
#include <Logger.h>
#endif /* CVR_PLATFORM_RPI */

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

#if !defined ( CVR_PLATFORM_RPI )
#include <PutFrameHelper.h>
#endif /* CVR_PLATFORM_RPI */

#include <atomic>
#include <iostream>
#include <iomanip>
#include <queue>

#if !defined ( CVR_PLATFORM_RPI )
#include "KinesisVideoProducer.h"
#include "CachingEndpointOnlyCallbackProvider.h"
#include <IotCertCredentialProvider.h>
#endif /* CVR_PLATFORM_RPI */

#include "rdk_debug.h"

#if !defined ( CVR_PLATFORM_RPI )
using namespace std;
using namespace com::amazonaws::kinesis::video;
using namespace log4cplus;

#ifdef __cplusplus
extern "C" {
#endif
#include "sysUtils.h"
//long compute_stats();

#ifdef __cplusplus
}
#endif

LOGGER_TAG("com.amazonaws.kinesis.video.gstreamer");

#define ACCESS_KEY_ENV_VAR "AWS_ACCESS_KEY_ID"
#define DEFAULT_REGION_ENV_VAR "AWS_DEFAULT_REGION"
#define DEFAULT_STREAM_NAME "STREAM_NAME"
#define SECRET_KEY_ENV_VAR "AWS_SECRET_ACCESS_KEY"
#define SESSION_TOKEN_ENV_VAR "AWS_SESSION_TOKEN"
#define KVS_LOG_CONFIG_ENV_VER "KVS_LOG_CONFIG"
#define KVSINITMAXRETRY 5
#define CLIPUPLOAD_READY_TIMEOUT_DURATION_IN_SECONDS 25
#define CLIPUPLOAD_MAX_TIMEOUT_DURATION_IN_MILLISECOND 15000

//Kinesis Video Stream definitions
#define DEFAULT_FRAME_DATA_SIZE_BYTE (1024*1024)
#define DEFAULT_RETENTION_PERIOD_HOURS 2
#define DEFAULT_KMS_KEY_ID ""
#define DEFAULT_STREAMING_TYPE STREAMING_TYPE_OFFLINE
#define DEFAULT_CONTENT_TYPE "video/h264,audio/aac"
#define DEFAULT_MAX_LATENCY_SECONDS 60
#define DEFAULT_FRAGMENT_DURATION_MILLISECONDS 2000
#define DEFAULT_TIMECODE_SCALE_MILLISECONDS 1
#define DEFAULT_KEY_FRAME_FRAGMENTATION TRUE
#define DEFAULT_FRAME_TIMECODES TRUE
#define DEFAULT_ABSOLUTE_FRAGMENT_TIMES FALSE
#define DEFAULT_FRAGMENT_ACKS TRUE
#define DEFAULT_RESTART_ON_ERROR FALSE
#define DEFAULT_RECALCULATE_METRICS TRUE
#define DEFAULT_STREAM_FRAMERATE 25
#define DEFAULT_AVG_BANDWIDTH_BPS (4 * 1024 * 1024)
#define DEFAULT_BUFFER_DURATION_SECONDS 120
#define DEFAULT_REPLAY_DURATION_SECONDS 40
#define DEFAULT_CONNECTION_STALENESS_SECONDS 120
#define DEFAULT_CODEC_ID "V_MPEG4/ISO/AVC"
#define DEFAULT_TRACKNAME "kinesis_video"
#define STORAGE_SIZE_STREAM1 (3 * 1024 * 1024)
#define STORAGE_SIZE_STREAM2 (4 * 1024 * 1024)
#define DEFAULT_STORAGE_SIZE_STREAM (3 * 1024 * 1024)
#define MAX_STORAGE_SIZE_STREAM (10 * 1024 * 1024)
#define DEFAULT_ROTATION_TIME_SECONDS 2400
#define DEFAULT_VIDEO_TRACKID 1
#define DEFAULT_FRAME_DURATION_MS 1

#define DEFAULT_AUDIO_VIDEO_DRIFT_TIMEOUT_SECOND 5

#define DEFAULT_AUDIO_TRACK_NAME "audio"
#define DEFAULT_AUDIO_CODEC_ID "A_AAC"
#define DEFAULT_AUDIO_TRACKID 2
#define DEFAULT_CODECID_H264VIDEO_CAM_STREAM3  "014d001fffe1002d674d001f9a6402802dff80b5010101400000fa000013883a1800b720002dc72ef2e3430016e40005b8e5de5c2801000468ee3c80"
#define DEFAULT_CODECID_H264VIDEO_CAM2_STREAM1 "014d401fffe1002e674d401f8d8d402802dff80b7010101400000fa000013883a1802490000d59faef2e343004920001ab3f5de5c28001000468ef7c80"
#define DEFAULT_CODECID_H264VIDEO_CAM2_STREAM3 "014d401fffe1002e674d401f8d8d402802dff80b7010101400000fa000013883a1802490000d59faef2e343004920001ab3f5de5c28001000468ee3880"
#define DEFAULT_CODECID_AACAUDIO "1408"
#define DEFAULT_CACHE_TIME_IN_SECONDS 86400 //24*3600 ~ 1 day


static mutex custom_data_mtx;

/************************************************* common api's start*****************************************/
namespace com { namespace amazonaws { namespace kinesis { namespace video {
typedef struct _CustomData {
  _CustomData():
    first_frame(true),
    firstkeyframetimestamp(0),
    //stream_status(STATUS_SUCCESS),
    base_pts(0),
    max_frame_pts(0),
    //key_frame_pts(0),
    kinesis_video_producer(nullptr),
    frame_data_size(DEFAULT_FRAME_DATA_SIZE_BYTE),
    clip_senttokvssdk_time(0),
    connection_error(false),
    stream_in_progress(false),
    mainloop_running(false),
    clip_upload_status_(false),
    streaming_type(DEFAULT_STREAMING_TYPE),
    gkvsclip_audio(0),
    gkvsclip_abstime(0),
    gkvsclip_livemode(0),
    kinesis_video_stream(nullptr),
    //audio related params
    putFrameHelper(nullptr),
    eos_triggered(false),
    pipeline_blocked(false),
    total_track_count(1),
    storageMem(0),
    put_frame_failed(false),
    put_frame_flushed(false),
    cvr_stream_id(3)
  {
    producer_start_time = chrono::duration_cast<nanoseconds>(systemCurrentTime().time_since_epoch()).count();
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d):  producer_start_time is %lld\n", __FILE__, __LINE__,producer_start_time);
  }

  //gst components
  GstElement *pipeline, *source, *filter, *appsink, *h264parse, *queue, *tsdemux;
  GstElement *appsink_video, *aac_parse, *appsink_audio, *audio_queue, *video_queue, *video_filter, *audio_filter;

  GstBus *bus;
  GMainLoop *main_loop;

  //kvs components
  unique_ptr<KinesisVideoProducer> kinesis_video_producer;
  shared_ptr<KinesisVideoStream> kinesis_video_stream;
  char stream_name[ MAX_STREAM_NAME_LEN ];
  char clip_name[ MAX_STREAM_NAME_LEN ];
  volatile bool connection_error;
  volatile bool stream_in_progress;
  volatile bool mainloop_running;

  uint8_t *frame_data;
  size_t frame_data_size;

  // indicate whether a video key frame has been received or not.
  volatile bool first_frame;

  //first keyframe timesatmps in clip
  uint64_t firstkeyframetimestamp;

  // Since each file's timestamp start at 0, need to add all subsequent file's timestamp to base_pts starting from the
  // second file to avoid fragment overlapping. When starting a new putMedia session, this should be set to 0.
  // Unit: ns
  uint64_t base_pts;

  // Max pts in a file. This will be added to the base_pts for the next file. When starting a new putMedia session,
  // this should be set to 0.
  // Unit: ns
  uint64_t max_frame_pts;

  // Used in file uploading only. Assuming frame timestamp are relative. Add producer_start_time to each frame's
  // timestamp to convert them to absolute timestamp. This way fragments dont overlap after token rotation when doing
  // file uploading.
  uint64_t producer_start_time;

  //time at which gst pipeline finished pushing data to kvs sdk using put_frame, captured at main loop finish
  uint64_t clip_senttokvssdk_time;

  //Mutex needed for the condition variable for client ready locking.
  std::mutex clip_upload_mutex_;

  //Condition variable used to signal the clip has been uploaded.
  std::condition_variable clip_upload_status_var_;
  
  //Indicating that the clip uploaded
  volatile bool clip_upload_status_;

  //streaming type
  STREAMING_TYPE streaming_type;

  //Indicating that the clip has audio
  unsigned short gkvsclip_audio;

  //Indicating that the clip has abs timestamp
  unsigned short gkvsclip_abstime;

  //Indicating that the clip has live mode enabled
  unsigned short gkvsclip_livemode;

  //storage space to sdk
  uint64_t storageMem;

  //audio related params
  mutex audio_video_sync_mtx;
  condition_variable audio_video_sync_cv;

  // indicate if either audio or video media pipeline is currently blocked. If so, the other pipeline line will wake up
  // the blocked one when the time is right.
  volatile bool pipeline_blocked;

  // helper object for syncing audio and video frames to avoid fragment overlapping.
  unique_ptr<PutFrameHelper> putFrameHelper;

  // when uploading file, whether one of audio or video pipeline has reached eos.
  atomic_bool eos_triggered;

  // whether putFrameHelper has reported a putFrame failure.
  atomic_bool put_frame_failed;

  // whether putFrameHelper flush done
  atomic_bool put_frame_flushed ;

  // key:     trackId
  // value:   whether application has received the first frame for trackId.
  map<int, bool> stream_started;

  uint32_t total_track_count;

  int cvr_stream_id;

} CustomData;

CustomData data = {};

//char gclip[MAX_PATH_LEN + 1] = "";
//char glevelclip[MAX_PATH_LEN + 1] = "";
const gchar *audiopad = "audio";
const gchar *videopad = "video";

class SampleClientCallbackProvider : public ClientCallbackProvider {
 public:
  UINT64 getCallbackCustomData() override {
    return reinterpret_cast<UINT64> (this);
  }

  StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
    return storageOverflowPressure;
  }

  static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
};

class SampleStreamCallbackProvider : public StreamCallbackProvider {
  UINT64 custom_data_;
 public:
  SampleStreamCallbackProvider(UINT64 custom_data) : custom_data_(custom_data) {}

  UINT64 getCallbackCustomData() override {
    return custom_data_;
  }

  StreamConnectionStaleFunc getStreamConnectionStaleCallback() override {
    return streamConnectionStaleHandler;
  };

  StreamErrorReportFunc getStreamErrorReportCallback() override {
    return streamErrorReportHandler;
  };

  DroppedFrameReportFunc getDroppedFrameReportCallback() override {
    return droppedFrameReportHandler;
  };

  FragmentAckReceivedFunc getFragmentAckReceivedCallback() override {
    return FragmentAckReceivedHandler;
  };

 private:
  static STATUS
  streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                               UINT64 last_buffering_ack);

  static STATUS
  streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UPLOAD_HANDLE upload_handle, UINT64 errored_timecode,
                           STATUS status_code);

  static STATUS
  droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                            UINT64 dropped_frame_timecode);

  static STATUS
  FragmentAckReceivedHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                             UPLOAD_HANDLE upload_handle, PFragmentAck pAckReceived);
};

class SampleCredentialProvider : public StaticCredentialProvider {
  // Test rotation period is 40 second for the grace period.
  const std::chrono::duration<uint64_t> ROTATION_PERIOD = std::chrono::seconds(DEFAULT_ROTATION_TIME_SECONDS);
 public:
  SampleCredentialProvider(const Credentials &credentials) :
      StaticCredentialProvider(credentials) {}

  void updateCredentials(Credentials &credentials) override {
    // Copy the stored creds forward
    credentials = credentials_;

    // Update only the expiration
    auto now_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    auto expiration_seconds = now_time + ROTATION_PERIOD;
    credentials.setExpiration(std::chrono::seconds(expiration_seconds.count()));
    LOG_INFO("New credentials expiration is " << credentials.getExpiration().count());
  }
};

class SampleDeviceInfoProvider : public DefaultDeviceInfoProvider {
  int m_stream_id;
 public:
  SampleDeviceInfoProvider(int stream_id) : m_stream_id(stream_id) {}

  device_info_t getDeviceInfo() override {
    auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();

    switch (m_stream_id) {
      case 0 :
        device_info.storageInfo.storageSize = STORAGE_SIZE_STREAM1;
        break;
      case 1 :
        device_info.storageInfo.storageSize = STORAGE_SIZE_STREAM2;
        break;
      case 2 :
        device_info.storageInfo.storageSize = DEFAULT_STORAGE_SIZE_STREAM;
        break;
      case 3 :
        device_info.storageInfo.storageSize = DEFAULT_STORAGE_SIZE_STREAM;
        break;
      default:
        device_info.storageInfo.storageSize = DEFAULT_STORAGE_SIZE_STREAM;
        break;
    }

    if (data.storageMem != 0 && data.storageMem >= STORAGE_SIZE_STREAM2 && data.storageMem <= MAX_STORAGE_SIZE_STREAM) {
        device_info.storageInfo.storageSize = data.storageMem;
    }

    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : streamID : %d : storage size : %llu\n", __FILE__, __LINE__,m_stream_id, device_info.storageInfo.storageSize);

    return device_info;
  }
};

STATUS
SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
  UNUSED_PARAM(custom_handle);
  LOG_WARN("Reporting storage overflow. Bytes remaining " << remaining_bytes);
  return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                                  STREAM_HANDLE stream_handle,
                                                                  UINT64 last_buffering_ack) {
  LOG_WARN("Reporting stream stale. Last ACK received " << last_buffering_ack);
  CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
  {
    if( customDataObj->mainloop_running ) {
      std::lock_guard<std::mutex> lk(custom_data_mtx);
      customDataObj->connection_error = true;
      LOG_ERROR("streamConnectionStaleHandler : Main loop quit done" );
      customDataObj->mainloop_running = false;
      g_main_loop_quit(customDataObj->main_loop);
    }
  }
  return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                       UPLOAD_HANDLE upload_handle, UINT64 errored_timecode, STATUS status_code) {
  
  std::stringstream status_strstrm;
  status_strstrm << "0x" << std::hex << status_code;
  
  LOG_ERROR("Reporting stream error. Errored timecode: " << errored_timecode << " Status: "
                                                         << status_strstrm.str() );

  CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
  //customDataObj->stream_status = status_code;

  //if (status_code == static_cast<UINT32>(STATUS_DESCRIBE_STREAM_CALL_FAILED) &&
  if (customDataObj->kinesis_video_stream != NULL) {
    {
      if( customDataObj->mainloop_running ) {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        customDataObj->connection_error = true;
        LOG_ERROR("streamErrorReportHandler : Main loop quit done" );
        customDataObj->mainloop_running = false;
        g_main_loop_quit(customDataObj->main_loop);
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):  kvsclip upload error %s, %s\n",
		      __FILE__, __LINE__, customDataObj->clip_name, status_strstrm.str().c_str());
      }
    }
  }
  return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode) {
  CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);

  if( customDataObj->kinesis_video_stream!=NULL) {
    LOG_DEBUG("SampleStreamCallbackProvider::droppedFrameReportHandler : " << " dropped_frame_timecode :" << dropped_frame_timecode );
  }
  
  return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::FragmentAckReceivedHandler(UINT64 custom_data,STREAM_HANDLE stream_handle,
                                                         UPLOAD_HANDLE upload_handle,PFragmentAck pFragmentAck) {

  CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
  static uint64_t clipcount = 0;
  static uint64_t clipuploadtime = 0;  

  //Ignore multiple acks
  if ( (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_PERSISTED) && ( false == customDataObj->clip_upload_status_ ) )  {

    clipcount++;
    uint64_t time_now = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
    uint64_t time_diff = time_now - customDataObj->clip_senttokvssdk_time;
    clipuploadtime+=time_diff;
    uint64_t avgtime_clipupload = clipuploadtime/clipcount ;
    
    //clip_name,epochtime_senttokvssdk, currenttime, epochfragmenttimecode_server, fragmentnumber, timediff
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
      __FILE__, __LINE__, customDataObj->clip_name, pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff );

    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload stats:%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,clipcount);
    {
      std::lock_guard<std::mutex> lock( customDataObj->clip_upload_mutex_ );
      customDataObj->clip_upload_status_ = true;
      customDataObj->clip_upload_status_var_.notify_one();
    }
  }
  return STATUS_SUCCESS;
}

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;

unique_ptr<Credentials> credentials_;

//kinesis producer init
static void kinesis_video_init(CustomData *data) {

  unique_ptr<DeviceInfoProvider> device_info_provider = make_unique<SampleDeviceInfoProvider>(data->cvr_stream_id);
  unique_ptr<ClientCallbackProvider> client_callback_provider = make_unique<SampleClientCallbackProvider>();
  unique_ptr<StreamCallbackProvider> stream_callback_provider = make_unique<SampleStreamCallbackProvider>(
    reinterpret_cast<UINT64>(data));

  char const *defaultRegion;
  char const *iot_get_credential_endpoint;
  char const *cert_path;
  char const *private_key_path;
  char const *role_alias;
  char const *ca_cert_path;
  char const *streamname;
  string defaultRegionStr;

  unique_ptr<CredentialProvider> credential_provider;
  if (nullptr!=(iot_get_credential_endpoint = getenv("IOT_GET_CREDENTIAL_ENDPOINT")) &&
      nullptr!=(cert_path = getenv("CERT_PATH")) &&
      nullptr!=(private_key_path = getenv("PRIVATE_KEY_PATH")) &&
      nullptr!=(role_alias = getenv("ROLE_ALIAS")) &&
            nullptr!=(ca_cert_path = getenv("CA_CERT_PATH")) &&
            nullptr!=(streamname = getenv("STREAM_NAME"))) {
        LOG_INFO("Using IoT credentials for Kinesis Video Streams : "  << streamname << " iot_get_credential_endpoint : " << iot_get_credential_endpoint << " cert_path : " << cert_path
          << " private_key_path : " << private_key_path << " role_alias : " << role_alias << " ca_cert_path : " << ca_cert_path );
        
    credential_provider = make_unique<IotCertCredentialProvider>(iot_get_credential_endpoint,
                string(cert_path),
                string(private_key_path),
                string(role_alias),
                string(ca_cert_path),
                string(streamname)
                                                                        );
  }

  //stream name
  STRNCPY(data->stream_name, streamname,MAX_STREAM_NAME_LEN);
  data->stream_name[MAX_STREAM_NAME_LEN -1] = '\0';
  LOG_INFO("kinesisVideoInit enter data stream name " << data->stream_name);

  //region
  if (nullptr==(defaultRegion = getenv(DEFAULT_REGION_ENV_VAR)))
  {
    defaultRegionStr = DEFAULT_AWS_REGION;
  }
  else
  {
    defaultRegionStr = string(defaultRegion);
  }

  LOG_INFO("kinesis_video_init defaultRegion = " << defaultRegionStr);
  
  //cache callback
  unique_ptr<DefaultCallbackProvider>
          cachingEndpointOnlyCallbackProvider = make_unique<CachingEndpointOnlyCallbackProvider>(
          move(client_callback_provider),
          move(stream_callback_provider),
          move(credential_provider),
          defaultRegionStr,
          "",
          "",
          "",
          "",
          std::chrono::seconds(DEFAULT_CACHE_TIME_IN_SECONDS));
 
  data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
                                                                      move(cachingEndpointOnlyCallbackProvider));

  /*data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
                                                                  move(client_callback_provider),
                                                                  move(stream_callback_provider),
                                                                  move(credential_provider),
                                                                  defaultRegionStr);*/

  LOG_INFO("Kinesis Video Streams Client is ready");
}

//Api to set kvs tags
static void setKVSTags(std::map<string, string> &tagsmap) {
  char fw_name[FW_NAME_MAX_LENGTH] = {0};
  char device_mac[CAM_MAC_MAX_LENGTH] = {0};
  char version_num[VER_NUM_MAX_LENGTH] = {0};
  char tag_name[MAX_TAG_NAME_LEN] = {0};
  char tag_val[MAX_TAG_VALUE_LEN] = {0};

  //Retrieve the Firmware version
  if (getCameraFirmwareVersion(fw_name) != 0) {
     RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): ERROR in reading camera firmware version\n", __FILE__, __LINE__);
  }
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): The firmware image is %s\n", __FILE__, __LINE__, fw_name);

  //update tag with firmware name
  sprintf(tag_name, "RDKC_FIRMWARE");
  sprintf(tag_val, "%s", fw_name);
  tagsmap.emplace(std::make_pair(tag_name, tag_val));

  //Retrieve the Device MAC
  if (getDeviceMacValue(device_mac) != 0) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): ERROR in reading camera MAC\n", __FILE__, __LINE__);
  }
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): The device MAC is %s\n", __FILE__, __LINE__, device_mac);

  //update tag with mac address
  memset(tag_name, 0, MAX_TAG_NAME_LEN);
  memset(tag_val, 0, MAX_TAG_NAME_LEN);
  sprintf(tag_name, "RDKC_MAC");
  sprintf(tag_val, "%s", device_mac);
  tagsmap.emplace(std::make_pair(tag_name, tag_val));

  //Retrieve the Version Number
  if (getCameraVersionNum(version_num) != 0) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): ERROR in reading camera image version number\n", __FILE__, __LINE__);
  }
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): The device image version number is %s\n", __FILE__, __LINE__, version_num);

  //update tag with mac address
  memset(tag_name, 0, MAX_TAG_NAME_LEN);
  memset(tag_val, 0, MAX_TAG_NAME_LEN);
  sprintf(tag_name, "RDKC_VERSION");
  sprintf(tag_val, "%s", version_num);
  tagsmap.emplace(std::make_pair(tag_name, tag_val));
}

//kinesis stream init
static void kinesis_video_stream_init(CustomData *data) {

  bool use_absolute_fragment_times = DEFAULT_ABSOLUTE_FRAGMENT_TIMES;
  std::map<string, string> tags;

  if ( ! data->gkvsclip_livemode ) { 
    data->streaming_type = STREAMING_TYPE_OFFLINE;
    use_absolute_fragment_times = true;
  }else {
    data->streaming_type = STREAMING_TYPE_REALTIME;
    use_absolute_fragment_times = false;
  }

  string content_type;
  if ( data->gkvsclip_audio ) {
    content_type = "video/h264,audio/aac";
  } else {
    content_type = "video/h264";
  }

  setKVSTags(tags);
  for ( auto& x: tags) {
	  RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","tag tagname - %s tagvalue - %s\n", x.first.c_str(),x.second.c_str());
  }
  auto stream_definition = make_unique<StreamDefinition>(data->stream_name,
                                                          hours(DEFAULT_RETENTION_PERIOD_HOURS),
                                                          &tags,
                                                          DEFAULT_KMS_KEY_ID,
                                                          data->streaming_type,
                                                          content_type,
                                                          duration_cast<milliseconds> (seconds(DEFAULT_MAX_LATENCY_SECONDS)),
                                                          milliseconds(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
                                                          milliseconds(DEFAULT_TIMECODE_SCALE_MILLISECONDS),
                                                          DEFAULT_KEY_FRAME_FRAGMENTATION,
                                                          DEFAULT_FRAME_TIMECODES,
                                                          use_absolute_fragment_times,
                                                          DEFAULT_FRAGMENT_ACKS,
                                                          DEFAULT_RESTART_ON_ERROR,
                                                          DEFAULT_RECALCULATE_METRICS,
                                                          NAL_ADAPTATION_FLAG_NONE,
                                                          DEFAULT_STREAM_FRAMERATE,
                                                          DEFAULT_AVG_BANDWIDTH_BPS,
                                                          seconds(DEFAULT_BUFFER_DURATION_SECONDS),
                                                          seconds(DEFAULT_REPLAY_DURATION_SECONDS),
                                                          seconds(DEFAULT_CONNECTION_STALENESS_SECONDS),
                                                          DEFAULT_CODEC_ID,
                                                          DEFAULT_TRACKNAME,
                                                          nullptr,
                                                          0,
                                                          MKV_TRACK_INFO_TYPE_VIDEO,
                                                          vector<uint8_t>(),
                                                          DEFAULT_VIDEO_TRACKID);
  if ( data->gkvsclip_audio ) {
    LOG_INFO("Kinesis video stream init audio video case");
    stream_definition->addTrack(DEFAULT_AUDIO_TRACKID, DEFAULT_AUDIO_TRACK_NAME, DEFAULT_AUDIO_CODEC_ID, MKV_TRACK_INFO_TYPE_AUDIO);
    data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));
    //mkv_timecode,audio_queue_size,video_queue_size,audio_buffer_size,video_buffer_size
    data->putFrameHelper = make_unique<PutFrameHelper>(data->kinesis_video_stream,1000000,20,20,400,70000);
    data->stream_started.clear();
  } else {
    LOG_INFO("Kinesis video stream init video case");
    data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));
    {
      std::lock_guard<std::mutex> lk(custom_data_mtx);
      data->stream_in_progress = false;
    }
  }

  // since we are starting new putMedia, timestamp need not be padded.
  data->base_pts = 0;
  data->max_frame_pts = 0;
  
  // reset state
  //data->stream_status = STATUS_SUCCESS;
  {
    std::lock_guard<std::mutex> lk(custom_data_mtx);
    data->connection_error = false;
  }

  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Stream %s is ready\n", __FILE__, __LINE__, data->stream_name );
}

//kvs video stream uninit
void kinesis_video_stream_uninit(CustomData *data, uint64_t& hangdetecttime) {
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs stream uninit started\n", __FILE__, __LINE__);
  if (data->kinesis_video_stream != NULL) {
    //LOG_INFO("kinesis_video_stream_uninit - Enter");
    if ( ( data->gkvsclip_audio ) && ( nullptr != data->putFrameHelper ) ) {
      // signal putFrameHelper to release all frames still in queue.
      hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      if( !data->put_frame_flushed.load() ) 
      {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs put frame flush after stream uninit\n", __FILE__, __LINE__);
        data->putFrameHelper->flush();
        //data->putFrameHelper->putEofr();
        data->put_frame_flushed = true;
      }
    }
    data->kinesis_video_stream->stop();
    hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    data->kinesis_video_producer->freeStream(data->kinesis_video_stream);
    data->kinesis_video_stream = NULL;
    //LOG_INFO("kinesis_video_stream_uninit - Exit");
  }
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs stream uninit done\n", __FILE__, __LINE__);
}

//recreate stream
static void recreate_stream(CustomData *data, uint64_t& hangdetecttime) {
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : Attempt to recreate kinesis video stream \n", __FILE__, __LINE__);
  hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  kinesis_video_stream_uninit(data,hangdetecttime);
  hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  //sleep required between free stream and recreate stream to avoid crash
  this_thread::sleep_for(std::chrono::seconds(3));
  bool do_repeat = true;
  int retry=0;
  do {
      try {
        hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        kinesis_video_stream_init(data);
        do_repeat = false;
      } catch (runtime_error &err) {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Failed to create kinesis video stream : retrying \n", __FILE__, __LINE__);
        this_thread::sleep_for(std::chrono::seconds(2));
        retry++;
        if ( retry > KVSINITMAXRETRY ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : FATAL : Max retry reached in recreate_stream exit process %d\n", __FILE__, __LINE__);
          exit(1);
        }
      }
      {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        data->connection_error = false;
      }
  } while(do_repeat);
}

//reset stream
static void reset_stream(CustomData *data, uint64_t& hangdetecttime) {
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : Attempt to reset kinesis video stream \n", __FILE__, __LINE__);
  hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  bool do_repeat = true;
  int retry=0;
  do {
      try {
        data->kinesis_video_stream->stopSync();
        hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        data->kinesis_video_stream->resetStream();
        do_repeat = false;
      } catch (runtime_error &err) {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Failed to reset kinesis video stream : retrying \n", __FILE__, __LINE__);
        this_thread::sleep_for(std::chrono::seconds(2));
        retry++;
        if ( retry > KVSINITMAXRETRY ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : FATAL : Max retry reached in reset stream exit process %d\n", __FILE__, __LINE__);
          exit(1);
        }
      }
      {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        data->connection_error = false;
      }
  } while(do_repeat);
}
/************************************************* common api's end*****************************************/

/************************************************* video api's start ***************************************/

static void create_kinesis_video_frame(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
                               void *data, size_t len) {

 frame->flags = flags;
 frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
 frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
 //frame->duration = 0; // with audio, frame can get as close as 0.01ms
 frame->duration = DEFAULT_FRAME_DURATION_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
 frame->size = static_cast<UINT32>(len);
 frame->frameData = reinterpret_cast<PBYTE>(data);
 frame->trackId = DEFAULT_TRACK_ID;
 //std::cout << "DTS: " << setw(10) << frame->decodingTs << "PTs: " << setw(10) << frame->presentationTs << std::endl;
}

static bool put_frame(shared_ptr<KinesisVideoStream> kinesis_video_stream, const nanoseconds &pts, const nanoseconds &dts,
              FRAME_FLAGS flags, void *data, size_t len) {
 Frame frame;
 create_kinesis_video_frame(&frame, pts, dts, flags, data, len);
 return kinesis_video_stream->putFrame(frame);
}

static GstFlowReturn on_new_sample(GstElement *sink, CustomData *data) {

 GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));
 GstCaps *gstcaps = gst_sample_get_caps(sample);
 GstStructure *gststructforcaps = gst_caps_get_structure(gstcaps, 0);
 GstFlowReturn ret = GST_FLOW_OK;
 GstBuffer *buffer;
 size_t buffer_size;
 bool delta, dropFrame;
 bool streamstartstatus = true;

 if (!data->stream_in_progress) {
   const GValue *gstStreamFormat = gst_structure_get_value(gststructforcaps, "codec_data");
   gchar *cpd = gst_value_serialize(gstStreamFormat);
   //LOG_INFO("video only case : adding codec private data : " << cpd );
   RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : codec private data is %s\n", __FILE__, __LINE__,cpd);
   if ( (NULL == cpd) || (!strlen(cpd)) ) {
    streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_H264VIDEO_CAM_STREAM3));
   }else {
    streamstartstatus = data->kinesis_video_stream->start(std::string(cpd));
    //check stream status for retry
    if( false == streamstartstatus ) {
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Error in starting stream with valid codec id hence retrying with default\n", __FILE__, __LINE__);
      streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_H264VIDEO_CAM_STREAM3));
    }
  }
  g_free(cpd);
  //we have tried stream start with default and non-default cpd's
  if( true == streamstartstatus ) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : stream started \n", __FILE__, __LINE__);
    data->stream_in_progress = true;
  } else {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error - Failed to start stream\n", __FILE__, __LINE__);
    gst_sample_unref(sample);
    {
      std::lock_guard<std::mutex> lk(custom_data_mtx);
      data->connection_error = true;
      if( data->mainloop_running ) {
        LOG_ERROR("stream start error : Main loop quit done" );
        data->mainloop_running = false;
        g_main_loop_quit(data->main_loop);
      }
    }
    return GST_FLOW_ERROR;
  }
 }

 bool connection_error;
 {
   std::lock_guard<std::mutex> lk(custom_data_mtx);
   connection_error = data->connection_error;
 }

 // dropping all incoming frames as we are restarting streams. A better solution in
 // the future is to direct them to file storage.
 if (!connection_error) {

   GstBuffer *buffer = gst_sample_get_buffer(sample);
   size_t buffer_size = gst_buffer_get_size(buffer);
   
   dropFrame =  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                 GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
                 (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
                 (!GST_BUFFER_PTS_IS_VALID(buffer)); //frame with invalid pts cannot be processed.
    if (dropFrame) {
      if (!GST_BUFFER_PTS_IS_VALID(buffer)) {
          LOG_WARN("on_new_sample : Dropping frame due to invalid presentation timestamp.");
      } else {
          LOG_WARN("on_new_sample : Dropping invalid frame.");
      }
      goto Cleanup;
    }

   FRAME_FLAGS kinesis_video_flags = FRAME_FLAG_NONE;
   bool delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

   if (buffer_size > data->frame_data_size) {
     delete[] data->frame_data;
     data->frame_data_size = buffer_size * 2;
     data->frame_data = new uint8_t[data->frame_data_size];
   }

   //if abs timestamp is disabled
   if( ! data->gkvsclip_abstime ) {
    uint64_t current_pts = buffer->pts;

    LOG_DEBUG("buffer_PTS from gst pipeline "<<  buffer->pts );
    LOG_DEBUG("buffer_DTS from gst pipeline "<<  buffer->dts );

    data->max_frame_pts = MAX(data->max_frame_pts, buffer->pts);

    buffer->pts = buffer->pts + data->base_pts + data->producer_start_time;
    buffer->dts = buffer->dts + data->base_pts + data->producer_start_time;
   }

  //store key frame timestamps
   if (!delta) {
    if (data->first_frame) {
      data->first_frame = false;
      data->firstkeyframetimestamp = buffer->pts;
      kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
    }else {
      //std::cout << "Not setting key frames for this clip again";
      kinesis_video_flags = FRAME_FLAG_NONE;
     }
   } else {
    kinesis_video_flags = FRAME_FLAG_NONE;
   }

   //std::cout << "buffer_pts videoonly " << buffer->pts << std::endl;
   //std::cout << "buffer_dts videoonly" << buffer->dts << std::endl;
   LOG_DEBUG("buffer_pts videoonly pts" <<  buffer->pts );
   LOG_DEBUG("buffer_pts videoonly dts" <<  buffer->dts );

   gst_buffer_extract(buffer, 0, data->frame_data, buffer_size);
 
   try {
    //std::cout << kinesis_video_flags << std::endl;
    if (!put_frame(data->kinesis_video_stream, std::chrono::nanoseconds(buffer->pts),
                    std::chrono::nanoseconds(buffer->dts), kinesis_video_flags, data->frame_data, buffer_size)) {
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error - Failed to put frame\n", __FILE__, __LINE__);
      gst_sample_unref(sample);
      {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        data->connection_error = true;
        if( data->mainloop_running ) {
          LOG_ERROR("put_frame error : Main loop quit done" );
          data->mainloop_running = false;
          g_main_loop_quit(data->main_loop);
        }
      }
      return GST_FLOW_ERROR;
    }
   }catch (runtime_error &err) {
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): RunTime Error - Failed to put frame \n", __FILE__, __LINE__);
      gst_sample_unref(sample);
      {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        data->connection_error = true;      
        if( data->mainloop_running ) {
          LOG_ERROR("put_frame runtime error : Main loop quit done" );
          data->mainloop_running = false;
          g_main_loop_quit(data->main_loop);
        }
      }
      return GST_FLOW_ERROR;
   }
 } else {
   gst_sample_unref(sample);
   return GST_FLOW_EOS;
 }

Cleanup:
 gst_sample_unref(sample);
 return GST_FLOW_OK;
}

static GstFlowReturn on_no_sample(GstElement *sink, CustomData *data) {
  LOG_DEBUG("Underflow - No Data");
}

/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details */
  gst_message_parse_error(msg, &err, &debug_info);
  //g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  LOG_ERROR("Error received from element " << GST_OBJECT_NAME (msg->src) << "Error : " << err->message );
  //g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error(&err);
  g_free(debug_info);

#if 0
  if( data->mainloop_running ) {
    LOG_DEBUG("error_cb : Main loop quit done" );
    data->mainloop_running = false;
    g_main_loop_quit(data->main_loop);
  }
#endif
}


static void cb_ts_pad_created(GstElement *element, GstPad *pad, CustomData *data) {
  gchar *pad_name = gst_pad_get_name(pad);

  LOG_DEBUG("New TS source pad found: " << pad_name);
  if (g_str_has_prefix(pad_name, videopad)) {
    if (gst_element_link_pads(data->tsdemux, pad_name, data->queue, "sink")) {
      LOG_DEBUG("Video source pad linked successfully.");
    } else {
      LOG_ERROR("Video source pad link failed");
    }
    g_free(pad_name);
  } else if (g_str_has_prefix(pad_name, audiopad)) {
    LOG_DEBUG("Audio pad has been detected");
    g_free(pad_name);
  }

}

//video pipeline gstreamer callback api
static gboolean cb_message(GstBus *bus, GstMessage *msg, gpointer newdata) {
 CustomData* data = (CustomData*) newdata;
  FRAME_FLAGS kinesis_video_flags;
  kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
  uint8_t dummy_frame;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err;
      gchar *debug;

      gst_message_parse_error(msg, &err, &debug);
      LOG_ERROR("Error: " << err->message);
      g_error_free(err);
      g_free(debug);

      if( data->mainloop_running ) {
        LOG_ERROR("cb_message_GST_MESSAGE_ERROR : Main loop quit done" );
        data->mainloop_running = false;
        g_main_loop_quit(data->main_loop);
      }
      break;
    }
    case GST_MESSAGE_EOS: {

      //sending EoFr
      if (data->kinesis_video_stream != NULL) {
        LOG_DEBUG("Sending EoFr");
        data->kinesis_video_stream->putFrame(EOFR_FRAME_INITIALIZER);

        //reset state
        data->first_frame = true;
      }

      if( data->mainloop_running ) {
        LOG_DEBUG("cb_message_GST_MESSAGE_EOS : Main loop quit done" );
        data->mainloop_running = false;
        g_main_loop_quit(data->main_loop);
      }
      
      break;
    }
    case GST_MESSAGE_CLOCK_LOST: {
      /* Get a new clock */
      LOG_DEBUG("message : GST_MESSAGE_CLOCK_LOST.");
      
      if( data->mainloop_running ) {
        LOG_ERROR("cb_message_GST_MESSAGE_CLOCK_LOST : Main loop quit done" );
        data->mainloop_running = false;
        g_main_loop_quit(data->main_loop);
      }
      break;
    }
    default:
      /* Unhandled message */
      LOG_DEBUG("message type : " << gst_message_type_get_name(GST_MESSAGE_TYPE (msg)));
      LOG_DEBUG("message : GST_UNHANDLED_MESSAGE.");
      break;
  }

  return TRUE;
}

//video gstreamer pipeline_init
static int video_pipeline_init() {
  LOG_DEBUG("video_pipeline_init - Enter");

  /* Init GStreamer */
  gst_init(NULL,NULL);

  //gst buffer
  data.frame_data = new uint8_t[DEFAULT_FRAME_DATA_SIZE_BYTE];

  LOG_DEBUG("video_pipeline_init - Initializing gstreamer pipeline");

  /* Create the elements */
  data.source = gst_element_factory_make("filesrc", "source");
  
  /*check gst elements */
  if (!data.source) {
    //g_printerr("source element could not be created.\n");
    LOG_ERROR("video_pipeline_init - source element could not be created");
    return 1;
  }

  data.tsdemux = gst_element_factory_make("tsdemux", "tsdemux");
  if (!data.tsdemux) {
    //g_printerr("tsdemux element could not be created.\n");
    LOG_ERROR("video_pipeline_init - tsdemux element could not be created");
    return 1;
  }

  data.queue = gst_element_factory_make("queue", "queue");
  if (!data.queue) {
    //g_printerr("queue element could not be created.\n");
    LOG_ERROR("video_pipeline_init - queue element could not be created");
    return 1;
  }

  data.h264parse = gst_element_factory_make("h264parse", "h264parse");
  if (!data.h264parse) {
    //g_printerr("data.h264parse element could not be created.\n");
    LOG_ERROR("video_pipeline_init - h264parse element could not be created");
    return 1;
  }

  data.filter = gst_element_factory_make("capsfilter", "filter");
  if (!data.filter) {
    //g_printerr("filter element could not be created.\n");
    LOG_ERROR("video_pipeline_init - filter element could not be created");
    return 1;
  }
  data.appsink = gst_element_factory_make("appsink", "appsink");
  if (!data.appsink) {
    //g_printerr("appsink element could not be created.\n");
    LOG_ERROR("video_pipeline_init - appsink element could not be created");
    return 1;
  }

  /*
   *
   * gst-launch-1.0 filesrc -e location=20180731175011.ts ! tsdemux name=ts ! h264parse ! video/x-h264,stream-format=avc,alignment=au ! queue ! kvssink
   *
   */

  /* Create an empty pipeline */
  data.pipeline = gst_pipeline_new("fileingest-pipeline");

  g_object_set(G_OBJECT (data.tsdemux), "name", "ts", NULL);

  GstCaps *h264_caps = gst_caps_new_simple("video/x-h264",
                                           "stream-format", G_TYPE_STRING, "avc",
                                           "alignment", G_TYPE_STRING, "au",
                                           NULL);

  g_object_set(G_OBJECT (data.filter), "caps", h264_caps, NULL);
  gst_caps_unref(h264_caps);

  /* Configure source and appsink */
  //g_object_set(G_OBJECT (data.source), "blocksize", 655356, NULL);
  //g_object_set(G_OBJECT (data.source), "blocksize", 4096, NULL);

  //source property - 128k
  g_object_set(G_OBJECT (data.source), "blocksize", 131072, NULL);

  if ( ! data.gkvsclip_livemode ) {
    g_object_set(G_OBJECT (data.appsink), "emit-signals", TRUE, "sync", FALSE, NULL); // offline mode
  } else {
    g_object_set(G_OBJECT (data.appsink), "emit-signals", TRUE, "sync", TRUE, NULL); // realtime mode
  }
  //g_object_set(G_OBJECT (data.appsink), "drop", TRUE, "max-buffers", 512, NULL);

  /* Build the pipeline */
  if (!data.pipeline || !data.source || !data.tsdemux || !data.queue || !data.h264parse || !data.filter || !data.appsink) {
    g_printerr("video_pipeline_init - Not all elements could be created.\n");
    return 1;
  }

  gst_bin_add_many(GST_BIN (data.pipeline), data.source, data.tsdemux, data.queue, data.h264parse, data.filter, data.appsink,
                   NULL);

  if (!gst_element_link_many(data.queue, data.h264parse, data.filter, data.appsink, NULL)) {
    LOG_ERROR("video_pipeline_init - First Elements could not be linked.");
    gst_object_unref(data.pipeline);
    return 1;
  }

  if (!gst_element_link_many(data.source, data.tsdemux, NULL)) {
    LOG_ERROR("video_pipeline_init - Elements could not be linked.");
    gst_object_unref(data.pipeline);
    return 1;
  }

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
#if 1
  data.bus = gst_element_get_bus(data.pipeline);
  gst_bus_add_signal_watch(data.bus);
#else
  data.bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
#endif
  
  g_signal_connect(data.appsink, "new-sample", G_CALLBACK(on_new_sample), &data);
  g_signal_connect(data.queue, "underrun", G_CALLBACK(on_no_sample), &data);
  
#if 1
  g_signal_connect (G_OBJECT(data.bus), "message::error", (GCallback) error_cb, &data);
  g_signal_connect (G_OBJECT(data.bus), "message::eos", (GCallback) cb_message, &data);
#else
  data.m_busId = gst_bus_add_watch_full(data.bus, G_PRIORITY_DEFAULT, cb_message, &data, NULL);
  //m_bus_event_source_id = gst_bus_add_watch_full(data.bus, G_PRIORITY_DEFAULT, cb_message, &data, NULL);
#endif
  g_signal_connect(data.tsdemux, "pad-added", (GCallback) cb_ts_pad_created, &data);

  gst_object_unref(data.bus);
  LOG_DEBUG("gstreamer pipeline is ready and video_pipeline_init complete.");

  LOG_DEBUG("video_pipeline_init - Exit");

  return 0;
}

//video_pipeline_uninit
static int video_pipeline_uninit() {

  LOG_DEBUG("video_pipeline_uninit - Enter");
  
  //if (data.main_loop!=NULL) 
  {
    LOG_DEBUG("video_pipeline_uninit - Clearing resources after main loop finish");
    //g_source_remove(data.m_busId);
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    LOG_DEBUG("video_pipeline_uninit - Enter - 1");

    g_main_loop_unref(data.main_loop);
    gst_bus_remove_signal_watch(data.bus);
    //data.main_loop =  NULL;
  }

  LOG_DEBUG("video_pipeline_uninit - Before free");

  if ( ! data.gkvsclip_audio ) {
    delete[] data.frame_data;
  }

  LOG_DEBUG("video_pipeline_uninit - Exit");
  return 0;
}

/************************************************* audio api's start *****************************************/

void create_kinesis_video_frame_av(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
                                void *data, size_t len, UINT64 track_id) {
    frame->flags = flags;
    frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    frame->duration = 0; // with audio, frame can get as close as 0.01ms
    frame->size = static_cast<UINT32>(len);
    frame->frameData = reinterpret_cast<PBYTE>(data);
    frame->trackId = track_id;
}

bool all_stream_started(CustomData *data) {
    bool started = true;
    if (data->stream_started.size() < data->total_track_count) {
        started = false;
    } else {
        for (map<int, bool>::iterator it = data->stream_started.begin(); it != data->stream_started.end(); ++it) {
            if (!it->second) {
                started = false;
                break;
            }
        }
    }
    return started;
}

static GstFlowReturn on_new_sample_av(GstElement *sink, CustomData *data) {
    std::unique_lock<std::mutex> lk(data->audio_video_sync_mtx);
    GstSample *sample = nullptr;
    GstBuffer *buffer;
    size_t buffer_size;
    bool delta, dropFrame;
    FRAME_FLAGS kinesis_video_flags;
    gchar *g_stream_handle_key = gst_element_get_name(sink);
    int track_id = (string(g_stream_handle_key).back()) - '0';
    g_free(g_stream_handle_key);
    uint8_t *data_buffer;
    Frame frame;
    GstFlowReturn ret = GST_FLOW_OK;
    bool streamstartstatus = true;

    sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));

    // extract cpd for the first frame for each track
    if (!data->stream_started[track_id]) {
        GstCaps *gstcaps = (GstCaps *) gst_sample_get_caps(sample);
        GST_LOG("caps are %" GST_PTR_FORMAT, gstcaps);
        GstStructure *gststructforcaps = gst_caps_get_structure(gstcaps, 0);
        const GValue *gstStreamFormat = gst_structure_get_value(gststructforcaps, "codec_data");
        gchar *cpd = gst_value_serialize(gstStreamFormat);
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) :  codec private data is %s \n", __FILE__, __LINE__,cpd);
        
        if ( (NULL == cpd) || (!strlen(cpd)) ) {
          if( DEFAULT_VIDEO_TRACKID == track_id ) {
            streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_H264VIDEO_CAM_STREAM3),track_id);
          } else if ( DEFAULT_AUDIO_TRACKID == track_id ) {
            streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_AACAUDIO),track_id);
          } else {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Invalid track id\n", __FILE__, __LINE__);
          }
        }else {
          streamstartstatus = data->kinesis_video_stream->start(std::string(cpd),track_id);
          //check stream status for retry
          if( false == streamstartstatus ) {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Error in starting stream with valid codec id hence retrying with default\n", __FILE__, __LINE__);
            if( DEFAULT_VIDEO_TRACKID == track_id ) {
              streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_H264VIDEO_CAM_STREAM3),track_id);
            } else if ( DEFAULT_AUDIO_TRACKID == track_id ) {
              streamstartstatus = data->kinesis_video_stream->start(std::string(DEFAULT_CODECID_AACAUDIO),track_id);
            }
          }
        }
        g_free(cpd);
        //we have tried stream start with default and non-default cpd's
        if( true == streamstartstatus ) {
          RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : stream started for track %d\n", __FILE__, __LINE__,track_id);
          data->stream_started[track_id] = true;
        }

        // block pipeline until cpd for all tracks have been received. Otherwise we will get STATUS_INVALID_STREAM_STATE
        if (!all_stream_started(data)) {
            data->audio_video_sync_cv.wait_for(lk, seconds(DEFAULT_AUDIO_VIDEO_DRIFT_TIMEOUT_SECOND), [data]{
                return all_stream_started(data);
            });

            if(!all_stream_started(data)) {
                LOG_ERROR("getcpd : Drift between audio and video is above threshold");
                gst_sample_unref(sample);
                {
                  std::lock_guard<std::mutex> lk(custom_data_mtx);
                  data->connection_error = true;
                  if( data->mainloop_running ) {
                    LOG_ERROR("getcpd : Main loop quit done" );
                    data->mainloop_running = false;
                    g_main_loop_quit(data->main_loop);
                  }
                }
                return GST_FLOW_ERROR;
            }
        } else {
            data->audio_video_sync_cv.notify_all();
        }
    }

    buffer = gst_sample_get_buffer(sample);
    buffer_size = gst_buffer_get_size(buffer);

    dropFrame =  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                 GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
                 (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
                 (!GST_BUFFER_PTS_IS_VALID(buffer)); //frame with invalid pts cannot be processed.
    if (dropFrame) {
        if (!GST_BUFFER_PTS_IS_VALID(buffer)) {
            LOG_WARN("on_new_sample_av : Dropping frame due to invalid presentation timestamp.");
        } else {
            LOG_WARN("on_new_sample_av : Dropping invalid frame.");
        }
        goto CleanUp;
    }

    delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    kinesis_video_flags = FRAME_FLAG_NONE;

    {
      if (!data->putFrameHelper->putFrameFailed()) {
        data->put_frame_failed = true;
        ret = GST_FLOW_ERROR;
        goto CleanUp;
      }

      LOG_DEBUG("audio_video buffer_PTS from gst pipeline "<<  buffer->pts << " track_id  " << track_id );
      LOG_DEBUG("audio_video buffer_DTS from gst pipeline "<<  buffer->dts  << " track_id  " << track_id );
      
      //if abs timestamp is disabled
      if( ! data->gkvsclip_abstime ) {
        uint64_t current_pts = buffer->pts;

        data->max_frame_pts = MAX(data->max_frame_pts, buffer->pts);

        // make sure the timestamp is continuous across multiple clips
        buffer->pts = buffer->pts + data->base_pts + data->producer_start_time;
        //buffer->dts = buffer->pts;
        buffer->dts = buffer->dts + data->base_pts + data->producer_start_time;

        LOG_DEBUG("buffer_pts video_audio " <<  buffer->pts << " track_id  " << track_id );

        /*if (data->max_frame_pts > current_pts) {
          //std::cout << "Skipping current " << buffer->pts << " frame time code less than previous " << data->max_frame_pts << std::endl;
          goto CleanUp;
        }*/
      }


      if (!delta && track_id == DEFAULT_VIDEO_TRACKID) {
        if (data->first_frame) {
        // start cutting fragment at second video key frame because we can have audio frames before first video key frame
        data->first_frame = false;
        data->firstkeyframetimestamp = buffer->pts;
      }
      }
      //std::cout << "buffer_pts audio_video " << buffer->pts << " track_id  " << track_id << std::endl;
      //std::cout << "buffer_dts audio_video " << buffer->dts << std::endl;
      //LOG_DEBUG("buffer_pts audio_video " <<  buffer->pts << " track_id  " << track_id );

      /*if (CHECK_FRAME_FLAG_KEY_FRAME(kinesis_video_flags)) {
          data->key_frame_pts = buffer->pts;
      }*/
    }
#if 0
    //current buffer level in queue
    guint video_level_buffer;
    guint video_level_bytes;

    //g_print("Current video queue buffer level : %u", data->video_queue->get_property("current-level-buffers" ) );
    g_object_get (G_OBJECT (data->video_queue), "current-level-buffers", &video_level_buffer, NULL);
    g_object_get (G_OBJECT (data->video_queue), "current-level-bytes", &video_level_bytes, NULL);

    g_print("Current video queue level buffer : %u\n", video_level_buffer );
    g_print("Current video queue level bytes  : %u\n", video_level_bytes );

    guint audio_level_buffer;
    guint audio_level_bytes;

    g_object_get (G_OBJECT (data->audio_queue), "current-level-buffers", &audio_level_buffer, NULL);
    g_object_get (G_OBJECT (data->audio_queue), "current-level-bytes", &audio_level_bytes, NULL);

    g_print("Current audio queue level buffer : %u\n", audio_level_buffer );
    g_print("Current audio queue level bytes  : %u\n", audio_level_bytes );
#endif

    //LOG_DEBUG("getFrameDataBuffer buffer_size " <<  buffer_size << " track_id  " << track_id );
    data_buffer = data->putFrameHelper->getFrameDataBuffer(buffer_size, track_id == DEFAULT_VIDEO_TRACKID);
    // if there is too much drift between audio and video, block one of the stream.
    if (data_buffer == nullptr) {
      data->pipeline_blocked = true;
      data->audio_video_sync_cv.wait_for(lk, seconds(DEFAULT_AUDIO_VIDEO_DRIFT_TIMEOUT_SECOND), [data, &data_buffer, buffer_size, track_id]{
          data_buffer = data->putFrameHelper->getFrameDataBuffer(buffer_size, track_id == DEFAULT_VIDEO_TRACKID);
          return data_buffer != nullptr;
      });
      if (data_buffer == nullptr) {
          LOG_ERROR("getFrameDataBuffer : Drift between audio and video is above threshold");
          gst_sample_unref(sample);
          {
            std::lock_guard<std::mutex> lk(custom_data_mtx);
            data->connection_error = true;
            if( data->mainloop_running ) {
              LOG_ERROR("getFrameDataBuffer : Main loop quit done" );
              data->mainloop_running = false;
              g_main_loop_quit(data->main_loop);
            }
          }
          return GST_FLOW_ERROR;
      }
      data->pipeline_blocked = false;
    }
    gst_buffer_extract(buffer, 0, data_buffer, buffer_size);

    create_kinesis_video_frame_av(&frame, std::chrono::nanoseconds(buffer->pts), std::chrono::nanoseconds(buffer->dts),
                               kinesis_video_flags, data_buffer, buffer_size, track_id);
    {
      frame.decodingTs = 0;
    }

    data->putFrameHelper->putFrameMultiTrack(frame, track_id == DEFAULT_VIDEO_TRACKID);
    
    // putFrameMultiTrack can trigger state change in putFrameHelper that can allow the other track to proceed.
    if (data->pipeline_blocked) {
      data->audio_video_sync_cv.notify_all();
    }

CleanUp:
    if (sample != nullptr) {
      gst_sample_unref(sample);
    }

    return ret;
}

/* Copyright (C) GStreamer developers
   Here licensed under the MIT license
*/

/* This function is called when an error message is posted on the bus */
static void error_cb_av(GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info;

    /* Print error details on the screen */
    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
    g_clear_error(&err);
    g_free(debug_info);

#if 0
    if( data->mainloop_running ) {
      LOG_DEBUG("error_cb_av : Main loop quit done" );
      data->mainloop_running = false;
      g_main_loop_quit(data->main_loop);
    }
#endif
}

static void eos_cb_av(GstElement *sink, CustomData *data) {
  if (!data->eos_triggered.load()) {
    // Media pipeline for one track has ended. Next time eos_cb_av is called means the entire file has been received.
    data->eos_triggered = true;
    data->audio_video_sync_cv.notify_all();
  } else {

#if 0
  if( ! data->gkvsclip_abstime ) {
    // bookkeeping base_pts. add 1ms to avoid overlap.
    data->base_pts = data->base_pts + data->max_frame_pts + duration_cast<nanoseconds>(milliseconds(1)).count();
    data->max_frame_pts = 0;
    LOG_DEBUG("eos_cb_av : base_pts : " << data->base_pts << " max_frame_pts : " << data->max_frame_pts );
  }
#endif

    // signal putFrameHelper to release all frames still in queue.
    if( !data->put_frame_flushed.load() ) 
    {
      RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs put frame flush called in EOS\n", __FILE__, __LINE__);
      data->putFrameHelper->flush();
      if (!data->putFrameHelper->putFrameFailed()) {
        LOG_ERROR("Putframe error detected.");
        data->put_frame_failed = true;
      }
      data->putFrameHelper->putEofr();

      data->put_frame_flushed = true;
      RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs put frame flush complete in EOS\n", __FILE__, __LINE__);
    }
    
#if 0
    //sending EoFr
    if (data->kinesis_video_stream != NULL) {
      LOG_DEBUG("Sending EoFr");
      data->kinesis_video_stream->putFrame(EOFR_FRAME_INITIALIZER);
      //data->putFrameHelper->putEofr();

      //reset state
      //data->first_frame = true;
    }
#endif

    LOG_DEBUG("Terminating pipeline due to EOS");
    if( data->mainloop_running ) {
      data->mainloop_running = false;
      g_main_loop_quit(data->main_loop);
    }
  }
}

static gboolean demux_pad_cb_av(GstElement *element, GstPad *pad, CustomData *data) {
    GstPad *video_sink = gst_element_get_static_pad(GST_ELEMENT(data->video_queue), "sink");
    GstPad *audio_sink = gst_element_get_static_pad(GST_ELEMENT(data->audio_queue), "sink");

    GstPadLinkReturn link_ret;
    gboolean ret = TRUE;
    gchar *pad_name = gst_pad_get_name(pad);

    // link queue to corresponding sinks
    if (gst_pad_can_link(pad, video_sink)) {
        link_ret = gst_pad_link(pad, video_sink);
    } else {
        link_ret = gst_pad_link(pad, audio_sink);
    }

    gst_object_unref(video_sink);
    gst_object_unref(audio_sink);

    if (link_ret != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link demuxer's pad " << string(pad_name));
        ret = FALSE;
    }
    g_free(pad_name);
    return ret;
}

//video-audio gst pipeline
int video_audio_pipeline_init() {
    GstStateChangeReturn ret;

    /* init GStreamer */
    gst_init(NULL,NULL);

    //reset state
    data.eos_triggered = false;
    data.put_frame_failed = false;
    
    //reset state
    data.first_frame = true;


    //GstElement *data.appsink_video, *data.appsink_audio, *audio_queue, *video_queue, *pipeline, *video_filter, *data.audio_filter;
    string video_caps_string, audio_caps_string;
    GstCaps *caps;

    video_caps_string = "video/x-h264, stream-format=(string) avc, alignment=(string) au";
    audio_caps_string = "audio/mpeg, stream-format=(string) raw";

    data.video_filter = gst_element_factory_make("capsfilter", "video_filter");
    if(!data.video_filter) {
      g_printerr("video_filter element could not be created:\n");
    }

    caps = gst_caps_from_string(video_caps_string.c_str());
    g_object_set(G_OBJECT (data.video_filter), "caps", caps, NULL);
    gst_caps_unref(caps);

    data.audio_filter = gst_element_factory_make("capsfilter", "data.audio_filter");
    if(!data.audio_filter) {
      g_printerr("audio_filter element could not be created:\n");
    }

    caps = gst_caps_from_string(audio_caps_string.c_str());
    g_object_set(G_OBJECT (data.audio_filter), "caps", caps, NULL);
    gst_caps_unref(caps);

    // hardcoding appsink name and track id
    const string video_appsink_name = "appsink_" + to_string(DEFAULT_VIDEO_TRACKID);
    const string audio_appsink_name = "appsink_" + to_string(DEFAULT_AUDIO_TRACKID);

    data.appsink_video = gst_element_factory_make("appsink", (gchar *) video_appsink_name.c_str());
    if(!data.appsink_video) {
      g_printerr("appsink_video element could not be created:\n");
    }
    
    data.appsink_audio = gst_element_factory_make("appsink", (gchar *) audio_appsink_name.c_str());
    if(!data.appsink_audio) {
      g_printerr("appsink_audio element could not be created:\n");
    }

    /* configure appsink */
    g_object_set(G_OBJECT (data.appsink_video), "emit-signals", TRUE, "sync", FALSE, NULL);
    g_signal_connect(data.appsink_video, "new-sample", G_CALLBACK(on_new_sample_av), &data);
    g_signal_connect(data.appsink_video, "eos", G_CALLBACK(eos_cb_av), &data);
    g_object_set(G_OBJECT (data.appsink_audio), "emit-signals", TRUE, "sync", FALSE, NULL);
    g_signal_connect(data.appsink_audio, "new-sample", G_CALLBACK(on_new_sample_av), &data);
    g_signal_connect(data.appsink_audio, "eos", G_CALLBACK(eos_cb_av), &data);
    LOG_DEBUG("appsink configured");

    data.audio_queue = gst_element_factory_make("queue", "audio_queue");
    if(!data.audio_queue) {
      g_printerr("audio_queue element could not be created:\n");
    }
    
    data.video_queue = gst_element_factory_make("queue", "video_queue");
    if(!data.video_queue) {
      g_printerr("video_queue element could not be created:\n");
    }

#if 0
    g_object_set(G_OBJECT (data.audio_queue), "max-size-buffers", 100, "max-size-bytes", 1048576, NULL);
    g_object_set(G_OBJECT (data.video_queue), "max-size-buffers", 100, "max-size-bytes", 1048576, NULL);
    g_object_set(G_OBJECT (data.appsink_video), "drop", TRUE, "max-buffers", 512, NULL);
    g_object_set(G_OBJECT (data.appsink_audio), "drop", TRUE, "max-buffers", 512, NULL);
#endif

    data.pipeline = gst_pipeline_new("audio-video-kinesis-pipeline");

    if (!data.pipeline || !data.appsink_video || !data.appsink_audio || !data.video_queue || !data.audio_queue || !data.video_filter || !data.audio_filter) {
        g_printerr("Not all elements could be created 1:\n");
        return 1;
    }


    data.source = gst_element_factory_make("filesrc", "source");
    //g_object_set(G_OBJECT (filesrc), "location", file_path.c_str(), NULL);
    //source property - 128k
    g_object_set(G_OBJECT (data.source), "blocksize", 131072, NULL);

    data.tsdemux = gst_element_factory_make("tsdemux", "tsdemux");
    g_object_set(G_OBJECT (data.tsdemux), "name", "ts", NULL);

    data.h264parse = gst_element_factory_make("h264parse", "h264parse");
    data.aac_parse = gst_element_factory_make("aacparse", "aac_parse");
    if(!data.aac_parse) {
      g_printerr("aac_parse element could not be created:\n");
    }

    if (!data.tsdemux  || !data.source || !data.h264parse || !data.aac_parse) {
        g_printerr("Not all elements could be created 2:\n");
        return 1;
    }

    gst_bin_add_many(GST_BIN (data.pipeline), data.appsink_video, data.appsink_audio, data.source, data.tsdemux, data.h264parse, data.aac_parse, data.video_queue,
                      data.audio_queue, data.video_filter, data.audio_filter,
                      NULL);

    if (!gst_element_link_many(data.source, data.tsdemux,
                                NULL)) {
        g_printerr("Elements could not be linked.\n");
        gst_object_unref(data.pipeline);
        return 1;
    }

    if (!gst_element_link_many(data.video_queue, data.h264parse, data.video_filter, data.appsink_video,
                                NULL)) {
        g_printerr("Video elements could not be linked.\n");
        gst_object_unref(data.pipeline);
        return 1;
    }

    if (!gst_element_link_many(data.audio_queue, data.aac_parse, data.audio_filter, data.appsink_audio,
                                NULL)) {
        g_printerr("Audio elements could not be linked.\n");
        gst_object_unref(data.pipeline);
        return 1;
    }

    g_signal_connect(data.tsdemux, "pad-added", G_CALLBACK(demux_pad_cb_av), &data);

    /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
    data.bus = gst_element_get_bus(data.pipeline);
    gst_bus_add_signal_watch(data.bus);
    g_signal_connect (G_OBJECT(data.bus), "message::error", (GCallback) error_cb_av, &data);
    gst_object_unref(data.bus);

    return 0;
}
/************************************************* audio api's end *****************************************/

/************************************************* Wrapper api's start *************************************/
int kvs_init(int stream_id, uint64_t storageMem = 0) {
  LOG_INFO("kvs_init - Enter");

  static bool islogconfigdone = false;

  if (storageMem != 0) {
    data.storageMem = storageMem;
  }

  //init kvs log config
  if( false == islogconfigdone ) {
    char const *kvslogconfig;

    if (nullptr == (kvslogconfig = getenv(KVS_LOG_CONFIG_ENV_VER))) {
      kvslogconfig = "/etc/kvs_log_configuration";
    }
          
    PropertyConfigurator::doConfigure(kvslogconfig);

    LOG_DEBUG("kvs_init - kvs log config :" << kvslogconfig);

    islogconfigdone = true;
  }

  //set stream id
  data.cvr_stream_id = stream_id;

  //init kinsis video
  try {
    kinesis_video_init(&data);
  } catch (runtime_error &err) {
    data.connection_error = true;
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Time out error in kinesis_video_init connection_error %d \n", __FILE__, __LINE__, data.connection_error );
    return -1;
  }

  data.connection_error = false;
  LOG_INFO("kvs_init - Exit");
  return 0;
}

//kvs_video_stream init
int kvs_stream_init( unsigned short& kvsclip_audio, unsigned short& kvsclip_abstime, unsigned short& kvsclip_livemode, unsigned short& contentchangestatus, uint64_t& hangdetecttime ) {
  LOG_INFO("kvs_stream_init - Enter");
  
  static bool isgstinitdone = false;
  int ret = 0;
  
  //update custom data parameters
  data.gkvsclip_audio = kvsclip_audio;
  data.gkvsclip_abstime = kvsclip_abstime;
  data.gkvsclip_livemode = kvsclip_livemode;

  if(data.gkvsclip_audio) {
    data.total_track_count = 2;
  } else {
    data.total_track_count = 1;
  }

  //Ensure video pipeline is deleted before stream init
  //Initilaize gst pipeline once for application if absolute timestamp is enabled
  if(data.gkvsclip_abstime) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Absolute time is enabled\n", __FILE__, __LINE__);
    
    //ensure gst pipeline is deleted in case it's initilaized before
    if ( true == isgstinitdone ) {
      video_pipeline_uninit();
      isgstinitdone = false;
    }

    if (isgstinitdone == false) {
      if(data.gkvsclip_audio) {
        ret = video_audio_pipeline_init();
        if( 0 != ret ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in videoaudio pipeline initilaiztion :%d\n", __FILE__, __LINE__,data.gkvsclip_abstime);
          return 3;
        }
      } else {
        ret = video_pipeline_init();
        if( 0 != ret ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in video pipeline initilaiztion :%d\n", __FILE__, __LINE__,data.gkvsclip_abstime );
          return 3;
        }
      }
      isgstinitdone = true;
    }
  }
  
  //In normal case init then recreate
  if( 0 == contentchangestatus ) {
    //init kinesis stream
    try {
      kinesis_video_stream_init(&data);
    } catch (runtime_error &err) {
      {
        std::lock_guard<std::mutex> lk(custom_data_mtx);
        data.connection_error = true;
      }
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Time out error in kinesis_video_stream_init connection_error %d \n", __FILE__, __LINE__, data.connection_error );
      recreate_stream(&data,hangdetecttime);
    }
  } else { //in content change recreate stream
    recreate_stream(&data,hangdetecttime);
  }

  {
      std::lock_guard<std::mutex> lk(custom_data_mtx);
      data.connection_error = false;
  }
  LOG_INFO("kvs_stream_init - Exit");
  return 0;
}

//kvs_stream_play
/* For every video clip create the pipeline, set the  GStreamer pipeline to playing state and stop the pipeline */
int kvs_stream_play( char *clip, unsigned short& clip_audio, unsigned short& clip_abstime, unsigned short& clip_livemode, uint64_t& hangdetecttime ) {
  bool connection_error = false;

  unsigned short clipaudioflag = clip_audio;
  unsigned short clipabstime = clip_abstime;
  unsigned short cliplivemode = clip_livemode;

  //Check if the clip content has changed - videoaudioclip -> videoclip / videoclip -> videoaudioclip
  if( data.gkvsclip_audio != clipaudioflag ) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Change in clip content found : prior audio enable flag : %d , current audio enable flag : %d\n", 
            __FILE__, __LINE__, data.gkvsclip_audio, clipaudioflag);
    return 1;
  }

  {
    std::lock_guard<std::mutex> lk(custom_data_mtx);
    connection_error = data.connection_error;
  }

  LOG_DEBUG("kvs_stream_play - clip name is :" << clip << " connection_error :" << connection_error );

  STRNCPY(data.clip_name, clip,MAX_STREAM_NAME_LEN);
  data.clip_name[MAX_STREAM_NAME_LEN -1] = '\0';

  GstStateChangeReturn ret;
  int retstatus=0;

  //Initilaize gst pipeline for every clip if absolute timestamp is not enabled
  if( ! data.gkvsclip_abstime) {
    if(data.gkvsclip_audio) {
        retstatus = video_audio_pipeline_init();
        if( 0 != retstatus ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in videoaudio pipeline initilaiztion \n", __FILE__, __LINE__);
          return 3;
        }
      } else {
        retstatus = video_pipeline_init();
        if( 0 != retstatus ) {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in video pipeline initilaiztion \n", __FILE__, __LINE__);
          return 3;
        }
      }
  }

  //RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Pipeline init done %s memory stats %ld\n", __FILE__, __LINE__, data.clip_name, compute_stats() );

  //reset states
  data.first_frame = true;
  data.firstkeyframetimestamp = 0;
  data.put_frame_flushed = false;

  g_object_set(G_OBJECT (data.source), "location", data.clip_name, NULL);

  LOG_DEBUG("kvs_stream_play - changing gstreamer pipeline to play state");
  
  /* start streaming */
  ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG_ERROR("kvs_stream_play - Unable to set the pipeline to the playing state.");
    //goto CleanUp;
    if(data.gkvsclip_abstime) {
      gst_element_set_state(data.pipeline, GST_STATE_NULL);
      g_main_loop_unref(data.main_loop);
    } else {
      video_pipeline_uninit();
    }
    return -1;
  }
  
  //LOG_DEBUG("GStreamer_start - After changing gstreamer pipeline to play state");
  //RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Pipeline state changed to playing %s memory stats %ld\n", __FILE__, __LINE__, data.clip_name, compute_stats() );


  //null check for main loop
  //if (data.main_loop ==  NULL) 
  {
    data.main_loop = g_main_loop_new(NULL, FALSE);
  }
  LOG_DEBUG("GStreamer_start - After creating main loop");
  
  hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  data.mainloop_running = true;
  g_main_loop_run(data.main_loop);

  uint64_t startts = data.producer_start_time;
  uint64_t firsframetts = data.firstkeyframetimestamp;
  auto starttstp = system_clock::time_point{nanoseconds{startts}};
  auto firsframettstp = system_clock::time_point{nanoseconds{firsframetts}};
  std::time_t starttstp_epochts = std::chrono::system_clock::to_time_t(starttstp);
  std::time_t firsframettstp_epochts = std::chrono::system_clock::to_time_t(firsframettstp);

#if 0
  std::string starttstpstring (std::ctime(&starttstp_epochts) );
  std::string firsframettstpstring (std::ctime(&firsframettstp_epochts) );
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): put media main loop finished producerts,firstkeyframets:%ld,%ld,%s,%s\n",
          __FILE__, __LINE__,starttstp_epochts,firsframettstp_epochts,starttstpstring.substr(0,starttstpstring.length()-1).c_str(),firsframettstpstring.substr(0,firsframettstpstring.length()-1).c_str() );
#endif
  hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): put media main loop finished producerts,firstkeyframets:%ld,%ld\n",__FILE__, __LINE__,starttstp_epochts,firsframettstp_epochts);
  
  if( ! data.gkvsclip_abstime ) {
    // bookkeeping base_pts. add 1ms to avoid overlap.
    data.base_pts = data.base_pts + data.max_frame_pts + duration_cast<nanoseconds>(milliseconds(1)).count();
    data.max_frame_pts = 0;
    LOG_DEBUG("GStreamer_start : base_pts : " << data.base_pts << " max_frame_pts : " << data.max_frame_pts );
  }

  //update timestamp
  data.clip_senttokvssdk_time = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
  //std::cout << "data.clip_senttokvssdk_time : " << data.clip_senttokvssdk_time << std::endl;
  
  if(data.gkvsclip_abstime) {
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    g_main_loop_unref(data.main_loop);
  } else {
    video_pipeline_uninit();
    this_thread::sleep_for(std::chrono::seconds(2));
  }

  //LOG_INFO("=======================gstreamer_start_Main_Loop_Finish_gst_Uninit=========================Memory stats (KB) " << compute_stats() );
  //RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): pipeline uninit complete for clip %s memory stats %ld\n", __FILE__, __LINE__, data.clip_name, compute_stats() );

  if( data.streaming_type == STREAMING_TYPE_OFFLINE ) {
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Awaiting for the upload status... \n", __FILE__, __LINE__);
    unique_lock<mutex> lock(data.clip_upload_mutex_);
    do {
        if (!data.clip_upload_status_var_.wait_for(lock,
                                                          std::chrono::seconds(
                                                                  CLIPUPLOAD_READY_TIMEOUT_DURATION_IN_SECONDS),
                                                          []() {
                                                              RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): signal received - return - %d \n", __FILE__, __LINE__,data.clip_upload_status_);
                                                              return data.clip_upload_status_;
                                                          })) {
            hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to get clip upload status - Timeout \n", __FILE__, __LINE__);
      	    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): kvsclip upload failed %s \n",__FILE__, __LINE__, data.clip_name );
            {
              std::lock_guard<std::mutex> lk(custom_data_mtx);
              data.connection_error = true;
            }
            break;
        }
        
        if (data.clip_upload_status_) {
          RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): clip upload status success. \n", __FILE__, __LINE__);
          data.clip_upload_status_ = false;
          {
            std::lock_guard<std::mutex> lk(custom_data_mtx);
            data.connection_error = false;
          }
          break;
        }

    } while (true);
  }
 
  //check any connection error during kinesis upload after gst main loop exits
  {
    std::lock_guard<std::mutex> lk(custom_data_mtx);
    connection_error = data.connection_error;
  }

  //connection error found in current clip upload
  if ( true == connection_error ) {
  
  #if 0
    static int retry = 0;
    uint64_t time_now = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();    
    uint64_t time_diff_millisecs = time_now - data.clip_senttokvssdk_time;
    
    if( time_diff_millisecs > CLIPUPLOAD_MAX_TIMEOUT_DURATION_IN_MILLISECOND ) 
    {
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Max timeout reached in clip upload abort upload \n", __FILE__, __LINE__ );
      RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):  kvsclip upload failed %s \n",__FILE__, __LINE__, data.clip_name );
      retry = 0;
      return -1;
    } else {
      retry++;
    }
    //RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):  Data upload failed retry : %d time_diff : %d\n", __FILE__, __LINE__, retry, time_diff_millisecs);
  #endif
  
    LOG_INFO(" KVS : Data upload failed");

    //recreate stream in error cases
    recreate_stream(&data,hangdetecttime);
    //reset_stream(&data,hangdetecttime);  //TBD
  
    //LOG_INFO("=======================Main_Loop_Finish_ConnError_KvsReInit=========================Memory stats (KB) " << compute_stats() ) ;
    //RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): stream was recreated for clip %s memory stats %ld\n", __FILE__, __LINE__, data.clip_name, compute_stats() );
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): stream was recreated for clip %s\n", __FILE__, __LINE__, data.clip_name);
    {
      std::lock_guard<std::mutex> lk(custom_data_mtx);
      connection_error = data.connection_error;
    }
    
    LOG_DEBUG("kvs_stream_play - Re-created network connection error status = " << ((connection_error == 0) ? "NO_ERROR" : "ERROR" ) );
    LOG_DEBUG("kvs_stream_play exit - reconnection");
    //return 2;
    return -1;

  } else { // no connection error in current clip upload
    LOG_INFO(" KVS : Data upload successful");
    LOG_DEBUG("kvs_stream_play exit - success.");
    return 0;
  }
}
#endif /* CVR_PLATFORM_RPI */
/************************************************* Wrapper api's end *************************************/
