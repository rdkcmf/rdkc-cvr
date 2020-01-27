#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <Logger.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <PutFrameHelper.h>
#include <atomic>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <queue>
#include <unordered_map>
#include "KinesisVideoProducer.h"
#include "CachingEndpointOnlyCallbackProvider.h"
#include <IotCertCredentialProvider.h>
#include "rdk_debug.h"
#include "kvsuploadCallback.h"

using namespace std;
using namespace com::amazonaws::kinesis::video;
using namespace log4cplus;

#ifdef __cplusplus
extern "C" {
#endif

#include "event_config.h"

long compute_stats();

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
#define MAX(a,b)        (((a) > (b)) ? (a) : (b))

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
#define CVR_THRESHHOLD_COUNT_IN_MILLISECONDS  60000 //15sec*4 ~ 60 sec

/*Global variables - TBD - convert to private */
kvsUploadCallback* callbackObj;
static unordered_map<string, string> clipmapwithsequencenumber;

/************************************************* common api's start*****************************************/
namespace com { namespace amazonaws { namespace kinesis { namespace video {
typedef struct _CustomData {
  _CustomData():
    first_frame(true),
    stream_started(false),
    h264_stream_supported(false),
    kinesis_video_producer(nullptr),
    frame_data_size(DEFAULT_FRAME_DATA_SIZE_BYTE),
    clip_senttokvssdk_time(0),
    lastclippersisted_time(0),
    stream_in_progress(false),
    streaming_type(DEFAULT_STREAMING_TYPE),
    gkvsclip_audio(0),
    gkvsclip_abstime(0),
    gkvsclip_livemode(0),
    kinesis_video_stream(nullptr),
    //audio related params
    putFrameHelper(nullptr),
    pipeline_blocked(false),
    total_track_count(1),
    put_frame_failed(false),
    put_frame_flushed(false),
    cvr_stream_id(3)
  {
    lastclippersisted_time = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
  }

  //kvs components
  unique_ptr<KinesisVideoProducer> kinesis_video_producer;
  shared_ptr<KinesisVideoStream> kinesis_video_stream;
  char stream_name[ MAX_STREAM_NAME_LEN ];
  char clip_name[ MAX_STREAM_NAME_LEN ];
  volatile bool stream_in_progress;

  uint8_t *frame_data;
  size_t frame_data_size;
  // indicate whether a video key frame has been received or not.
  volatile bool first_frame;

  //time at which gst pipeline finished pushing data to kvs sdk using put_frame, captured at main loop finish
  uint64_t clip_senttokvssdk_time;

  //last persisted clip time
  uint64_t lastclippersisted_time;

  //Mutex needed for the condition variable for client ready locking.
  std::mutex clip_upload_mutex_;

  //Condition variable used to signal the clip has been uploaded.
  std::condition_variable clip_upload_status_var_;

  //streaming type
  STREAMING_TYPE streaming_type;

  //Indicating that the clip has audio
  unsigned short gkvsclip_audio;

  //Indicating that the clip has abs timestamp
  unsigned short gkvsclip_abstime;

  //Indicating that the clip has live mode enabled
  unsigned short gkvsclip_livemode;

  // indicate if either audio or video media pipeline is currently blocked. If so, the other pipeline line will wake up
  // the blocked one when the time is right.
  volatile bool pipeline_blocked;

  // helper object for syncing audio and video frames to avoid fragment overlapping.
  unique_ptr<PutFrameHelper> putFrameHelper;

  // whether putFrameHelper has reported a putFrame failure.
  atomic_bool put_frame_failed;

  // whether putFrameHelper flush done
  atomic_bool put_frame_flushed ;

  // key:     trackId
  // value:   whether application has received the first frame for trackId.
  bool stream_started;
  bool h264_stream_supported;
  uint32_t total_track_count;
  int cvr_stream_id;

} CustomData;

CustomData data = {};

const char *audiopad = "audio";
const char *videopad = "video";

class SampleClientCallbackProvider : public ClientCallbackProvider {
  public:
    UINT64 getCallbackCustomData() override {
      return reinterpret_cast<UINT64> (this);
    }

    ClientReadyFunc getClientReadyCallback() override {
      return clientReadyHandler;
    }

    StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
      return storageOverflowPressure;
    }

   private:
    static STATUS clientReadyHandler(UINT64 custom_data, CLIENT_HANDLE client_handle);
    static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
};

class SampleStreamCallbackProvider : public StreamCallbackProvider {
    UINT64 custom_data_;

  public:
    SampleStreamCallbackProvider(UINT64 custom_data) : custom_data_(custom_data) {}
    
    UINT64 getCallbackCustomData() override {
        return custom_data_;
    }

    StreamUnderflowReportFunc getStreamUnderflowReportCallback() {
        return streamUnderflowReportHandler;
    };

    StreamLatencyPressureFunc getStreamLatencyPressureCallback() {
        return streamLatencyPressureHandler;
    };

    DroppedFrameReportFunc getDroppedFrameReportCallback() {
        return droppedFrameReportHandler;
    };

    StreamConnectionStaleFunc getStreamConnectionStaleCallback() {
        return streamConnectionStaleHandler;
    };

    DroppedFragmentReportFunc getDroppedFragmentReportCallback() {
        return droppedFragmentReportHandler;
    };

    StreamErrorReportFunc getStreamErrorReportCallback() {
        return streamErrorReportHandler;
    };

    StreamReadyFunc getStreamReadyCallback() {
        return streamReadyHandler;
    };

    StreamClosedFunc getStreamClosedCallback() {
        return streamClosedHandler;
    };

    StreamDataAvailableFunc getStreamDataAvailableCallback() {
        return streamDataAvailableHandler;
    };

    FragmentAckReceivedFunc getFragmentAckReceivedCallback() {
        return FragmentAckReceivedHandler;
    };

    BufferDurationOverflowPressureFunc getBufferDurationOverflowPressureCallback() {
        return bufferDurationOverflowPressureHandler;
    };

   private:
    static STATUS streamUnderflowReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle);
    static STATUS streamLatencyPressureHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,UINT64 buffer_duration);
    static STATUS droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 dropped_frame_timecode);
    static STATUS streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 last_ack_duration);
    static STATUS droppedFragmentReportHandler(UINT64 custom_data,STREAM_HANDLE stream_handle,UINT64 timecode);
    static STATUS streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UPLOAD_HANDLE upload_handle, 
                                        UINT64 errored_timecode, STATUS status_code);
    static STATUS streamReadyHandler(UINT64 custom_data, STREAM_HANDLE stream_handle);
    static STATUS streamClosedHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,UPLOAD_HANDLE stream_upload_handle);
    static STATUS streamDataAvailableHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, PCHAR stream_name, 
                                           UPLOAD_HANDLE stream_upload_handle, UINT64 duration_available,UINT64 size_available);
    static STATUS FragmentAckReceivedHandler(UINT64 custom_data,STREAM_HANDLE stream_handle,UPLOAD_HANDLE upload_handle,PFragmentAck pFragmentAck);
    static STATUS bufferDurationOverflowPressureHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 remaining_duration);
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
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): New credentials expiration is %u\n", __FILE__, __LINE__, credentials.getExpiration().count());
  }
};

class SampleDeviceInfoProvider : public DefaultDeviceInfoProvider {
  int m_stream_id;
 public:
  SampleDeviceInfoProvider(int stream_id) : m_stream_id(stream_id) {}

  device_info_t getDeviceInfo() override {
    auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();
    LOG_ERROR("SampleDeviceInfoProvider : stream id :" << m_stream_id );
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
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): SampleDeviceInfoProvider : storage size : %u\n", __FILE__, __LINE__, device_info.storageInfo.storageSize);

    return device_info;
  }
};

/* client callbacks start*/
STATUS SampleClientCallbackProvider::clientReadyHandler(UINT64 custom_data, CLIENT_HANDLE client_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): clientReadyHandler %u\n", __FILE__, __LINE__, client_handle);
    return STATUS_SUCCESS;
}

STATUS SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
  UNUSED_PARAM(custom_handle);
  RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Reporting storage overflow. Bytes remaining %u\n", __FILE__, __LINE__, remaining_bytes);
  return STATUS_SUCCESS;
}
/* client callbacks end*/


/* stream callbacks start*/
STATUS SampleStreamCallbackProvider::streamUnderflowReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamUnderflowReportHandler : stream_handle : %u\n", __FILE__, __LINE__, stream_handle );
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamLatencyPressureHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 buffer_duration) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamLatencyPressureHandler : buffer_duration %u, stream_handle :%u\n"
                        __FILE__, __LINE__ , buffer_duration, stream_handle);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode)
{
    CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
    if( customDataObj->kinesis_video_stream != NULL)
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::droppedFrameReportHandler : dropped_frame_timecode : %u , stream_handle :%u\n"
            __FILE__, __LINE__ , dropped_frame_timecode, stream_handle);
    }
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 last_ack_duration) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamConnectionStaleHandler : last_ack_duration : %u , stream_handle :%u\n"
            __FILE__, __LINE__ , last_ack_duration,  stream_handle);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFragmentReportHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 timecode) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::droppedFragmentReportHandler : fragment_timecode : %u , stream_handle : %u\n"
            __FILE__, __LINE__ , timecode , stream_handle);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                       UPLOAD_HANDLE upload_handle, UINT64 errored_timecode, 
                                                       STATUS status_code)
{
    std::stringstream status_strstrm;
    status_strstrm << "0x" << std::hex << status_code;
    CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);

    if (customDataObj->kinesis_video_stream != NULL) {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):  streamErrorReportHandler Error code : %s , stream_handle : %u , upload_handle : %u, \n"
               ,__FILE__, __LINE__, status_strstrm.str().c_str(), stream_handle, upload_handle);
    }
    callbackObj->onUploadError(customDataObj->clip_name, status_strstrm.str().c_str());
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamReadyHandler(UINT64 custom_data, STREAM_HANDLE stream_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d):SampleStreamCallbackProvider::streamReadyHandler invoked : stream handle : %u\n", __FILE__, __LINE__,stream_handle);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamClosedHandler(UINT64 custom_data,
                                                    STREAM_HANDLE stream_handle,
                                                    UPLOAD_HANDLE stream_upload_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamClosedHandler invoked : stream handle : %u : stream upload handle : %u\n"
            __FILE__, __LINE__,stream_handle, stream_upload_handle);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamDataAvailableHandler(UINT64 custom_data,
                                                           STREAM_HANDLE stream_handle,
                                                           PCHAR stream_name,
                                                           UPLOAD_HANDLE stream_upload_handle,
                                                           UINT64 duration_available,
                                                           UINT64 size_available) {
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamDataAvailableHandler invoked\n",__FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::FragmentAckReceivedHandler(UINT64 custom_data,STREAM_HANDLE stream_handle,
                                                         UPLOAD_HANDLE upload_handle,PFragmentAck pFragmentAck)
{
    CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
    static uint64_t clipcount = 0;
    static uint64_t clipuploadtime = 0;

    LOG_DEBUG("Reporting fragment ACK received. Fragment timecode " << pFragmentAck->timestamp );
    LOG_DEBUG("Reporting fragment ACK received. Fragment type " << pFragmentAck->ackType );
    LOG_DEBUG("Reporting fragment ACK received. Fragment seq number " << pFragmentAck->sequenceNumber );

    if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_BUFFERING) {
        std::string sequencenumber(pFragmentAck->sequenceNumber);
        std::string clipname(customDataObj->clip_name);
        clipmapwithsequencenumber[pFragmentAck->sequenceNumber] = customDataObj->clip_name;
        return;
    }

    if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_RECEIVED) {
        //Debug Purpose
        if ( clipmapwithsequencenumber.size() > 5 ) {
            for ( auto& x: clipmapwithsequencenumber )
                std:cout <<  x.first << ": " << x.second << std::endl;
        }
        return;
    }

    if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_PERSISTED)
    {
        clipcount++;
        uint64_t time_now = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
        data.lastclippersisted_time = time_now;
        uint64_t time_diff = time_now - customDataObj->clip_senttokvssdk_time;
        clipuploadtime+=time_diff;
        uint64_t avgtime_clipupload = clipuploadtime/clipcount ;
        string persistedclip;
        unordered_map<string,string>::const_iterator mapiterator = clipmapwithsequencenumber.find (pFragmentAck->sequenceNumber);

        if ( mapiterator != clipmapwithsequencenumber.end()) {
            persistedclip = mapiterator->second;
            clipmapwithsequencenumber.erase(pFragmentAck->sequenceNumber);
        }

        //epochtime_senttokvssdk, currenttime, epochfragmenttimecode_server, fragmentnumber, timediff
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
                __FILE__, __LINE__, persistedclip.c_str(), pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff );
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload persisted time and clip sent time :%lld,%lld \n",__FILE__, __LINE__,data.lastclippersisted_time,customDataObj->clip_senttokvssdk_time);
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload stats :%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,clipcount);

        callbackObj->onUploadSuccess(persistedclip.c_str());
    }
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler(UINT64 custom_data,
                                                                      STREAM_HANDLE stream_handle,
                                                                      UINT64 remaining_duration) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler invoked : remaining_duration : %u, stream_handle : %u\n"
                           , __FILE__, __LINE__, remaining_duration, stream_handle);
    return STATUS_SUCCESS;
}

/* stream callbacks end*/

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;

unique_ptr<Credentials> credentials_;

//kinesis producer init
static void kinesisVideoInit(CustomData *data, char* stream_name)
{

    STRNCPY(data->stream_name, stream_name,MAX_STREAM_NAME_LEN);
    data->stream_name[MAX_STREAM_NAME_LEN -1] = '\0';
    LOG_INFO("kinesisVideoInit enter data stream name" << data->stream_name);

    unique_ptr<DeviceInfoProvider> device_info_provider = make_unique<SampleDeviceInfoProvider>(data->cvr_stream_id);
    unique_ptr<ClientCallbackProvider> client_callback_provider = make_unique<SampleClientCallbackProvider>();

    unique_ptr<StreamCallbackProvider> stream_callback_provider = make_unique<SampleStreamCallbackProvider>(
            reinterpret_cast<UINT64>(data));

    char const *accessKey;
    char const *secretKey;
    char const *sessionToken;
    char const *defaultRegion;
    char const *iot_get_credential_endpoint;
    char const *cert_path;
    char const *private_key_path;
    char const *role_alias;
    char const *ca_cert_path;

    string defaultRegionStr;
    string sessionTokenStr;
    if (nullptr==(accessKey = getenv(ACCESS_KEY_ENV_VAR)))
    {
        accessKey = "AccessKey";
    }

    if (nullptr==(secretKey = getenv(SECRET_KEY_ENV_VAR)))
    {
        secretKey = "SecretKey";
    }

    if (nullptr==(sessionToken = getenv(SESSION_TOKEN_ENV_VAR)))
    {
        sessionTokenStr = "";
    }
    else
    {
        sessionTokenStr = string(sessionToken);
    }

    if (nullptr==(defaultRegion = getenv(DEFAULT_REGION_ENV_VAR)))
    {
        defaultRegionStr = DEFAULT_AWS_REGION;
    }
    else
    {
        defaultRegionStr = string(defaultRegion);
    }

    LOG_INFO("kinesisVideoInit defaultRegion = " << defaultRegionStr);
    credentials_ = make_unique<Credentials>(string(accessKey),
            string(secretKey),
            sessionTokenStr,
            std::chrono::seconds(180));
    unique_ptr<CredentialProvider> credential_provider;
    if (nullptr!=(iot_get_credential_endpoint = getenv("IOT_GET_CREDENTIAL_ENDPOINT")) &&
            nullptr!=(cert_path = getenv("CERT_PATH")) &&
            nullptr!=(private_key_path = getenv("PRIVATE_KEY_PATH")) &&
            nullptr!=(role_alias = getenv("ROLE_ALIAS")) &&
            nullptr!=(ca_cert_path = getenv("CA_CERT_PATH"))) {
        LOG_DEBUG("Using IoT credentials for Kinesis Video Streams : iot_get_credential_endpoint :" << iot_get_credential_endpoint << " cert_path : " << cert_path
                << " private_key_path : " << private_key_path << " role_alias : " << role_alias << " ca_cert_path : " << ca_cert_path << " Session Token: " << sessionTokenStr.c_str());
        credential_provider = make_unique<IotCertCredentialProvider>(iot_get_credential_endpoint,
                cert_path,
                private_key_path,
                role_alias,
                ca_cert_path,
                data->stream_name
                );
    }
    else
    {
        LOG_INFO("Using Sample credentials for Kinesis Video Streams");
        credential_provider = make_unique<SampleCredentialProvider>(*credentials_.get());
    }

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
                DEFAULT_CACHE_TIME_IN_SECONDS);

    data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
            move(cachingEndpointOnlyCallbackProvider));

    LOG_INFO("Kinesis Video Streams Client is ready");
}

//kinesis stream init
static void kinesisVideoStreamInit(CustomData *data)
{

	bool use_absolute_fragment_times = DEFAULT_ABSOLUTE_FRAGMENT_TIMES;
	if ( ! data->gkvsclip_livemode )
	{ 
		data->streaming_type = STREAMING_TYPE_OFFLINE;
		use_absolute_fragment_times = true;
	}
	else
	{
		data->streaming_type = STREAMING_TYPE_REALTIME;
		use_absolute_fragment_times = false;
	}

	string content_type;
	if ( data->gkvsclip_audio )
	{
		content_type = "video/h264,audio/aac";
	}
	else
	{
		content_type = "video/h264";
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): content_type = %s\n", __FILE__, __LINE__, content_type.c_str());


	auto stream_definition = make_unique<StreamDefinition>(data->stream_name,
			hours(DEFAULT_RETENTION_PERIOD_HOURS),
			nullptr,
			DEFAULT_KMS_KEY_ID,
			STREAMING_TYPE_REALTIME, //data->streaming_type,
			content_type,
			duration_cast<milliseconds> (seconds(DEFAULT_MAX_LATENCY_SECONDS)),
			milliseconds(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
			milliseconds(DEFAULT_TIMECODE_SCALE_MILLISECONDS),
			DEFAULT_KEY_FRAME_FRAGMENTATION,
			DEFAULT_FRAME_TIMECODES,
			false, //use_absolute_fragment_times,
			DEFAULT_FRAGMENT_ACKS,
			DEFAULT_RESTART_ON_ERROR,
			DEFAULT_RECALCULATE_METRICS,
			NAL_ADAPTATION_ANNEXB_CPD_NALS | NAL_ADAPTATION_ANNEXB_NALS, 
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

	if ( data->gkvsclip_audio )
	{
		LOG_INFO("Kinesis video stream init audio video case");
		stream_definition->addTrack(DEFAULT_AUDIO_TRACKID, DEFAULT_AUDIO_TRACK_NAME, DEFAULT_AUDIO_CODEC_ID, MKV_TRACK_INFO_TYPE_AUDIO);
		data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));
		data->putFrameHelper = make_unique<PutFrameHelper>(data->kinesis_video_stream,1000000,20,20,400,70000);
		//data->stream_started.clear();
	}
	else
	{
		LOG_INFO("Kinesis video stream init video case");
		data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));
		data->stream_in_progress = false;
	}

	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Stream %s is ready\n", __FILE__, __LINE__, data->stream_name );
}

//kvs video stream uninit
void kinesisVideoStreamUninit(CustomData *data)
{
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs stream uninit started\n", __FILE__, __LINE__);
    if (data->kinesis_video_stream != NULL)
    {
        if ( ( data->gkvsclip_audio ) && ( nullptr != data->putFrameHelper ) ) {
            // signal putFrameHelper to release all frames still in queue.
            if( !data->put_frame_flushed.load() ) 
            {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs put frame flush after stream uninit\n", __FILE__, __LINE__);
                data->putFrameHelper->flush();
                data->putFrameHelper->putEofr();
                data->put_frame_flushed = true;
            }
        }
        data->kinesis_video_stream->stopSync();
        data->kinesis_video_producer->freeStream(data->kinesis_video_stream);
        data->kinesis_video_stream = NULL;
    }
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs stream uninit done\n", __FILE__, __LINE__);
}

//recreate stream
static bool recreate_stream(CustomData *data)
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : Attempt to recreate kinesis video stream \n", __FILE__, __LINE__);
	kinesisVideoStreamUninit(data);
	//sleep required between free stream and recreate stream to avoid crash
	bool do_repeat = true;
	int retry=0;
	do
	{
		try
		{
			kinesisVideoStreamInit(data);
			do_repeat = false;
		}
		catch (runtime_error &err)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : Failed to create kinesis video stream : retrying \n", __FILE__, __LINE__);
			this_thread::sleep_for(std::chrono::seconds(2));
			retry++;
			if ( retry > KVSINITMAXRETRY )
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : FATAL : Max retry reached in recreate_stream exit process %d\n", __FILE__, __LINE__);
				return false;
			}
		}
	} while(do_repeat);
    
        return true;
}

static void create_kinesis_video_frame(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
                               void *data, size_t len)
{

	frame->flags = flags;
	frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->duration = DEFAULT_FRAME_DURATION_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
	frame->size = static_cast<UINT32>(len);
	frame->frameData = reinterpret_cast<PBYTE>(data);
	frame->trackId = DEFAULT_TRACK_ID;
}

static bool put_frame(shared_ptr<KinesisVideoStream> kinesis_video_stream, void* data, const nanoseconds &pts, const nanoseconds &dts,
              FRAME_FLAGS flags, size_t len)
{
    Frame frame;
    create_kinesis_video_frame(&frame, pts, dts, flags, data, len);
    return kinesis_video_stream->putFrame(frame);
}

int kvsInit(kvsUploadCallback* callback, int stream_id)
{
	LOG_DEBUG("kvsInit - Enter");
	callbackObj = callback; 
	static bool islogconfigdone = false;
	char stream_name[MAX_STREAM_NAME_LEN];

	//init kvs log config
	if( false == islogconfigdone )
	{
		char const *kvslogconfig;
		char *defaultstream;

		if (nullptr == (kvslogconfig = getenv(KVS_LOG_CONFIG_ENV_VER)))
		{
			kvslogconfig = "/etc/kvs_log_configuration";
		}

		PropertyConfigurator::doConfigure(kvslogconfig);

		LOG_DEBUG("kvsInit - kvs log config :" << kvslogconfig);

		if (nullptr == (defaultstream = getenv(DEFAULT_STREAM_NAME)))
		{
			SNPRINTF(stream_name, MAX_STREAM_NAME_LEN, "TEST-CVR");
		}
		else
		{
			SNPRINTF(stream_name, MAX_STREAM_NAME_LEN, defaultstream);
		}
		islogconfigdone = true;
	}

	LOG_DEBUG("kvsInit - kvs Stream Name :" << stream_name);
	//set stream id
	data.cvr_stream_id = stream_id;

	//init kinesis video
	try
	{
		kinesisVideoInit(&data, stream_name);
	}
	catch (runtime_error &err)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Time out error in kinesisVideoInit connection_error \n", __FILE__, __LINE__);
		return -1;
	}

	LOG_DEBUG("kvsInit - Exit");
	return 0;
}

//kvs_video_stream init
int kvsStreamInit( unsigned short& kvsclip_audio, unsigned short& kvsclip_abstime, unsigned short& kvsclip_livemode, unsigned short& contentchangestatus)
{
    LOG_DEBUG("kvsStreamInit - Enter");

    static bool isgstinitdone = false;
    bool ret = false;

    //update custom data parameters
    data.gkvsclip_audio = kvsclip_audio;
    data.gkvsclip_abstime = kvsclip_abstime;
    data.gkvsclip_livemode = kvsclip_livemode;

    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): audio=%d, abstime=%d, livemode=%d\n", __FILE__, __LINE__, kvsclip_audio, kvsclip_abstime, kvsclip_livemode);

    if(data.gkvsclip_audio)
    {
        data.total_track_count = 2;
    }
    else
    {
        data.total_track_count = 1;
    }

    //Ensure video pipeline is deleted before stream init
    if(data.gkvsclip_abstime)
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Absolute time is enabled\n", __FILE__, __LINE__);

    }

    //In normal case init then recreate
    if( 0 == contentchangestatus )
    {
        //init kinesis stream
        try
        {
            kinesisVideoStreamInit(&data);
            ret = true;
        }
        catch (runtime_error &err)
        {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Time out error in kinesisVideoStreamInit connection_error \n", __FILE__, __LINE__);
            ret = recreate_stream(&data);
        }
    }
    else
    { //in content change recreate stream
        ret = recreate_stream(&data);
    }

    LOG_DEBUG("kvsStreamInit - Exit");
    return ret;
}

int kvsUploadFrames(unsigned short& clip_audio, unsigned short& clip_abstime, unsigned short& clip_livemode, RDKC_FrameInfo frameData,char* filename, bool isEOF = false )
{ 
    FRAME_FLAGS kinesis_video_flags = FRAME_FLAG_NONE;
    unsigned short clipaudioflag = clip_audio;
    unsigned short clipabstime = clip_abstime;
    unsigned short cliplivemode = clip_livemode;
    static int clipsize=0;

    //Check if the clip content has changed - videoaudioclip -> videoclip / videoclip -> videoaudioclip
    if( data.gkvsclip_audio != clipaudioflag )
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Change in clip content found : prior audio enable flag : %d , current audio enable flag : %d\n", 
                __FILE__, __LINE__, data.gkvsclip_audio, clipaudioflag);
        return 1;
    }

    STRNCPY(data.clip_name, filename, MAX_STREAM_NAME_LEN);
    data.clip_name[MAX_STREAM_NAME_LEN -1] = '\0';

    int retstatus=0;
    size_t buffer_size = frameData.frame_size;
    clipsize+=buffer_size;

    //reset states
    data.put_frame_flushed = false;

    //update timestamp
    data.h264_stream_supported = true;
    if (!data.stream_started)
    {
        data.stream_started = true;
    }

    if(data.first_frame)
    {
        RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVRUPLOAD", "Setting the Key Frame flag (very first frame)\n");
        if ( (frameData.pic_type == 1) || (frameData.pic_type == 2))
        {
            data.clip_senttokvssdk_time = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
            kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
        }
        data.first_frame = false;
    }
    else
    {
        kinesis_video_flags = FRAME_FLAG_NONE;
        if(isEOF)
        {
            data.first_frame = true;
        }
    }

    std::chrono::nanoseconds frametimestamp_nano = std::chrono::milliseconds(frameData.frame_timestamp);

    if(isEOF)
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs clip size:%d\n", __FILE__, __LINE__,clipsize);
        clipsize=0;
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Sending EoFr\n", __FILE__, __LINE__);
        data.kinesis_video_stream->putFrame(EOFR_FRAME_INITIALIZER);

        //compare clip sent time with clip persisted time to detect hang in application
        if( data.clip_senttokvssdk_time > data.lastclippersisted_time ) {
            uint64_t cliptime_diff = data.clip_senttokvssdk_time - data.lastclippersisted_time;
            if( cliptime_diff > CVR_THRESHHOLD_COUNT_IN_MILLISECONDS) {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): FATAL : Threshold limit reached in upload failure exit process \n", __FILE__, __LINE__);
                exit(1);
            }
        }
    }
    else
    {
        if (!put_frame(data.kinesis_video_stream, (void*)frameData.frame_ptr, std::chrono::nanoseconds(frametimestamp_nano),
                    std::chrono::nanoseconds(frametimestamp_nano), kinesis_video_flags,  buffer_size))
        {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error - Failed to put frame\n", __FILE__, __LINE__);
            return -1;
        }
    }

    return 0;
}
