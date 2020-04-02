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
#include "RdkCVideoCapturer.h"
#include "RdkCPluginFactory.h"

#ifdef RTMSG
#include "rtConnection.h"
#include "rtLog.h"
#include "rtMessage.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef XFINITY_SUPPORT
#include "rdkc_config_sections.h"
#endif

//#include "video_analysis.h" //vai_result_t
#include "polling_config.h"
#include "event_config.h"   //EventType
#include "AUD_conf.h"	//AUD_Conf
#include "main.h"   //ReadAllConf
#include "iav_ioctl.h" //IAV_PIC_TYPE_I_FRAME
#include "cgi_image.h"	//set_audio_mic_enable_2
#include "rdk_debug.h"
#include "dev_config.h"
#ifdef __cplusplus
}
#endif
#define RDKC_STREAM_FLAG_VIDEO                           0x01
#define RDKC_STREAM_FLAG_AUDIO                           0x02
#define RDKC_STREAM_FLAG_PADDING                         0x04
#define RDKC_STREAM_FLAG_ABSTIMESTAMP                    0x08
#define MAX_ENCODE_STREAM_NUM				4

#define VIDEO_DURATION_MAX              60	// 60 senconds
#define XFINITY_POLLING_CONFIG_TIMEOUT	90

#ifndef XCAM2
#define  DEF_CVR_CHANNEL		3	// stream D to be used by default
#else
#define  DEF_CVR_CHANNEL		1	// to fix the higher clip size due to higher fps(RDKC-4436), use 4th stream for cvr. Need to revert back to 1(stream B) for xCam2 once we fix the higher clip issue.
#endif

#define IP_ACQUIRED_FILE                 "/tmp/.IPAcquired"

//extern struct ThreadControl hydraThreadControl[];
//extern ThreadControl hydraThreadControl[];
#define CVR_FAILURE			-1
#define CVR_SUCCESS			 0

#define CVR_CLIP_PATH                    "/tmp/cvr"
#define CVR_CLIP_DURATION              	 20   //seconds
#define CVR_CLIP_NUMBER                	 2
#define CVR_FILE_PATH_LEN                256

#define CVR_EVENT_TYPE_PEOPLE_MASK       0X10
#define CVR_EVENT_TYPE_TAMPER_MASK	 0x20
#define CVR_EVENT_TYPE_MOTION_MASK	 0x03

#define HW_TIMER			 ((const char*)"/proc/ambarella/ambarella_hwtimer")
#define AUDIO_DURATION			 60
#define LOCK_FILENAME_CVR_DAEMON         "/tmp/cvr_daemon.lock"

#define SOURCE_BUFFER_WIDTH    		 1280
#define SOURCE_BUFFER_HEIGHT   		 720
#define CVR_STREAM_MAPPED_BUFFER       	 0x3  //Mapping CVR STREAM to 4th source buffer

#define CVR_AUDIO_UNKNOWN      		 -1
#define CVR_AUDIO_DISABLED      	  0
#define CVR_AUDIO_ENABLED       	  1

#define OD_MAX_NUM			  32
#define OD_FRAMES_MAX 			  5
#define STR_OD_DATA_MAX_LEN		  250

#ifdef RTMSG
#define RT_MSG_CVR_Q_SIZE_LIMIT		  30

#define DEFAULT_EV_QUIET_TIME		  30
#define DEFAULT_EVT_TSTAMP		  0

#define RTMSG_DYNAMIC_LOG_REQ_RES_TOPIC   "RDKC.ENABLE_DYNAMIC_LOG"
#define RTMSG_CVR_TOPIC			  "RDKC.CVR"
#endif

#define OPTIMIZED_VIDEO_PROFILE_FILE      "/opt/usr_config/OptimizedVideoProfile_Enable.txt"

/* Enable run time debug logging */
static int enable_debug = 0;
#define RDK_LOG_DEBUG1 (enable_debug ? (RDK_LOG_INFO) : (RDK_LOG_DEBUG))
#define ENABLE_CVR_RDK_DEBUG_LOG_FILE     "/tmp/.enable_cvr_rdk_debug"

using namespace std;
typedef enum cvr_clip_status
{
    CVR_CLIP_GEN_START = 0,
    CVR_CLIP_GEN_END,
    CVR_CLIP_GEN_UNKNOWN,
    CVR_CLIP_GEN_PROGRESS
}cvr_clip_status_t;

//typedef enum {
//    CVR_CLIP_GEN_START = 0,
//    CVR_CLIP_GEN_END,
//    CVR_CLIP_GEN_UNKNOWN
//}CVR_CLIP_GEN_STATUS;
//
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

typedef struct vai_object
{
        uint16_t   od_id; // For tracking
        uint8_t   type;     //object type
        uint8_t   confidence;   //the confidence on the current detection result
        uint16_t   start_x; // The top-left coordinate of the object
        uint16_t   start_y; // The top-left coordinate of the object
        uint16_t   width; // The width of the object
        uint16_t   height;// The height of the object
} vai_object_t;

typedef struct vai_od_frame_data
{
        char va_engine_version[10];
        float motion_score;
        uint32_t b_box_x_ord;
        uint32_t b_box_y_ord;
        uint32_t b_box_height;
        uint32_t b_box_width;
} vai_od_frame_data_t;

typedef struct vai_result
{
        uint64_t   timestamp;			// framePTS
        uint16_t   num;         		// number of objects detected
        vai_object_t vai_objects[OD_MAX_NUM];   // The object result is placed by od_id and save from 0 -> max
#ifdef _SUPPORT_OBJECT_DETECTION_IV_
        uint16_t   event_type;                  // Event type
        float   motion_level;                   // A percent of pixels under motion, range 0.0f~100.0f
        float   motion_level_raw;               // A percent of pixels under motion, range 0.0f~100.0f
#endif
        vai_od_frame_data_t od_frame_data;
	uint64_t  curr_time;			// Time of day
} vai_result_t;

class CVR : public kvsUploadCallback
{
    private:
      //Motion Level counters for No Motion/Low Motion/Medium Motion/High Motion
      int count_no;
      int count_low;
      int count_med;
      int count_high;

      int amba_hwtimer_init(void);
      void amba_hwtimer_exit(int fd);
      unsigned long amba_hwtimer_msec(int fd);
      uint8_t calculate_motion_level(float motion_level_raw_sum,int frame_num, uint8_t event_type_raw);
      int get_motion_statistics_info(RDKC_FrameInfo *p_cvr_frame, unsigned int *p_frame_num_count,uint8_t *p_event_type_raw, float *p_motion_level_raw_sum);
      int cvr_daemon_check_filelock(char *fname);
      int get_audio_stream_id(int audio_index);

#ifdef XFINITY_SUPPORT
      All_Conf *g_pconf;
      int cvr_read_config(CloudRecorderConf *pCloudRecorderInfo);
#else
      int cvr_read_config(RdkCCloudRecorderConf *pCloudRecorderInfo);
#endif
      int cvr_enable_audio(bool val);
      int cvr_check_rfcparams();

      int get_quiet_interval();
      int cvr_get_event_info( EventType *event_type,time_t *event_datetime,time_t cvr_starttime);
      void cvr_init_audio_stream();

      static int cvr_audio_status;
      //volatile sig_atomic_t reload_cvr_flag;
      //volatile sig_atomic_t term_flag;

      int init_flag;
      int load_config_flag;
      int check_polling_config_timeout;
      EventType event_type;
      time_t event_datetime; //record the time that event happened.
      time_t cvr_starttime;
      char starttime[200];
      char endtime[200];
      //struct timeval start_t;
      //struct timeval end_t;
      struct timespec start_t;
      struct timespec end_t;
      uint8_t  motion_statistics_info[VIDEO_DURATION_MAX + 8];
      char m_fpath[CVR_FILE_PATH_LEN];
      int m_fd;
      unsigned int frame_num_count;//record the number of frame which has padding data every seconds
      float motion_level_raw_sum;//record the sum of motion level change every seconds
      uint8_t event_type_raw;
      int motion_level_idx;
      pid_t pid;
      int file_fd;

      int i;
      int ccode;
      int ts_fd;
      int cvr_fd;
      int cvr_afd;
      int cvr_flag;
      int hwtimer_fd;
      static int local_stream_err;
      RDKC_FrameInfo cvr_frame;
      RDKC_FrameInfo cvr_key_frame;
      int file_len;       //duration of each file seconds
      int file_num; //number of files in m3u8 file
      int file_format; // file format : TS, MP4
      int has_an_iframe;
      int target_duration;
      unsigned int sequence;
      unsigned long true_len;
      char fpath[CVR_FILE_PATH_LEN];
      char file_name[CVR_FILE_PATH_LEN];
      int cfd;
      char cmd[200];
      int isIPAcquired;
      unsigned long start_msec;
      struct tm* tv;
      static RdkCPluginFactory* temp_factory; //creating plugin factory instance
      static RdkCVideoCapturer* recorder;
      camera_resource_config_t *conf;
      video_stream_config_t *v_stream_conf;
      int m_streamid;
      unsigned short kvsclip_audio;/* audio enable flag */
      unsigned short kvsclip_abstime;/* abs timestamp flag */
      unsigned short kvsclip_livemode;/* live mode flag */

	/* rtmessage */
      static int top;
      static float low_bound_motion_score;
#ifdef RTMSG
      static rtConnection connectionRecv;
      static rtConnection connectionSend;
      static rtError err;
#endif
      /* Determines OD_frame upload feature is enabled via RFC or not */
      static bool od_frame_upload_enabled;
      static bool smart_tn_enabled;
      static vai_result_t vai_result_recved_rtmsg;
      static vai_result_t vai_result_recved;
      static vai_result_t od_frames[OD_FRAMES_MAX];
      static char str_od_data[STR_OD_DATA_MAX_LEN];
      static char va_engine_version[10];
      static bool first_frame_of_clip;
      static std::queue<vai_result_t, std::list<vai_result_t> > rtmessageCVRQ;
      static std::mutex rtmessageCVRMutex;
#ifdef RTMSG
      static volatile bool smartTnEnabled;
      static bool rtmessageCVRThreadExit;
      static void push_msg(vai_result_t vai_result_recved_rtmsg);
      static void pop_msg(vai_result_t *vai_result_recved_rtmsg);
      //Callback finction for topics on CVR
      static void on_message_cvr(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      //Callback function for topics on smart thumbnail 
      static void on_message_smt_TN(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      //Callback function for topics on dynamic Logging
      static void on_message_dyn_log(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure);
      static void receive_rtmessage();
      static void notify_smt_TN_clipStatus(cvr_clip_status_t status, const char* clip_name,  unsigned int event_ts = DEFAULT_EVT_TSTAMP);
#endif
      static std::condition_variable msgCv;
#ifdef RTMSG
        time_t cvr_event_seconds[EVENT_TYPE_MAX];
        time_t prev_cvr_event_seconds[EVENT_TYPE_MAX];
        time_t event_quiet_time;
#endif
        bool iskvsInitDone;
        bool iskvsStreamInitDone;
        std::map<long, EventType> eventMap;
        cvr_clip_status_t clipStatus;

        void sort_od_frame_data();
        void pop_od_frame_data(int *top);
        int push_od_frame_data(int *top, float *low_bound_motion_score, float curr_motion_score, vai_result_t *vai_res);
        int update_od_frame_data(vai_result_t *vai_recvd_res);
        int reset_od_frame_data();
        int stringify_od_frame_data();
        bool check_enabled_rfc_feature(char* rfc_feature_fname,char* rfc_feature);
        bool pushFrames(RDKC_FrameInfo& frameInfo, 
                char* fileName,
                int stream_id, 
                unsigned short kvsclip_audio, 
                unsigned short kvsclip_abstime, 
                unsigned short kvsclip_livemode,
                EventType eventType = EVENT_TYPE_MAX, 
                bool isEOF = false);

        void onUploadSuccess(char* recName);
        void onUploadError(char* recName, const char* streamStatus);
    public:
      CVR();
      ~CVR();
      int cvr_init(int argc, char **argv,CloudRecorderConf *pCloudRecorderInfo);
      void do_cvr(void * pCloudRecorderInfo);
      int cvr_close(char *argv[]);
      static volatile sig_atomic_t term_flag;
      static void self_term(int sig);
      static volatile sig_atomic_t reload_cvr_flag;
      static volatile sig_atomic_t reload_cvr_config;
      static void reload_config(int dummy);
      void setCVRStreamId(int streamid);
      int getCVRStreamId();
      static void notify_smt_TN_uploadStatus(cvr_upload_status status, char* upload_fname);

};
#endif
