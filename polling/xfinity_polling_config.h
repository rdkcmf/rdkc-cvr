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
#ifndef  _XFINITY_POLLING_CONFIG_H_
#define  _XFINITY_POLLING_CONFIG_H_

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <fstream>
#include "HttpClient.h"
#include "xmlParser.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "dev_config.h"
#include "sc_def_value.h"
#include "main.h"
#include "ls_ctrl.h"
#include "cgi_image.h"
#include "cgi_lan.h"
#include "cgi_event.h"


#ifndef XFINITY_SUPPORT
#include "rdkc_config_sections.h"
#endif
#ifdef USE_MFRLIB
#include "mfrApi.h"
#endif

#ifdef __cplusplus
}
#endif

#define DEFAULT_POLLING_URL   "https://config-cvr.g.xfinityhome.com/config"
#define XPC_MAX_ERROR_COUNT			5
#define XPC_PARAM_LEN				128
#define XPC_URL_LEN				1024
#define XPC_MAC_STRING_LEN			12
#define XPC_WAIT_RANDOM_TIME_MAX		60
#define XPC_ENVETN_INTERVAL_DEF			30
#define XPC_DATA_LEN			        4096
#define XPC_ATTRIBUTE_ZERO_COUNT		0
#define XPC_ATTRIBUTE_VALUE_MAX_LEN		512
#define XPC_ERR_SLEEP_TIME			1

#define DEFAULT_MIN_INTERVAL 			1
#define DEFAULT_MAX_INTERVAL 			20
#define DEFAULT_RETRY_FACTOR 			2

#define RDKC_FAILURE                           -1
#define RDKC_SUCCESS                    	0
#define BUF_SIZE				512

#define XPC_EVENT_NAME_MOT_STR			"XFINITY_MOTION"
#define XPC_EVENT_NAME_MOT_PEOPLE_STR		"XFINITY_MOTION_PEOPLE"
#define XPC_EVENT_NAME_MOT_TAMPER_STR		"XFINITY_MOTION_TAMPER"

#define XPC_XML_TAG_NAME_CONFIG			"config"
#define XPC_XML_TAG_NAME_SYSTEM			"system"
#define XPC_XML_TAG_NAME_LUX			"lux"
#define XPC_XML_TAG_NAME_THRESHOLD		"threshold"
#define XPC_XML_TAG_NAME_CVR			"cvr"
#define XPC_XML_TAG_NAME_EVENTS			"events"
#define XPC_XML_TAG_NAME_MOTION			"motion"
#define XPC_XML_TAG_NAME_HUMAN			"human"
#define XPC_XML_TAG_NAME_TAMPER			"tamper"
#define XPC_XML_TAG_NAME_DETECTION		"detection"
#define XPC_XML_TAG_NAME_SEGMENTS		"segments"
#define XPC_XML_TAG_NAME_LBR			"LBR"
#define XPC_XML_ATTR_INTERVAL			"interval"
#define XPC_XML_ATTR_URL			"url"
#define XPC_XML_ATTR_ENABLED			"enabled"
#define XPC_XML_ATTR_AUTH			"auth"
#define XPC_XML_ATTR_DURATION			"duration"
#define XPC_XML_ATTR_FORMAT			"format"
#define XPC_XML_ATTR_QUIET_INTERVAL		"quietInterval"
#define XPC_XML_ATTR_MIN			"min"
#define XPC_XML_ATTR_LOW			"low"
#define XPC_XML_ATTR_MED			"med"
#define XPC_XML_ATTR_MAX			"max"
#define XPC_XML_TAG_NAME_H264			"H264"
#define XPC_XML_ATTR_RESOLUTION			"resolution"
#define XPC_XML_ATTR_FPS			"fps"
#define XPC_XML_ATTR_GOP			"gop"
#define XPC_XML_ATTR_ENV			"env"
#define XPC_XML_ATTR_SENSITIVITY		"sensitivity"
#define XPC_XML_TAG_NAME_METRICS		"metrics"
#define XPC_XML_TAG_NAME_DATASET1		"dataset1"
#define XPC_XML_TAG_NAME_DATASET2		"dataset2"
#define XPC_XML_TAG_NAME_DATASET3		"dataset3"
#define XPC_XML_TAG_NAME_DISABLED		"disabled"
#define XPC_XML_TAG_NAME_MODEL			"model"
#define XPC_XML_TAG_NAME_SERIAL			"serial"
#define XPC_XML_TAG_NAME_MAC			"mac"
#define XPC_XML_TAG_NAME_FIRMWARE		"firmware"
#define XPC_XML_TAG_NAME_WIRED			"wired"
#define XPC_XML_TAG_NAME_SSID			"ssid"
#define XPC_XML_TAG_NAME_RADIO			"radio"
#define XPC_XML_TAG_NAME_STREAM			"stream"
#define XPC_XML_TAG_NAME_CPU			"cpu"
#define XPC_XML_TAG_NAME_MEMORY			"memory"
#define XPC_XML_TAG_NAME_SNR			"snr"
#define XPC_XML_TAG_NAME_RSSI			"rssi"
#define XPC_XML_TAG_NAME_INDOOR			"indoor"
#define XPC_XML_TAG_NAME_OUTDOOR		"outdoor"
#define XPC_XML_ATTR_DAY2NIGHT			"day2night"
#define XPC_XML_ATTR_NIGHT2DAY			"night2day"
#define XPC_XML_ATTR_VERSION                    "version"
#define XPC_XML_ATTR_SOURCE                     "source"
#define XPC_XML_ATTR_FORCE                      "force"
#define XPC_XML_ATTR_TIMEOUT                    "timeout"
#ifdef  _SUPPORT_AAC_
#define XPC_XML_ATTR_AUDIO			"audio"
#endif

enum XPC_STATUS
{
	XPC_ERROR = -1,
	XPC_OK = 0,
	XPC_FAIL,
	XPC_CONNECT_ERR,
	XPC_SEND_ERR,
	XPC_RESPONSE_ERROR,
	XPC_XML_ERROR,
	XPC_MAX
};

class XfinityPollingConfig {

public:
        XfinityPollingConfig();
        ~XfinityPollingConfig();

	int XPC_init(char* process);
        void XPC_do_polling();
        void XPC_exit();

private:
        static void XPC_signal_handler(int s);
        static void XPC_register_signal_handler(void);
	static volatile sig_atomic_t XPC_get_term_flag();
        int XPC_process_control(char* process);
        int XPC_check_filelock(char *fname);
#ifdef XFINITY_SUPPORT
	All_Conf *g_pconf;
	void XPC_wait_polling_interval(CloudRecorderConf CloudRecorderInfo, time_t start_polling_time);
#else
	void XPC_wait_polling_interval(RdkCCloudRecorderConf CloudRecorderInfo, time_t start_polling_time);
#endif
	char *XPC_parse_response(char *buff);
	long XPC_parse_period_time_iso8601(char *period_time);
	int XPC_parse_config_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_apply_config();
	static int XPC_read_polling_seq();
	void XPC_add_http_header(char *auth, int count);
	int XPC_parse_motion_segment(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_metrics_segment(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_events_segment(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_cvr_segment(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_system_segment(All_Conf *pconf, xmlNodePtr tag);
	int XPC_append_mac_to_url(char *out_url, int len, char *in_url);
	int XPC_save_event_entry(All_Conf *pconf, int enable_flag, int interval, EventType event_type);
	int XPC_parse_motion_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_dataset_attr(All_Conf *pconf, xmlNodePtr tag, int index);
	int XPC_parse_dataset1_disabled(All_Conf *pconf, xmlNodePtr dataset1_tag, int *enable_flag);
	int XPC_parse_dataset2_disabled(All_Conf *pconf, xmlNodePtr dataset2_tag, int *enable_flag);
	int XPC_parse_dataset3_disabled(All_Conf *pconf, xmlNodePtr dataset3_tag, int *enable_flag);
	int XPC_parse_segments_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_h264_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_lbr_attr(All_Conf *pconf, xmlNodePtr tag, AbrTargetBitrate *AbrBitrate);
	int XPC_parse_events_attr(All_Conf *pconf, xmlNodePtr tag, int *interval);
	int XPC_parse_events_motion_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled);
	int XPC_parse_events_human_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled);
	int XPC_parse_cvr_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_events_tamper_attr(All_Conf *pconf, xmlNodePtr tag, int *enabled);
	int XPC_parse_threshold_attr(All_Conf *pconf, xmlNodePtr tag);
	int XPC_parse_firmware_attr(All_Conf *pconf, xmlNodePtr tag);
	void getCloudRecorderRetryAttribute();
	void retryAtExpRate();
	void freeResource(char *receiveData);
	bool isComcastPartner();

	HttpClient *http_client;
	char *receive_content;
	char *xml_content;
	XmlParser *xml_parser;
        char xml_parser_content[XPC_DATA_LEN];
        char polling_server_url[XPC_URL_LEN+1];
        char polling_server_auth[XFINITY_AUTH_LEN+1];
        char recovery_polling_server_url[XPC_URL_LEN+1]; // recovery url if corrupted
	char MyXPCMsg[CGI_ERR_MSG_SIZE_MAX];
	static double waitingInterval;
	static double defaultInterval;
        static int minInterval;
        static int maxInterval;
	static double retryFactor;
	static volatile sig_atomic_t term_flag;
	static volatile sig_atomic_t forced_polling;
};

#endif

