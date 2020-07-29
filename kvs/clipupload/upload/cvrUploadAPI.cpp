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
#include "HttpClient.h"
#include "cJSON.h"

extern "C"{
 #include "polling_config.h"
}

#define MAXSIZE			512
#define CVR_THRESHHOLD_COUNT    4
#define CVR_FLUSH_COUNT         1
#define MAXRETRY                5
#define RDKC_FAILURE     	-1
#define RDKC_SUCCESS		0
#define STAT_UPLOAD_INTERVAL  30
#define DNS_CACHE_TIMEOUT     60
#define PAYLOAD_SIZE 1024
#define PACKAGE_SIZE    2048 
#define URL_LENGTH  256
#define AUTH_TOKEN_SIZE	1024

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

typedef struct _stat_config
{
    int isEnabled;
    int interval;
    int failurePercent;
    char url[URL_LENGTH];
    char auth_token[AUTH_TOKEN_SIZE];
}cvrStatConfig;

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
static rtConnection connectionSend;
static rtError err;
uint64_t hangdetecttime = 0;

//cvr stats variables
static cvrStatConfig statConfig = {false/*disabled*/, 0 /*interval*/, 0/*failurePercent*/, NULL/*URL*/, NULL/*Token*/};
static int motionCount = 0;
static int numClipPushed = 0;
static int numClipPushSuccess = 0;
static int numClipPushFailure = 0;
static float percentageCalculated = 0.0f;
static char statuploadInterval[CONFIG_ATTR_MAX] = {0};


static std::condition_variable msgCv_kvs;
static std::mutex statisticsMutex;
static std::condition_variable statCv;
static bool kvsUploadThreadExit = true;
static bool cvrStatisticsUploadThreadExit = true;
static bool uploadData = false;
static int waitingInterval = 1;


/*
 *  @description decode ascii time string (when specified in days,hors,minute and seconds) into duration of time
 *  @param char*   text  string to be decoded
 *  @return duration  (seconds)
 */
static time_t parseISO8601Duration(char *duration)
{
	char *iter;
	int value    = 0;
	int year     = 0;
	int month    = 0;
	int seconds  = 0;
	int minutes  = 0;
	int hours    = 0;
	int days     = 0;
	time_t iso_duration = 0;

	iter = duration;

	if (*iter++ == 'P')
	{
		while (*iter != '\0')
		{
			value = 0;
			while (*iter >= '0' && *iter <= '9')
			{
				/* assumes ASCII sequence! */
				value = 10 * value + *iter++ - '0';
			}

                        switch (*iter++)
                        {
				case 'D':
					days = value;
					break;
				case 'H':
					hours = value;
					break;
			        case 'M':
                                        minutes = value;
					break;
				case 'S':
					seconds = value;
					break;
				default:
					break;
			}
		}
		iso_duration = seconds + 60 * minutes + 3600 * hours + 86400 * days;
	}
	return iso_duration;
}

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
    int isstatuploadenabled = false;
    int kvsmotionCount = 0;
    int kvsnumClipPushed = 0;
    int kvsnumClipPushSuccess = 0;
    int kvsnumClipPushFailure = 0;
    float kvspercentageCalculated = 0.0f;
    struct timespec cvrstatstartTime;
    struct timespec cvrstatcurrTime;
    clock_gettime(CLOCK_REALTIME, &cvrstatstartTime);
    
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

        isstatuploadenabled = statConfig.isEnabled;

        if(isstatuploadenabled)
        {
            clock_gettime(CLOCK_REALTIME, &cvrstatcurrTime);
        }

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
                    if ( retry > MAXRETRY ) {
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

            if(isstatuploadenabled) {
                kvsnumClipPushed++;
            }

            hangdetecttime = chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            ret = kvs_stream_play(upload_params.fpath,upload_params.gkvsclip_audio,upload_params.gkvsclip_abstime,upload_params.gkvsclip_livemode,hangdetecttime);
            if( -1 == ret ) { //failure
                cvr_upload_retry = false;
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d):  kvs_stream_play : upload failed %s\n", __FILE__, __LINE__, p_file_name);
                if(isstatuploadenabled) {
                    kvsnumClipPushFailure++;
                }
                notify_smt_TN(CVR_UPLOAD_FAIL, p_file_name);
            } else if ( 0 == ret ) { //success
                cvr_upload_retry = false;
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): kvs_stream_play : upload success : %s\n", __FILE__, __LINE__, p_file_name);
                if( upload_params.event_type == EVENT_TYPE_MOTION) {
                    if(isstatuploadenabled) {
                        kvsmotionCount++;
                    }
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs Upload Successful with Motion\n", __FUNCTION__, __LINE__);
                } else {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs Upload Successful without Motion\n", __FUNCTION__, __LINE__);
                }

                if(isstatuploadenabled) {
                    kvsnumClipPushSuccess++;
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

        if(isstatuploadenabled && ((cvrstatcurrTime.tv_sec - cvrstatstartTime.tv_sec) >= statConfig.interval))
        {
            kvspercentageCalculated = (static_cast<float>(kvsnumClipPushFailure)/static_cast<float>(kvsnumClipPushed))*100;
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): upload stats:  kvsnumClipPushed : %d,  kvsnumClipPushFailure: %d, kvsnumClipPushSuccess: %d, kvsmotionCount : %d, kvspercentageCalculated : %.2f\n",
                    __FILE__, __LINE__, kvsnumClipPushed, kvsnumClipPushFailure, kvsnumClipPushSuccess,kvsmotionCount,kvspercentageCalculated);
            if(static_cast<int>(kvspercentageCalculated) >= statConfig.failurePercent)
            {
                motionCount = kvsmotionCount;
                numClipPushed = kvsnumClipPushed;
                numClipPushSuccess = kvsnumClipPushSuccess;
                numClipPushFailure = kvsnumClipPushFailure;
                percentageCalculated = kvspercentageCalculated;
                std::lock_guard<std::mutex> lk(statisticsMutex);
                uploadData = true;
                statCv.notify_one();
            }
            kvsmotionCount = 0;
            kvsnumClipPushed = 0;
            kvsnumClipPushSuccess = 0;
            kvsnumClipPushFailure = 0;
            kvspercentageCalculated = 0.0f;
            clock_gettime(CLOCK_REALTIME, &cvrstatstartTime);
        }
    }
}

/**
 * @description: This function is used to sleep for some waitingInterval  before next retry.
 * @return: Error code.
 */
static int retryAtExpRate()
{
    int ret = RDKC_FAILURE;
    int retryFactor = 2;
    if(waitingInterval <= STAT_UPLOAD_INTERVAL)
    {
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s: %d: Waiting for %d seconds!\n", __FILE__, __LINE__, (int)waitingInterval);
        sleep(waitingInterval);  
        waitingInterval *= retryFactor;
        ret = RDKC_SUCCESS;
    }
    return ret;
}   

/** @descripion: cvr stats upload thread
 *  @param: arg
 *  @return: void
 */
static void* cvrStatisticsUpload(void* arg)
{
    cJSON *cvrstatmessage;
    char* statmessage = NULL;
    char packHead[PACKAGE_SIZE] = {0};
    int uploadTimeInterval = 30;
    int remainingTime = 0;
    long response_code = 0;
    HttpClient* httpClient = NULL;
    struct timespec startTime;
    struct timespec currTime;

    while(!cvrStatisticsUploadThreadExit)
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): cvrStatisticsUpload thread before sleep\n", __FILE__, __LINE__);
        std::unique_lock<std::mutex> lk(statisticsMutex);
        statCv.wait(lk,[]{return uploadData == true;});
        cvrstatmessage = cJSON_CreateObject();
        cJSON_AddNumberToObject(cvrstatmessage, "failure", numClipPushFailure);
        cJSON_AddNumberToObject(cvrstatmessage, "success", numClipPushSuccess);
        cJSON_AddStringToObject(cvrstatmessage, "period", statuploadInterval);
        cJSON_AddNumberToObject(cvrstatmessage, "motion-count", motionCount);
        statmessage = cJSON_PrintUnformatted(cvrstatmessage);

        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): cvr failure stats data is %s\n", __FILE__, __LINE__,statmessage);

        motionCount = 0;
        numClipPushed = 0;
        numClipPushSuccess = 0;
        numClipPushFailure = 0;
        percentageCalculated = 0.0f;

        if(uploadData)
        {
            uploadData = false;
            clock_gettime(CLOCK_REALTIME, &startTime);

            httpClient = new HttpClient();
            if(NULL != httpClient)
            {
                /* Open the URL */
                httpClient->open(statConfig.url, DNS_CACHE_TIMEOUT);
            }
            else
            {
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to open the URL\n", __FUNCTION__, __LINE__);
                continue;
            }

            int fileLen = strlen(statmessage);
            httpClient->resetHeaderList();
            memset(packHead, 0, sizeof(packHead));
            snprintf(packHead, sizeof(packHead), "application/json");
            httpClient->addHeader( "Content-Type", packHead);
            memset(packHead, 0, sizeof(packHead));
            snprintf(packHead, sizeof(packHead), "%s", statConfig.auth_token);
            httpClient->addHeader( "Authorization", packHead);
            memset(packHead, 0, sizeof(packHead));
            snprintf(packHead, sizeof(packHead), "%d", fileLen);
            httpClient->addHeader( "Content-Length", packHead);
            
            int retry = 0;
            int curlCode = 0;
            while(true)
            {
                clock_gettime(CLOCK_REALTIME, &currTime);
                remainingTime = uploadTimeInterval - (currTime.tv_sec - startTime.tv_sec);
                curlCode = httpClient->post_binary(statConfig.url, statmessage, &response_code, fileLen, remainingTime);
                if ((response_code >= RDKC_HTTP_RESPONSE_OK) && (response_code < RDKC_HTTP_RESPONSE_REDIRECT_START))
                {
                    break;
                }
                else
                {
                    retry++;
                    if(RDKC_SUCCESS != retryAtExpRate())
                    {
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): retries done with current exp wait interval %d\n",__FILE__, __LINE__,waitingInterval);
                        break;
                    }
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Retrying again for %d times with %d sec remaining timeout  because last try failed  with response code %lu and curl code %d\n",__FUNCTION__,__LINE__,retry, remainingTime, response_code, curlCode);
                }
            }

            //log success/failure for telemetry
            if ((response_code >= RDKC_HTTP_RESPONSE_OK) && (response_code < RDKC_HTTP_RESPONSE_REDIRECT_START))
            {
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): cvr statistics uploaded successfully\n", __FUNCTION__, __LINE__);
            }
            else
            {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): CVRupload failed with response code %lu and curl code %d\n", __FUNCTION__, __LINE__, response_code, curlCode);
            }

            /* Close the connection */
            httpClient->close();
            cJSON_Delete(cvrstatmessage);
            free(statmessage);
            statmessage = NULL;
            RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): cvr stat upload is done\n", __FUNCTION__, __LINE__);
        }
    }
}

/**
 * @description: This function is used to update cvr stats configuration.
 * @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
int updateStatConfiguration()
{
    cvrStats_provision_info_t *cvrstatConfig = NULL;
    int retry = 0;
    char buff[10] = {'\0'};

    // Read cvr stats config.
    cvrstatConfig = (cvrStats_provision_info_t*) malloc(sizeof(cvrStats_provision_info_t));
    if (NULL == cvrstatConfig) {
        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error allocating memory for cvr stats confn", __FILE__, __LINE__);
        return RDKC_FAILURE;
    }

    while (true) {
        if (RDKC_SUCCESS != readCVRStatsConfig(cvrstatConfig)) {
            RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error reading cvr stats Config.\n", __FILE__, __LINE__);
            retry++;
            sleep(10);
            if ( retry >  MAXRETRY ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) : FATAL : Max retry reached in readCVRStatsConfig exit process %d\n", __FILE__, __LINE__);
                exit(1);
            }
        } else {
            break;
        }
    }

    memset(buff, 0, sizeof(buff));
    strcpy(buff, cvrstatConfig->enabled);
    if(!strlen(buff)) {
	    statConfig.isEnabled = 0;
    } else {
    	statConfig.isEnabled = atoi(buff);
    }

    memset(buff, 0, sizeof(buff));
    strcpy(buff, cvrstatConfig->interval);
    memset(statuploadInterval, 0, sizeof(statuploadInterval));
    strcpy(statuploadInterval, cvrstatConfig->interval);

    if(!strlen(buff)) {
	    statConfig.interval = 0;
    } else {
        statConfig.interval = parseISO8601Duration(buff);
    }

    memset(buff, 0, sizeof(buff));
    strcpy(buff, cvrstatConfig->failurePercent);
    if(!strlen(buff)) {
	    statConfig.failurePercent = 0;
    } else {
    	statConfig.failurePercent = atoi(buff);
    }

    strcpy(statConfig.url, cvrstatConfig->url);
    strcpy(statConfig.auth_token, cvrstatConfig->auth_token);

    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): isEnabled: %d.\n", __FILE__, __LINE__, statConfig.isEnabled);
    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): interval: %d.\n", __FILE__, __LINE__, statConfig.interval);
    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): stat interval: %s.\n", __FILE__, __LINE__, statuploadInterval);
    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): failurePercent: %d.\n", __FILE__, __LINE__, statConfig.failurePercent);
    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): url: %s.\n", __FILE__, __LINE__, statConfig.url);
    RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): auth_token: %s.\n", __FILE__, __LINE__, statConfig.auth_token);
	
    if (cvrstatConfig) {
        free(cvrstatConfig);
        cvrstatConfig = NULL;
    }

    return RDKC_SUCCESS;
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
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): can't create kvs upload thread\n", __FILE__, __LINE__ );
        return RDKC_FAILURE;
    }

    pthread_setname_np(kvsUploadThread,"kvsUploadthread");
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): kvs upload thread created\n", __FILE__, __LINE__ );

    sleep(2);        
    pthread_t cvrstatThread;
    cvrStatisticsUploadThreadExit = false;
    if( RDKC_SUCCESS != pthread_create(&cvrstatThread, NULL, cvrStatisticsUpload, NULL) ) {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): can't create cvr stats thread\n", __FILE__, __LINE__ );
        return RDKC_FAILURE;
    }

    pthread_setname_np(cvrstatThread,"cvrstatthread");
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): cvr stats thread created\n", __FILE__, __LINE__ );

    if (0 == access(CVRSTATS_CONFIG_FILE, F_OK)) {
        updateStatConfiguration();
    }
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
    cvrStatisticsUploadThreadExit = true;
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Closing the kvs thread connection\n", __FILE__, __LINE__);
}
