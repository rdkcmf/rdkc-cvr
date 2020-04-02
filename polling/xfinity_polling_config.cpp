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
#include "rdk_debug.h"
#include "xfinity_polling_config.h"
extern  "C"
{
#include "secure_wrapper.h"
}

#include<iostream>
using namespace std;

#define LOCK_FILENAME_XPC		"/tmp/xfinity_polling_config.lock"

volatile sig_atomic_t XfinityPollingConfig::term_flag = 0 ;
volatile sig_atomic_t XfinityPollingConfig::forced_polling = 0 ;
double XfinityPollingConfig::waitingInterval = DEFAULT_MIN_INTERVAL ;
double XfinityPollingConfig::defaultInterval = DEFAULT_MIN_INTERVAL ;
int XfinityPollingConfig::minInterval = DEFAULT_MIN_INTERVAL ;
int XfinityPollingConfig::maxInterval = DEFAULT_MAX_INTERVAL ;
double XfinityPollingConfig::retryFactor = DEFAULT_RETRY_FACTOR ;

/** @description XfinityPollingConfig constructor.
 */
XfinityPollingConfig::XfinityPollingConfig()
	:http_client(NULL)
	,receive_content(NULL)
	,xml_content(NULL)
	,xml_parser(NULL)
{

	memset(polling_server_url, 0, sizeof(polling_server_url));
        memset(polling_server_auth, 0, sizeof(polling_server_auth));
	memset(recovery_polling_server_url, 0, sizeof(polling_server_url));
	memset(xml_parser_content, 0, sizeof(xml_parser_content));
	memset(MyXPCMsg, 0, sizeof(MyXPCMsg));

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): XfinityPollingConfig constructor \n", __FILE__, __LINE__);
}

/** @description XfinityPollingConfig destructor.
 */
XfinityPollingConfig::~XfinityPollingConfig(){

	if(NULL != http_client)
	{
		delete http_client;
		http_client = NULL;
	}

	if(NULL != xml_parser)
	{
		delete xml_parser;
		xml_parser = NULL;
	}

	if(RDKC_FAILURE == config_release())
	{
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to release the paramters\n", __FILE__, __LINE__);
	}

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): XfinityPollingConfig destructor \n", __FILE__, __LINE__);
}

/** @description Init Xfinity Polling Config.
 *  @param process main process name.
 *  @return integer (XPC_OK if success, XPC_ERROR if failure).
 */
int XfinityPollingConfig::XPC_init(char* process)
{
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): XfinityPollingConfig Init \n", __FILE__, __LINE__);
	/*Getting the retry attributes*/
	config_init();
	getCloudRecorderRetryAttribute();

	/*if(RDKC_FAILURE == config_release())
	{
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to release the paramters\n", __FILE__, __LINE__);
	}*/

	/* Create HttpClient instance */
	if( NULL == http_client )
	{
        	http_client = new HttpClient();
	}

	if( NULL == http_client )
	{
		return XPC_ERROR;
	}

	if(NULL == xml_parser)
	{
		xml_parser = new XmlParser();
	}

	if(NULL == xml_parser)
	{
		return XPC_ERROR;
	}
	/* Register signal handler */
	XPC_register_signal_handler();

	/* Process Control */
	if( XPC_process_control(process) < 0)
	{
		return XPC_ERROR;
	}

	return XPC_OK;
}
/** @description XPC Signal handler.
 *  @param s signal
 *  @return void.
 */
void XfinityPollingConfig::XPC_signal_handler(int s)
{
        if (SIGTERM == s)
        {
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): catch SIGTERM...\n", __FILE__, __LINE__);
                term_flag = 1;
        }
        else if (SIGINT == s)
        {
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): catch SIGINT...\n", __FILE__, __LINE__);
                term_flag = 1;
        }
        else if (SIGPIPE == s)
        {
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): catch SIGPIPE...\n", __FILE__, __LINE__);
        }
        else if (SIGUSR1 == s)
        {
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): catch SIGUSR1...and forcing the xfinity polling\n", __FILE__, __LINE__);
		forced_polling = 1;
        }
	else
        {
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): catch unknow signal (%d)...\n", __FILE__, __LINE__,s);
        }

}

/** @description Register the XPC Signal handler.
 *  @param  void.
 *  @return void.
 */
void XfinityPollingConfig::XPC_register_signal_handler(void)
{
        signal(SIGTERM, XPC_signal_handler);
        signal(SIGINT, XPC_signal_handler);
        // connection broken
        signal(SIGPIPE, XPC_signal_handler);
        // forced polling
	signal(SIGUSR1,XPC_signal_handler);
}

/** @description Check file lock used for process control.
 *  @param fname file name to be checked.
 *  @return fd file descriptor.
 */
int XfinityPollingConfig::XPC_check_filelock(char *fname)
{
        int fd = -1;
        pid_t pid = -1;

	char str[50] = "xfinity_polling_config" ;

        fd =  open(fname, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(fd < 0 && errno == EEXIST)
        {
                fd = open(fname, O_RDONLY, 0644);
                if (fd >= 0)
                {
                        read(fd, &pid, sizeof(pid));
                        kill(pid, SIGTERM);
                        close(fd);
                        sleep(1);
                      	if (CheckAppsPidAlive( (char*)str , pid))
                      	{
                        	kill(pid, SIGTERM);
                      	}
                }
                unlink(fname);
                return -2;
        }

        return fd;
}

/** @description Get termination flag.
 *  @param void.
 *  @return term_flag termination flag.
 */
volatile sig_atomic_t XfinityPollingConfig::XPC_get_term_flag(){

        return term_flag;
}

/** @description Parse the response.
 *  @param buff Response buffer.
 *  @return xml Parsed response.
 */
char* XfinityPollingConfig::XPC_parse_response(char *buff)
{
	char *phrase = NULL;
	char *phrase1 = NULL;
	char *xml = NULL;
	int content_len = 0;

	// parse return code
	phrase=strstr(buff,"HTTP/");
	if(!phrase)
	{
		return xml;
	}
	phrase1=strchr(phrase,' ');
	if(!phrase1)
	{
		return xml;
	}
	phrase1++;
	if (strncmp(phrase1,"200 OK",strlen("200 OK")))
	{
		return xml;
	}
	// find content lenth
	if((phrase = strstr(buff, "Content-Length:")) != NULL)
	{
		phrase += strlen("Content-Length:");
		while (*phrase == ' ' || *phrase == '\t')
		{
			phrase++;
		}
		content_len = atoi(phrase);
		phrase = strstr(phrase,"\r\n");
		if (!phrase)
		{
			return xml;
		}
	}
	// find hearder end
	if ((phrase = strstr(buff, "\r\n\r\n")) != NULL)
	{
		phrase += 4;

		/*if (content_len != strlen(phrase))
		{
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Content length is not right!\n", __FUNCTION__, __LINE__);
			return XPC_RESPONSE_ERROR;
		}*/
		xml = phrase;
	}
	return xml;

}

/** @description Parse the period time.
 *  @param period_time Time interval.
 *  @return ret_time Parsed period time.
 */
long XfinityPollingConfig::XPC_parse_period_time_iso8601(char *period_time)
{
	char *phrase = NULL;
	char *phrase1 = NULL;
	char *phrase2 = NULL;
	long ret_time = 0;	// seconds
	int temp_value = 0;

	// Parse the URL to ip, URI, port and protocol
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Start parsing...[time=%s]\n", __FUNCTION__, __LINE__, period_time);

	if (NULL == period_time)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Invalid parameter!\n", __FUNCTION__, __LINE__);
		return ret_time;
	}

	phrase = period_time;
	if (*phrase != 'P')
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Not a period time!\n", __FUNCTION__, __LINE__);
		return ret_time;
	}
	phrase++;

	if (phrase1 = strchr(phrase, 'Y'))
	{
		temp_value = atoi(phrase);
		ret_time += temp_value*365*30*24*60*60;
		phrase = phrase1 + 1;
	}

	if (phrase1 = strchr(phrase, 'M'))
	{
		phrase2 = strchr(phrase, 'T');
		// Here we need judge the M is Month or Minute
		if (NULL == phrase2 || phrase1 < phrase2)
		{
			temp_value = atoi(phrase);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Month=[%d]!\n", __FUNCTION__, __LINE__, temp_value);
			ret_time += temp_value*30*24*60*60;
			phrase = phrase1 + 1;
		}
	}

	if (phrase1 = strchr(phrase, 'D'))
	{
		temp_value = atoi(phrase);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Day=[%d]!\n", __FUNCTION__, __LINE__, temp_value);
		ret_time += temp_value*24*60*60;
		phrase = phrase1 + 1;
	}

	if (strchr(phrase, 'T'))
	{
		phrase++;
		if (phrase1 = strchr(phrase, 'H'))
		{
			temp_value = atoi(phrase);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Hour=[%d]!\n", __FUNCTION__, __LINE__, temp_value);
			ret_time += temp_value*60*60;
			phrase = phrase1 + 1;
		}
		if (phrase1 = strchr(phrase, 'M'))
		{
			temp_value = atoi(phrase);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Minute=[%d]!\n", __FUNCTION__, __LINE__, temp_value);
			ret_time += temp_value*60;
			phrase = phrase1 + 1;
		}

		if (phrase1 = strchr(phrase, 'S'))
		{
			temp_value = atoi(phrase);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Second=[%d]!\n", __FUNCTION__, __LINE__, temp_value);
			ret_time += temp_value;
		}
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): ret_time=%d!\n", __FUNCTION__, __LINE__, ret_time);
	return ret_time;
}

/** @description Apply the changes to config file.
 *  @param void.
 *  @return integer depending on conditions.
 */
int XfinityPollingConfig::XPC_apply_config()
{
	int ret = XPC_OK;
	int cfg_chg_flag = 0;
	xmlNodePtr tag = NULL;
	xmlNodePtr root = NULL;
	const char* tagname = NULL;

	if(NULL != xml_parser)
        {
		memset(xml_parser_content, 0, sizeof(xml_parser_content));
	        strncpy(xml_parser_content, xml_content,strlen(xml_content));
		//std::cout << "xml parser content  from config server [" << (const char *)xml_parser << " ] from server!! and length is " << strlen(xml_parser_content) << "\n";
		if(RDKC_SUCCESS != xml_parser->getParsedXml((const char*)xml_parser_content, strlen(xml_parser_content))) {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error geting xml parsed... XML content is not correct.\n", __FILE__, __LINE__);
			ret = XPC_XML_ERROR;
	                xml_parser->freeXML();
			return ret;
		}

		// Read all config settings
		g_pconf = (All_Conf*)malloc(sizeof(All_Conf));
		if(NULL == g_pconf) {
	                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Memory allocation error for All Conf!!\n", __FUNCTION__, __LINE__);
			return XPC_ERROR;
		}
		memset(g_pconf, 0, sizeof(All_Conf));

		if (ReadAllConf(g_pconf, MyXPCMsg) != 0)
		{
			ret = XPC_ERROR;
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error reading all conf settings.\n", __FILE__, __LINE__);
	                xml_parser->freeXML();
			if(g_pconf) {
				free(g_pconf);
			}
		        return ret;
		}

		// Do really action
		root = xml_parser->getRootNode();
		XPC_parse_config_attr(g_pconf, root);	// Parse polling config url,auth and interval

		for(tag = xml_parser->getFirstChild(root);tag!=NULL; tag= xml_parser->getNextSibling(tag))
		{
			tagname = xml_parser->getTagName(tag);
	                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);
			if(0 == strcmp(tagname, XPC_XML_TAG_NAME_SYSTEM))
			{
				XPC_parse_system_segment(g_pconf, tag);
			}
			else if(0 == strcmp(tagname, XPC_XML_TAG_NAME_CVR))
			{
				XPC_parse_cvr_segment(g_pconf, tag);
			}
			else if(0 == strcmp(tagname, XPC_XML_TAG_NAME_EVENTS))
			{
				XPC_parse_events_segment(g_pconf, tag);
			}
			else if(0 == strcmp(tagname, XPC_XML_TAG_NAME_DETECTION))
			{
				XPC_parse_motion_segment(g_pconf, tag);
			}
			else if(0 == strcmp(tagname, XPC_XML_TAG_NAME_METRICS))
			{
				XPC_parse_metrics_segment(g_pconf, tag);
			}
			else
			{
	                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
			}
		}

		// save all config settings to memory anf flash
		if (SaveAllConf(g_pconf, MyXPCMsg, 1, &cfg_chg_flag))
		{
			ret = XPC_ERROR;
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error saving all conf settings.\n", __FILE__, __LINE__);
			xml_parser->freeXML();
			if(g_pconf) {
				free(g_pconf);
			}
		        return ret;
		}
	        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Saving all config into flash! config change flag[%d] \n", __FUNCTION__, __LINE__, cfg_chg_flag);

		// Make the new settings work
		if (cfg_chg_flag)
		{
	                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Applying new settings!\n", __FUNCTION__, __LINE__);
			AllFuncReact(g_pconf, MyXPCMsg);
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): XML parser object is null\n", __FUNCTION__, __LINE__);
		ret = RDKC_FAILURE;
	}

	xml_parser->freeXML();
	if(g_pconf) {
		free(g_pconf);
	}
        return ret;

}

/** @description Parse the motion segment.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_motion_segment(All_Conf *pconf, xmlNodePtr tag)
{
	xmlNodePtr motion_tag = NULL;
	const char* motion_tagname = NULL;

	for(motion_tag = xml_parser->getFirstChild(tag);motion_tag!=NULL;motion_tag=xml_parser->getNextSibling(motion_tag))
	{
		motion_tagname = xml_parser->getTagName(motion_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,motion_tagname);
		if(0 == strcmp(motion_tagname, XPC_XML_TAG_NAME_MOTION))
		{
			// parse motion detection attribute: enabled, env, sensitivity
			XPC_parse_motion_attr(pconf, motion_tag);
		}
	}
	return XPC_OK;
}

/** @description Parse the metrics segment.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_metrics_segment(All_Conf *pconf, xmlNodePtr tag)
{
	xmlNodePtr metrics_tag = NULL;
	xmlNodePtr dataset1_tag = NULL;
	xmlNodePtr dataset2_tag = NULL;
	xmlNodePtr dataset3_tag = NULL;
	const char* metrics_tagname = NULL;
	int i = 0;
	int dataset1_enable = 0;
	int dataset2_enable = 0;
	int dataset3_enable = 0;

	for(metrics_tag = xml_parser->getFirstChild(tag);metrics_tag!=NULL;metrics_tag= xml_parser->getNextSibling(metrics_tag))
	{
		metrics_tagname = xml_parser->getTagName(metrics_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,metrics_tagname);
		if(0 == strcmp(metrics_tagname, XPC_XML_TAG_NAME_DATASET1))
		{
			//parse dataset1 attribute:interval,url,auth
			XPC_parse_dataset_attr(pconf, metrics_tag, 0);
			// If xml config file contains tag dataset1, we shoud enable the metrics
			for (i = 0; i< METRICS_DATASET_1_NUM_MAX; i++)
			{
				BSET(dataset1_enable, i);
			}
			for(dataset1_tag= xml_parser->getFirstChild(metrics_tag);dataset1_tag!=NULL;dataset1_tag= xml_parser->getNextSibling(dataset1_tag))
			{
				if(0 == strcmp(xml_parser->getTagName(dataset1_tag), XPC_XML_TAG_NAME_DISABLED))
				{
					//parse dataset1 disabled:model,serial,mac,firmware,wired,ssid,radio,stream
					XPC_parse_dataset1_disabled(pconf, dataset1_tag, &dataset1_enable);
				}
				//else if...
				else
				{
                                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
				}
			}
			if (pconf->MetricsInfo.dataset_info[0].enabled != dataset1_enable)
			{
				pconf->MetricsInfo.dataset_info[0].enabled = dataset1_enable;
				pconf->chg_flag.metrics_chg = 1;
			}
		}
		else if(0 == strcmp(metrics_tagname, XPC_XML_TAG_NAME_DATASET2))
		{
			//parse dataset2 attribute:interval,url,auth
			XPC_parse_dataset_attr(pconf, metrics_tag, 1);
			// If xml config file contains tag dataset2, we shoud enable the metrics dataset2
			for (i = 0; i< METRICS_DATASET_2_NUM_MAX; i++)
			{
				BSET(dataset2_enable, i);
			}
			for(dataset2_tag= xml_parser->getFirstChild(metrics_tag);dataset2_tag!=NULL;dataset2_tag= xml_parser->getNextSibling(dataset2_tag))
			{
				if(0 == strcmp( xml_parser->getTagName(dataset2_tag), XPC_XML_TAG_NAME_DISABLED))
				{
					//parse dataset2 disabled:cpu,memory,snr
					XPC_parse_dataset2_disabled(pconf, dataset2_tag, &dataset2_enable);
				}
				//else if...
				else
				{
                                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
				}
			}
			if (pconf->MetricsInfo.dataset_info[1].enabled != dataset2_enable)
			{
				pconf->MetricsInfo.dataset_info[1].enabled = dataset2_enable;
				pconf->chg_flag.metrics_chg = 1;
			}
		}
		else if(0 == strcmp(metrics_tagname, XPC_XML_TAG_NAME_DATASET3))
		{
			//parse dataset3 attribute:interval,url,auth
			XPC_parse_dataset_attr(pconf, metrics_tag, 2);
			// If xml config file contains tag dataset3, we shoud enable the metrics dataset3
			for (i = 0; i< METRICS_DATASET_3_NUM_MAX; i++)
			{
				BSET(dataset3_enable, i);
			}
			for(dataset3_tag=xml_parser->getFirstChild(metrics_tag);dataset3_tag!=NULL;dataset3_tag=xml_parser->getNextSibling(dataset3_tag))
			{
				if(0 == strcmp(xml_parser->getTagName(dataset3_tag), XPC_XML_TAG_NAME_DISABLED))
				{
					//parse dataset3 disabled:rssi
					XPC_parse_dataset3_disabled(pconf, dataset3_tag, &dataset3_enable);
				}
				//else if...
				else
				{
                                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
				}
			}
			if (pconf->MetricsInfo.dataset_info[2].enabled != dataset3_enable)
			{
				pconf->MetricsInfo.dataset_info[2].enabled = dataset3_enable;
				pconf->chg_flag.metrics_chg = 1;
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
		}
	}

	return XPC_OK;
}

/** @description Parse the events segment.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_events_segment(All_Conf *pconf, xmlNodePtr tag)
{
	int i = 0;
	xmlNodePtr events_tag = NULL;
	int interval = -1;
	int motion_events_enable = -1;
	int human_events_enable = -1;
	int tamper_events_enable = -1;
	int ret = XPC_OK;
	char str[2];

	const char* eventTagName = NULL;

	// parse events attribute: , url, auth, quite_interval
	XPC_parse_events_attr(pconf, tag, &interval);

	for(events_tag = xml_parser->getFirstChild(tag);events_tag!=NULL;events_tag= xml_parser->getNextSibling(events_tag))
	{
		eventTagName = xml_parser->getTagName(events_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,eventTagName);
		if(0 == strcmp(eventTagName, XPC_XML_TAG_NAME_MOTION))
		{
			XPC_parse_events_motion_attr(pconf, events_tag, &motion_events_enable);
		}
#ifdef _SUPPORT_OBJECT_DETECTION_
		else if(0 == strcmp(eventTagName, XPC_XML_TAG_NAME_HUMAN))
		{
			XPC_parse_events_human_attr(pconf, events_tag, &human_events_enable);
		}
#endif
#ifdef _SUPPORT_TAMPER_DETECTION_
		else  if(0 == strcmp(eventTagName, XPC_XML_TAG_NAME_TAMPER))
		{
			XPC_parse_events_tamper_attr(pconf, events_tag, &tamper_events_enable);
		}
#endif
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
		}
	}

	XPC_save_event_entry(pconf, motion_events_enable, interval, EVENT_TYPE_MOTION);
#ifdef _SUPPORT_OBJECT_DETECTION_
	XPC_save_event_entry(pconf, human_events_enable, interval, EVENT_TYPE_PEOPLE);
	//control IV engine human detection enable or disable here, to save CPU resource.
	memset(str, 0, sizeof(str));
	snprintf(str, sizeof(str), "%d", human_events_enable);
	if (set_human_detect(str, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
	{
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): failed to set IV engine human detection!\n", __FUNCTION__, __LINE__);
		ret = XPC_FAIL;
		return ret;
	}
#endif
#ifdef _SUPPORT_TAMPER_DETECTION_
	XPC_save_event_entry(pconf, tamper_events_enable, interval, EVENT_TYPE_TAMPER);
#endif

	return ret;
}

/** @description Parse the cvr segment.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_cvr_segment(All_Conf *pconf, xmlNodePtr tag)
{
	xmlNodePtr cvr_tag = NULL;
	xmlNodePtr segments_tag = NULL;
	const char* tagname = NULL;
	const char* cvr_tagname = NULL;
	const char* segments_tagname = NULL;
	AbrTargetBitrate abr_bitrate;
	char str[ABR_KEY_AREA_LEN];
	int ret = XPC_OK;

	tagname = xml_parser->getTagName(tag);
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	// parse cvr attribute: enabled, url, auth, audio
	XPC_parse_cvr_attr(pconf, tag);
	for(cvr_tag = xml_parser->getFirstChild(tag);cvr_tag!=NULL;cvr_tag= xml_parser->getNextSibling(cvr_tag))
	{
		cvr_tagname = xml_parser->getTagName(cvr_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,cvr_tagname);
		if(0 == strcmp(cvr_tagname, XPC_XML_TAG_NAME_SEGMENTS))
		{
			// parse segments attribute: duration, format
			XPC_parse_segments_attr(pconf, cvr_tag);
			for(segments_tag= xml_parser->getFirstChild(cvr_tag);segments_tag!=NULL;segments_tag= xml_parser->getNextSibling(segments_tag))
			{
				segments_tagname = xml_parser->getTagName(segments_tag);
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, segments_tagname);
				if(0 == strcmp(segments_tagname, XPC_XML_TAG_NAME_LBR))
				{
					// parse LBR attribute: min,max
					memset(&abr_bitrate, 0, sizeof(abr_bitrate));
					XPC_parse_lbr_attr(pconf, segments_tag, &abr_bitrate);
				}
				else if(0 == strcmp(segments_tagname, XPC_XML_TAG_NAME_H264))
				{
					//parse H264 attribute : resolution
					XPC_parse_h264_attr(pconf, segments_tag);
				}
				else
				{
                                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
				}
				// Must set ABR bitrate after we parsed H264 resolution! Otherwise customer changed resolution, we may set ABR bitrate to wrong group.
				memset(str, 0, sizeof(str));
				ABR_SetConf(&abr_bitrate, str, DEF_CVR_CHANNEL, pconf->img_conf.h264[DEF_CVR_CHANNEL].quality_level-1);
				if (set_abr_conf_by_index(str, 1, MyXPCMsg, pconf, (LIST *)NULL, DEF_CVR_CHANNEL, pconf->img_conf.h264[DEF_CVR_CHANNEL].resolution) != 0)
				{
                                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set ABR bitrate!\n", __FUNCTION__, __LINE__);
					ret = XPC_XML_ERROR;
				}
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
		}
	}
	return ret;
}

/** @description Parse the system segment.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_system_segment(All_Conf *pconf, xmlNodePtr tag)
{
	xmlNodePtr system_tag = NULL;
	xmlNodePtr lux_tag = NULL;
	const char* tagname = NULL;
	const char* system_tagname = NULL;
	const char* lux_tagname = NULL;

	tagname = xml_parser->getTagName(tag);
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	for(system_tag=xml_parser->getFirstChild(tag);system_tag!=NULL;system_tag=xml_parser->getNextSibling(system_tag))
	{
		system_tagname = xml_parser->getTagName(system_tag);
		if(0 == strcmp(system_tagname, XPC_XML_TAG_NAME_LUX))
		{
			for(lux_tag=xml_parser->getFirstChild(system_tag);lux_tag!=NULL;lux_tag= xml_parser->getNextSibling(lux_tag))
			{
				lux_tagname = xml_parser->getTagName(lux_tag);
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, lux_tagname);
				if(0 == strcmp(lux_tagname, XPC_XML_TAG_NAME_THRESHOLD))
				{
					// parse day night switch threashold
					XPC_parse_threshold_attr(pconf, lux_tag);
				}
			}
		}
	}
	return XPC_OK;
}

/** @description Parse config attributes.
 *  @param pconf config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_config_attr(All_Conf *pconf, xmlNodePtr tag)
{
	int ret = 0;
	int tmp = 0;
	char tmp_buf[XPC_URL_LEN+1];
	char url_buf[XPC_URL_LEN+1];
	int redirect_url_flg = 0;

	int attribute_count = 0;
	const char* tagname = NULL;
	char* interval = NULL;
	char* url = NULL;
	char* auth = NULL;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}


        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,tagname);
	if (!strcasecmp(tagname, XPC_XML_TAG_NAME_CONFIG))
	{
		// polling interval
		interval = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(interval, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_INTERVAL, interval);
		if (NULL != interval)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", interval);
			tmp = XPC_parse_period_time_iso8601(tmp_buf);
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%d", tmp);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): polling_interval=%s!\n", __FUNCTION__, __LINE__, tmp_buf);

			if (set_xfinity_polling_interval(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling interval!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != interval)
			{
				free(interval);
				interval = NULL;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(POLLING_INTERVAL, tmp_buf) != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling interval via Config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
			}
#endif
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_INTERVAL);
		}

		// url
		url = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(url, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_URL,url);
		if (NULL != url)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", url);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): polling config url=%s!\n", __FUNCTION__, __LINE__, url);

			memset(url_buf, 0, sizeof(url_buf));
			if (strncasecmp(tmp_buf,"http://", strlen("http://")))
			{
				strncpy(url_buf, tmp_buf, sizeof(url_buf));
			}
			else
			{
				strncpy(url_buf, DEF_CLOUD_RECORDER_POLLING_URL, sizeof(url_buf));
				redirect_url_flg = 1;
				RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): URL start with \"http://\", replace with default url!\n", __FUNCTION__, __LINE__);
				AddLog((char*)CVR_SEC,SYSLOG_WARNING, CVR_HTTP_REQUEST, (char*)tmp_buf);
			}

			// Store recovery polling URL only once immediately after the first polling
			if ( strlen(recovery_polling_server_url) == 0 )
			{
				strncpy(recovery_polling_server_url, url_buf, sizeof(url_buf));
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Recovery polling url: %s!\n", __FUNCTION__, __LINE__, recovery_polling_server_url);
			}

			if (set_xfinity_polling_url(url_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling url!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != url)
			{
				free(url);
				url = NULL;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(POLLING_URL, url_buf) != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling url via config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
			}
#endif
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_URL);
		}

		// auth
		if (0 == redirect_url_flg)
		{
			auth = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
			memset(auth, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
			xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUTH, auth);
			if (NULL != auth)
			{
				memset(tmp_buf, 0, sizeof(tmp_buf));
				snprintf(tmp_buf, sizeof(tmp_buf), "%s", auth);
                                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): upload_auth=%s!\n", __FUNCTION__, __LINE__, auth);
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): upload_auth=hidden!\n", __FUNCTION__, __LINE__);

				if (set_xfinity_polling_auth(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
				{
                                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling auth!\n", __FUNCTION__, __LINE__);
					ret = XPC_XML_ERROR;
				}

				if(NULL != auth)
				{
					free(auth);
					auth = NULL;
				}
#ifndef XFINITY_SUPPORT
				if(rdkc_envSet(POLLING_AUTH, auth) != RDKC_SUCCESS) {
					RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling auth via config Manager!\n", __FUNCTION__, __LINE__);
					return RDKC_FAILURE;
				}
#endif
			}
		}
		else
		{
			if (set_xfinity_polling_auth((char*)"", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling auth!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(POLLING_AUTH, " ") != RDKC_SUCCESS) {
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set polling auth via config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
			}
#endif
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag name [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_TAG_NAME_CONFIG);
	}
	return XPC_OK;
}

/** @description Append Mac Id to URL.
 *  @param out_url Output URL.
 *  @param len length of URL.
 *  @param in_url Input URL.
 *  @return integer (XPC_OK if success, XPC_ERROR if failure).
 */
int XfinityPollingConfig::XPC_append_mac_to_url(char *out_url, int len, char *in_url)
{
	unsigned char macaddr[MAC_ADDR_LEN];
	char mac_string[XPC_MAC_STRING_LEN+1];
	char *ptr = NULL;

	memset(mac_string, 0, sizeof(mac_string));
	if (get_mac_address((unsigned char*)macaddr) < 0)
	{
		return XPC_ERROR;
	}
	transcode_mac_to_string_by_separator((unsigned char*) macaddr, '\0', mac_string, XPC_MAC_STRING_LEN+1, 0);

	ptr = strrchr(in_url,'/');
	if(NULL != ptr)
	{
		if(strcasecmp(mac_string,ptr+1))
		{
			snprintf(out_url, len, "%s/%s", in_url, mac_string);
		}
		else
		{
			snprintf(out_url, len, "%s", in_url);
		}
	}

	return XPC_OK;
}

/** @description Save the event details.
 *  @param pconf Config settings.
 *  @param enable_flag Motion events enable flag.
 *  @param interval Eevent interval.
 *  @param event_type Type of event.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_save_event_entry(All_Conf *pconf, int enable_flag, int interval, EventType event_type)
{
        int evt_idx = -1,free_idx=-1;
	int i = 0;
	char *p_event_name = NULL;
	int ret = XPC_OK;

	switch(event_type)
	{
		case EVENT_TYPE_MOTION:
			p_event_name = (char*)XPC_EVENT_NAME_MOT_STR;
			break;
#ifdef _SUPPORT_OBJECT_DETECTION_
		case EVENT_TYPE_PEOPLE:
			p_event_name = (char*)XPC_EVENT_NAME_MOT_PEOPLE_STR;
			break;
#endif
#ifdef _SUPPORT_TAMPER_DETECTION_
		case EVENT_TYPE_TAMPER:
			p_event_name = (char*)XPC_EVENT_NAME_MOT_TAMPER_STR;
			break;
#endif
		default:
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Invalid parameter! Event type = %d!\n", __FUNCTION__, __LINE__, event_type);
			return XPC_ERROR;
	}

	for (i=0;i < EVENT_TRIGGER_ITEM_NUMBER_MAX-1; i++)
	{
		if (!pconf->event_conf.event_conf_item[i].event_name[0] && !pconf->event_conf.event_conf_item[i].enable)
		{
			if (free_idx < 0)
			{
				free_idx = i;
			}
		}
		if (!strcmp(p_event_name, pconf->event_conf.event_conf_item[i].event_name))
		{
			evt_idx = i;
			break;
		}
	}
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Event index = %d!free_idx=%d!\n", __FUNCTION__, __LINE__, evt_idx, free_idx);
	if (evt_idx < 0)
	{
		if (free_idx >= 0)
		{
			memset(&(pconf->event_conf.event_conf_item[free_idx]),0,sizeof(EventConfItem));
			strncpy(pconf->event_conf.event_conf_item[free_idx].event_name, p_event_name, EVENT_ID_NAME_MAX_LENGTH);
			pconf->event_conf.event_conf_item[free_idx].event_name[EVENT_ID_NAME_MAX_LENGTH] = 0;
			pconf->chg_flag.event_chg = 1;
			evt_idx = free_idx;
			if (-1 == interval)
			{
				interval = XPC_ENVETN_INTERVAL_DEF;
			}
		}
	}
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Event index = %d!\n", __FUNCTION__, __LINE__, evt_idx);
	if (evt_idx >= 0)
	{
		// If no enable parameter present, save nothing
		if (-1 != enable_flag && pconf->event_conf.event_conf_item[evt_idx].enable != enable_flag)
		{
			pconf->event_conf.event_conf_item[evt_idx].enable = enable_flag;
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].event_type != event_type)
		{
			pconf->event_conf.event_conf_item[evt_idx].event_type = event_type;
			pconf->chg_flag.event_chg = 1;
		}
		// If no interval parameter present, save nothing
		if (-1 != interval &&  pconf->event_conf.event_conf_item[evt_idx].interval != interval)
		{
			pconf->event_conf.event_conf_item[evt_idx].interval = interval;
			pconf->chg_flag.event_chg = 1;
		}
		if (!(pconf->event_conf.event_conf_item[evt_idx].action_list_flag & (1<<ACTION_TYPE_HTTP)))
		{
			pconf->event_conf.event_conf_item[evt_idx].action_list_flag = (1<<ACTION_TYPE_HTTP);
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].sche_list.enable != EVENT_SCHEULE_DISABLE)
		{
			pconf->event_conf.event_conf_item[evt_idx].sche_list.enable = EVENT_SCHEULE_DISABLE;
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].fileconf.file_format != FILE_FORMAT_MP4)
		{
			pconf->event_conf.event_conf_item[evt_idx].fileconf.file_format = FILE_FORMAT_MP4;
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].fileconf.pre_len != 5)
		{
			pconf->event_conf.event_conf_item[evt_idx].fileconf.pre_len = 5;
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].fileconf.post_len != 5)
		{
			pconf->event_conf.event_conf_item[evt_idx].fileconf.post_len = 5;
			pconf->chg_flag.event_chg = 1;
		}
		if (pconf->event_conf.event_conf_item[evt_idx].input_source!= 1)
		{
			pconf->event_conf.event_conf_item[evt_idx].input_source = 1;
			pconf->chg_flag.event_chg = 1;
		}
	}

	// save overall event enable/disable config only for Comcast partner
	if(true == isComcastPartner()) {
		for (i=0; i<EVENT_TRIGGER_ITEM_NUMBER_MAX-1; i++)
		{
			if (pconf->event_conf.event_conf_item[i].enable)
			{
				break;
			}
		}

		// save overall event enable/disable config
		if (set_h_en_trig((char*)((i>=EVENT_TRIGGER_ITEM_NUMBER_MAX-1)?"0":"1"), 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set envent enable!!\n", __FUNCTION__, __LINE__);
			ret = XPC_FAIL;
		}
		else
		{
			RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): event_trigger=%d!\n", __FUNCTION__, __LINE__, (!(i>=EVENT_TRIGGER_ITEM_NUMBER_MAX-1)));
		}
	}

	return ret;
}

/** @description Parse motion attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_motion_attr(All_Conf *pconf, xmlNodePtr tag)
{
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int motion_sensitivity = 0;
	int ret = XPC_OK;

	char* enable = NULL;
	char* env = NULL;
	char* sensitivity = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

	// enabled
	enable = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(enable, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENABLED, enable);
	if (NULL != enable)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", enable);

		if (!strcmp(tmp_buf, "true"))
		{
			if(RDKC_SUCCESS != rdkc_set_user_setting(MD_MODE, "1"))
                        {
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to enable md_mode in usr_config.\n", __FILE__, __LINE__);
                        }

			if (set_mot((char*)"1", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set motion enable flag!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

		}
		else
		{
			if(RDKC_SUCCESS != rdkc_set_user_setting(MD_MODE, "0"))
                        {
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to disable md_mode in usr_config.\n", __FILE__, __LINE__);
                        }

			if (set_mot((char*)"0", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set motion enable flag!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}
		}

		if(NULL != enable)
		{
			free(enable);
			enable = NULL;
		}
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): motion_enable=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENABLED);
	}
	// env
	env = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(env, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENV, env);
	if (NULL != env)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", env);

		if (!strcmp(tmp_buf, XPC_XML_TAG_NAME_INDOOR))
		{
#ifdef _SUPPORT_OBJECT_DETECTION_IV_
			if (set_scene((char*)"1", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set motion scene!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}
#endif
		}
		else if (!strcmp(tmp_buf, XPC_XML_TAG_NAME_OUTDOOR))
		{
#ifdef _SUPPORT_OBJECT_DETECTION_IV_
			if (set_scene((char*)"2", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set motion scene!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}
#endif
		}
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Invalid motion env [%s]!\n", __FUNCTION__, __LINE__, tmp_buf);
		}

		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): motion env is %s!\n", __FUNCTION__, __LINE__, tmp_buf);
		if(NULL != env)
		{
			free(env);
			env = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENV);
	}

	// sensitivity
	sensitivity = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(sensitivity, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_SENSITIVITY, sensitivity);
	if (NULL != sensitivity)
	{
		motion_sensitivity = atoi(sensitivity);
		if ((motion_sensitivity > 0 && motion_sensitivity < 60) || motion_sensitivity > 100)
		{
			motion_sensitivity = 60;
		}

		memset(tmp_buf2, 0, sizeof(tmp_buf2));
		snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", motion_sensitivity/10);
		if (set_mot_sensitivity1(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set motion sensitivity!\n", __FUNCTION__, __LINE__);
			ret = XPC_XML_ERROR;
		}

		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): motion sensitivity=%d!\n", __FUNCTION__, __LINE__, motion_sensitivity);
		if(NULL != sensitivity)
		{
			free(sensitivity);
			sensitivity = NULL;
		}

	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_SENSITIVITY);
	}

	return ret;
}

/** @description Parse dataset attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param index Index of dataset.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_dataset_attr(All_Conf *pconf, xmlNodePtr tag, int index)
{
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int interval = 0;
	int timeout = 0;

	char* metric_interval = NULL;
	char* url = NULL;
	char* auth = NULL;
	char* time_out = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}


        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	//  dataset metric_interval
	metric_interval = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(metric_interval, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_INTERVAL, metric_interval);
	if (NULL != metric_interval)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", metric_interval);
		interval = XPC_parse_period_time_iso8601(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): dataset->interval=%d!\n", __FUNCTION__, __LINE__, interval);
		if (pconf->MetricsInfo.dataset_info[index].interval != interval)
		{
			pconf->MetricsInfo.dataset_info[index].interval = interval;
			pconf->chg_flag.metrics_chg = 1;
		}

		if(NULL != metric_interval)
		{
			free(metric_interval);
			metric_interval = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_INTERVAL);
	}
	// dataset url
	url = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(url, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_URL,url);
	if (NULL != url)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", url);

		if (0 != strlen(tmp_buf) && strncasecmp(tmp_buf,"http://", strlen("http://")))
		{
			// check url whether include mac address, if not, add it to url.
			memset(tmp_buf2, 0, sizeof(tmp_buf2));
			XPC_append_mac_to_url(tmp_buf2, sizeof(tmp_buf2), tmp_buf);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): dataset->url=%s!\n", __FUNCTION__, __LINE__, tmp_buf2);
			if (strcmp(pconf->MetricsInfo.dataset_info[index].url, tmp_buf2))
			{
				strcpy(pconf->MetricsInfo.dataset_info[index].url,tmp_buf2);
				pconf->chg_flag.metrics_chg = 1;
			}
		}

		if(NULL != url)
		{
			free(url);
			url = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_URL);
	}
	// dataset auth
	auth = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(auth, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUTH, auth);
	if (NULL != auth)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", auth);

                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): dataset->auth=[%s]!\n", __FUNCTION__, __LINE__, tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): dataset->auth=hidden!\n", __FUNCTION__, __LINE__);
		if (strcmp(pconf->MetricsInfo.dataset_info[index].auth, tmp_buf))
		{
			strcpy(pconf->MetricsInfo.dataset_info[index].auth,tmp_buf);
			pconf->chg_flag.metrics_chg = 1;
		}

		if(NULL != auth)
		{
			free(auth);
			auth = NULL;
		}
	}

	// timeout
	time_out = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(time_out, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_TIMEOUT, time_out);
	if (NULL != time_out)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", time_out);

		timeout = XPC_parse_period_time_iso8601(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): dataset->timeout=%d!\n", __FUNCTION__, __LINE__, timeout);
                if (pconf->MetricsInfo.dataset_info[index].timeout != timeout)
		{
			pconf->MetricsInfo.dataset_info[index].timeout = timeout;
			pconf->chg_flag.metrics_chg = 1;
		}

		if(NULL != time_out)
		{
			free(time_out);
			time_out = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_TIMEOUT);
	}
	return XPC_OK;
}

/** @description Parse dataset1 info.
 *  @param pconf Config settings.
 *  @param dataset1_tag dataset1 tag.
 *  @param enable_flag dataset1 enable flag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_dataset1_disabled(All_Conf *pconf, xmlNodePtr dataset1_tag, int *enable_flag)
{
	xmlNodePtr disabled1_tag = NULL;
	const char* dataset1_tagname = NULL;
	const char* disabled1_tagname = NULL;
	dataset1_tagname = xml_parser->getTagName(dataset1_tag);

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,dataset1_tagname);

	for(disabled1_tag= xml_parser->getFirstChild(dataset1_tag);disabled1_tag!=NULL;disabled1_tag=xml_parser->getNextSibling(disabled1_tag))
	{
		disabled1_tagname = xml_parser->getTagName(disabled1_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, disabled1_tagname);
		if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_MODEL))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_MODEL);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_SERIAL))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_SERIAL);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_MAC))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_MAC);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_FIRMWARE))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_FW);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_WIRED))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_WIRED);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_SSID))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_SSID);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_RADIO))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_RATIO);
		}
		else if (0 == strcmp(disabled1_tagname, XPC_XML_TAG_NAME_STREAM))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_STREAM);
		}
		else
		{
			RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find dataset1 disabled XML tag name!\n", __FUNCTION__, __LINE__);
		}
	}
	return XPC_OK;
}

/** @description Parse dataset2 info.
 *  @param pconf Config settings.
 *  @param dataset2_tag dataset2 tag.
 *  @param enable_flag dataset2 enable flag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_dataset2_disabled(All_Conf *pconf, xmlNodePtr dataset2_tag, int *enable_flag)
{
	xmlNodePtr disabled2_tag = NULL;
	const char* dataset2_tagname = NULL;
	const char* disabled2_tagname = NULL;

	dataset2_tagname = xml_parser->getTagName(dataset2_tag);
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, dataset2_tagname);

	for(disabled2_tag= xml_parser->getFirstChild(dataset2_tag);disabled2_tag!=NULL;disabled2_tag= xml_parser->getNextSibling(disabled2_tag))
	{
		disabled2_tagname = xml_parser->getTagName(disabled2_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, disabled2_tagname);
		if (0 == strcmp(disabled2_tagname, XPC_XML_TAG_NAME_CPU))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_CPU);
		}
		else if (0 == strcmp(disabled2_tagname, XPC_XML_TAG_NAME_MEMORY))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_MEMORY);
		}
		else if (0 == strcmp(disabled2_tagname, XPC_XML_TAG_NAME_SNR))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_SNR);
		}
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
		}
	}
	return XPC_OK;
}

/** @description Parse dataset3 info.
 *  @param pconf Config settings.
 *  @param dataset3_tag dataset3 tag.
 *  @param enable_flag dataset3 enable flag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_dataset3_disabled(All_Conf *pconf, xmlNodePtr dataset3_tag, int *enable_flag)
{
	xmlNodePtr disabled3_tag = NULL;
	const char* dataset3_tagname = NULL;
	const char* disabled3_tagname = NULL;

	dataset3_tagname = xml_parser->getTagName(dataset3_tag);
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, dataset3_tagname);

	for(disabled3_tag= xml_parser->getFirstChild(dataset3_tag);disabled3_tag!=NULL;disabled3_tag= xml_parser->getNextSibling(disabled3_tag))
	{
		disabled3_tagname = xml_parser->getTagName(disabled3_tag);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, disabled3_tagname);
		if (0 == strcmp(disabled3_tagname, XPC_XML_TAG_NAME_RSSI))
		{
			BCLR(*enable_flag, DATASET_ENABLE_BIT_RSSI);
		}
		//else if...
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): XML tag is invalid!\n", __FUNCTION__, __LINE__);
		}
	}
	return XPC_OK;

}

/** @description Parse segments attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_segments_attr(All_Conf *pconf, xmlNodePtr tag)
{
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int tmp = 0;
	int ret = XPC_OK;

	int attribute_count = 0;
	const char* tagname = NULL;
	char* duration = NULL;
	char* format = NULL;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);
	if (!strcmp(tagname, XPC_XML_TAG_NAME_SEGMENTS))
	{
		// duration
		duration = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(duration, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_DURATION, duration);
		if (NULL != duration)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", duration);
                        tmp = XPC_parse_period_time_iso8601(tmp_buf);
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%d", tmp);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): cvr duration=%d!\n", __FUNCTION__, __LINE__, tmp);
			if (set_cloud_recorder_video_duration(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr duration!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != duration)
			{
				free(duration);
				duration = NULL;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(VIDEO_DURATION, tmp_buf) != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr duration via  config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
			}
#endif
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_DURATION);
		}
		// format
		format = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(format, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_FORMAT,format);
		if (NULL != format)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", format);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): format=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
			if (!strcmp(tmp_buf, "video/mp2t"))
			{
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", CLOUD_RECORDER_VIDEO_TYPE_TS);
			}
			else
			{
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", CLOUD_RECORDER_VIDEO_TYPE_MP4);
			}
			if (set_cloud_recorder_video_format(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr farmat!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != format)
			{
				free(format);
				format = NULL;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(VIDEO_FORMAT, tmp_buf2) != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr farmat via config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
			}
#endif
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_FORMAT);
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag name [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_TAG_NAME_SEGMENTS);
	}
	return ret;
}

/** @description Parse h264 attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_h264_attr(All_Conf *pconf, xmlNodePtr tag)
{
	int ret = XPC_OK;
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int tmp = 0;

	int attribute_count = 0;
	const char* tagname = NULL;
	char* resolution = NULL;
	char* fps = NULL;
	char* gop = NULL;


	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);
	if (!strcasecmp(tagname, XPC_XML_TAG_NAME_H264))
	{
		//resolution
		resolution = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(resolution, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_RESOLUTION,resolution);
		if (NULL != resolution)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			memset(tmp_buf2, 0, sizeof(tmp_buf2));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", resolution);

			if (!strcmp(tmp_buf,"1280x720"))
			{
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", 4);
			}
			else if (!strcmp(tmp_buf,"640x480"))
			{
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", 3);
			}
			else if (!strcmp(tmp_buf,"320x240"))
			{
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", 2);
			}
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d):resolution=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
			if (set_resolution_in(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL, VIDEO_CODEC_H264, H264_STREAM_INDEX_3) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set resolution!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != resolution)
			{
				free(resolution);
				resolution = NULL;
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d)Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_RESOLUTION);
		}
		//fps
		fps = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(fps, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_FPS,fps);
		if (NULL != fps)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", fps);
			if (atoi(tmp_buf) >= 1 && atoi(tmp_buf) <= 30)
			{
				tmp = atoi(tmp_buf);
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d):fps=%d!\n",__FUNCTION__, __LINE__, tmp);
				memset(tmp_buf2, 0, sizeof(tmp_buf2));
				snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", tmp);
				if (set_frate_in(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL, VIDEO_CODEC_H264, H264_STREAM_INDEX_3) != 0)
				{
                                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set fps!\n", __FUNCTION__, __LINE__);
					ret = XPC_XML_ERROR;
				}
			}

			if(NULL != fps)
			{
				free(fps);
				fps = NULL;
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_FPS);
		}
		//gop
		gop = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(gop, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_GOP,gop);
		if (NULL != gop)
		{
			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "%s", gop);
			tmp = atoi(tmp_buf);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): gop=%d\n", __FUNCTION__, __LINE__, tmp);
			memset(tmp_buf2, 0, sizeof(tmp_buf2));
			snprintf(tmp_buf2, sizeof(tmp_buf2), "%d", tmp);
			if (set_gov_length_in(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL, VIDEO_CODEC_H264, H264_STREAM_INDEX_3) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set gop!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != gop)
			{
				free(gop);
				gop = NULL;
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Dont find XML tag attribute [%s]!\n",__FUNCTION__, __LINE__, XPC_XML_ATTR_GOP);
		}
	}
	return ret;
}

/** @description Parse LBR attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param AbrBitrate Target Adaptive bitrate.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_lbr_attr(All_Conf *pconf, xmlNodePtr tag, AbrTargetBitrate *AbrBitrate)
{

	char* max = NULL;
	char* min = NULL;
	char* low = NULL;
	char* med = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,tagname);
	if (!strcmp(tagname, XPC_XML_TAG_NAME_LBR))
	{
		max = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(max, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_MAX,max);

		min = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
		memset(min, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
		xml_parser->getAttributeValue(tag, XPC_XML_ATTR_MIN,min);
		if ((NULL != max) && (NULL != min))
		{
			// no bitrate
			if (atoi(min) > 0)
			{
				AbrBitrate->no_mot_bitrate = atoi(min);
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): No motion bitrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->no_mot_bitrate);
			}

			// high bitrate
			if (atoi(max) > 0)
			{
				AbrBitrate->high_mot_bitrate = atoi(max);
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): high motion bitrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->high_mot_bitrate);
			}

			// low bitrate
			low = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
			memset(low, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
			xml_parser->getAttributeValue(tag, XPC_XML_ATTR_LOW, low);
			if (NULL != low)
			{
				if (atoi(low) > 0)
				{
					AbrBitrate->low_mot_bitrate = atoi(low);
                                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Low motion bitrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->low_mot_bitrate);
				}
			}
			else
			{
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_LOW);
			}
			// med bitrate
			med = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
			memset(med, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
			xml_parser->getAttributeValue(tag, XPC_XML_ATTR_MED, med);
			if (NULL != med)
			{
				if (atoi(med) > 0)
				{
					AbrBitrate->mid_mot_bitrate = atoi(med);
                                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): mid motion bitrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->mid_mot_bitrate);
				}
			}
			else
			{
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_MED);
			}

			if (AbrBitrate->mid_mot_bitrate <= 0)
			{
				AbrBitrate->mid_mot_bitrate = AbrBitrate->high_mot_bitrate * 70 / 100;
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d) Mid motion bietrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->mid_mot_bitrate);
			}
			if (AbrBitrate->low_mot_bitrate <= 0)
			{
				AbrBitrate->low_mot_bitrate = AbrBitrate->mid_mot_bitrate * 70 / 100;
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d) Low motion bietrate=%d!\n", __FUNCTION__, __LINE__, AbrBitrate->low_mot_bitrate);
			}
		}

		if(NULL != min)
		{
			free(min);
			min = NULL;
		}
		if(NULL != med)
		{
			free(med);
			med = NULL;
		}
		if(NULL != low)
		{
			free(low);
			low = NULL;
		}
		if(NULL != max)
		{
			free(max);
			max = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag name [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_TAG_NAME_LBR);
	}
	return XPC_OK;
}

/** @description Parse events attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param interval Event interval.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_events_attr(All_Conf *pconf, xmlNodePtr tag, int *interval)
{
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int ret = XPC_OK;
        int timeout = 0;

	char* url = NULL;
	char* auth = NULL;
	char* quiet_interval = NULL;
	char* time_out = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,tagname);

	// url
	url = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(url, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_URL,url);
	if (NULL != url)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", url);
		if (0 != strlen(tmp_buf) && strncasecmp(tmp_buf,"http://", strlen("http://")))
		{
			memset(tmp_buf2, 0, sizeof(tmp_buf2));
			// check url whether include mac address, if not, add it to url.
			XPC_append_mac_to_url(tmp_buf2, sizeof(tmp_buf2), tmp_buf);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): events url is [%s]!\n", __FUNCTION__, __LINE__, tmp_buf2);
			if (set_http_url(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set events url!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}
		}
		else
		{
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Events url is NULL!\n", __FUNCTION__, __LINE__);
		}

		if(NULL != url)
		{
			free(url);
			url = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_URL);
	}
	// auth
	auth = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(auth, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUTH, auth);
	if (NULL != auth)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", auth);
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): events auth=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): events auth=hidden!\n", __FUNCTION__, __LINE__);
		if (set_http_auth(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set events auth!\n", __FUNCTION__, __LINE__);
			ret = XPC_XML_ERROR;
		}

		if(NULL != auth)
		{
			free(auth);
			auth = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_AUTH);
	}
	// event quite interval
	quiet_interval = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(quiet_interval, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_QUIET_INTERVAL,quiet_interval);
	if (NULL != quiet_interval)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", quiet_interval);
		*interval = XPC_parse_period_time_iso8601(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): event quiet interval=%d!\n", __FUNCTION__, __LINE__, *interval);

                memset(tmp_buf, 0, sizeof(tmp_buf));
                snprintf(tmp_buf, sizeof(tmp_buf), "%lu", *interval);
                if(RDKC_SUCCESS != rdkc_set_user_setting(EVENT_QUIET_TIME ,tmp_buf))
                {
                   RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","error in setting user event_quite_time\n");
                }
		if(NULL != quiet_interval)
		{
			free(quiet_interval);
			quiet_interval = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_QUIET_INTERVAL);
	}
        // event timeout
	time_out = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(time_out, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_TIMEOUT,time_out);
	if (NULL != time_out)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", time_out);
		timeout = XPC_parse_period_time_iso8601(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): event timeout=%d!\n", __FUNCTION__, __LINE__, timeout);
                memset(tmp_buf, 0, sizeof(tmp_buf));
                snprintf(tmp_buf, sizeof(tmp_buf), "%d", timeout);
                if (set_http_event_timeout(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set event timeout!\n", __FUNCTION__, __LINE__);
			ret = XPC_XML_ERROR;
		}

		if(NULL != time_out)
		{
			free(time_out);
			time_out = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_TIMEOUT);
	}

        return ret;
}

/** @description Parse motion events attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param enabled Motion Events enabled.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_events_motion_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled)
{
	char tmp_buf[XPC_URL_LEN+1];
	char* enable = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	// enabled
	enable = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(enable, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENABLED,enable);
	if (NULL != enable)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", enable);
		if (!strcmp(tmp_buf, "true"))
		{
			*enabled = 1;
		}
		else
		{
			*enabled = 0;
		}
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): motion events enabled=%d!\n", __FUNCTION__, __LINE__, *enabled);

		if(NULL != enable)
		{
			free(enable);
			enable = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENABLED);
	}

	return XPC_OK;
}

/** @description Parse human events attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param enabled Human Events enabled.
 *  @return integer success or failure.
 */
#ifdef _SUPPORT_OBJECT_DETECTION_
int XfinityPollingConfig::XPC_parse_events_human_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled)
{
	char tmp_buf[XPC_URL_LEN+1];
	char* enable = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	// enabled
	enable = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(enable, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENABLED,enable);
	if (NULL != enable)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", enable);

		if (!strcmp(tmp_buf, "true"))
		{
			*enabled = 1;
		}
		else
		{
			*enabled = 0;
		}
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): human events enabled=%d!\n", __FUNCTION__, __LINE__, *enabled);

		if(NULL != enable)
		{
			free(enable);
			enable = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENABLED);
	}

	return XPC_OK;
}
#endif

/** @description Parse Tamper events attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @param enabled Tamper events enabled.
 *  @return integer success or failure.
 */
#ifdef _SUPPORT_TAMPER_DETECTION_
int XfinityPollingConfig::XPC_parse_events_tamper_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled)
{
	char tmp_buf[XPC_URL_LEN+1];
	char* enable = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__, tagname);

	// enabled
	enable = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(enable, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENABLED,enable);
	if (NULL != enable)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", enable);

		if (!strcmp(tmp_buf, "true"))
		{
			*enabled = 1;
		}
		else
		{
			*enabled = 0;
		}
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tamper events enabled=%d!\n", __FUNCTION__, __LINE__, *enabled);

		if(NULL != enable)
		{
			free(enable);
			enable = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENABLED);
	}

	return XPC_OK;
}
#endif

/** @description Parse cvr attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_cvr_attr(All_Conf *pconf, xmlNodePtr tag)
{
	char tmp_buf[XPC_URL_LEN+1];
	char tmp_buf2[XPC_URL_LEN+1];
	int enabled = 0;
	char *p = NULL;
	int ret = XPC_OK;
	int timeout = 0;
#ifdef _SUPPORT_AAC_
//	int audio_enable = 0;
#endif
	int attribute_count = 0;
	const char* tagname = NULL;
	char* cvrEnabled = NULL;
	char* cvrURL = NULL;
	char* cvrAudioEnable = NULL;
	char* cvrTimeout = NULL;
	char* cvrAuth = NULL;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): tag name is [%s]!\n", __FUNCTION__, __LINE__,tagname);

	// enabled
	cvrEnabled = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(cvrEnabled, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_ENABLED,cvrEnabled);
	if (NULL != cvrEnabled)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", cvrEnabled);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): cvr_enabled=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
		if (!strcmp(tmp_buf, "true"))
		{
			if (set_cloud_recorder_enable((char*)"1", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr enabled!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

#ifndef XFINITY_SUPPORT
			if(rdkc_envSet(POLLING_ENABLE, (char*)"1") != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr enabled via Config Manager!\n", __FUNCTION__, __LINE__);
				if(NULL != cvrEnabled)
				{
					free(cvrEnabled);
					cvrEnabled = NULL;
				}
				return RDKC_FAILURE;
			}
#endif
		}
		else
		{
			if (set_cloud_recorder_enable((char*)"0", 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr enabled!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

#ifndef XFINITY_SUPPORT
                        if(rdkc_envSet(POLLING_ENABLE, (char*)"0") != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr enabled via config Manager!\n", __FILE__, __LINE__);
				if(NULL != cvrEnabled)
				{
					free(cvrEnabled);
					cvrEnabled = NULL;
				}
				return RDKC_FAILURE;
                        }
#endif
		}

		if(NULL != cvrEnabled)
		{
			free(cvrEnabled);
			cvrEnabled = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_ENABLED);
	}

	// url
	cvrURL = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(cvrURL, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_URL, cvrURL);
	if (NULL != cvrURL)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", cvrURL);

		if (0 != strlen(tmp_buf) && strncasecmp(tmp_buf,"http://", strlen("http://")))
		{
			memset(tmp_buf2, 0, sizeof(tmp_buf2));
			// check url whether include mac address, if not, add it to url.
			XPC_append_mac_to_url(tmp_buf2, sizeof(tmp_buf2), tmp_buf);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): upload_url=%s!\n", __FUNCTION__, __LINE__, tmp_buf2);
			if (set_cloud_recorder_video_address(tmp_buf2, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
			{
                                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr upload url!\n", __FUNCTION__, __LINE__);
				ret = XPC_XML_ERROR;
			}

			if(NULL != cvrURL)
			{
				free(cvrURL);
				cvrURL = NULL;
			}

#ifndef XFINITY_SUPPORT
                        if(rdkc_envSet(VIDEO_ADDRESS, tmp_buf2) != RDKC_SUCCESS) {
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr upload url via config Manager!\n", __FUNCTION__, __LINE__);
				return RDKC_FAILURE;
                        }
#endif
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_URL);
	}
	// auth
	cvrAuth = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(cvrAuth, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUTH, cvrAuth);
	if (NULL != cvrAuth)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", cvrAuth);

                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): upload_auth=%s!\n", __FUNCTION__, __LINE__, tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): upload_auth=hidden!\n", __FUNCTION__, __LINE__);
		if (set_cloud_recorder_video_auth(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr upload auth!\n", __FUNCTION__, __LINE__);
			ret = XPC_XML_ERROR;
		}

		if(NULL != cvrAuth)
		{
			free(cvrAuth);
			cvrAuth = NULL;
		}
#ifndef XFINITY_SUPPORT
		if(rdkc_envSet(VIDEO_AUTH, tmp_buf) != RDKC_SUCCESS) {
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr upload auth via Config Manager!\n", __FUNCTION__, __LINE__);
			return RDKC_FAILURE;
		}
#endif
	}

        // timeout
	cvrTimeout = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(cvrTimeout, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_TIMEOUT, cvrTimeout);
	if (NULL != cvrTimeout)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", cvrTimeout);

		timeout = XPC_parse_period_time_iso8601(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): cvr timeout=%d!\n", __FUNCTION__, __LINE__, timeout);
                memset(tmp_buf, 0, sizeof(tmp_buf));
                snprintf(tmp_buf, sizeof(tmp_buf), "%d", timeout);
                if (set_cloud_recorder_video_timeout(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr timeout!\n", __FUNCTION__, __LINE__);
			ret = XPC_XML_ERROR;
		}

		if(NULL != cvrTimeout)
		{
			free(cvrTimeout);
			cvrTimeout = NULL;
		}
#ifndef XFINITY_SUPPORT
                if(rdkc_envSet(POLLING_TIMEOUT, tmp_buf) != RDKC_SUCCESS) {
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set cvr timeout via Config Manager!\n", __FUNCTION__, __LINE__);
			return RDKC_FAILURE;
                }
#endif

	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_TIMEOUT);
	}

#ifdef _SUPPORT_AAC_
#if 0
	//parse audio, if audio="true" turn on audio, otherwise turn off audio
	audio_enable = 0;
	cvrAudioEnable = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset(cvrAudioEnable, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUDIO, cvrAudioEnable);
	if (NULL != cvrAudioEnable)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", cvrAudioEnable);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): audio = %s!\n", __FUNCTION__, __LINE__, tmp_buf);
		if (0 == strcmp("true",tmp_buf))
		{
			audio_enable = 1;
		}

		if(NULL != cvrAudioEnable)
		{
			free(cvrAudioEnable);
			cvrAudioEnable = NULL;
		}
	}

	//take action depend on audio_enable
	if (1 == audio_enable)
	{
		//set audio of channel which is sepcially for cvr to enable
		char value_enable[2] = "1";
		if(set_stream_audio_enable_in(value_enable,1,MyXPCMsg,pconf,NULL,VIDEO_CODEC_H264,H264_STREAM_INDEX_3) != 0)
		{
                        RDK_LOG(RDK_LOG_INFO,"%s(%d): Failed to set audio of H264[%d] enable!\n", __FUNCTION__, __LINE__,H264_STREAM_INDEX_3);
                        ret = XPC_XML_ERROR;
		}
		//set audio_mode enable
		if(set_audio_mic_enable_2(value_enable, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
		{
                        RDK_LOG(RDK_LOG_INFO,"%s(%d): Failed to set audio enable!\n", __FUNCTION__, __LINE__);
                        ret = XPC_XML_ERROR;
		}
		//set in_audio_type is AAC
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%d", AUDIO_CODEC_AAC);
		if(set_audio_mic_encoded_type_2(tmp_buf,1,MyXPCMsg,pconf,(LIST *)NULL)!=0)
		{
                        RDK_LOG(RDK_LOG_INFO,"%s(%d): Failed to set audio encode type to AAC!\n", __FUNCTION__, __LINE__);
                        ret = XPC_XML_ERROR;
		}
	}
	else
	{
		//set audio_mode disable
		char value_disable[2] = "0";
                if(set_audio_mic_enable_2(value_disable, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
                {
                        RDK_LOG(RDK_LOG_INFO,"%s(%d): Failed to set audio disable!\n", __FUNCTION__, __LINE__);
                        ret = XPC_XML_ERROR;
                }
	}
#endif
#endif

	return ret;
}

/** @description Parse threshold attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_threshold_attr(All_Conf *pconf, xmlNodePtr tag)
{
	int ret = XPC_OK;
	char tmp_buf[XPC_URL_LEN+1];
	int day2night_threahold = OPEN_IR_LED_THR;
	int night2day_threahold = CLOSE_IR_LED_THR;

	int attribute_count = 0;
	const char* tagname = NULL;
	char* day2night = NULL;
	char* night2day = NULL;

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

	//parse day night switch thresholds
	if (2 != sscanf(pconf->dncfg.dn_threshold,"%d,%d",&day2night_threahold,&night2day_threahold))
	{
		day2night_threahold = OPEN_IR_LED_THR;
		night2day_threahold = CLOSE_IR_LED_THR;
	}

	// day2night
	day2night = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (day2night, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_DAY2NIGHT,day2night);
	if (NULL != day2night)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", day2night);
		day2night_threahold = atoi(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): day2night millilux is %d!\n", __FUNCTION__, __LINE__, day2night_threahold);
		if(NULL != day2night)
		{
			free(day2night);
			day2night = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_DAY2NIGHT);
	}

	// night2day
	night2day = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (night2day, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_NIGHT2DAY, night2day);
	if (NULL != night2day)
	{
		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "%s", night2day);
		night2day_threahold = atoi(tmp_buf);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): night2day millilux is %d!\n", __FUNCTION__, __LINE__, night2day_threahold);
		if(NULL != night2day)
		{
			free(night2day);
			night2day = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_NIGHT2DAY);
	}

	// save to config file
	snprintf(tmp_buf, sizeof(tmp_buf), "%d,%d", day2night_threahold, night2day_threahold);
	if (set_dn_threshold(tmp_buf, 1, MyXPCMsg, pconf, (LIST *)NULL) != 0)
	{
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to set dn threshold!\n", __FUNCTION__, __LINE__);
		ret = XPC_XML_ERROR;
	}

	return ret;
}

/** @description Parse firmware attributes.
 *  @param pconf Config settings.
 *  @param tag xml_list tag.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_parse_firmware_attr(All_Conf *pconf, xmlNodePtr tag)
{
        char cfg_ver[XFINITY_FW_VERSION_LEN];
        char fw_ver[XFINITY_FW_VERSION_LEN];
        char fw_url[XPC_URL_LEN+1];
        char fw_auth[XPC_URL_LEN+1];
        char fw_force[XPC_URL_LEN+1];
        char tmp_buf[512+1];
 
	char t1[50] = "auto_upgrade" ;
	char t2[50] = "tflash" ;
	char t3[50] = "upgrade.cgi" ;

        int ret = XPC_OK;
        int flag = 0;

	char* version = NULL;
	char* source = NULL;
	char* auth = NULL;
	char* force = NULL;
	const char* tagname = NULL;
	int attribute_count = 0;

	if (!access(UPG_FILE, F_OK)|| GetAppsNum((char*)t1)>0 || GetAppsNum((char*)t2)>0 || GetAppsNum((char*)t3)>0)
        {
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Device is busy!\n", __FUNCTION__, __LINE__);
                return XPC_OK;
        }

	tagname = xml_parser->getTagName(tag);
	attribute_count = xml_parser->getAttributeCount(tag);
	if (XPC_ATTRIBUTE_ZERO_COUNT == attribute_count)
	{
                RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):received an empty tag, tag[%s]!\n", __FUNCTION__, __LINE__,tagname);
		return XPC_OK;
	}

        // version
	version= (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (version, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_VERSION, version);
	if (NULL != version)
	{
		memset(cfg_ver,0,sizeof(cfg_ver));
		snprintf(cfg_ver, sizeof(cfg_ver), "%s", version);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): config server's version is %s!\n", __FUNCTION__, __LINE__, cfg_ver);
		if(NULL != version)
		{
			free(version);
			version = NULL;
		}
        }
        else
        {
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_VERSION);
        }
        // source
	source = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (source, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_SOURCE, source);
	if (NULL != source)
	{
		memset(fw_url, 0, sizeof(fw_url));
		snprintf(fw_url, sizeof(fw_url), "%s", source);
                fw_url[XPC_URL_LEN]=0;

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): source=%s!\n", __FUNCTION__, __LINE__, fw_url);
		if (!strncasecmp(fw_url,"http://", strlen("http://")))
		{
			memset(fw_url, 0, sizeof(fw_url));
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): URL start with \"http://\", do nothing!\n", __FUNCTION__, __LINE__);
		}
		if(NULL != source)
		{
			free(source);
			source = NULL;
		}
        }
        else
        {
	        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_SOURCE);
        }
	// auth
	auth = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (auth, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_AUTH, auth);
	if (NULL != auth)
	{
		memset(fw_auth, 0, sizeof(fw_auth));
		snprintf(fw_auth, sizeof(fw_auth), "%s", auth);
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): fw download auth=%s!\n", __FUNCTION__, __LINE__, fw_auth);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): fw download auth=hidden!\n", __FUNCTION__, __LINE__);
		if(NULL != auth)
		{
			free(auth);
			auth = NULL;
		}
	}
	else
	{
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_AUTH);
	}
        // force
	force = (char*)malloc(XPC_ATTRIBUTE_VALUE_MAX_LEN);
	memset (force, 0, XPC_ATTRIBUTE_VALUE_MAX_LEN);
	xml_parser->getAttributeValue(tag, XPC_XML_ATTR_FORCE, force);
	if (NULL != force)
	{
		memset(fw_force, 0, sizeof(fw_force));
		snprintf(fw_force, sizeof(fw_force), "%s", force);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): force=%s!\n", __FUNCTION__, __LINE__, fw_force);
		if(NULL != force)
		{
			free(force);
			force = NULL;
		}
        }
        else
        {
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Don't find XML tag attribute [%s]!\n", __FUNCTION__, __LINE__, XPC_XML_ATTR_FORCE);
        }

        if (strlen(cfg_ver) && strlen(fw_url) && strlen(fw_force))
        {
                get_fw_version((char*)fw_ver, sizeof(fw_ver));
                if (!strcmp(fw_force, "true"))
                {
                        if (strcmp(fw_ver,cfg_ver))
                        {
                                 flag = 1;
                                 RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): force=%s fw_ver=%s cfg_ver=%s!\n", __FUNCTION__, __LINE__, fw_force,fw_ver,cfg_ver);
                        }
                }
                else if (!strcmp(fw_force,"false"))
                {
                        // always think V3.0.02.xx is bigger than V1.0.02.xx
                        if (strcmp(fw_ver,cfg_ver) < 0)
                        {
                                flag = 1;
                                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): force=%s fw_ver=%s cfg_ver=%s!\n", __FUNCTION__, __LINE__, fw_force,fw_ver,cfg_ver);
                        }
                }
        }
        if (1 == flag)
        {
		if (strlen(fw_auth))
		{
		//	snprintf(tmp_buf,sizeof(tmp_buf)-1, "/usr/local/bin/auto_upgrade %s %d NULL \"%s\" >/dev/null 2>/dev/null &", fw_url, 2000, fw_auth);
		        v_secure_system("/usr/local/bin/auto_upgrade %s %d NULL \"%s\" >/dev/null 2>/dev/null &", fw_url, 2000, fw_auth);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): command:%s!\n", __FUNCTION__, __LINE__, "/usr/local/bin/auto_upgrade %s %d NULL \"%s\" >/dev/null 2>/dev/null &", fw_url, 2000, fw_auth);
                }
		else
		{
			//snprintf(tmp_buf,sizeof(tmp_buf)-1, "/usr/local/bin/auto_upgrade %s %d >/dev/null 2>/dev/null &", fw_url, 2000);
		        v_secure_system("/usr/local/bin/auto_upgrade %s %d >/dev/null 2>/dev/null &", fw_url, 2000);
                        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): command:%s!\n", __FUNCTION__, __LINE__, "/usr/local/bin/auto_upgrade %s %d >/dev/null 2>/dev/null &", fw_url, 2000);
                }
                //system(tmp_buf);
                //RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): command:%s!\n", __FUNCTION__, __LINE__, tmp_buf);
        }

        return ret;


}

/**
 * @description This function is used to get retry attribute.
 *
 * @param void.
 *
 * @return void.
 */

void XfinityPollingConfig::getCloudRecorderRetryAttribute()
{
	const char* waitingInt = rdkc_envGet(XPC_RETRY_MIN_INTERVAL);
	const char* minInt = rdkc_envGet(XPC_RETRY_MIN_INTERVAL);
	const char* maxInt = rdkc_envGet(XPC_RETRY_MAX_INTERVAL);
	const char* factor = rdkc_envGet(XPC_RETRY_FACTOR);

	if(NULL != waitingInt)
	{
		waitingInterval = atof(waitingInt);
		waitingInterval = (0 == waitingInterval) ? DEFAULT_MIN_INTERVAL : waitingInterval;
		defaultInterval = waitingInterval;
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

	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s: %d: Retry min interval=%d, max interval=%d, retry factor=%.2f!\n", __FILE__, __LINE__,minInterval, maxInterval, retryFactor);
}

/**
 * @description This function is used to sleep for some waitingInterval  before next retry.
 *
 * @param void.
 *
 * @return void.
 */
void XfinityPollingConfig::retryAtExpRate()
{
	RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s: %d: Waiting for %d seconds!\n", __FILE__, __LINE__, (int)waitingInterval);
	sleep((int)waitingInterval);
        
	waitingInterval *= retryFactor;
	
	if(waitingInterval > maxInterval)
	{
		waitingInterval = defaultInterval;
	}
}

/** @description Wait for Polling interval.
 *  @param CloudRecorderInfo CloudRecorderInfo read from config file.
 *  @param start_polling_time Time at which polling started.
 *  @return void.
 */
#ifdef XFINITY_SUPPORT
void XfinityPollingConfig::XPC_wait_polling_interval(CloudRecorderConf CloudRecorderInfo, time_t start_polling_time)
#else
void XfinityPollingConfig::XPC_wait_polling_interval(RdkCCloudRecorderConf CloudRecorderInfo, time_t start_polling_time)
#endif
{
        time_t current_time = 0;

	current_time = sc_linear_time(NULL);
        if (CloudRecorderInfo.polling_interval + start_polling_time > current_time)
        {
        	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Polling interval is %d. Need sleep %d seconds to do next polling.\n", __FILE__, __LINE__, CloudRecorderInfo.polling_interval, (CloudRecorderInfo.polling_interval + start_polling_time - current_time));
                sleep(CloudRecorderInfo.polling_interval + start_polling_time - current_time);
	}
}
int XfinityPollingConfig::XPC_read_polling_seq()
{
	int fd = -1;
	int sequence_no = 0;
	int ret_sequence_no = -1;
	int retry = 3;

	while (retry > 0)
	{
		if (0 == access(XFINITY_POLLING_SEQ_FILE, F_OK))
		{
			// File exist
			fd = open(XFINITY_POLLING_SEQ_FILE, O_RDWR, 0644);
			if (fd >= 0)
			{
				read(fd, &sequence_no, sizeof(sequence_no));
				ret_sequence_no = sequence_no;
				sequence_no++;
				lseek(fd, 0, SEEK_SET);
				write(fd, &sequence_no, sizeof(sequence_no));
				close(fd);
				break;
			}
			else
			{
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): File error.\n", __FILE__, __LINE__);
				sleep(XPC_ERR_SLEEP_TIME);
			}
		}
		else
		{
			// File is not exist, because X-seq number begain with 0, so set ret_sequence_no = 0 and write 1 into file.
			fd = open(XFINITY_POLLING_SEQ_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
			if (fd >= 0)
			{
				ret_sequence_no = 0;
				sequence_no = 1;
				write(fd, &sequence_no, sizeof(sequence_no));
				close(fd);
				break;
			}
			else
			{
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): File error.\n", __FILE__, __LINE__);
				sleep(XPC_ERR_SLEEP_TIME);
			}
		}
		retry --;
	}
	return ret_sequence_no;
}
/** @description Add http headers
 *  @param auth, count(no of retries).
 *  @return void.
 */
void  XfinityPollingConfig::XPC_add_http_header(char *auth, int count)
{
	char request_uuid[XFINITY_REQUEST_UUID_LEN];
	char fw_ver[XFINITY_FW_VERSION_LEN];
        unsigned char macaddr[MAC_ADDR_LEN];
	char mac_string[XPC_MAC_STRING_LEN+1];
	char userAgent[XPC_PARAM_LEN];
	char xSeq[XPC_PARAM_LEN];
	char cnt[XPC_PARAM_LEN];
	int x_seq = 0;

	memset(fw_ver, 0, sizeof(fw_ver));
	memset(macaddr, 0, sizeof(macaddr));
	memset(userAgent, 0, sizeof(userAgent));
	memset(mac_string, 0, sizeof(mac_string));
	memset(request_uuid, 0, sizeof(request_uuid));
	memset(xSeq, 0, sizeof(xSeq));
	memset(cnt, 0, sizeof(cnt));
	/* Prepare the HTTP header */

	if (strlen(auth) != 0)
	{
		//RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Connect auth is [%s]!\n", __FILE__, __LINE__, auth);
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Connect auth is [hidden]!\n", __FILE__, __LINE__);
		http_client->addHeader((char*)"Authorization",auth);
	}
#ifdef USE_MFRLIB
                char modelName[128];
                mfrSerializedData_t stdata = {NULL, 0, NULL};
                mfrSerializedType_t stdatatype = mfrSERIALIZED_TYPE_MODELNAME;
                
                if(mfrGetSerializedData(stdatatype, &stdata) == mfrERR_NONE)
                {       
                        strncpy(modelName,stdata.buf,stdata.bufLen);
			modelName[stdata.bufLen] = '\0';
                        RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Model Name = %s\n", __FILE__, __LINE__,modelName);
                        
                        
                        if (stdata.freeBuf != NULL)
                        {       
                                stdata.freeBuf(stdata.buf);
                                stdata.buf = NULL;
                        }
                }
                else    
                {       
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): GET ModelName failed.\n", __FILE__, __LINE__);
                
                }
#endif
	if (0 == get_mac_address(macaddr) && 0 == get_fw_version(fw_ver, sizeof(fw_ver)))
	{

	        transcode_mac_to_string_by_separator(macaddr, '\0', mac_string, XPC_MAC_STRING_LEN+1, 0);
#ifdef USE_MFRLIB
                snprintf(userAgent, sizeof(userAgent), "Sercomm %s %s %s", modelName, fw_ver, mac_string);
#else
		snprintf(userAgent, sizeof(userAgent), "Sercomm %s %s %s", SC_MODEL_NAME, fw_ver, mac_string);
#endif
		http_client->addHeader((char*)"User-Agent",userAgent);
	}
	else
	{
#ifdef USE_MFRLIB
		snprintf(userAgent, sizeof(userAgent), "Sercomm %s", modelName);
#else
		snprintf(userAgent, sizeof(userAgent), "Sercomm %s", SC_MODEL_NAME);
#endif
		http_client->addHeader("User-Agent",userAgent);
	}
	x_seq = XPC_read_polling_seq();
	if (-1 != x_seq)
	{
		snprintf(xSeq, sizeof(xSeq), "%d", x_seq);
		http_client->addHeader("X-Seq",xSeq);
	}
	else
	{
		//-1 means error happened when reading X-seq number from file.
		snprintf(xSeq, sizeof(xSeq), "%X", x_seq);
		http_client->addHeader("X-Seq",xSeq);
	}

	if (0 == GenarateXfinityRequestUUID(request_uuid, sizeof(request_uuid), XFINITY_REQUEST_POLLING_CONFIG))
	{
		http_client->addHeader(XFINITY_REQUEST_UUID, request_uuid);
	}
	if (count > 0)
        {
		snprintf(cnt, sizeof(cnt), "%d", count);
		http_client->addHeader(XFINITY_RETRY,cnt);
	}
	http_client->addHeader("Connection","close");

}
/** @description free data receiving variable.
 *  @param receiving data variable.
 *  @return void.
 */
void XfinityPollingConfig::freeResource(char *receiveData)
{
        if(NULL != receiveData)
        {
                free(receiveData);
                receiveData = NULL;
        }

}

/** @description Is the partner Comcast?
 *  @param void.
 *  @return true if partner is comcast, else false.
 */
bool XfinityPollingConfig::isComcastPartner()
{
        char partnerId[BUF_SIZE]={""};
        memset(partnerId, 0, BUF_SIZE);

	/* Partner is Comcast if partner id is not available in usr_config or if partner id in usr_config is comcast */
	if( (RDKC_SUCCESS != rdkc_get_user_setting(PARTNER_ID, partnerId)) ||
	    ( (RDKC_SUCCESS == rdkc_get_user_setting(PARTNER_ID, partnerId)) && (0 == strcmp(partnerId,"comcast")) )
	  ) {
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xfinity polling config Partner ID: %s\n", __FILE__, __LINE__, partnerId);
		return true;
	}
	else {
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xfinity polling config Partner ID: %s\n", __FILE__, __LINE__, partnerId);
		return false;
	}
}

/** @description Do the polling process.
 *  @param void.
 *  @return void.
 */
void XfinityPollingConfig::XPC_do_polling()
{
        int retry_time = 0;
        char url_tmp[XPC_URL_LEN+1];
        char recovery_polling_server_auth[] = {"\0"};
#ifdef XFINITY_SUPPORT
        CloudRecorderConf CloudRecorderInfo;
#else
	RdkCCloudRecorderConf CloudRecorderInfo;
#endif
	
        time_t start_polling_time = 0;
        time_t current_time = 0;
        int curlCode = 0;
	int ret = -1;
	char *receiveData = NULL;
	int error_count = 0;

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xfinity polling config process start at %lu!\n", __FILE__, __LINE__, sc_linear_time(NULL));

        // Read cloud recorder server info firstly
#ifdef XFINITY_SUPPORT
        memset(&CloudRecorderInfo, 0, sizeof(CloudRecorderConf));
        if (ReadCloudRecorderInfo(&CloudRecorderInfo))
#else
        memset(&CloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
	if (RdkCReadCloudRecorderSection (&CloudRecorderInfo) == RDKC_FAILURE)
#endif
        {
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Read cloud recorder configuration error!\n", __FUNCTION__, __LINE__);
                XPC_exit();
        }

	while (!XPC_get_term_flag())
        {
		memset(url_tmp, 0, sizeof(url_tmp));
		//Check for the polling auth lenght, if 0 use default url.
		if (0 != strlen(CloudRecorderInfo.polling_auth))
                {
			strncpy(url_tmp, CloudRecorderInfo.polling_url, sizeof(url_tmp));
                        strncpy(polling_server_auth, CloudRecorderInfo.polling_auth, sizeof(polling_server_auth));
                }
                else
                {
                        strncpy(url_tmp, DEF_CLOUD_RECORDER_POLLING_URL, sizeof(url_tmp));
                        memset(polling_server_auth, 0, sizeof(polling_server_auth));
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Polling auth not available, redirecting to default url!, \n", __FUNCTION__, __LINE__);
                }

		XPC_append_mac_to_url(polling_server_url, sizeof(polling_server_url), url_tmp);
                XPC_append_mac_to_url(recovery_polling_server_url, sizeof(recovery_polling_server_url), DEFAULT_POLLING_URL);    

		current_time = sc_linear_time(NULL);
		if ((!forced_polling) && (start_polling_time != 0) && (CloudRecorderInfo.polling_interval  + start_polling_time > current_time))
                {
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Need wait enough time to polling next config.\n", __FILE__, __LINE__);
                        XPC_wait_polling_interval(CloudRecorderInfo,start_polling_time);
			continue;
                }


                if (!strncasecmp(polling_server_url,"http://", strlen("http://")))
                {
                        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d):Polling config not support http.(URL:%s)\n", __FILE__, __LINE__, polling_server_url);
                        XPC_wait_polling_interval(CloudRecorderInfo,start_polling_time);
                        continue;
                }

		/* recv response from server using curl */
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Receiving content from server using curl \n", __FUNCTION__, __LINE__);
		start_polling_time = sc_linear_time(NULL);

		while((waitingInterval < CloudRecorderInfo.polling_interval) && (!XPC_get_term_flag()))
		{
			if(NULL != http_client) {

				/* Open the connection and get the content from server */

				/* if we get curl code 6 or 7 consecutively, then use recovery polling url*/
				if( error_count >= XPC_MAX_ERROR_COUNT ) {
					error_count = XPC_MAX_ERROR_COUNT;
                                       // strncpy(recovery_polling_server_url, DEFAULT_POLLING_URL, sizeof(DEFAULT_POLLING_URL));
                                      //  recovery_polling_server_url[sizeof(DEFAULT_POLLING_URL)-1] = '\0';  
					RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Using recovery url %s to receive data from server!! \n",__FILE__, __LINE__,recovery_polling_server_url);
					http_client->open(recovery_polling_server_url);
					XPC_add_http_header(polling_server_auth, retry_time);
					receive_content = http_client->get(recovery_polling_server_url, &curlCode);
				}
				else {
					http_client->open(polling_server_url);
					XPC_add_http_header(polling_server_auth, retry_time);
					receive_content = http_client->get(polling_server_url, &curlCode);
				}
				if(!curlCode) {
					RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Data Received  Successfully \n", __FUNCTION__, __LINE__);

					receiveData = (char*)malloc(strlen(receive_content)+1);
					memset(receiveData, 0, strlen(receive_content)+1);
					strncpy(receiveData, receive_content, strlen(receive_content)+1);

					/* Close the connection */
					http_client->close();
					forced_polling = 0;
					waitingInterval = minInterval;
					error_count = 0;
					break;
				}
				else {
					RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Failed to receive data from server curlcode=%d \n",__FILE__, __LINE__,curlCode);

					// if we get curl code 6 or 7 consecutively, then monitor the error
					if( (CURLE_COULDNT_RESOLVE_HOST == curlCode ) || (CURLE_COULDNT_CONNECT == curlCode) ) {
						error_count++;
					}
					else {
						error_count = 0;
					}

					/* Close the connection */
					http_client->close();

					RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d):  Retrying ........ \n", __FILE__, __LINE__);
					retry_time++;
					retryAtExpRate();
					continue;
				}
			}
		}
		
		// using standard output library to print the data received as the length is more which can't be printed fully using rdklogger
		//std::cout << "Received data [" << receiveData << "] from server!!\n";
		//RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Receive data [%s] from server.\n", __FILE__, __LINE__, receiveData);

                // Check if the response in HTTP layer
		if(NULL != receiveData)
		{
			xml_content = XPC_parse_response(receiveData);
			if (NULL == xml_content)
			{
				RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): HTTP response is not correct. [%s]\n", __FILE__, __LINE__, receiveData);
				freeResource(receiveData);
				retry_time++;
				XPC_wait_polling_interval(CloudRecorderInfo,start_polling_time);
				continue;
			}
		}
		//std::cout << "Received xml content  from config server [" << xml_content << " ] from server!! and length is "<< strlen(xml_content)<<"\n";

		// Apply new parameter from server
// #ifndef XFINITY_SUPPORT
// 		if(RDKC_SUCCESS != config_init()) {
//                         RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): Error loading config manager.\n", __FILE__, __LINE__);
//                 }
// #endif
                if (XPC_OK != XPC_apply_config())
                {
                        retry_time++;

                        XPC_wait_polling_interval(CloudRecorderInfo,start_polling_time);
			freeResource(receiveData);
                        continue;
                }

// #ifndef XFINITY_SUPPORT
// 		config_release();
// #endif
		freeResource(receiveData);
                // After receive config file from server correctly, set retry_time = 0
                retry_time = 0;

                // Read cloud recorder server info every time
                // Must add read cloud recorder server info here because we should sleep new interval value

#ifdef XFINITY_SUPPORT
                memset(&CloudRecorderInfo, 0, sizeof(CloudRecorderConf));
        	if (ReadCloudRecorderInfo(&CloudRecorderInfo))
#else
                memset(&CloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
        	if (RdkCReadCloudRecorderSection (&CloudRecorderInfo) == RDKC_FAILURE)
#endif
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Read cloud recorder configuration error!\n", __FUNCTION__, __LINE__);
                        break;
	        }
	}
}

/** @description Control the XPC process.
 *  @param process Name of the process.
 *  @return integer success or failure.
 */
int XfinityPollingConfig::XPC_process_control(char* process)
{
        pid_t pid = 0;
        int file_fd = 0;

        file_fd = XPC_check_filelock((char*)LOCK_FILENAME_XPC);
        if ( -2 == file_fd)
        {
                file_fd = XPC_check_filelock((char*)LOCK_FILENAME_XPC);
        }

        if (file_fd < 0)
        {
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): %s run error!\n", __FILE__, __LINE__, process);
                XPC_exit();
		return XPC_ERROR;
        }

        pid = getpid();
        write(file_fd, &pid, sizeof(pid));
        close(file_fd);

	return XPC_OK;
}

/** @description Exit Xfinity Polling Config.
 *  @param void.
 *  @return void.
 */
void XfinityPollingConfig::XPC_exit()
{
        unlink(LOCK_FILENAME_XPC);
        RDK_LOG(RDK_LOG_WARN,"LOG.RDK.CVRPOLL","%s(%d): xfinity_polling_config exit.\n", __FILE__, __LINE__);
}

int main(int argc, char *argv[]) {

        /* ENABLING RDK LOGGER */
        rdk_logger_init("/etc/debug.ini");

	/* Create XfinityPollingConfig object */
	XfinityPollingConfig *xpc = new XfinityPollingConfig();

	/* Init Xfinity Polling Config */
	if (NULL != xpc) {
		if( xpc->XPC_init(argv[0]) < 0) {
			return 0;
		}
	}
	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xfinity polling config process start sleep at %lu!\n", __FILE__, __LINE__, sc_linear_time(NULL));
	if (argc > 1)
	{
		// Wait for a period time get from parameter
		sleep(atoi(argv[1]));
	}
	else
	{
		// Wait for a random time between 0 ~ 60 seconds as request by customer
		sleep(random()%XPC_WAIT_RANDOM_TIME_MAX);
	}
	/* Start the polling process which never returns */
        if( NULL != xpc )
	{
		xpc->XPC_do_polling();
	}

        if( NULL != xpc )
	{
		xpc->XPC_exit();
		delete xpc;
		xpc = NULL;
	}

	return 0;

}
