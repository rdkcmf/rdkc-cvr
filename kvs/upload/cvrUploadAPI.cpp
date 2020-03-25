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
#include <iostream>
#include <cstdlib>
#include <pthread.h>
#include <cstring>
#include <unistd.h>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <signal.h>
#include <queue>
#include <fstream>
#include <chrono>
#include "rdk_debug.h"
#include "cvrUploadAPI.h"
#include "rtConnection.h"
#include "rtLog.h"
#include "rtMessage.h"
#include "event_config.h"   //EventType
#include "kvsupload.h"

#define MAXSIZE			512
#define CVR_THRESHHOLD_COUNT    4
#define CVR_FLUSH_COUNT         1
#define KVSINITMAXRETRY         5
#define RDKC_FAILURE     	-1
#define RDKC_SUCCESS		0

typedef struct cvr_upload_params_s
{
        char fpath[MAXSIZE];		/* clip path */
        char starttime[MAXSIZE];	/* clip start time */
        char endtime[MAXSIZE];		/* clip end time */
        int event_type;			/* event name */
        unsigned int event_datetime;    /* event date and time */
        char m_fpath[MAXSIZE];		/* motion level file path */
        int motion_level_idx;		/* motion level index */
	char str_od_data[MAXSIZE]; 	/* od_frame_data */
	char va_engine_version[10];
	bool smartTnEnabled;
        unsigned short gkvsclip_audio;
        unsigned short gkvsclip_abstime;
        unsigned short gkvsclip_livemode;
        int stream_id;
} cvr_upload_params_t;


enum CVR_UPLOAD_STATUS
{
        CVR_UPLOAD_OK = 0,
        CVR_UPLOAD_FAIL,
        CVR_UPLOAD_CONNECT_ERR,
        CVR_UPLOAD_SEND_ERR,
        CVR_UPLOAD_RESPONSE_ERROR,
        CVR_UPLOAD_TIMEOUT,
        CVR_UPLOAD_MAX,
	CVR_UPLOAD_CURL_ERR
};

using namespace std;
static cvr_upload_params_t param;
static std::queue<cvr_upload_params_t> msgQ_kvs;
static std::mutex msgQueueMutex_kvs;
static std::condition_variable msgCv_kvs;
static bool kvsUploadThreadExit = true;
static rtConnection connectionSend;
static rtError err;
uint64_t hangdetecttime = 0;

/** @descripion: get file size
 *  @param[in] args: filename
 *  @return: file size
 */
static int filesize(const char* filename)
{
        std::ifstream::pos_type size;
        std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
        size = in.tellg();
        return static_cast<int> (size);
}

/**
 * @description: This function is used to notify smart thumbnail about cvr upload status
 * @param[in]: status, uploadedclip_filename
 * @return: None
 */
static void notify_smt_TN(CVR_UPLOAD_STATUS status, char* upload_fname)
{
	char upload_file_basename[20] = {0};
	const char* ext_delim = strchr(upload_fname, '.');

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): upload file name:%s\n",__FILE__, __LINE__, upload_fname);
	strncpy(upload_file_basename, upload_fname, (ext_delim - upload_fname));
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): upload file base name:%s\n",__FILE__, __LINE__, upload_file_basename);
	
        rtMessage msg;
        rtMessage_Create(&msg);
        rtMessage_SetInt32(msg, "status", status);
	rtMessage_SetString(msg, "uploadFileName", upload_file_basename);

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Sending CVR upload status to smart thumbnail, status code:%d\n",__FILE__, __LINE__, status);
        rtError err = rtConnection_SendMessage(connectionSend, msg, "RDKC.CVR.UPLOAD.STATUS");
        rtLog_Debug("SendRequest:%s", rtStrError(err));

        if (err != RT_OK)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) Error sending msg via rtmessage\n", __FILE__,__LINE__);
        }
        rtMessage_Release(msg);
}

/** @descripion: pull the message from the kvs queue
 *  @param[out] msg: msg received from queue
 *  @return: void
 */
static void PopMsg_kvs(cvr_upload_params_t *msg)
{
        std::unique_lock<std::mutex> lock(msgQueueMutex_kvs);
        msgCv_kvs.wait(lock,[]{return !msgQ_kvs.empty();});

	*msg = msgQ_kvs.front();
        msgQ_kvs.pop();

        lock.unlock();
}

/** @descripion: Push the msg into kvs queue
 *  @param[in] msg: message to push
 *  @return: void
 */
static void PushMsg_kvs(cvr_upload_params_t msg)
{
        int queue_size = msgQ_kvs.size();
        pid_t pid = 0;
        cvr_upload_params_t upload_params;

        //start - code to check hang in upload thread
        static int clipcounter = 0 ;
        static uint64_t hangdetecttime1 = 0 ;
        static uint64_t hangdetecttime2 = 0 ;
        static uint64_t hangdetecttime3 = 0 ;
        if ( 0 != hangdetecttime ) {
                clipcounter++;
                if( 1 == clipcounter ) {
                        hangdetecttime1 = hangdetecttime;
                }else if ( 2 == clipcounter ) {
                        hangdetecttime2 = hangdetecttime;
                }else if ( 3 == clipcounter ) {
                        hangdetecttime3 = hangdetecttime;
                        clipcounter = 0;
                }
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Hang testing : %lld, %lld, %lld, %lld\n", __FUNCTION__, __LINE__, hangdetecttime1, hangdetecttime2, hangdetecttime3, hangdetecttime);

                if ( (hangdetecttime1 == hangdetecttime2) && (hangdetecttime2 == hangdetecttime3) ) {
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Hang detected in upload thread : %lld \n", __FUNCTION__, __LINE__, hangdetecttime);
                        fflush(stdout);

                        kill(getpid(), SIGFPE);
                        fflush(stdout);
                        kill(getpid(), SIGTERM);
                }
        }
        //end - code to check hang in upload thread

        //Flush out older clips in case of threshold
        if (queue_size > CVR_THRESHHOLD_COUNT) {
                memset(&upload_params, 0, sizeof(cvr_upload_params_t));
                PopMsg_kvs(&upload_params);
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Flushing old clip %s and %s\n", __FUNCTION__, __LINE__, upload_params.fpath, upload_params.m_fpath);
                unlink(upload_params.fpath);
                unlink(upload_params.m_fpath);
                /*if( clipflushcount >= CVR_FLUSH_COUNT ) {
                        pid = getpid();
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Threshold limit reached in upload failure : %d : sending sigterm to Thread id : %d \n", __FUNCTION__, __LINE__,clipflushcount,pid);
                        clipflushcount = 0;
                        kill(pid, SIGTERM);
                }*/
        }
        
        std::unique_lock<std::mutex> lock(msgQueueMutex_kvs);
        msgQ_kvs.push(msg);
        lock.unlock();
        msgCv_kvs.notify_one();
}

/** @descripion: kvs upload thread
 *  @param[in] args: input arguments if any
 *  @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
static void * kvsUpload(void* args)
{
        cvr_upload_params_t upload_params;
	memset(&upload_params, 0, sizeof(cvr_upload_params_t));
        pid_t pid = 0;
        srand(time(NULL));
        uint32_t retryafterTime = 0;
        static bool iskvsinitdone=false;
        static bool iskvsstreaminitdone=false;
        unsigned short contentchangestatus = 0;

        //kvs init
        int ret_kvs = 0;
        //rt connection
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Making rt send connection \n", __FILE__, __LINE__);
        rtLog_SetLevel(RT_LOG_INFO);
        rtLog_SetOption(rdkLog);
        rtConnection_Create(&connectionSend, "CVR_UPLOAD_SEND", "tcp://127.0.0.1:10001");

        while(!kvsUploadThreadExit) {
		int queue_size = msgQ_kvs.size();
                int ret = RDKC_SUCCESS;
                memset(&upload_params, 0, sizeof(cvr_upload_params_t));
                PopMsg_kvs(&upload_params);
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs clip to server : %s, %s, %s, %d, %lu, %s, %d, %u, %u, %u, %d\n", __FILE__, __LINE__, 
                                upload_params.fpath, upload_params.starttime, upload_params.endtime, upload_params.event_type,
                                upload_params.event_datetime, upload_params.m_fpath, upload_params.motion_level_idx,
                                upload_params.gkvsclip_audio, upload_params.gkvsclip_abstime, upload_params.gkvsclip_livemode, upload_params.stream_id);

                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs clip size:%d\n", __FILE__, __LINE__,filesize(upload_params.fpath));


                if(upload_params.gkvsclip_audio) {
                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs clip has audio\n", __FILE__, __LINE__);
                }

                if ( false == iskvsinitdone ) {
                        do {
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Invoking kvs_init\n", __FILE__, __LINE__);
                                ret_kvs = kvs_init(upload_params.stream_id);

                                static int retry = 0;
                                if (0 != ret_kvs) {
                                        retry++;
                                        if ( retry > KVSINITMAXRETRY ) {
                                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : FATAL : Max retry reached in kvs_init exit process %d\n", __FILE__, __LINE__);
                                                exit(1);
                                        }
                                        iskvsinitdone = false;
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : kvs_init Failed retry number : %d \n", __FILE__, __LINE__,retry);
                                        sleep(2);
                                } else if ( 0 == ret_kvs ) {
                                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs_init success \n", __FILE__, __LINE__);
                                        iskvsinitdone = true;
                                }
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): After Invoking kvs_init\n", __FILE__, __LINE__);
                        } while ( ret_kvs != 0);
                }
                
                hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                //kvs stream init
                if ( false == iskvsstreaminitdone ) {
                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Invoking kvs_stream_init \n", __FILE__, __LINE__ );
                        contentchangestatus = 0;
                        ret_kvs = kvs_stream_init(upload_params.gkvsclip_audio,upload_params.gkvsclip_abstime,upload_params.gkvsclip_livemode,contentchangestatus,hangdetecttime);                                
                        if ( 0 == ret_kvs ) {
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs_stream_init success \n", __FILE__, __LINE__);
                                iskvsstreaminitdone = true;
                        } else {
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs_stream_init failure exit \n", __FILE__, __LINE__);
                                exit(1);
                        }
                }

                //parse filename
                char *p_file_name = NULL;
                if (NULL != (p_file_name = strrchr(upload_params.fpath, '/'))) {
		        p_file_name++;                                                                                      
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): clip file name : %s\n", __FILE__, __LINE__,p_file_name);
		}

                //delete motion file
                if ( strlen(upload_params.m_fpath)!= 0 ) {
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Deleting motion clip %s\n", __FILE__, __LINE__, upload_params.m_fpath);
                        unlink(upload_params.m_fpath);
                }

                bool cvr_upload_retry = false;
                do {
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): check values : %u %u %u\n", __FILE__, __LINE__, 
                                upload_params.gkvsclip_audio,upload_params.gkvsclip_abstime,upload_params.gkvsclip_livemode);

                        hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        ret = kvs_stream_play(upload_params.fpath,upload_params.gkvsclip_audio,upload_params.gkvsclip_abstime,upload_params.gkvsclip_livemode,hangdetecttime);
                        if( -1 == ret ) { //failure
                                cvr_upload_retry = false;
                                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d):  kvs_stream_play : upload failed %s\n", __FILE__, __LINE__, p_file_name);
                                notify_smt_TN(CVR_UPLOAD_FAIL, p_file_name);
                        } else if ( 0 == ret ) { //success
                                cvr_upload_retry = false;
                                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): kvs_stream_play : upload success : %s\n", __FILE__, __LINE__, p_file_name);
                                if( upload_params.event_type == EVENT_TYPE_MOTION) {
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs Upload Successful with Motion\n", __FUNCTION__, __LINE__);
				} else {
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs Upload Successful without Motion\n", __FUNCTION__, __LINE__);
				}
                        	notify_smt_TN(CVR_UPLOAD_OK, p_file_name);
                        } else if ( 1 == ret ) { //content change
                                cvr_upload_retry = true;
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d):  content change found for clip reinit kvs stream %s\n", __FILE__, __LINE__, p_file_name);
                                iskvsstreaminitdone = false;
                                contentchangestatus = 1;
                                //kvs stream re-init
                                if ( false == iskvsstreaminitdone ) {
                                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Re-Invoking kvs_stream_init \n", __FILE__, __LINE__ );
                                        ret_kvs = kvs_stream_init(upload_params.gkvsclip_audio,upload_params.gkvsclip_abstime,upload_params.gkvsclip_livemode,contentchangestatus,hangdetecttime);                                
                                        if ( 0 == ret_kvs ) {
                                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Re-Invoking kvs_stream_init success \n", __FILE__, __LINE__);
                                                iskvsstreaminitdone = true;
                                        }
                                }
                        } /*else if ( 2 == ret ) { //retry logic
                                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): retry clip upload %s\n", __FILE__, __LINE__, p_file_name);
                                cvr_upload_retry = true;
                        } */else if ( 3 == ret ) { //pipeline error
                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in video pipeline initilaization for %s\n", __FILE__, __LINE__, p_file_name);
                                sleep(2);
                                cvr_upload_retry = true;
                        }else {
                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Unknown return status for clip %s\n", __FILE__, __LINE__, p_file_name);
                        }
                } while (cvr_upload_retry) ;
                //delete clip file
                if ( strlen(upload_params.fpath) != 0 ) {
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Deleting clip %s\n", __FILE__, __LINE__, upload_params.fpath);
                        unlink(upload_params.fpath);
                }
        }
}

/**
 * @description: This function is used to push the data into queue.
 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
 * @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
int cvr_upload(char* fpath, char* starttime, char* endtime, int event_type, unsigned int event_datetime, char* m_fpath, 
               int motion_level_idx, char* str_od_data , char* va_engine_version, bool smartTnEnabled,
               unsigned short& kvsclip_audio, unsigned short& kvsclip_abstime, unsigned short&  kvsclip_livemode, int stream_id)
{
	memset(&param, 0, sizeof(cvr_upload_params_t));

	/* fill the structure as per input argument, and push the structure into queue */
	strcpy(param.fpath,fpath);
	strcpy(param.starttime,starttime);
	strcpy(param.endtime,endtime);
        param.event_type = event_type;
        param.event_datetime = event_datetime;
	strcpy(param.m_fpath,m_fpath);
        param.motion_level_idx = motion_level_idx;
	if(str_od_data) {
		strcpy(param.str_od_data, str_od_data);
	}

	if(va_engine_version) {
		strcpy(param.va_engine_version, va_engine_version);
	}

	param.smartTnEnabled = smartTnEnabled;
        param.gkvsclip_audio = kvsclip_audio ; /* audio enable flag */
        param.gkvsclip_abstime = kvsclip_abstime; /* abs timestamp flag */
        param.gkvsclip_livemode = kvsclip_livemode; /* live mode flag */
        param.stream_id = stream_id;

        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): kvsclip_audio : %d : kvsclip_abstime: %d : kvsclip_livemode : %d : stream id : %d\n", 
                           __FILE__, __LINE__, param.gkvsclip_audio, param.gkvsclip_abstime, param.gkvsclip_livemode, param.stream_id);

        PushMsg_kvs(param);
	return RDKC_SUCCESS;
}

/** @descripion: initialize cvr upload
 *  @param: void.
 *  @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
int cvr_upload_init(int argc, char* argv[])
{
	/* ENABLING RDK LOGGER */
        rdk_logger_init("/etc/debug.ini");

        pthread_t kvsUploadThread;

        /* Create kvs upload thread */
        kvsUploadThreadExit = false;
        if( RDKC_SUCCESS != pthread_create(&kvsUploadThread, NULL, kvsUpload, NULL) ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): can't create kvs upload thread", __FILE__, __LINE__ );
                return RDKC_FAILURE;
        }
        pthread_setname_np(kvsUploadThread,"kvsUploadthread");
        return RDKC_SUCCESS;
}

/** @descripion: close cvr upload
 *  @param: void.
 *  @return: void
 */
void cvr_upload_close()
{
	//Closing the connection
        kvsUploadThreadExit = true;
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Closing the kvs thread connection\n", __FILE__, __LINE__);
}
