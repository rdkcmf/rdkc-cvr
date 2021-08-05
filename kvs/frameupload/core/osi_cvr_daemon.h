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
#ifndef _CVRDAEMON_H_
#define _CVRDAEMON_H_

#include <iostream>
#include <time.h>
#include <errno.h>
#include <cmath>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sstream>
#include <sys/time.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <list>
#include <thread>
#include "kvsuploadCallback.h"
#include "RFCCommon.h"

#ifdef _HAS_XSTREAM_
#include "xStreamerConsumer.h"
#else
#include "RdkCVideoCapturer.h"
#include "RdkCPluginFactory.h"
#endif //_HAS_XSTREAM_

#ifdef RTMSG
#include "rtConnection.h"
#include "rtLog.h"
#include "rtMessage.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif
#include "sysUtils.h"
#include "rdk_debug.h"
#include "dev_config.h"
#include "polling_config.h"
#ifdef __cplusplus
}
#endif
#define RDKC_STREAM_FLAG_VIDEO          0x01
#define RDKC_STREAM_FLAG_AUDIO          0x02
#define RDKC_STREAM_FLAG_PADDING        0x04
#define RDKC_STREAM_FLAG_ABSTIMESTAMP   0x08
#define MAX_ENCODE_STREAM_NUM	        4
#define VIDEO_DURATION_MAX              60	// 60 senconds
#define XFINITY_POLLING_CONFIG_TIMEOUT  90
#ifndef XCAM2
#define  DEF_CVR_CHANNEL	        3	// stream D to be used by default
#else
#define  DEF_CVR_CHANNEL		1	// to fix the higher clip size due to higher fps(RDKC-4436), use 4th stream for cvr. Need to revert back to 1(stream B) for xCam2 once we fix the higher clip issue.
#endif
#define CVR_FAILURE			-1
#define CVR_SUCCESS			0
#define CVR_CLIP_PATH                   "/tmp/cvr"
#define CVR_CLIP_DURATION              	15   //seconds
#define CVR_CLIP_NUMBER                	2
#define CVR_FILE_PATH_LEN               256
#define HW_TIMER			((const char*)"/proc/ambarella/ambarella_hwtimer")
#define AUDIO_DURATION			60
#define LOCK_FILENAME_CVR_DAEMON        "/tmp/cvr_daemon.lock"
#define CVR_AUDIO_UNKNOWN      		-1
#define CVR_AUDIO_DISABLED      	0
#define CVR_AUDIO_ENABLED       	1
#define DEFAULT_EVT_TSTAMP		0
#define DEFAULT_EV_QUIET_TIME		30
#define XFINITY_POLLING_SEQ_FILE   "/tmp/.xfinity_polling_sequence"

// to dump the h264 file into a file, please make the DEBUG_DUMP_H264 to '1'
#define DEBUG_DUMP_H264 0

using namespace std;
typedef enum cvr_clip_status
{
    CVR_CLIP_GEN_START = 0,
    CVR_CLIP_GEN_END,
    CVR_CLIP_GEN_UNKNOWN,
    CVR_CLIP_GEN_PROGRESS
}cvr_clip_status_t;

typedef enum {
    CVR_UPLOAD_OK = 0,
    CVR_UPLOAD_FAIL,
    CVR_UPLOAD_CONNECT_ERR,
    CVR_UPLOAD_SEND_ERR,
    CVR_UPLOAD_RESPONSE_ERROR,
    CVR_UPLOAD_TIMEOUT,
    CVR_UPLOAD_MAX,
    CVR_UPLOAD_CURL_ERR
}cvr_upload_status;

class CVR : public kvsUploadCallback
{
    private:
      int amba_hwtimer_init(void);
      void amba_hwtimer_exit(int fd);
      unsigned long amba_hwtimer_msec(int fd);
      int cvr_daemon_check_filelock(char *fname);
      int cvr_read_config(cvr_provision_info_t *pCloudRecorderInfo);
      int cvr_enable_audio(bool val);
      int cvr_check_rfcparams();
      void cvr_init_audio_stream();
      static int cvr_audio_status;

      int init_flag;
      int load_config_flag;
      int check_polling_config_timeout;
      time_t cvr_starttime;
      struct timespec start_t;
      struct timespec end_t;
      char m_fpath[CVR_FILE_PATH_LEN];
      int ccode;
      int ts_fd;
      int cvr_flag;
      int hwtimer_fd;
      static int local_stream_err;
      int file_len;       //duration of each file seconds
      int has_an_iframe;
      int target_duration;
      unsigned int sequence;
      unsigned long true_len;
      char fpath[CVR_FILE_PATH_LEN];
      char file_name[CVR_FILE_PATH_LEN];
      char cmd[200];
      int isIPAcquired;
      unsigned long start_msec;
      struct tm* tv;
      XStreamerConsumer objConsumer;
      frameInfoH264 *cvr_frame;
      frameInfoH264 *cvr_key_frame;
      stream_hal_stream_config _videoConfig;
      int32_t _streamFd;

#ifdef DEBUG_DUMP_H264
      static int write_bytes;
      static FILE *fp;
      static int frame_num;
#endif //DEBUG_DUMP_H264
      int m_streamid;
      unsigned short kvsclip_audio;/* audio enable flag */
      unsigned short kvsclip_highmem;/* highmem flag */
      static int top;
      static float low_bound_motion_score;
      /* rtmessage */
#ifdef RTMSG
      static rtConnection connectionRecv;
      static rtConnection connectionSend;
      static rtError err;
      static volatile bool smartTnEnabled;
      static bool rtmessageCVRThreadExit;
      //Callback function for topics on smart thumbnail 
      static void on_message_smt_TN(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      static void on_message_cvrconf(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      //Callback function for topics on dynamic Logging
      static void on_message_dyn_log(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      static void receive_rtmessage();
      static void notify_smt_TN_clipStatus(cvr_clip_status_t status, const char* clip_name,  unsigned int event_ts = DEFAULT_EVT_TSTAMP);
      time_t event_quiet_time;
#endif
      bool iskvsInitDone;
      bool iskvsStreamInitDone;
      cvr_clip_status_t clipStatus;
      uint64_t m_storageMem;
      bool check_enabled_rfc_feature(char* rfc_feature_fname,char* rfc_feature);
      bool createkvsstream(int stream_id, unsigned short recreateflag);
      int pushFrames(frameInfoH264* frameInfo,
            char* fileName,
            bool isEOF = false);
      void onUploadSuccess(char* recName);
      void onUploadError(char* recName, const char* streamStatus);
    public:
      CVR();
      ~CVR();
      int cvr_init(unsigned short kvsclip_audio,cvr_provision_info_t *pCloudRecorderInfo,uint64_t storageMemory = 0);
      void do_cvr(void * pCloudRecorderInfo);
      int cvr_close();
      static volatile sig_atomic_t term_flag;
      static void self_term(int sig);
      static volatile sig_atomic_t reload_cvr_flag;
      static void reload_config();
      void setCVRStreamId(int streamid);
      int getCVRStreamId();
      static void notify_smt_TN_uploadStatus(cvr_upload_status status, char* upload_fname);
};
#endif
