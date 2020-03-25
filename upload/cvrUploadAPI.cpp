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
#include "rdk_debug.h"
#include "cvrupload.h"
#include "cvrUploadAPI.h"

#define MAXSIZE			512
#define CVR_THRESHHOLD_COUNT 4

typedef struct cvr_upload_params_s
{
        char fpath[MAXSIZE];		/* clip path */
        char starttime[MAXSIZE];	/* clip start time */
        char endtime[MAXSIZE];		/* clip end time */
        int event_type;			/* event name */
        unsigned int event_datetime;		/* event date and time */
        char m_fpath[MAXSIZE];		/* motion level file path */
        int motion_level_idx;		/* motion level index */
	char str_od_data[MAXSIZE]; 		/* od_frame_data */
	char va_engine_version[10];
	bool smartTnEnabled;
} cvr_upload_params_t;

static std::queue<cvr_upload_params_t> msgQ;
static bool cvrUploadThreadExit = true;
static std::mutex msgQueueMutex;
static std::condition_variable msgCv;
static CVRUpload *cvrUpload = NULL;
static cvr_upload_params_t param;

/**
 * @description: This function is used to call the function which upload data to the http server.
 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
 * @return: Error code.
 */
static int upload_cvr_clips(char *fpath, char *stime, char *etime, int eventType, unsigned int eventDatetime, char *m_fpath , int motion_level_idx, char* str_od_data, char* va_engine_version, bool smartTnEnabled)
{
        if (NULL == cvrUpload) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): cvrUpload is NULL.\n", __FILE__, __LINE__);
                return RDKC_FAILURE;
	}

        int ret = cvrUpload->doCVRUpload(fpath, stime, etime, eventType, eventDatetime, m_fpath, motion_level_idx, str_od_data, va_engine_version, smartTnEnabled);
	if(ret == CVR_UPLOAD_OK) {
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Data upload successful\n", __FILE__, __LINE__);
		return RDKC_SUCCESS;
	} else if (ret == CVR_UPLOAD_CURL_ERR) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Data upload failed due to curl error code\n", __FILE__, __LINE__);
		return ret;
	} else {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Data upload failed\n", __FILE__, __LINE__);
		return RDKC_FAILURE;
        }
}

/** @descripion: Push the msg into queue
 *  @param[in] msg: message to push
 *  @return: void
 */
static void PushMsg(cvr_upload_params_t msg)
{
        std::unique_lock<std::mutex> lock(msgQueueMutex);
        msgQ.push(msg);
        lock.unlock();

        msgCv.notify_one();
}

/** @descripion: pull the message from the queue
 *  @param[out] msg: msg received from queue
 *  @return: void
 */
static void PopMsg(cvr_upload_params_t *msg)
{
        std::unique_lock<std::mutex> lock(msgQueueMutex);
        msgCv.wait(lock,[]{return !msgQ.empty();});

	*msg = msgQ.front();
        msgQ.pop();

        lock.unlock();
}

/** @descripion: cvr upload thread
 *  @param[in] args: input arguments if any
 *  @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
static void * CvrUploadThread(void* args)
{
        cvr_upload_params_t upload_params;
	memset(&upload_params, 0, sizeof(cvr_upload_params_t));
        pid_t pid = 0;

        while(!cvrUploadThreadExit) {
		int queue_size = msgQ.size();
                static int uploadfailcount=0;
                int ret = RDKC_SUCCESS;
		if (queue_size > CVR_THRESHHOLD_COUNT) {
		  for (int i =0; i < (queue_size -1); i++ ) { 
		     memset(&upload_params, 0, sizeof(cvr_upload_params_t));
		     PopMsg(&upload_params);
		     RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Deleting %s and %s\n", __FUNCTION__, __LINE__, upload_params.fpath, upload_params.m_fpath);
		     unlink(upload_params.fpath);
		     unlink(upload_params.m_fpath);
		  }
		}

                memset(&upload_params, 0, sizeof(cvr_upload_params_t));
                PopMsg(&upload_params);

                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Data to server : %s, %s, %s, %d, %lu, %s, %d\n", __FUNCTION__, __LINE__, upload_params.fpath, upload_params.starttime, upload_params.endtime, upload_params.event_type, upload_params.event_datetime, upload_params.m_fpath, upload_params.motion_level_idx);

                //Uploading  data to the server.
                ret = upload_cvr_clips(upload_params.fpath, upload_params.starttime, upload_params.endtime, upload_params.event_type, upload_params.event_datetime, upload_params.m_fpath, upload_params.motion_level_idx, upload_params.str_od_data, upload_params.va_engine_version, upload_params.smartTnEnabled);
                if (RDKC_SUCCESS == ret) {
			if( upload_params.event_type == EVENT_TYPE_MOTION) {
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Upload Successful with Motion\n", __FUNCTION__, __LINE__);
			}
			else {
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Upload Successful without Motion\n", __FUNCTION__, __LINE__);
			}
                        uploadfailcount = 0; //reset upon success
                }
                else {
                        //dns resolve error
                        if(ret == CVR_UPLOAD_CURL_ERR) {
                                uploadfailcount++;
                                if( CVR_THRESHHOLD_COUNT == uploadfailcount ) {
                                        uploadfailcount = 0;
                                        pid = getpid();
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Threshold limit reached in upload failure : %d : sending sigterm to Thread id : %d : \n", __FUNCTION__, __LINE__,uploadfailcount,pid);
                                        kill(pid, SIGTERM);
                                }
                        }
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error Uploading the File\n", __FUNCTION__, __LINE__);
                }
        }
}

/**
 * @description: This function is used to push the data into queue.
 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
 * @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
int cvr_upload(char* fpath, char* starttime, char* endtime, int event_type, unsigned int event_datetime, char* m_fpath, int motion_level_idx, char* str_od_data , char* va_engine_version, bool smartTnEnabled)
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
	PushMsg(param);

	return RDKC_SUCCESS;
}

/** @descripion: initialize cvr upload
 *  @param: void.
 *  @return: RDKC_SUCCESS on success, RDKC_FAILURE on failure.
 */
int cvr_upload_init()
{
	/* ENABLING RDK LOGGER */
        rdk_logger_init("/etc/debug.ini");

        pthread_t cvrUploadThread;

        /* Initialising the Connection */
	cvrUpload = CVRUpload::getCVRUploadInstance();
        if (NULL == cvrUpload) {
                cvr_upload_close();
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Conncetion Closed\n", __FILE__, __LINE__);
                return RDKC_FAILURE;
        }

        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Connection Initialised Successfully.\n", __FILE__, __LINE__);

	/* Create cvr upload thread */
        cvrUploadThreadExit = false;
        if( RDKC_SUCCESS != pthread_create(&cvrUploadThread, NULL, CvrUploadThread, NULL) ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): can't create thread", __FILE__, __LINE__ );
                return RDKC_FAILURE;
        }
        pthread_setname_np(cvrUploadThread,"cvr_upload");
        return RDKC_SUCCESS;

}

/** @descripion: close cvr upload
 *  @param: void.
 *  @return: void
 */
void cvr_upload_close()
{
	//Closing the connection
	cvrUploadThreadExit = true;
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Closing the connection\n", __FILE__, __LINE__);
	if (NULL != cvrUpload) {
		cvrUpload->deleteCVRUplaodInstance();
	}
}

