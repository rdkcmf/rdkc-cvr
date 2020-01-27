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
#ifndef _CVR_UPLOAD_H_
#define _CVR_UPLOAD_H_

#include "conf_sec.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __cplusplus
#include "HttpClient.h"
extern "C" {
#endif
#include "dev_config.h"
#include "sc_md5.h"
#include "sc_tool.h"
#include "SYS_log.h"
#include "system_config.h"
#include "lan_config.h"
#include "base64ed.h"
#include "reset_to_default.h"
#include "sc_debug.h"
#include "rdk_debug.h"
#ifdef USE_MFRLIB
#include "mfrApi.h"
#endif

#ifndef XFINITY_SUPPORT
#include "rdkc_config_sections.h"
#endif

#include "rtConnection.h"
#include "rtLog.h"
#include "rtMessage.h"

#ifdef __cplusplus
}
#endif

#define CVR_USER_LEN                            128
#define CVR_URL_LEN                             526
#define CVR_UPLOAD_TIMEOUT_INTERVAL             20
#define CVR_UPLOAD_SEND_LEN                     2048
#define CVR_UPLOAD_AUTH_MAX_LENGTH       	1024

#define DEFAULT_MIN_INTERVAL 			1
#define DEFAULT_MAX_INTERVAL 			20
#define DEFAULT_RETRY_FACTOR 			2

#define DEFAULT_DNS_CACHE_TIMEOUT		60

#define CVR_UPLOAD_FILENAME_TAG         	"X-FileName"
#define CVR_UPLOAD_MOTION_LEVEL_PATH_TAG        "X-MotionLevel-Path"
#define CVR_UPLOAD_CAPTURE_TIME_TAG     	"X-Capture-Time"
#define CVR_UPLOAD_EVENT_TYPE_TAG               "X-EVENT-TYPE"
#define CVR_UPLOAD_EVENT_DATETIME_TAG  		"X-EVENT-DATETIME"
#define CVR_UPLOAD_CAPTURE_DURATION_TAG	 	"X-Capture-Duration"
#define CVR_UPLOAD_CAPTURE_END_TAG      	"X-Capture-End"
#define CVR_UPLOAD_MOTION_LEVEL_TAG             "X-MOTION-LEVELS"
#define CVR_UPLOAD_MOTION_LEVEL_VERSION_TAG     "X-MOTION-VERSION"
#define CVR_UPLOAD_MOTION_ROI			"X-MOTION-ROI"
#define CVR_UPLOAD_VA_VERSION			"X-XCV-Version"

#define CVR_UPLOAD_SMART_THUMBNAIL_TAG          "X-IMAGE-UPLOAD"
#define CVR_UPLOAD_SMART_THUMBNAIL_STR          "ENABLED"

#define RDKC_FAILURE     		-1
#define RDKC_SUCCESS		         0

#define SIZE     			 256

#define FW_NAME_MAX_LENGTH	256

typedef struct CVRServConfig
{
	int enable;
        char file_name[CVR_URL_LEN+1];
        char url[CVR_URL_LEN+1];
        char auth[CVR_UPLOAD_AUTH_MAX_LENGTH+1];
        char user[CVR_USER_LEN+1];
        char pass[CVR_USER_LEN+1];
        int timeout;
}CVRServConfig;

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

int getCameraImageName(char *out);

#ifdef __cplusplus
class CVRUpload{

public:

	int doCVRUpload(char *fpath, char *starttime, char *endtime, int event_type,unsigned int event_datetime, char *m_fpath , int motion_level_idx,char* str_od_data, char* va_engine_version, bool smartTnEnabled);
	static CVRUpload *getCVRUploadInstance();
	void deleteCVRUplaodInstance();
	bool isCVRInstanceInitialized();
	void initializeRetryAttribute();

private:

	CVRUpload();
	~CVRUpload();
	int readCVRServerConfig();
	int postDataToCVRServer(const char *data, long *response_code);
	int postFileToCVRServer(char *file_path, int start_upload_time, int file_len, long *response_code);
	int LOGInit();
	void getCloudRecorderRetryAttribute();
	int retryAtExpRate();
	void freeResources();
        static void cvrUploadSignalHandler(int signal);
        static void cvrUploadRegisterSignalHandler();
	void getDNSCacheTimeout();
	void stringifyEventDateTime(char* strEvtDateTime , size_t evtdatetimeSize, time_t evtDateTime);
	static void notify_smt_TN(CVR_UPLOAD_STATUS status, char* fname);
	static rtConnection connectionSend;
	static rtError err;

	static CVRUpload* cvrUpload;
	HttpClient* httpClient;
	CVRServConfig* cvr_serv_conf;
	static double waitingInterval;
        static int minInterval;
        static int maxInterval;
	static double retryFactor;
	bool  isInitialized;
	char* upload_file_name;
	char* m_level_path;
        static volatile sig_atomic_t term_flag;
	static long dnsCacheTimeout;
	static char fw_name[ FW_NAME_MAX_LENGTH ];
	static char mac_string[XFINITY_MAC_STRING_LEN + 1];
};
#endif
#endif
