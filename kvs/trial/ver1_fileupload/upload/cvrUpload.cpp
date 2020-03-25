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

#include "cvrupload.h"

CVRUpload *CVRUpload::cvrUpload = NULL;
volatile sig_atomic_t CVRUpload::term_flag = 0;
double CVRUpload::waitingInterval = DEFAULT_MIN_INTERVAL ;
int CVRUpload::minInterval = DEFAULT_MIN_INTERVAL ;
int CVRUpload::maxInterval = DEFAULT_MAX_INTERVAL ;
double CVRUpload::retryFactor = DEFAULT_RETRY_FACTOR ;
long CVRUpload::dnsCacheTimeout = DEFAULT_DNS_CACHE_TIMEOUT;
char CVRUpload::fw_name[FW_NAME_MAX_LENGTH] = "";
char CVRUpload::mac_string[XFINITY_MAC_STRING_LEN + 1] ="";

/**
 * @description: Invoking RDK Logger initialization function
 * @param: void
 * @return: Error Code
 */
int CVRUpload::LOGInit()
{
	/* RDK logger initialization */
	if (RDK_SUCCESS == rdk_logger_init("/etc/debug.ini"))
		return RDKC_SUCCESS;
	return RDKC_FAILURE;
}


CVRUpload::CVRUpload(): isInitialized(false), upload_file_name(NULL),m_level_path(NULL), cvr_serv_conf(NULL)
{

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): CvrUpload Instance Initialising ....!\n", __FILE__, __LINE__);

	LOGInit();  //Initializing RDK_LOGGER
	cvrUploadRegisterSignalHandler();  //Register signal handler

	initializeRetryAttribute();

	httpClient = new HttpClient();

	/* Get server configuration */
        if (RDKC_SUCCESS != readCVRServerConfig()) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to get config!\n", __FILE__, __LINE__);
        }
	else {
        	/* Open the URL */
        	if(NULL != httpClient) {
                	httpClient->open((char*)cvr_serv_conf->url, dnsCacheTimeout);
                	isInitialized = true;
        	}
        	else {
                	RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to open the URL\n", __FILE__, __LINE__);
        	}
	}

        int ret = getCameraImageName(fw_name);
        if (ret == RDKC_FAILURE)
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): ERROR in reading camera firmware name\n", __FILE__, __LINE__);

        unsigned char macaddr[MAC_ADDR_LEN];
        memset(macaddr, 0, sizeof(macaddr));

        if (0 == get_mac_address(macaddr)) {
        memset(mac_string, 0, sizeof(mac_string));
        transcode_mac_to_string_by_separator(macaddr, '\0', mac_string, XFINITY_MAC_STRING_LEN + 1, 0);
        }
        else {
          RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): ERROR in reading camera mac\n", __FILE__, __LINE__);
          strcpy(mac_string,"No MACADDR");
        }

}

CVRUpload::~CVRUpload()
{

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): CvrUpload Instance Deleted Successfully!\n", __FILE__, __LINE__);
	if(NULL != httpClient)
	{
		httpClient->close();
		delete httpClient;
		httpClient = NULL;
	}
}

/**
 * @description: This function is used to initialized the instance for CVRUpload.
 *
 * @param: void
 *
 * @return: cvrupload instance.
 */
CVRUpload *CVRUpload::getCVRUploadInstance()
{

	if (NULL == cvrUpload)
		cvrUpload = new CVRUpload();
	return cvrUpload;
}
/**
 * @description: This function is used to check if instance of CVRUpload is initialized .
 *
 * @param: void
 *
 * @return: True/False.
 */

bool  CVRUpload::isCVRInstanceInitialized()
{

	return isInitialized;
}

/**
 * @description: This function is used to delete cvrUpload Instance.
 *
 * @param: void
 *
 * @return: void.
 */
void CVRUpload::deleteCVRUplaodInstance()
{

	if(NULL != cvrUpload)
	{
		delete cvrUpload;
		cvrUpload = NULL;
	}
}


/**
 * @description: This function is used to free memory.
 *
 * @param: void
 *
 * @return: void.
 */
void CVRUpload::freeResources()
{
	if(NULL != upload_file_name)
	{
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Remove file [%s] from camera ram.\n", __FILE__, __LINE__, upload_file_name);
		unlink(upload_file_name);               // Remove the file at the end
		free(upload_file_name);
		upload_file_name = NULL;
	}
	if(NULL !=  m_level_path)
	{
		unlink(m_level_path);
		free(m_level_path);
		m_level_path=NULL;
	}
	if(NULL != cvr_serv_conf)
	{
		free(cvr_serv_conf);
		cvr_serv_conf=NULL;
	}
}

/**
 * @description: CVR Upload Signal handler.
 *
 * @param[in]: Signal
 *
 * @return: void
 */
void CVRUpload::cvrUploadSignalHandler(int signal)
{
        if (SIGINT == signal) {
                term_flag = 1;
        }
        else if (SIGPIPE == signal) {
        }
        else if (SIGALRM == signal) {
        }
}

/**
 * @description: Register the CVR Upload Signal handler.
 *
 * @param: void
 *
 * @return: void
 */
void CVRUpload::cvrUploadRegisterSignalHandler()
{
        signal(SIGINT, cvrUploadSignalHandler);
        // connection broken
        signal(SIGPIPE, cvrUploadSignalHandler);
}

/**
 * @description: This function is used to initailze server config.
 *
 * @param: void
 *
 * @return: Error code.
 */
int CVRUpload::readCVRServerConfig()
{
#ifdef XFINITY_SUPPORT
	CloudRecorderConf CloudRecorderInfo;

        // Read cloud recorder server info
        memset(&CloudRecorderInfo, 0, sizeof(CloudRecorderConf));

	if (ReadCloudRecorderInfo(&CloudRecorderInfo))
#else
        RdkCCloudRecorderConf CloudRecorderInfo;

	// Read cloud recorder server info
        memset(&CloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
        if (RdkCReadCloudRecorderSection (&CloudRecorderInfo) == RDKC_FAILURE)
#endif
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Read cloud recorder configuration error!\n", __FILE__, __LINE__);
                return  RDKC_FAILURE;
        }

        cvr_serv_conf = (struct CVRServConfig*)malloc(sizeof(struct CVRServConfig));
        memset(cvr_serv_conf, 0, sizeof(struct CVRServConfig));

        cvr_serv_conf->enable = CloudRecorderInfo.enable;
        strcpy((char*)cvr_serv_conf->url, CloudRecorderInfo.video_address);
        strcpy((char*)cvr_serv_conf->auth, CloudRecorderInfo.video_auth);
        strcpy((char*)cvr_serv_conf->user, CloudRecorderInfo.video_username);
        strcpy((char*)cvr_serv_conf->pass, CloudRecorderInfo.video_pass);
        cvr_serv_conf->timeout = CloudRecorderInfo.timeout;

        return RDKC_SUCCESS;
}

/**
 * @description This function is used to set DNS cache timeout for httpclient
 *
 * @param: void.
 *
 * @return: void.
 */
void CVRUpload::getDNSCacheTimeout()
{
        const char* cacheTimeout = rdkc_envGet(DNS_CACHE_TIMEOUT);

	if(NULL != cacheTimeout) {
                dnsCacheTimeout = atol(cacheTimeout);
                dnsCacheTimeout = (0 == dnsCacheTimeout) ? DEFAULT_DNS_CACHE_TIMEOUT : dnsCacheTimeout;
        }

        RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): DNS chache timeout: %ld ", __FILE__, __LINE__, dnsCacheTimeout);
}

/**
 * @description This function is used to get retry attribute.
 *
 * @param: void.
 *
 * @return: void.
 */
void CVRUpload::getCloudRecorderRetryAttribute()
{
	const char* waitingInt = rdkc_envGet(CVR_UPLOAD_RETRY_MIN_INTERVAL);
	const char* minInt = rdkc_envGet(CVR_UPLOAD_RETRY_MIN_INTERVAL);
	const char* maxInt = rdkc_envGet(CVR_UPLOAD_RETRY_MAX_INTERVAL);
	const char* factor = rdkc_envGet(CVR_UPLOAD_RETRY_FACTOR);

	if(NULL != waitingInt)
	{
		waitingInterval = atof(waitingInt);
		waitingInterval = (0 == waitingInterval) ? DEFAULT_MIN_INTERVAL : waitingInterval;
	}
	if(NULL != minInt)
	{
		minInterval = atoi(minInt);
		minInterval = (0 == minInterval) ? DEFAULT_MIN_INTERVAL : minInterval;
	}
	if(NULL != maxInt)
	{
		maxInterval= atoi(maxInt);
		maxInterval = (0 == maxInterval) ? DEFAULT_MAX_INTERVAL : maxInterval;
	}
	if(NULL != factor)
	{
		retryFactor = atof(factor);
		retryFactor = (0 == retryFactor) ? DEFAULT_RETRY_FACTOR : retryFactor;
	}
	if(maxInterval < minInterval)
	{
		maxInterval = DEFAULT_MAX_INTERVAL;
		minInterval = DEFAULT_MIN_INTERVAL;
	}

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s: %d: Retry min interval=%d, max interval=%d, retry factor=%.2f!\n", __FILE__, __LINE__,minInterval, maxInterval, retryFactor);

}

/**
 * @description: This function is used to initialize the retry attribute.
 *
 * @param: void.
 *
 * @return: void.
 */
void CVRUpload::initializeRetryAttribute()
{
	getCloudRecorderRetryAttribute();
	getDNSCacheTimeout();
}
/**
 * @description: This function is used to sleep for some waitingInterval  before next retry.
 *
 * @param[in]: start_upload_time.
 *
 * @return: Error code.
 */
int CVRUpload::retryAtExpRate()
{
	int ret = RDKC_FAILURE;

	if(waitingInterval <= maxInterval)
	{
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s: %d: Waiting for %d seconds!\n", __FILE__, __LINE__, (int)waitingInterval);
		sleep((int)waitingInterval);
		waitingInterval *= retryFactor;
		ret = RDKC_SUCCESS;
	}

	return ret;
}

/**
 * @description: This function is used to post the file to the http server.
 *
 * @param[in]: Path of the file to be uploaded, upload start time, object of cvrUpload utility
 *             HTTP Server URL.
 *
 * @return: Error code.
 */
int CVRUpload::postFileToCVRServer(char *file_path, int start_upload_time, int file_len, long *response_code)
{

	int ret = CVR_UPLOAD_OK;
	int file_fd = 0;
	int read_len = 0;
	time_t current_time = 0;
	char read_buf[CVR_UPLOAD_SEND_LEN];
	char *data=NULL;
	char *ptr = NULL;

	if (NULL == file_path)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Invalid file name!\n", __FILE__, __LINE__);
		return CVR_UPLOAD_FAIL;
	}

	file_fd = open(file_path, O_RDONLY);
	if (file_fd <= 0)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to Open File :%s !\n", __FILE__, __LINE__, file_path);
		return CVR_UPLOAD_FAIL;
	}

	data =(char*)malloc(file_len*sizeof(char));
	memset(data,0,file_len);
	ptr=data;

	while((read_len = read(file_fd, read_buf, sizeof(read_buf))) > 0)
	{
		memcpy(ptr, read_buf, read_len);
		ptr += read_len;
		//RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Data read:%s, read_len = %d !\n", __FILE__, __LINE__, read_buf, read_len);
		memset(read_buf,0,CVR_UPLOAD_SEND_LEN);
	}

	current_time = sc_linear_time(NULL);
	if (CVR_UPLOAD_TIMEOUT_INTERVAL + start_upload_time <= current_time)
	{
		ret = CVR_UPLOAD_TIMEOUT;
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s: %d: Send data to server failed!\n", __FILE__, __LINE__);
		free(data);
		data = NULL;
		close(file_fd);
		return ret;
	}
        int remainingTime = CVR_UPLOAD_TIMEOUT_INTERVAL - (current_time - start_upload_time);
	//RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Data to send :%s !\n", __FILE__, __LINE__, data);
	int curlCode = httpClient->post_binary((char*)cvr_serv_conf->url,(char*)data, response_code, file_len,remainingTime);

        if(*response_code == RDKC_HTTP_RESPONSE_FORBIDDEN)
        {
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d) Data post Failed Response Code : %ld\n",__FILE__, __LINE__,*response_code);
                ret = CVR_UPLOAD_RESPONSE_ERROR;
        }
	else if((*response_code >= RDKC_HTTP_RESPONSE_OK) && (*response_code < RDKC_HTTP_RESPONSE_REDIRECT_START))
	{
		RDK_LOG(  RDK_LOG_DEBUG ,"LOG.RDK.CVRUPLOAD","%s(%d): Data post Successfully, Response Code : %ld\n", __FILE__, __LINE__,*response_code);
		ret = CVR_UPLOAD_OK;
	}
	else
	{
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Data post Failed. Response code = %ld and  curlCode = %d\n", __FILE__, __LINE__, *response_code,curlCode);
		*response_code= curlCode;
		ret = CVR_UPLOAD_FAIL;
	}
	free(data);
	data = NULL;
	close(file_fd);

	return ret;

}

/**
 * @description: Convert event date and time to ISO 8601 format.
 *
 * @param[in]: strEvtDateTime,evtdatetimeSize, evtDateTime.
 *
 * @return: void
 */
void  CVRUpload::stringifyEventDateTime(char* strEvtDateTime , size_t evtdatetimeSize, time_t evtDateTime)
{
	struct tm *tv = NULL;

	if(NULL == strEvtDateTime) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Using invalid memory location!\n",__FILE__, __LINE__);
		return;
	}

	tv = gmtime(&evtDateTime);

	strftime(strEvtDateTime, evtdatetimeSize,"%FT%TZ",tv);
}


/**
 * @description: This function is used to call the function which upload data to the http server.
 *
 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
 *
 * @return: Error code.
 */
int CVRUpload::doCVRUpload(char *fpath, char *stime, char *etime, int eventType,unsigned int eventDatetime, char *m_fpath , int motion_level_idx, char* str_od_data, char* va_engine_version, bool smartTnEnabled)
{

	int ret = CVR_UPLOAD_OK;
	int file_len = 0;
	struct stat file_stat;
	int retry = 0;
	char *p_file_name = NULL;
	char auth[CVR_URL_LEN+1];
	char base64_auth[CVR_URL_LEN+1];
	char pack_head[CVR_UPLOAD_SEND_LEN+1];
	int event_type = -1;
	time_t start_upload_time = 0;
	time_t current_time = 0;
	struct timeval tv;
	char date_string[128+1];
	char event_datetime_string[128+1];//record the time that event happend.
	char request_uuid[XFINITY_REQUEST_UUID_LEN];
	char fw_ver[XFINITY_FW_VERSION_LEN];
	//unsigned char macaddr[MAC_ADDR_LEN];
	//char mac_string[XFINITY_MAC_STRING_LEN+1];
	char starttime[200];
	char endtime[200];
	long duration = 0;
	struct timeval end_tv;
	int timeout = CVR_UPLOAD_TIMEOUT_INTERVAL;
	char base64_m_level_data[CVR_URL_LEN+1];
	char m_level_data[CVR_URL_LEN+1];
	char x_motion_levels[2*CVR_URL_LEN];
	char* m_level_ptr = NULL;
	unsigned int length_m_level_data = 0;
	int m_fd = -1;
	unsigned int count_read = 0;
	long response_code = 0;
	int motion_level_counter = 0;
	#ifdef USE_MFRLIB
        mfrSerializedData_t stdata = {NULL, 0, NULL};
        mfrSerializedType_t stdatatype = mfrSERIALIZED_TYPE_MODELNAME;
	#endif
	// get upload file name
	upload_file_name= (char*)malloc(SIZE);
	snprintf(upload_file_name, strlen(fpath)+1, "%s", fpath);
	m_level_path = (char*)malloc(SIZE);
	snprintf(m_level_path, strlen(m_fpath)+1, "%s", m_fpath);

	/* get server configuration */
	if (RDKC_SUCCESS != readCVRServerConfig())
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to get config!\n", __FILE__, __LINE__);
		ret = CVR_UPLOAD_FAIL;
		freeResources();
		return ret;
	}

	//Check whether cvr conf is enabled
	if (0 == cvr_serv_conf->enable)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): CVR is disable!\n", __FILE__, __LINE__);
		ret = CVR_UPLOAD_FAIL;
		freeResources();
		return ret;
	}

	// As customer's requirement, not support http
	if (!strncasecmp(cvr_serv_conf->url,"http://", strlen("http://")))
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Invalid upload url [%s], not support http!\n",__FILE__, __LINE__, cvr_serv_conf->url);
		ret = CVR_UPLOAD_FAIL;
		freeResources();
		return ret;
	}

	// get file path
	snprintf(cvr_serv_conf->file_name, sizeof(cvr_serv_conf->file_name), "%s", upload_file_name);
	if (stat(cvr_serv_conf->file_name, &file_stat) < 0)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): invalid file [%s], errmsg=%s!\n",
				__FILE__, __LINE__, upload_file_name, strerror(errno));
		ret = CVR_UPLOAD_FAIL;
		freeResources();
		return ret;
	}
	file_len = file_stat.st_size;

	// set event type
	event_type = eventType;

	if (cvr_serv_conf->timeout > 0)
	{
		timeout = cvr_serv_conf->timeout;
	}

	start_upload_time = sc_linear_time(NULL);

        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s: Post file %s to %s, len=%d!\n", __FILE__, cvr_serv_conf->file_name, cvr_serv_conf->url, file_len);
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s: Posting cvr clip with size:%d\n", __FILE__,file_len);

	while (!term_flag)
	{
		// Start time
		current_time = sc_linear_time(NULL);
		if (timeout + start_upload_time <= current_time)
		{
			ret = CVR_UPLOAD_TIMEOUT;
			break;
		}

		//Reser httpClient Header List.
		httpClient->resetHeaderList();
		httpClient->addHeader( "Expect", "");   //removing expect header condition by explicitly setting Expect header to ""

		//httpClient->addHeader( "Host", hostname);
		memset(pack_head, 0, sizeof(pack_head));
		snprintf(pack_head, sizeof(pack_head), "video/%s",(strstr(cvr_serv_conf->file_name, ".ts") != NULL)?"mp2t":"mp4");
		httpClient->addHeader( "Content-Type", pack_head);
		memset(pack_head, 0, sizeof(pack_head));
		snprintf(pack_head, sizeof(pack_head), "%d",file_len);
		httpClient->addHeader( "Content-Length", pack_head);
		if (NULL != (p_file_name = strrchr(cvr_serv_conf->file_name, '/')))
		{
			p_file_name++;
		}
		else
		{
			p_file_name = cvr_serv_conf->file_name;
		}

		httpClient->addHeader(CVR_UPLOAD_FILENAME_TAG, p_file_name);

		// Add tag X-Capture-Time
		memset(starttime, 0, sizeof(starttime));
		snprintf(starttime, sizeof(starttime), "%s", stime);
		if (sscanf(starttime, "%d.%d", &tv.tv_sec, &tv.tv_usec) == 2)
		{
			tv.tv_usec = tv.tv_usec/1000; //convert to micro seconds
			get_time_special_string_in(date_string, sizeof(date_string), &tv, NULL, TIME_FORMAT_ISO8601, 0);
			httpClient->addHeader(CVR_UPLOAD_CAPTURE_TIME_TAG, date_string);
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s is [%s].\n", __FILE__, __LINE__, CVR_UPLOAD_CAPTURE_TIME_TAG, date_string);
		}

		// Add tag X-Capture-End
		memset(endtime, 0, sizeof(endtime));
		snprintf(endtime, sizeof(endtime), "%s", etime);
		if (sscanf(endtime, "%d.%d", &end_tv.tv_sec, &end_tv.tv_usec) == 2)
		{
			end_tv.tv_usec = end_tv.tv_usec/1000; // covert to micro seconds
			get_time_special_string_in(date_string, sizeof(date_string), &end_tv, NULL, TIME_FORMAT_ISO8601, 0);
			httpClient->addHeader(CVR_UPLOAD_CAPTURE_END_TAG, date_string);
			 RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s is [%s].\n", __FILE__, __LINE__, CVR_UPLOAD_CAPTURE_END_TAG, date_string);
		}

		// Add tag X-Capture-Duration
		duration = 1000000*(end_tv.tv_sec - tv.tv_sec) + end_tv.tv_usec - tv.tv_usec;
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): end_tv.tv_sec %d end_tv.tv_usec %d tv.tv_sec %d tv.tv_usec %d duration %d \n", __FILE__, __LINE__, end_tv.tv_sec, end_tv.tv_usec, tv.tv_sec, tv.tv_usec, duration);
		memset(pack_head, 0, sizeof(pack_head));
		snprintf(pack_head, sizeof(pack_head), "PT%.3lfS",(double)duration/1000000);
		httpClient->addHeader( CVR_UPLOAD_CAPTURE_DURATION_TAG, pack_head);
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s is [%.3lf].\n", __FILE__, __LINE__, CVR_UPLOAD_CAPTURE_DURATION_TAG, (double)duration/1000000);

		// Add tag X-Retry
		if(retry > 0)
		{
			memset(pack_head, 0, sizeof(pack_head));
			snprintf(pack_head, sizeof(pack_head), "%d",retry);
			httpClient->addHeader(XFINITY_RETRY, pack_head);
		}

		if (strlen(cvr_serv_conf->auth) != 0)
		{
			httpClient->addHeader( "Authorization", cvr_serv_conf->auth);
		}
		else if (strlen(cvr_serv_conf->user) != 0 ||  strlen(cvr_serv_conf->pass) != 0)
		{
			memset(auth, 0, sizeof(auth));
			memset(base64_auth, 0, sizeof(base64_auth));
			snprintf(auth,sizeof(auth), "%s:%s", cvr_serv_conf->user, cvr_serv_conf->pass);
			ret = base64encode(auth, strlen(auth), base64_auth, sizeof(base64_auth), NULL);
			if (ret < 0)
			{
				ret = 0;
			}
			base64_auth[ret] = 0;
			if (strlen(base64_auth))
			{
				memset(pack_head, 0, sizeof(pack_head));
				snprintf(pack_head, sizeof(pack_head), "Basic %s", base64_auth);
				httpClient->addHeader( "Authorization", pack_head);
			}
		}
		//memset(fw_ver, 0, sizeof(fw_ver));

		memset(pack_head, 0, sizeof(pack_head));
#ifdef USE_MFRLIB
		char modelName[128];
                if(mfrGetSerializedData(stdatatype, &stdata) == mfrERR_NONE)
                {
                        strncpy(modelName,stdata.buf,stdata.bufLen);
			modelName[stdata.bufLen] = '\0';
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): Model Name = %s\n", __FILE__, __LINE__,modelName);


                        if (stdata.freeBuf != NULL)
                        {
                                stdata.freeBuf(stdata.buf);
                                stdata.buf = NULL;
                        }
                }
                else
                {
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): GET ModelName failed.\n", __FILE__, __LINE__);

                }
                snprintf(pack_head, sizeof(pack_head), "Sercomm %s %s %s", modelName, fw_ver, mac_string);

#else
		snprintf(pack_head, sizeof(pack_head), "Sercomm %s %s %s", SC_MODEL_NAME, fw_name, mac_string);
#endif
		httpClient->addHeader( "User-Agent", pack_head);

		//tv.tv_sec = eventDatetime;
		//tv.tv_usec = 0;
		//get_time_special_string_in(event_datetime_string, sizeof(event_datetime_string), &tv, NULL, TIME_FORMAT_ISO8601, 0);
		stringifyEventDateTime(event_datetime_string, sizeof(event_datetime_string), eventDatetime);
		switch (event_type)
		{
			case EVENT_TYPE_MOTION:
				httpClient->addHeader(CVR_UPLOAD_EVENT_TYPE_TAG, XFINITY_EVENT_STR_MOTION);
				httpClient->addHeader(CVR_UPLOAD_EVENT_DATETIME_TAG,event_datetime_string);
				if( smartTnEnabled) {
					httpClient->addHeader(CVR_UPLOAD_SMART_THUMBNAIL_TAG,CVR_UPLOAD_SMART_THUMBNAIL_STR);
				}

				if(retry == 0)
                                {
                                   RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s header is set. %s=%s\n",XFINITY_EVENT_STR_MOTION,CVR_UPLOAD_EVENT_DATETIME_TAG,event_datetime_string);
				   if(smartTnEnabled) {
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","Smart Thumbnail header is set. %s=%s\n",CVR_UPLOAD_SMART_THUMBNAIL_TAG,CVR_UPLOAD_SMART_THUMBNAIL_STR);
				   } else {

					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","Smart thumbnail is not running or not enabled");
				   }
                                }

				break;
#ifdef _SUPPORT_OBJECT_DETECTION_
			case EVENT_TYPE_PEOPLE:
				httpClient->addHeader(CVR_UPLOAD_EVENT_TYPE_TAG, XFINITY_EVENT_STR_HUMAN);
				httpClient->addHeader(CVR_UPLOAD_EVENT_DATETIME_TAG,event_datetime_string);

				if(retry == 0)
                                {
                                   RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s header is set. %s=%s\n",XFINITY_EVENT_STR_MOTION,CVR_UPLOAD_EVENT_DATETIME_TAG,event_datetime_string);
                                }

				break;
#endif
#ifdef _SUPPORT_TAMPER_DETECTION_
			case EVENT_TYPE_TAMPER:
				httpClient->addHeader(CVR_UPLOAD_EVENT_TYPE_TAG, XFINITY_EVENT_STR_TAMPER);
				httpClient->addHeader(CVR_UPLOAD_EVENT_DATETIME_TAG,event_datetime_string);
				break;
#endif
			default:
				break;
		}
		memset(request_uuid, 0, sizeof(request_uuid));
		if (0 == GenarateXfinityRequestUUID(request_uuid, sizeof(request_uuid), XFINITY_REQUEST_VIDEO_UPLOAD))
		{
			httpClient->addHeader(XFINITY_REQUEST_UUID, request_uuid);
		}
		//Add X-MOTION-LEVELE
		length_m_level_data = motion_level_idx; //get motion statistics info length
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): m_level_path = %s, m_level_data_length = %d\n", __FILE__, __LINE__,m_level_path,length_m_level_data);
		memset(m_level_data,0,sizeof(m_level_data));
		memset(base64_m_level_data,0,sizeof(base64_m_level_data));
		memset(x_motion_levels,0,sizeof(x_motion_levels));
		if ((length_m_level_data > 0) && (!access(m_level_path,F_OK)))
		{
			m_fd = open(m_level_path,O_RDONLY);
			if (-1 != m_fd)
			{
				count_read = read(m_fd,m_level_data,length_m_level_data);
				close(m_fd);

				/* Format m_level_data with space between motion levels for each second */
				m_level_ptr = x_motion_levels;
				for(motion_level_counter = 1; motion_level_counter<length_m_level_data; motion_level_counter++) {
					m_level_ptr += sprintf(m_level_ptr,"%u ",m_level_data[motion_level_counter]);
				}

				ret = base64encode(m_level_data, count_read, base64_m_level_data, sizeof(base64_m_level_data), NULL);
				if ((length_m_level_data == count_read) && (ret > 0))
				{
					httpClient->addHeader( CVR_UPLOAD_MOTION_LEVEL_TAG, base64_m_level_data);
				}
			}
			else
			{
				RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVRUPLOAD","%s(%d): Open file %s ERROR!!!\n", __FILE__, __LINE__,m_level_path);
			}
		}

		if(strlen(va_engine_version) != 0) {
			if(retry == 0) {
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): %s header is set to: %s\n",__FILE__,__LINE__,CVR_UPLOAD_VA_VERSION,va_engine_version);
			}
			httpClient->addHeader(CVR_UPLOAD_VA_VERSION, va_engine_version);
		}
		if(strlen(str_od_data) != 0) {
			if(retry == 0) {
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): %s header is set to: %s\n",__FILE__,__LINE__,CVR_UPLOAD_MOTION_ROI,str_od_data);
			}
			httpClient->addHeader(CVR_UPLOAD_MOTION_ROI, str_od_data);
		}

		httpClient->addHeader( "Connection" ,"close");

		current_time = sc_linear_time(NULL);
		if (CVR_UPLOAD_TIMEOUT_INTERVAL + start_upload_time <= current_time)
		{
			ret = CVR_UPLOAD_TIMEOUT;
			break;
		}

		if(retry == 0)
		{
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s : %u \n ", __FILE__, __LINE__, CVR_UPLOAD_MOTION_LEVEL_VERSION_TAG, m_level_data[0]);
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): %s : %s \n", __FILE__, __LINE__, CVR_UPLOAD_MOTION_LEVEL_TAG, x_motion_levels);
		}

		/* Send file to server */
		ret = postFileToCVRServer(cvr_serv_conf->file_name, start_upload_time, file_len, &response_code);

		if(RDKC_HTTP_RESPONSE_FORBIDDEN == response_code)
                {
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d) Failed to send file to server  with error 403 forbidden\n",__FILE__, __LINE__);
                        break;
                }

		if (CVR_UPLOAD_TIMEOUT == ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to send file to server,Timeout\n", __FILE__, __LINE__);
			break;
		}
		else if (CVR_UPLOAD_FAIL == ret)
		{
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to send file to server,Response code=%ld\n",__FILE__, __LINE__, response_code);

			current_time = sc_linear_time(NULL);
			if (waitingInterval > ((CVR_UPLOAD_TIMEOUT_INTERVAL + start_upload_time ) - current_time))
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to send file to server exceeded the time with Response code=%ld.\n",__FILE__, __LINE__,response_code);
				if ( ( 6 == response_code )  || (51 == response_code) || (60 == response_code) ) {
					ret = CVR_UPLOAD_CURL_ERR;
				} else {
					ret = CVR_UPLOAD_FAIL;
				}
				break;
			}

			//Sleep for waitingInterval before retrying or break if the limit exceeds
			if(RDKC_SUCCESS != retryAtExpRate())
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Failed to send file to server exceeded the max interval with Response code==%ld.\n",__FILE__, __LINE__,response_code);
				ret = CVR_UPLOAD_FAIL;
				break;
			}
			++retry;
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Got Response code==%ld.Retry no %d \n",__FILE__, __LINE__,response_code,retry);
			continue;
		}

		if(!ret)
		{
			current_time = sc_linear_time(NULL);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d) Data has been sent %s and cvr time:%d secs\n",__FILE__, __LINE__, cvr_serv_conf->file_name, (current_time - start_upload_time)			);
			ret = CVR_UPLOAD_OK;
			break;
		}
	}

	waitingInterval = minInterval;
	freeResources();

	return ret;
}

/**
 * @description: This function is used to get the camera firmware version.
 *
 * @param[out]: firmware name
 *
 * @return: Error code.
 */
int getCameraImageName(char* out)
{
        size_t max_line_length = FW_NAME_MAX_LENGTH;
        char *file_buffer;
        char *locate_1 = NULL;
        FILE* fp;
        char* temp = out;
        fp = fopen("/version.txt","r");
        if(fp == NULL)
        {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in opening version.txt \n", __FILE__, __LINE__);
                return RDKC_FAILURE;
        }

        file_buffer = (char*)malloc(FW_NAME_MAX_LENGTH + 1);
        if(file_buffer == NULL)
        {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Error in malloc \n", __FILE__, __LINE__);
                fclose(fp);
                return RDKC_FAILURE;
        }
        while(getline(&file_buffer,&max_line_length,fp) != -1)
        {
                /* find the imagename string */
                locate_1 = strstr(file_buffer,"imagename");
                if(locate_1)
                {
                        locate_1 += strlen("imagename:");
                        /* copy the contents till linefeed */
                        while(*locate_1 != '\n')
                                *out++ = *locate_1++;
                        free(file_buffer);
			file_buffer = NULL;
                        fclose(fp);
                        return RDKC_SUCCESS;
                }
        }
        /* unable to get the image name */
        strcpy(out,"imagename entry not found");
        free(file_buffer);
	file_buffer = NULL;
        fclose(fp);
        return RDKC_SUCCESS;
}

#if 0
/**
 * @description: This function is used to initial the Upload process.
 *
 * @param: void
 *
 * @return: Success or Failure.
 */
int cvr_upload_init()
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRUPLOAD","%s(%d): CVR Upload with timeout %d \n", __FILE__, __LINE__, CVR_UPLOAD_TIMEOUT_INTERVAL);
	if (CVRUpload::getCVRUploadInstance() != NULL)
	{
		RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): CVR instance created successfully\n", __FILE__, __LINE__);
		//Get retry attribute
		return RDKC_SUCCESS;
	}
	else
	{
		cvr_upload_close();
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Conncetion Closed\n", __FILE__, __LINE__);
		return RDKC_FAILURE;
	}
}

/**
 * @description: This function is used to close the upload process.
 *
 * @param[in]: Path of the file to be uploaded,result,m level path .
 *
 * @return: void.
 */
void cvr_upload_close()
{
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Closing the connection\n", __FILE__, __LINE__);
	CVRUpload::getCVRUploadInstance()->deleteCVRUplaodInstance();
}

/**
 * @description: This function is used to call the function which upload data to the http server.
 *
 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
 *
 * @return: Error code.
 */
int cvr_upload(char *fpath, char *stime, char *etime, int eventType, int eventDatetime, char *m_fpath , int motion_level_idx)
{
	CVRUpload *cvrUpload = CVRUpload::getCVRUploadInstance();
	if(cvrUpload->isCVRInstanceInitialized())
	{
		if(cvrUpload->doCVRUpload(fpath, stime, etime, eventType, eventDatetime, m_fpath, motion_level_idx) == RDKC_SUCCESS)
		{
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Data upload successful\n", __FILE__, __LINE__);
			return RDKC_SUCCESS;
		}
		else
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRUPLOAD","%s(%d): Data upload failed\n", __FILE__, __LINE__);
			return RDKC_FAILURE;
		}
	}
}
#endif
