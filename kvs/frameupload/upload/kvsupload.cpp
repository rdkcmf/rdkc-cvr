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
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <Logger.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <queue>
#include <unordered_map>
#include "KinesisVideoProducer.h"
#include "StreamDefinition.h"
#include "CachingEndpointOnlyCallbackProvider.h"
#include <IotCertCredentialProvider.h>
#include "rdk_debug.h"
#include "kvsuploadCallback.h"
#ifdef _HAS_XSTREAM_
#include "xStreamerConsumer.h"
#endif

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
#define DEFAULT_MAX_LATENCY_SECONDS 30
#ifdef XCAM2
#define DEFAULT_FRAGMENT_DURATION_MILLISECONDS 15500
int time_difference = 16000;
#else
#define DEFAULT_FRAGMENT_DURATION_MILLISECONDS 14500
int time_difference = 15000;
#endif
#define DEFAULT_TIMECODE_SCALE_MILLISECONDS 1
#define DEFAULT_KEY_FRAME_FRAGMENTATION TRUE
#define DEFAULT_FRAME_TIMECODES TRUE
#define DEFAULT_ABSOLUTE_FRAGMENT_TIMES FALSE
#define DEFAULT_FRAGMENT_ACKS TRUE
#define DEFAULT_RESTART_ON_ERROR FALSE
#define DEFAULT_RECALCULATE_METRICS TRUE
#define DEFAULT_STREAM_FRAMERATE 25
#define DEFAULT_AVG_BANDWIDTH_BPS (4 * 1024 * 1024)
#define DEFAULT_BUFFER_DURATION_SECONDS 60
#define DEFAULT_REPLAY_DURATION_SECONDS 40
#define DEFAULT_CONNECTION_STALENESS_SECONDS 20
#define DEFAULT_CODEC_ID "V_MPEG4/ISO/AVC"
#define DEFAULT_TRACKNAME "kinesis_video"
#define STORAGE_SIZE_STREAM1 (6 * 1024 * 1024)
#define STORAGE_SIZE_STREAM2 (6 * 1024 * 1024)
#define MIN_STORGE_SIZE (6 * 1024 * 1024)
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
#define DEFAULT_CODECID_AACAUDIO "1588"
#define DEFAULT_CACHE_TIME_IN_SECONDS 86400 //24*3600 ~ 1 day
#define CVR_THRESHHOLD_COUNT_IN_MILLISECONDS  60000 //15sec*4 ~ 60 sec
#define AAC_HEADER_LENGTH 7
#define CVR_THRESHHOLD_FRAMEDROP_AUDIOVIDEO 1000
#define CVR_THRESHHOLD_FRAMEDROP_VIDEO 600

/*Global variables - TBD - convert to private */
kvsUploadCallback* callbackObj;
static map<uint64_t, std::string> clipmapwithtimecode;
static deque<std::string> queueclipName;
static bool isstreamerror_reported = false;
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
    gkvsclip_audio(0),
    kinesis_video_stream(nullptr),
    //audio related params
    total_track_count(1),
    storageMem(0),
    cvr_stream_id(3) {}

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

  //storage space to sdk
  uint64_t storageMem;

  //time at which gst pipeline finished pushing data to kvs sdk using put_frame, captured at main loop finish
  uint64_t clip_senttokvssdk_time;

  //last persisted clip time
  uint64_t lastclippersisted_time;

  //Mutex needed for the condition variable for client ready locking.
  std::mutex clip_upload_mutex_;

  //Condition variable used to signal the clip has been uploaded.
  std::condition_variable clip_upload_status_var_;

  //Indicating that the clip has audio
  unsigned short gkvsclip_audio;

  //Indicating SDK high mem settings
  unsigned short gkvsclip_highmem;

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
    LOG_INFO("SampleDeviceInfoProvider : stream id :" << m_stream_id );
    if(data.gkvsclip_highmem) {
        device_info.storageInfo.storageSize = STORAGE_SIZE_STREAM1;
    } else {
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
    }

    if (data.storageMem != 0 && data.storageMem >= MIN_STORGE_SIZE && data.storageMem <= MAX_STORAGE_SIZE_STREAM) {
        device_info.storageInfo.storageSize = data.storageMem;
    }
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): SampleDeviceInfoProvider : storage size : %llu \n", __FILE__, __LINE__, device_info.storageInfo.storageSize);

    return device_info;
  }
};

/* client callbacks start*/
STATUS SampleClientCallbackProvider::clientReadyHandler(UINT64 custom_data, CLIENT_HANDLE client_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): clientReadyHandler \n", __FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
	UNUSED_PARAM(custom_handle);
	static int reportingStorageBytesCount=0;
	if( (reportingStorageBytesCount % 10) == 0 ) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Reporting storage overflow. Bytes remaining %" PRIu64 "\n", __FILE__, __LINE__, remaining_bytes);
	}
	reportingStorageBytesCount++;
	return STATUS_SUCCESS;
}
/* client callbacks end*/


/* stream callbacks start*/
STATUS SampleStreamCallbackProvider::streamUnderflowReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle) {
	static int underFlowCount=0;
	if( (underFlowCount % 10) == 0 ) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamUnderflowReportHandler\n", __FILE__, __LINE__);
	}
	underFlowCount++;
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamLatencyPressureHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 buffer_duration) {
	static int latencyPressureCount=0;
	if( (latencyPressureCount % 10) == 0 ) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamLatencyPressureHandler : buffer_duration %" PRIu64 "\n", __FILE__, __LINE__);
	}
	latencyPressureCount++;
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode)
{
	static int droppedFrameReportHandlerCount=0;
	CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
	if( customDataObj->kinesis_video_stream != NULL)
	{
		if( (droppedFrameReportHandlerCount % 10) == 0 )
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::droppedFrameReportHandler : dropped_frame_timecode : %" PRIu64 "\n", __FILE__, __LINE__ , dropped_frame_timecode);
		}
		droppedFrameReportHandlerCount++;
	}
	return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 last_ack_duration) {
	static int connectionStaleCount =0;
	if( (connectionStaleCount % 10) == 0 ) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamConnectionStaleHandler : last_ack_duration : %" PRIu64 "\n", __FILE__, __LINE__ , last_ack_duration);
	}
	connectionStaleCount++;
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFragmentReportHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 timecode) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::droppedFragmentReportHandler : fragment_timecode : %" PRIu64 "\n", __FILE__, __LINE__ , timecode);
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
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):  streamErrorReportHandler Error code : %s\n", __FILE__, __LINE__, status_strstrm.str().c_str());
    }
    isstreamerror_reported = true;
    //callbackObj->onUploadError(customDataObj->clip_name, status_strstrm.str().c_str());
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamReadyHandler(UINT64 custom_data, STREAM_HANDLE stream_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d):SampleStreamCallbackProvider::streamReadyHandler invoked \n", __FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamClosedHandler(UINT64 custom_data,
                                                    STREAM_HANDLE stream_handle,
                                                    UPLOAD_HANDLE stream_upload_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): SampleStreamCallbackProvider::streamClosedHandler invoked\n", __FILE__, __LINE__);
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

	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","Reporting fragment ACK received. Fragment timecode %" PRIu64 "\n", pFragmentAck->timestamp );
	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","Reporting fragment ACK received. Fragment type %d\n", pFragmentAck->ackType );
	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","Reporting fragment ACK received. Fragment seq number %s\n", pFragmentAck->sequenceNumber );

	if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_BUFFERING) {
		if(!queueclipName.empty())
		{
			std::map<uint64_t, std::string>::iterator it;
			it = clipmapwithtimecode.find(pFragmentAck->timestamp);
			if (it == clipmapwithtimecode.end())
			{
				std::string clipName;
				clipName = queueclipName.front();
				queueclipName.pop_front();
				clipmapwithtimecode.insert(std::pair<uint64_t, std::string>(pFragmentAck->timestamp, clipName));
			}
		}
		return;
	}

	if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_RECEIVED) {
		//Debug Purpose
		if(clipmapwithtimecode.size() > 5)
		{
			for ( auto& x: clipmapwithtimecode) 
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","Pending clips: clipName - %s timecode - %" PRIu64 "\n", x.second.c_str(), x.first);
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

		std::map<uint64_t, std::string>::iterator it;
		it = clipmapwithtimecode.find(pFragmentAck->timestamp);
		persistedclip = it->second;
		if (it != clipmapwithtimecode.end())
		{
			//epochtime_senttokvssdk, currenttime, epochfragmenttimecode_server, fragmentnumber, timediff
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
					__FILE__, __LINE__, persistedclip.c_str(), pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff );
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload persisted time and clip sent time :%lld,%lld \n",__FILE__, __LINE__,data.lastclippersisted_time,customDataObj->clip_senttokvssdk_time);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload stats :%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,clipcount);

			callbackObj->onUploadSuccess(persistedclip.c_str());
			//remove the item found above, as it is already notified.
			//delete the entries lesser than this index. reason is, buffering ack might be received and persisted might not be.
			clipmapwithtimecode.erase(clipmapwithtimecode.begin(), it);
		}

		if(it == clipmapwithtimecode.end())
		{
			string persistedclip;
			//buffering missed and persisted ack is received.
			//pop the queue and send upload success.
			persistedclip = queueclipName.front();
			queueclipName.pop_front();

			//epochtime_senttokvssdk, currenttime, epochfragmenttimecode_server, fragmentnumber, timediff
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
					__FILE__, __LINE__, persistedclip.c_str(), pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff );
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload persisted time and clip sent time :%lld,%lld \n",__FILE__, __LINE__,data.lastclippersisted_time,customDataObj->clip_senttokvssdk_time);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload stats :%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,clipcount);
			callbackObj->onUploadSuccess(persistedclip.c_str());
		}
	}

	if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_ERROR)
	{
		std::string clipName;
		std::map<uint64_t, std::string>::iterator it;
		it = clipmapwithtimecode.find(pFragmentAck->timestamp);
		if (it != clipmapwithtimecode.end())
		{
			clipName = it->second;
			clipmapwithtimecode.erase (it);
			callbackObj->onUploadError(clipName.c_str(),"failed");
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","kvsclip upload unsuccessful %s\n",clipName.c_str());
		}
		if(it == clipmapwithtimecode.end())
		{
			//buffering missed, but a timecode is failed. Pop the queue and send failure notification.
			clipName = queueclipName.front();
			queueclipName.pop_front();
			callbackObj->onUploadError(clipName.c_str(),"failed");
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","kvsclip upload unsuccessful %s\n",clipName.c_str());
		}
		return;
	}
	return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler(UINT64 custom_data,
                                                                      STREAM_HANDLE stream_handle,
                                                                      UINT64 remaining_duration) {
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d):SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler invoked : remaining_duration : %" PRIu64 "\n" , __FILE__, __LINE__, remaining_duration);
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

	if(stream_name == NULL)
		throw runtime_error(std::string("stream name empty"));

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
			STREAMING_TYPE_REALTIME, 
			content_type,
			duration_cast<milliseconds> (seconds(DEFAULT_MAX_LATENCY_SECONDS)),
			milliseconds(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
			milliseconds(DEFAULT_TIMECODE_SCALE_MILLISECONDS),
			DEFAULT_KEY_FRAME_FRAGMENTATION,
			DEFAULT_FRAME_TIMECODES,
			true,
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
        data->stream_started = false;
        data->kinesis_video_stream->stop();
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
                               char *data, size_t len,int track_Id)

{
	//skip AAC header since sdk expects raw data only
	static void* datat;
        if(track_Id == 1) {
		datat=data;
	} else {
		datat=&data[AAC_HEADER_LENGTH];
		len=len-AAC_HEADER_LENGTH;
	}

	frame->flags = flags;
	frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->duration = 0;
	frame->size = static_cast<UINT32>(len);
	frame->frameData = reinterpret_cast<PBYTE>(datat);
	frame->trackId = track_Id;
}

static bool put_frame(shared_ptr<KinesisVideoStream> kinesis_video_stream, char* data, const nanoseconds &pts, const nanoseconds &dts,
              FRAME_FLAGS flags, size_t len,int id)
{
    Frame frame;
    create_kinesis_video_frame(&frame, pts, dts, flags, data, len,id);
    return kinesis_video_stream->putFrame(frame);
}

int kvsInit(kvsUploadCallback* callback, int stream_id, uint64_t storageMem = 0)
{
	LOG_DEBUG("kvsInit - Enter");
	callbackObj = callback; 
 	if (storageMem != 0) {
 	  data.storageMem = storageMem;
        }
	static bool islogconfigdone = false;
        char *defaultstream = NULL;
	char stream_name[MAX_STREAM_NAME_LEN];
	memset(stream_name, '\0', MAX_STREAM_NAME_LEN);

	//init kvs log config
	if( false == islogconfigdone )
	{
		char const *kvslogconfig = NULL;

		if (nullptr == (kvslogconfig = getenv(KVS_LOG_CONFIG_ENV_VER)))
		{
			kvslogconfig = "/etc/kvs_log_configuration";
		}

		PropertyConfigurator::doConfigure(kvslogconfig);

		LOG_DEBUG("kvsInit - kvs log config :" << kvslogconfig);

		islogconfigdone = true;
	}
	if (nullptr == (defaultstream = getenv(DEFAULT_STREAM_NAME)))
	{
		SNPRINTF(stream_name, MAX_STREAM_NAME_LEN, "TEST-CVR");
	}
	else
	{
		SNPRINTF(stream_name, MAX_STREAM_NAME_LEN, defaultstream);
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
int kvsStreamInit( unsigned short& kvsclip_audio, unsigned short& kvsclip_highmem, unsigned short& contentchangestatus)
{
    LOG_DEBUG("kvsStreamInit - Enter");

    static bool isgstinitdone = false;
    bool ret = false;

    //update custom data parameters
    data.gkvsclip_audio = kvsclip_audio;
    data.gkvsclip_highmem = kvsclip_highmem;
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): audio=%d, kvsclip_highmem =%d\n", __FILE__, __LINE__, kvsclip_audio,kvsclip_highmem);

    if(data.gkvsclip_audio)
    {
        data.total_track_count = 2;
    }
    else
    {
        data.total_track_count = 1;
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

#ifdef _HAS_XSTREAM_
int kvsUploadFrames(unsigned short& clip_audio, unsigned short& kvsclip_highmem, frameInfoH264 frameData,char* filename, bool isEOF = false ) {
#else
int kvsUploadFrames(unsigned short& clip_audio, unsigned short& kvsclip_highmem, RDKC_FrameInfo frameData,char* filename, bool isEOF = false ) {
#endif //_HAS_XSTREAM_
    FRAME_FLAGS kinesis_video_flags = FRAME_FLAG_NONE;
    unsigned short clipaudioflag = clip_audio;
    unsigned short cliphighmem = kvsclip_highmem;
    static int single_clip_size=0;
    static int frame_dropped_count=0;
    static int frame_dropped_flag=0;
    int track_id=1;
    //Check if the clip content has changed - videoaudioclip -> videoclip / videoclip -> videoaudioclip
    if( data.gkvsclip_audio != clipaudioflag || data.gkvsclip_highmem != cliphighmem || isstreamerror_reported ||  frame_dropped_flag)
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Recreating stream : prior audio enable flag : %d , current audio enable flag : %d"
                                " or highmem flag change between default data.gkvsclip_highmem : %d, kvsclip_highmem : %d"
                                " or isstreamerror reported as TRUE : %d or consecutive video clips failing flag as TRUE : %d frame_dropped_count: %d \n", 
                                __FILE__, __LINE__, data.gkvsclip_audio, clipaudioflag,data.gkvsclip_highmem,cliphighmem,isstreamerror_reported,frame_dropped_flag,frame_dropped_count);
            isstreamerror_reported = false;
            frame_dropped_flag = 0;
            frame_dropped_count = 0;
            return 1;
    }

    int retstatus=0;
    size_t buffer_size = frameData.frame_size;
    //update timestamp
    data.h264_stream_supported = true;
    if (!data.stream_started)
    {
        if(clipaudioflag)
            data.kinesis_video_stream->start(std::string(DEFAULT_CODECID_AACAUDIO),DEFAULT_AUDIO_TRACKID);
        data.stream_started = true;
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","Streaming Started \n");
    }

    if(data.first_frame)
    {
        STRNCPY(data.clip_name, filename, MAX_STREAM_NAME_LEN);
        data.clip_name[MAX_STREAM_NAME_LEN -1] = '\0';
        std::string clipName(filename);
        queueclipName.push_back(clipName);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs clip to server : %s \n", __FILE__, __LINE__,clipName.c_str());
        RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVRUPLOAD", "Setting the Key Frame flag (very first frame)\n");
        if ( (frameData.pic_type == 1) || (frameData.pic_type == 2))
        {
            kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
        }
        data.first_frame = false;
    } else {
        kinesis_video_flags = FRAME_FLAG_NONE;
        if(isEOF)
        {
            data.first_frame = true;
        }
    }

    //stream type 10 is as per "RDKC_FrameInfo" structure which indicates aac data
    if( frameData.stream_type == 10 )
    {   
        track_id = 2 ;
    }
    std::chrono::nanoseconds frametimestamp_nano = std::chrono::milliseconds(frameData.frame_timestamp);

    if(isEOF) {
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s Sending EoFr\n", __FILE__, __LINE__,data.clip_name );
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): %s kvs clip size:%d\n", __FILE__, __LINE__,data.clip_name, single_clip_size );
        single_clip_size = 0;
        //compare clip sent time with clip persisted time to detect hang in application
        if( data.clip_senttokvssdk_time > data.lastclippersisted_time ) {
            uint64_t cliptime_diff = data.clip_senttokvssdk_time - data.lastclippersisted_time;
            if( cliptime_diff > CVR_THRESHHOLD_COUNT_IN_MILLISECONDS) {
                pid_t pid = getpid();
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to get clip upload status - Timeout : %u : sending sigterm to Thread id : %d \n", __FUNCTION__, __LINE__,cliptime_diff,pid);
                kinesisVideoStreamUninit(&data);
                kill(pid, SIGTERM);
            }
        }
        data.clip_senttokvssdk_time = chrono::duration_cast<milliseconds>(systemCurrentTime().time_since_epoch()).count();
    } else {
        single_clip_size += buffer_size;
        if (!put_frame(data.kinesis_video_stream, (void*)frameData.frame_ptr, std::chrono::nanoseconds(frametimestamp_nano),
                    std::chrono::nanoseconds(frametimestamp_nano), kinesis_video_flags,  buffer_size,track_id)) {
            //log every minute
            if( (frame_dropped_count % 60) == 0 ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error - Failed to put frame : frame_dropped_count : %d : clip_name : %s stream type : %d\n",
                    __FILE__, __LINE__, frame_dropped_count, data.clip_name, frameData.stream_type);
            }
            frame_dropped_count++;
            retstatus = -1;
        } else {
            frame_dropped_count = 0;
        }
    }

    //Frame drop logic
    //TBD - Logic need to change based on latest SDK where drop is counted based on SDK callback
    if(data.gkvsclip_audio) {
        if(frame_dropped_count > CVR_THRESHHOLD_FRAMEDROP_AUDIOVIDEO) {
            frame_dropped_flag = 1;
        }
    } else {
        if(frame_dropped_count > CVR_THRESHHOLD_FRAMEDROP_VIDEO) {
            frame_dropped_flag = 1;
        }
    }
    return retstatus;
}
