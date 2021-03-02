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
#include "sysUtils.h"

long compute_stats();

#ifdef __cplusplus
}
#endif

LOGGER_TAG("com.amazonaws.kinesis.video.frameupload");

#define DEFAULT_REGION_ENV_VAR "AWS_DEFAULT_REGION"
#define KVS_LOG_CONFIG_ENV_VER "KVS_LOG_CONFIG"
#define KVSINITMAXRETRY 5

//Kinesis Video Stream definitions
#define DEFAULT_FRAME_DATA_SIZE_BYTE (1024*1024)
#define DEFAULT_RETENTION_PERIOD_HOURS 2
#define DEFAULT_KMS_KEY_ID ""
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
#define DEFAULT_FRAGMENT_ACKS TRUE
#define DEFAULT_RESTART_ON_ERROR TRUE
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
#define DEFAULT_AUDIO_TRACK_NAME "audio"
#define DEFAULT_AUDIO_CODEC_ID "A_AAC"
#define DEFAULT_AUDIO_TRACKID 2
#define DEFAULT_CODECID_AACAUDIO "1588"
#define DEFAULT_CACHE_TIME_IN_SECONDS 86400 //24*3600 ~ 1 day
#define CVR_THRESHHOLD_COUNT_IN_MILLISECONDS  60000 //15sec*4 ~ 60 sec
#define AAC_HEADER_LENGTH 7
#define CVR_THRESHHOLD_FRAMEDROP_AUDIOVIDEO 1000
#define CVR_THRESHHOLD_FRAMEDROP_VIDEO 600
#define CVR_STATUS_FILE "/tmp/.cvr_status"

/*Global variables - TBD - convert to private */
kvsUploadCallback* callbackObj;
static map<uint64_t, std::string> clipmapwithtimecode;
static deque<std::string> queueclipName;
static bool isstreamerror_reported = false;
typedef std::chrono::milliseconds ms;
/************************************************* common api's start*****************************************/
namespace com { namespace amazonaws { namespace kinesis { namespace video {
typedef struct _CustomData {
  _CustomData():
    first_frame(true),
    stream_started(false),
    kinesis_video_producer(nullptr),
    gkvsclip_audio(0),
    kinesis_video_stream(nullptr),
    //audio related params
    storageMem(0),
    cvr_stream_id(3) {}
  //kvs components
  unique_ptr<KinesisVideoProducer> kinesis_video_producer;
  shared_ptr<KinesisVideoStream> kinesis_video_stream;
  char stream_name[ MAX_STREAM_NAME_LEN ];
  char clip_name[ MAX_STREAM_NAME_LEN ];

  uint8_t *frame_data;
  // indicate whether a video key frame has been received or not.
  volatile bool first_frame;

  //storage space to sdk
  uint64_t storageMem;

  //time at which gst pipeline finished pushing data to kvs sdk using put_frame, captured at main loop finish
  std::chrono::system_clock::time_point clip_senttokvssdk_time;

  //last persisted clip time
  std::chrono::system_clock::time_point lastclippersisted_time;

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
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): New credentials expiration is %u\n", __FILE__, __LINE__, credentials.getExpiration().count());
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
    device_info.clientInfo.stopStreamTimeout = 30 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    device_info.clientInfo.createClientTimeout = 30 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    device_info.clientInfo.createStreamTimeout = 30 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): SampleDeviceInfoProvider : storage size : %llu \n", __FILE__, __LINE__, device_info.storageInfo.storageSize);

    return device_info;
  }
};

/* client callbacks start*/
STATUS SampleClientCallbackProvider::clientReadyHandler(UINT64 custom_data, CLIENT_HANDLE client_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): clientReadyHandler \n", __FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes)
{
	UNUSED_PARAM(custom_handle);
	static int reportingStorageBytesCount=0;
	if( (reportingStorageBytesCount % 10) == 0 ) {
		LOG_ERROR("Reporting storage overflow. Bytes remaining" << remaining_bytes);
	}
	reportingStorageBytesCount++;
	return STATUS_SUCCESS;
}
/* client callbacks end*/


/* stream callbacks start*/
STATUS SampleStreamCallbackProvider::streamUnderflowReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle)
{
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamUnderflowReportHandler - Enter\n", __FILE__, __LINE__);
	static int underFlowCount=0;
	if( (underFlowCount % 10) == 0 ) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::streamUnderflowReportHandler\n", __FILE__, __LINE__);
                underFlowCount=0;
	}
	underFlowCount++;
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamUnderflowReportHandler - Exit\n", __FILE__, __LINE__);
        return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamLatencyPressureHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 buffer_duration)
{
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamLatencyPressureHandler - Enter\n", __FILE__, __LINE__);
	static int latencyPressureCount=0;
	if( (latencyPressureCount % 10) == 0 ) {
		LOG_ERROR("SampleStreamCallbackProvider::streamLatencyPressureHandler : buffer_duration - " << buffer_duration);
                latencyPressureCount=0;
	}
	latencyPressureCount++;
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamLatencyPressureHandler - Exit\n", __FILE__, __LINE__);
        return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode)
{
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::droppedFrameReportHandler - Enter\n", __FILE__, __LINE__);
	static int droppedFrameReportHandlerCount=0;
	CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
	if( customDataObj->kinesis_video_stream != NULL)
	{
		if( (droppedFrameReportHandlerCount % 10) == 0 )
		{
			LOG_ERROR("SampleStreamCallbackProvider::droppedFrameReportHandler : dropped_frame_timecode : " << dropped_frame_timecode);
                        droppedFrameReportHandlerCount=0;
		}
		droppedFrameReportHandlerCount++;
	}
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::droppedFrameReportHandler - Exit \n", __FILE__, __LINE__);
	return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 last_ack_duration) {
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamConnectionStaleHandler - Enter \n", __FILE__, __LINE__);
	static int connectionStaleCount =0;
	if( (connectionStaleCount % 10) == 0 ) {
		LOG_ERROR("SampleStreamCallbackProvider::streamConnectionStaleHandler : last_ack_duration : " << last_ack_duration);
                connectionStaleCount=0;
	}
	connectionStaleCount++;
	RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): SampleStreamCallbackProvider::streamConnectionStaleHandler - Exit \n", __FILE__, __LINE__);
        return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::droppedFragmentReportHandler(UINT64 custom_data,
                                                             STREAM_HANDLE stream_handle,
                                                             UINT64 timecode) {
    static int droppedFrameCount=0;
    if( (droppedFrameCount % 10) == 0 ) {
        LOG_ERROR("SampleStreamCallbackProvider::droppedFragmentReportHandler : fragment_timecode : " << timecode);
        droppedFrameCount=0;
    }
    droppedFrameCount++;
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                       UPLOAD_HANDLE upload_handle, UINT64 errored_timecode, 
                                                       STATUS status_code)
{
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::streamErrorReportHandler - Enter \n", __FILE__, __LINE__ );
    std::stringstream status_strstrm;
    status_strstrm << "0x" << std::hex << status_code;
    CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);

    if (customDataObj->kinesis_video_stream != NULL) {
        LOG_ERROR("streamErrorReportHandler Error code : " <<  status_strstrm.str());
    }
    isstreamerror_reported = true;
    //callbackObj->onUploadError(customDataObj->clip_name, status_strstrm.str().c_str());
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::streamErrorReportHandler - Exit \n", __FILE__, __LINE__ );
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamReadyHandler(UINT64 custom_data, STREAM_HANDLE stream_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d):SampleStreamCallbackProvider::streamReadyHandler invoked \n", __FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamClosedHandler(UINT64 custom_data,
                                                    STREAM_HANDLE stream_handle,
                                                    UPLOAD_HANDLE stream_upload_handle) {
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::streamClosedHandler invoked\n", __FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamDataAvailableHandler(UINT64 custom_data,
                                                           STREAM_HANDLE stream_handle,
                                                           PCHAR stream_name,
                                                           UPLOAD_HANDLE stream_upload_handle,
                                                           UINT64 duration_available,
                                                           UINT64 size_available) {
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::streamDataAvailableHandler invoked\n",__FILE__, __LINE__);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::FragmentAckReceivedHandler(UINT64 custom_data,STREAM_HANDLE stream_handle,
                                                         UPLOAD_HANDLE upload_handle,PFragmentAck pFragmentAck)
{
    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): SampleStreamCallbackProvider::FragmentAckReceivedHandler - Enter \n", __FILE__, __LINE__ );
	CustomData *customDataObj = reinterpret_cast<CustomData *>(custom_data);
	static uint64_t totalclipcount = 0;
	static uint64_t totalclipuploadtime = 0;

	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","Reporting fragment ACK received. Fragment timecode %llu\n", pFragmentAck->timestamp );
	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","Reporting fragment ACK received. Fragment type %d\n", pFragmentAck->ackType );
	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","Reporting fragment ACK received. Fragment seq number %s\n", pFragmentAck->sequenceNumber );

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
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","Pending clips: clipName - %s timecode - %" PRIu64 "\n", x.second.c_str(), x.first);
		}    
		return;
	}

	if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_PERSISTED)
	{
		totalclipcount++;
                std::chrono::system_clock::time_point time_now = std::chrono::system_clock::now();
		data.lastclippersisted_time = time_now;

                //log current epoch time in file
                std::ofstream ofs(CVR_STATUS_FILE,std::ios::out | std::ios::binary);
                if (!ofs.is_open()) {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):  Error in opening persisted update file\n");
                } else {
                    ofs << std::chrono::system_clock::to_time_t(time_now);
                    ofs.close();
                }

                // Update only the expiration
		ms time_diff = std::chrono::duration_cast<ms>(time_now - customDataObj->clip_senttokvssdk_time);
		totalclipuploadtime+=time_diff.count();
		uint64_t avgtime_clipupload = totalclipuploadtime/totalclipcount ;

		std::map<uint64_t, std::string>::iterator it;
		it = clipmapwithtimecode.find(pFragmentAck->timestamp);
		if (it != clipmapwithtimecode.end())
		{
			string persistedclip;
			persistedclip = it->second;
			//epochtime_senttokvssdk, currenttime, epochfragmenttimecode_server, fragmentnumber, timediff
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
					__FILE__, __LINE__, persistedclip.c_str(), pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff.count());
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvs upload persisted time and clip sent time :%llu,%llu \n",__FILE__, __LINE__,std::chrono::duration_cast<std::chrono::milliseconds>(data.lastclippersisted_time.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::milliseconds>(customDataObj->clip_senttokvssdk_time.time_since_epoch()).count());
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvs upload stats :%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,totalclipcount);

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
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsclip upload successful %s, %lld, %s, %lld \n",
					__FILE__, __LINE__, persistedclip.c_str(), pFragmentAck->timestamp, pFragmentAck->sequenceNumber, time_diff.count());
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvs upload persisted time and clip sent time :%llu,%llu \n",__FILE__, __LINE__,std::chrono::duration_cast<std::chrono::milliseconds>(data.lastclippersisted_time.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::milliseconds>(customDataObj->clip_senttokvssdk_time.time_since_epoch()).count());
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvs upload stats :%lld,%lld \n",__FILE__, __LINE__,avgtime_clipupload,totalclipcount);

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
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","kvsclip upload unsuccessful %s\n",clipName.c_str());
		}
		if(it == clipmapwithtimecode.end())
		{
			//buffering missed, but a timecode is failed. Pop the queue and send failure notification.
			if (!queueclipName.empty())
			{
				clipName = queueclipName.front();
				queueclipName.pop_front();
				callbackObj->onUploadError(clipName.c_str(),"failed");
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","kvsclip upload unsuccessful %s\n",clipName.c_str());
			}
			else
			{
				callbackObj->onUploadError("0", "failed");
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","kvsclip upload unsuccessful\n");
			}
		}
		return;
	}
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d):SampleStreamCallbackProvider::FragmentAckReceivedHandler - Exit\n", __FILE__, __LINE__);
	return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler(UINT64 custom_data,
                                                                      STREAM_HANDLE stream_handle,
                                                                      UINT64 remaining_duration) {
    static int bufferdurationOverflowCount=0;
    if( (bufferdurationOverflowCount % 10) == 0 ) {
        LOG_ERROR("SampleStreamCallbackProvider::bufferDurationOverflowPressureHandler invoked : remaining_duration : " <<  remaining_duration);
        bufferdurationOverflowCount=0;
    }
    bufferdurationOverflowCount++;
    return STATUS_SUCCESS;
}

/* stream callbacks end*/

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;

unique_ptr<Credentials> credentials_;


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

//kinesis producer init
static void kinesisVideoInit(CustomData *data)
{
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

    LOG_INFO("kinesisVideoInit defaultRegion = " << defaultRegionStr);

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

    LOG_INFO("Kinesis Video Streams Client is ready");
}

//kinesis stream init
static void kinesisVideoStreamInit(CustomData *data)
{
	string content_type;
        std::map<string, string> tags;

	if ( data->gkvsclip_audio )
	{
		content_type = "video/h264,audio/aac";
	}
	else
	{
		content_type = "video/h264";
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): content_type = %s\n", __FILE__, __LINE__, content_type.c_str());

	setKVSTags(tags);
        for ( auto& x: tags) {
	        RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","tag tagname - %s tagvalue - %s\n", x.first.c_str(),x.second.c_str());
        }

	auto stream_definition = make_unique<StreamDefinition>(data->stream_name,
			hours(DEFAULT_RETENTION_PERIOD_HOURS),
			&tags,
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
	}

	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stream %s is ready\n", __FILE__, __LINE__, data->stream_name );
}

//kvs video stream uninit
void kinesisVideoStreamUninit(CustomData *data)
{
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : kvs stream uninit started\n", __FILE__, __LINE__);
    if (data->kinesis_video_stream != NULL)
    {
        data->stream_started = false;
        data->kinesis_video_stream->stop();
        data->kinesis_video_producer->freeStream(data->kinesis_video_stream);
        data->kinesis_video_stream = NULL;
    }
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : kvs stream uninit done\n", __FILE__, __LINE__);
}

void kinesisVideoStreamUninitSync(CustomData *data)
{
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : kvs stream uninit started\n", __FILE__, __LINE__);
    if (data->kinesis_video_stream != NULL)
    {
        data->stream_started = false;
        data->kinesis_video_stream->stopSync();
        data->kinesis_video_producer->freeStream(data->kinesis_video_stream);
        data->kinesis_video_stream = NULL;
    }
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : kvs stream uninit done\n", __FILE__, __LINE__);
}

//recreate stream
static bool recreate_stream(CustomData *data)
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : Attempt to recreate kinesis video stream \n", __FILE__, __LINE__);
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
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) : Failed to create kinesis video stream : retrying \n", __FILE__, __LINE__);
			this_thread::sleep_for(std::chrono::seconds(2));
			retry++;
			if ( retry > KVSINITMAXRETRY )
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) : FATAL : Max retry reached in recreate_stream exit process %d\n", __FILE__, __LINE__);
				return false;
			}
		}
	} while(do_repeat);
    
        return true;
}

//reset stream
static void reset_stream(CustomData *data) {
  RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) : Attempt to reset kinesis video stream \n", __FILE__, __LINE__);
  bool do_repeat = true;
  int retry=0;
  do {
      try {
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
  } while(do_repeat);
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

	//set stream id
	data.cvr_stream_id = stream_id;

	//init kinesis video
	try
	{
		kinesisVideoInit(&data);
	}
	catch (runtime_error &err)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): kinesisVideoInit exited with error: %s\n", __FILE__, __LINE__, err.what());
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
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): audio=%d, kvsclip_highmem =%d\n", __FILE__, __LINE__, kvsclip_audio,kvsclip_highmem);

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
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Time out error in kinesisVideoStreamInit \n", __FILE__, __LINE__);
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
    if( data.gkvsclip_audio != clipaudioflag)
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : prior audio enable flag : %d , current audio enable flag : %d\n", __FILE__, __LINE__, data.gkvsclip_audio, clipaudioflag);
        return 1;
    }
    if( data.gkvsclip_highmem != cliphighmem )
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : highmem flag change between default data.gkvsclip_highmem : %d, kvsclip_highmem : %d\n", __FILE__, __LINE__, data.gkvsclip_highmem,cliphighmem);
        return 1;
    }
    if( isstreamerror_reported )
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : isstreamerror reported as TRUE : %d \n", __FILE__, __LINE__, isstreamerror_reported);
        isstreamerror_reported = false;
        return 1;
    }
    if( frame_dropped_flag )
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : consecutive video clips failing flag as TRUE : %d frame_dropped_count: %d \n", __FILE__, __LINE__, frame_dropped_flag,frame_dropped_count);
        frame_dropped_flag = 0;
        frame_dropped_count = 0;
        //reset_stream(&data); //TBD
        return 1;
    }

    int retstatus=0;
    size_t buffer_size = frameData.frame_size;
    if (!data.stream_started)
    {
        if(clipaudioflag)
            data.kinesis_video_stream->start(std::string(DEFAULT_CODECID_AACAUDIO),DEFAULT_AUDIO_TRACKID);
        data.stream_started = true;
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","Streaming Started \n");
    }

    if(data.first_frame)
    {
        STRNCPY(data.clip_name, filename, MAX_STREAM_NAME_LEN);
        data.clip_name[MAX_STREAM_NAME_LEN -1] = '\0';
        std::string clipName(filename);
        queueclipName.push_back(clipName);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvs clip to server : %s \n", __FILE__, __LINE__,clipName.c_str());
        RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "Setting the Key Frame flag (very first frame)\n");
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
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): %s Sending EoFr\n", __FILE__, __LINE__,data.clip_name );
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s kvs clip size:%d\n", __FILE__, __LINE__,data.clip_name, single_clip_size );
        single_clip_size = 0;
        //compare clip sent time with clip persisted time to detect hang in application
        ms cliptime_diff = std::chrono::duration_cast<ms>(data.clip_senttokvssdk_time - data.lastclippersisted_time);
        if( cliptime_diff.count() > CVR_THRESHHOLD_COUNT_IN_MILLISECONDS ) {
            //to maintain 60 seconds send/ack time gap and to avoid if the second clip's ack is delayed than sending
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to get clip upload status - Timeout : %lld : attempt to recreate stream \n", __FUNCTION__, __LINE__, cliptime_diff.count());
            //In poor network condition hang seen with kinesisVideoStreamUninitSync api
            //kinesisVideoStreamUninitSync(&data);
            //reset_stream(&data); //TBD
            return 1;
        }
        data.clip_senttokvssdk_time = std::chrono::system_clock::now();
    } else {
        single_clip_size += buffer_size;
        if ( (frameData.pic_type == 1) || (frameData.pic_type == 2)) {
            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): [ IFRAME_ENCODER ] Timestamp %llu : \n", __FILE__, __LINE__,frametimestamp_nano );
        } else if ( frameData.pic_type == 3 ) {
            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): [ PFRAME_ENCODER ] Timestamp %llu : \n", __FILE__, __LINE__,frametimestamp_nano );
        } if( frameData.stream_type == 10 ) {
            track_id = 2 ;
            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): [ AUDIO_ENCODER ] Timestamp %llu : \n", __FILE__, __LINE__,frametimestamp_nano );
        }
        
        if (!put_frame(data.kinesis_video_stream, (void*)frameData.frame_ptr, std::chrono::nanoseconds(frametimestamp_nano),
                    std::chrono::nanoseconds(frametimestamp_nano), kinesis_video_flags,  buffer_size,track_id)) {
            //log every minute
            if( (frame_dropped_count % 60) == 0 ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Error - Failed to put frame : frame_dropped_count : %d : clip_name : %s stream type : %d\n",
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
