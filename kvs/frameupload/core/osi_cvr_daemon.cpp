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
#include "osi_cvr_daemon.h"
#include "kvsupload.h"

#ifdef BREAKPAD
    #include "breakpadwrap.h" 
#endif

#include <inttypes.h>
#define DATA_LEN                       128
#define CVR_RESOLUTION_CONF            "/opt/usr_config/cvr.conf"
#define CVR_RESOLUTION                 "resolution"
#define KVSINITMAXRETRY         5
#define KVS_SMARTRC "RFC_ENABLE_XCAM2_SMARTRC"
int CVR::cvr_audio_status = CVR_AUDIO_UNKNOWN;
vai_result_t CVR::vai_result_recved_rtmsg;
vai_result_t CVR::vai_result_recved;
vai_result_t CVR::od_frames[OD_FRAMES_MAX] = {0};
char CVR::str_od_data[STR_OD_DATA_MAX_LEN] = {0};
char CVR::va_engine_version [10] ={0};
bool CVR::first_frame_of_clip = true;
bool CVR::od_frame_upload_enabled = false;
int CVR::top = -1;
float CVR::low_bound_motion_score = 0.0;
char fileName[256] = {0};

#ifdef RTMSG
	rtConnection CVR::connectionRecv;
	rtConnection CVR::connectionSend;
	rtError CVR::err;
	bool CVR::rtmessageCVRThreadExit;
	volatile bool CVR::smartTnEnabled = true;
#endif

std::queue<vai_result_t, std::list<vai_result_t> > CVR::rtmessageCVRQ;
std::mutex CVR::rtmessageCVRMutex;
std::condition_variable CVR::msgCv;
int CVR::local_stream_err = 0;
volatile sig_atomic_t CVR::reload_cvr_config = 1;
using namespace std;

#define VIDEO_MASK 0x0F
	#if DEBUG_DUMP_H264
	int CVR::write_bytes = 0;
	FILE* CVR::fp = NULL;
	int CVR::frame_num = 0;
	FILE* CVR::fpAudioAAC = NULL;
	int CVR::frame_num_audio = 0;
#endif //DEBUG_DUMP_H264

void CVR::onUploadSuccess(char* cvrRecName)
{
    long recIndex = atol(cvrRecName);
    EventType eventType = eventMap.find(recIndex)->second;
    if(eventType == EVENT_TYPE_MOTION)
    {
        RDK_LOG(RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): kvs Upload Successful with Motion - %s\n", __FUNCTION__, __LINE__, cvrRecName);
    }
    else 
    {
        RDK_LOG(RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): kvs Upload Successful without Motion - %s\n", __FUNCTION__, __LINE__, cvrRecName);
    }
    std::map<long, EventType>::iterator it;
    it = eventMap.find(recIndex);
    if (it != eventMap.end())
    {
        eventMap.erase (it);
        RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): Deleted from the EventMap\n", __FUNCTION__, __LINE__);
    }
    else {
        RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): Error to find in the EventMap\n",__FUNCTION__, __LINE__);
    }

    RDK_LOG(RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): Notifying the upload status for the file %s\n",__FUNCTION__, __LINE__, cvrRecName);
    notify_smt_TN_uploadStatus(CVR_UPLOAD_OK, cvrRecName);
}

void CVR::onUploadError(char* cvrRecName, const char* streamStatus)
{
    notify_smt_TN_uploadStatus(CVR_UPLOAD_FAIL, cvrRecName);
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):  kvsclip upload error %s, %s\n",__FILE__, __LINE__, cvrRecName, streamStatus);
}

/** @description: Constructor
 *  @param[in] void
 */
CVR::CVR(): init_flag(0),
	load_config_flag(0),
	check_polling_config_timeout( XFINITY_POLLING_CONFIG_TIMEOUT ),
	event_type( EVENT_TYPE_MAX),
	_streamFd(-1),
#ifdef RTMSG
	event_quiet_time(DEFAULT_EV_QUIET_TIME),
#endif
	m_fd (-1),
	frame_num_count(0),
	motion_level_raw_sum(0),
	event_type_raw(0),
	event_datetime(DEFAULT_EVT_TSTAMP),
	motion_level_idx (1),
	pid(0),
	file_fd(0),
	clipStatus(CVR_CLIP_GEN_UNKNOWN),
	ccode(0),
	cvr_flag(0),
	hwtimer_fd(-1),
	//local_stream_err(0),
	file_len(CVR_CLIP_DURATION),
	file_num(CVR_CLIP_NUMBER),
	file_format(0),
	m_streamid(DEF_CVR_CHANNEL),
	has_an_iframe(0),
	target_duration(0),
	sequence(0),
	isIPAcquired(0),
	start_msec(0),
	count_no(0),
	count_low(0),
	count_med(0),
	count_high(0),
	iskvsInitDone(false),
	iskvsStreamInitDone(false),
	kvsclip_audio(0),
	kvsclip_highmem(0),
	m_storageMem(0),
	count_motion_mismatch(0)
{
#ifdef RTMSG
	rtmessageCVRThreadExit = false;
	memset(&cvr_event_seconds, 0, sizeof(cvr_event_seconds));
	memset(prev_cvr_event_seconds, 0, sizeof(prev_cvr_event_seconds));
#endif
	cvr_key_frame = (frameInfoH264 *)malloc(sizeof(frameInfoH264));
	memset(cvr_key_frame, 0, sizeof(frameInfoH264));

	// Memory allocation to be done at the consumer library
	cvr_frame = NULL;
}

CVR::~CVR()
{
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","Destructor called \n", __FILE__, __LINE__);
}

/** @description: amba timer init
 *  @param[in] void
 *  @return: int-file descriptor
 */
int CVR::amba_hwtimer_init(void)
{
	int fd = -1;

	if((fd = open(HW_TIMER, O_RDONLY)) < 0) {
		perror("open");
		return -1;
    	}
	return fd;
}

/** @description: exit 
 *  @param[in] :  int - fd file descriptor
 *  @return: void
 */
void CVR::amba_hwtimer_exit(int fd)
{
	if (fd >= 0) {
		close(fd);
    	}
}

/** @description: returns msec
 *  @param[in] : int - fd file descriptor
 *  @return: unsigned long
 */
unsigned long CVR::amba_hwtimer_msec(int fd)
{
        char pts[32];
	if (read(fd, pts, sizeof(pts)) < 0) {
                lseek(fd, 0L, SEEK_SET);
                perror("read");
                return 0;
        }
        else {
                lseek(fd, 0L, SEEK_SET);
                //the time unit of ambarella_hwtimer is 1/90000 sec.
                return (unsigned long)((strtoull((const char*)pts, (char **)NULL, 10))/90);
        }
}


#ifdef RTMSG
/** @description: push messgae into queue
 *  @param[in] vai_result_t struct
 *  @return: void
 */
void CVR::push_msg(vai_result_t vai_result_recved_rtmsg)
{
    std::unique_lock<std::mutex> lock(rtmessageCVRMutex);
    // Limiting the queue size to 30, by discarding the oldest element in the queue
    if( RT_MSG_CVR_Q_SIZE_LIMIT <= rtmessageCVRQ.size()) {
	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Maximum limit for the rtmessage CVR queue reached. Queue size is %d. Popping out one element from the queue!!!\n", __FILE__, __LINE__, rtmessageCVRQ.size() );
	rtmessageCVRQ.pop();
    }

    rtmessageCVRQ.push(vai_result_recved_rtmsg);
    lock.unlock();
}

/** @description: pop message from queue
 *  @param[in] *vai_result_recved_rtmsg - vai_result_t pointer
 *  @return: void
 */
void CVR::pop_msg(vai_result_t *vai_result_recved_rtmsg)
{
        if(!rtmessageCVRQ.empty()) {
            if( NULL != vai_result_recved_rtmsg) {
                std::unique_lock<std::mutex> lock(rtmessageCVRMutex);
                *vai_result_recved_rtmsg = rtmessageCVRQ.front();
                rtmessageCVRQ.pop();
                lock.unlock();
            }
        }
        else {
            usleep(100);
        }
}

/** @description:Callback function for the message received
 *  @param[in] hdr : pointer to rtMessage Header
 *  @param[in] buff : buffer for data received via rt message
 *  @param[in] n : number of bytes received
 *  @return: void
 */
void CVR::on_message_dyn_log(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure)
{
        char const*  module = NULL;
        char const*  logLevel = NULL;

        rtConnection con = (rtConnection) closure;

        rtMessage req;
        rtMessage_FromBytes(&req, buff, n);

        //Handle the rtmessage request
        if (rtMessageHeader_IsRequest(hdr))
        {
                char* tmp_buff = NULL;
                uint32_t tmp_buff_length = 0;

                rtMessage_ToString(req, &tmp_buff, &tmp_buff_length);
                rtLog_Info("Req : %.*s", tmp_buff_length, tmp_buff);
                free(tmp_buff);

                rtMessage_GetString(req, "module", &module);
                rtMessage_GetString(req, "logLevel", &logLevel);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.DYNAMICLOG","(%s):%d Module name: %s\n", __FUNCTION__, __LINE__, module);
                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.DYNAMICLOG","(%s):%d log level: %s\n", __FUNCTION__, __LINE__, logLevel);

                RDK_LOG_ControlCB(module, NULL, logLevel, 1);

                // create response
                rtMessage res;
                rtMessage_Create(&res);
                rtMessage_SetString(res, "reply", "Success");
                rtConnection_SendResponse(con, hdr, res, 1000);
                rtMessage_Release(res);
        }
        rtMessage_Release(req);
}

/** @description:Callback function for the message received
 *  @param[in] hdr : pointer to rtMessage Header
 *  @param[in] buff : buffer for data received via rt message
 *  @param[in] n : number of bytes received
 *  @return: void
 */
void CVR::on_message_smt_TN(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure)
{
    char const*  status = NULL;

    rtConnection con = (rtConnection) closure;

    rtMessage req;
    rtMessage_FromBytes(&req, buff, n);

    char* tempbuff = NULL;
    uint32_t buff_length = 0;

    rtMessage_ToString(req, &tempbuff, &buff_length);
    rtLog_Debug("Req : %.*s", buff_length, tempbuff);
    free(tempbuff);

    rtMessage_GetString(req, "status", &status);

    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","(%s):%d smart thumbnail status: %s\n", __FUNCTION__, __LINE__, status);

    if(!strcmp(status, "start")) {
        smartTnEnabled = true;
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","(%s):%d smart thumbnail value set to TRUE : %d\n", __FUNCTION__, __LINE__, smartTnEnabled);
    } else {
        smartTnEnabled = false;
        RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","(%s):%d smart thumbnail value set to FALSE : %d\n", __FUNCTION__, __LINE__, smartTnEnabled);
    }

    rtMessage_Release(req);
}

/** @description: convert iObject to th_Object
 *  @param[in]  hdr : constant pointer rtMessageHeader
 *  @param[in] buff : constant pointer uint8_t
 *  @param[in]    n : uint32_t
 *  @param[in] closure : void pointer
 *  @return: void
 */
void CVR::on_message_cvr(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure)
{
        static cvr_provision_info_t CloudRecorderInfo;
   	if ( reload_cvr_config ) {
        	memset(&CloudRecorderInfo, 0, sizeof(cvr_provision_info_t));
		if ( readCloudRecorderConfig(&CloudRecorderInfo) ) {
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Reload cloud recorder configuration error!\n", __FILE__, __LINE__);
		}
		reload_cvr_config = 0;
	}

    	int iscvrenabled = atoi(CloudRecorderInfo.enable);
	// No need to process the analytics message if cvr is disabled or stream error
	if ( ( iscvrenabled == 0) || (local_stream_err == 1) ) {
		return;
	}
	
        (void) closure;

        rtMessage m;
        rtMessage_FromBytes(&m, buff, n);
	char const*  vaEngineVersion = NULL;
	char const* s_timestamp = NULL;
        int32_t   event_type = 0;                  // Event type
        double   motion_level_raw = 0.0;
	double   motionScore = 0.0;
	int32_t  boundingBoxXOrd = 0;
	int32_t  boundingBoxYOrd = 0;
	int32_t  boundingBoxHeight = 0;
	int32_t  boundingBoxWidth = 0;
	char const* s_curr_time = NULL;
	
	 //if (rtMessageHeader_IsRequest(hdr))
        {
		if(od_frame_upload_enabled) {
			rtMessage_GetString(m, "vaEngineVersion", &vaEngineVersion);
		}
		rtMessage_GetString(m, "timestamp", &s_timestamp);
                //rtMessage_GetInt32(m, "num", &num);
                rtMessage_GetInt32(m, "event_type", &event_type);
                rtMessage_GetDouble(m, "motion_level_raw", &motion_level_raw);

		if(od_frame_upload_enabled) {
			rtMessage_GetDouble(m, "motionScore", &motionScore);
			rtMessage_GetInt32(m, "boundingBoxXOrd", &boundingBoxXOrd);
			rtMessage_GetInt32(m, "boundingBoxYOrd", &boundingBoxYOrd);
			rtMessage_GetInt32(m, "boundingBoxHeight", &boundingBoxHeight);
			rtMessage_GetInt32(m, "boundingBoxWidth", &boundingBoxWidth);
        	

			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): vaEngineVersion: %s\n", __FILE__, __LINE__, vaEngineVersion);
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): motionScore: %f\n",__FILE__, __LINE__, motionScore);
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): boundingBoxXOrd:%d\n", __FILE__, __LINE__,boundingBoxXOrd);
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): boundingBoxYOrd:%d\n", __FILE__, __LINE__,boundingBoxYOrd);
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): boundingBoxHeight:%d\n", __FILE__, __LINE__,boundingBoxHeight);
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): boundingBoxWidth:%d\n", __FILE__, __LINE__,boundingBoxWidth);
		}
		rtMessage_GetString(m, "currentTime", &s_curr_time);
		RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Current Tstamp::%s\n", __FILE__, __LINE__,s_curr_time);
	}

        rtLog_Debug("flags     :%d", hdr->flags);
        rtLog_Debug("is_request:%d", rtMessageHeader_IsRequest(hdr));

	memset(&vai_result_recved_rtmsg, 0, sizeof(vai_result_t));
	if(od_frame_upload_enabled) {
		memcpy(vai_result_recved_rtmsg.od_frame_data.va_engine_version, vaEngineVersion, sizeof(vai_result_recved_rtmsg.od_frame_data.va_engine_version));
	}
		
        //memcpy(&vai_result_recved_rtmsg.timestamp,&timestamp,sizeof(vai_result_recved_rtmsg.timestamp));
        //memcpy(&vai_result_recved_rtmsg.event_type,&event_type,sizeof(vai_result_recved_rtmsg.event_type));
        //memcpy(&vai_result_recved_rtmsg.timestamp,&timestamp,sizeof(uint32_t));
	std::istringstream iss(s_timestamp);
        iss >> vai_result_recved_rtmsg.timestamp;
	iss.clear();

        memcpy(&vai_result_recved_rtmsg.event_type,&event_type,sizeof(uint32_t));
	
        vai_result_recved_rtmsg.motion_level_raw = (float)motion_level_raw;
	if(od_frame_upload_enabled) {
		vai_result_recved_rtmsg.od_frame_data.motion_score =(float) motionScore;
		vai_result_recved_rtmsg.od_frame_data.b_box_x_ord = boundingBoxXOrd;
		vai_result_recved_rtmsg.od_frame_data.b_box_y_ord = boundingBoxYOrd;
		vai_result_recved_rtmsg.od_frame_data.b_box_height = boundingBoxHeight;
		vai_result_recved_rtmsg.od_frame_data.b_box_width = boundingBoxWidth;
	}

	iss.str(s_curr_time);
	iss >> vai_result_recved_rtmsg.curr_time;

	iss.clear();

        rtMessage_Release(m);

	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): data recieved in on_message\n", __FILE__, __LINE__);
	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): timestamp: %llu, od_event_type = %lu, motion_level_raw = %f curr_time: %llu \n", __FILE__, __LINE__,(unsigned long long)vai_result_recved_rtmsg.timestamp, (unsigned long)vai_result_recved_rtmsg.event_type, vai_result_recved_rtmsg.motion_level_raw, vai_result_recved_rtmsg.curr_time);

	push_msg(vai_result_recved_rtmsg);
}

/** @description: receive message
 *  @param[in]  :  void
 *  @return: void
 */
void CVR::receive_rtmessage()
{
    while(!rtmessageCVRThreadExit) {
	    err = rtConnection_Dispatch(connectionRecv);
	    rtLog_Debug("dispatch:%s", rtStrError(err));
	    if (err != RT_OK) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Error receiving msg via rtmessage\n",__FUNCTION__,__LINE__);
	    }
	    usleep(10000);
    }
}

#endif

/** @description: calculate motion level
 *  @param[in] motion_level_raw_sum - float
 *  @param[in] frame_num - int
 *  @param[in] event_type_raw - uint8_t
 *  @return: uint8_t
 */
uint8_t CVR::calculate_motion_level(float motion_level_raw_sum,int frame_num, uint8_t event_type_raw)
{
        uint8_t m_level = 0;
        const uint8_t m_level_mask[11] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A};
        unsigned int mask_idx = 0;
        int motion_level_raw_average = 0;
        motion_level_raw_average = (int)(motion_level_raw_sum / (frame_num * 1.0));

        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d):sum_raw = %f, sum_frame = %d, event_tyep = %02x\n", __FILE__, __LINE__,motion_level_raw_sum,frame_num,event_type_raw);
        if (0 == motion_level_raw_average) {
                mask_idx = 0;
        }
        else {
                mask_idx = ((motion_level_raw_average - 1) / 10) + 1;
        }
        //Set bit to present the percent change of motion level raw data
        m_level = m_level | m_level_mask[mask_idx];
        //Set bit to present event type
        m_level = m_level | event_type_raw;
        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d):m_level = %02x\n", __FILE__, __LINE__,m_level);
        return m_level;
}

/** @description: convert od frame data to string
 *  @param[in]  :  void
 *  @return: void
 */
int CVR::stringify_od_frame_data()
{
	int i;
	int num_od_frames = top;
	char str_tmp_od_data[50];
	str_od_data[250] = {0};

	if(top >=  0)
	{
        	for (i=0; i<= num_od_frames; i++)
		{
			str_tmp_od_data[100] = {0};

			snprintf(str_tmp_od_data, 50,"%lu[%d,%d,%d,%d],", od_frames[i].curr_time, od_frames[i].od_frame_data.b_box_x_ord, od_frames[i].od_frame_data.b_box_y_ord, od_frames[i].od_frame_data.b_box_height, od_frames[i].od_frame_data.b_box_width);

			strcat(str_od_data, str_tmp_od_data);
		}
	}

	str_od_data[strlen(str_od_data)-1 ] = '\0';

	//RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): top 5 frames: %s\n", __FILE__, __LINE__, str_od_data);
}

/** @description: sort od frame data.
 *  @param[in]  :  void
 *  @return: void
 */			
void CVR::sort_od_frame_data()
{
        int i,j;
        vai_result_t tmp_vai_data;

        //sorting the 5 frame array w.r.t motion score in descending order.
        for (j=0; j<(OD_FRAMES_MAX - 1); j++)
        {
                for (i=0; i<(OD_FRAMES_MAX - 1); i++)
                {
                        if (od_frames[i+1].od_frame_data.motion_score > od_frames[i].od_frame_data.motion_score)
                        {
                                tmp_vai_data = od_frames[i];
                                od_frames[i] = od_frames[i+1];
                                od_frames[i+1] = tmp_vai_data;
                        }
                }
        }
}

/** @description: pop od frame data from stack.
 *  @param[in] top -> points to top of frame stack 
 *  @return: void
 */
void CVR::pop_od_frame_data(int *top)
{
	if(*top == -1)
    	{
        	RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","\tError: OD frames slot is empty!! \n");
    	}
    	else
    	{
        	//Reseting the last slot of 5 frame array.
		od_frames[*top] = {0};
        	*top = *top - 1;
    	}
}	

/** @description: push od frame data to stack.
 *  @param[in] top -> points to top of frame stack
 *  @param[in] low_bound_motion_score -> points to low bound motion score 
 *  @param[in] curr_motion_score -> motion score for the curr frame 
 *  @param[in] vai_result_t -> points to vai result obtained by VA 
 *  @return: void
 */
int CVR::push_od_frame_data(int *top, float *low_bound_motion_score, float curr_motion_score, vai_result_t *vai_res)
{
        float *l_low_bound_m_score = low_bound_motion_score;
        float l_curr_m_score = curr_motion_score;
        int *l_top = top;
        
	//No empty slots
        if( *l_top == OD_FRAMES_MAX-1 )
        {
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","od frames slot is full, popping the frame with least motion score");
                pop_od_frame_data(l_top);
                //calling push recursively, to update the last slot of od frame array.
                push_od_frame_data(l_top, l_low_bound_m_score, l_curr_m_score, vai_res);
                *l_low_bound_m_score = l_curr_m_score;
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","Low bound motion score updated: %f", *l_low_bound_m_score);
        }
        else
        {
                *l_top = *l_top + 1;
		memcpy(&od_frames[*l_top], vai_res, sizeof(vai_result_t));
        }
}	

/** @description: reset od frame data.
 *  @param[in]  :  void
 *  @return: void
 */
int CVR::reset_od_frame_data()
{
	top = -1;
	low_bound_motion_score = 0.0;
	first_frame_of_clip = true;
	memset(va_engine_version, 0, sizeof(char) * 10);
	memset(od_frames, 0, sizeof(vai_result_t)*OD_FRAMES_MAX);
	memset(str_od_data, 0, sizeof(char)*STR_OD_DATA_MAX_LEN);
}

/** @description: update od frame data in stack.
 *  @param[in] vai_result_t -> points to vai result obtained by VA
 *  @return: void
 */	
int CVR::update_od_frame_data(vai_result_t *vai_recvd_res)
{
	float curr_motion_score = 0.0;

	curr_motion_score = vai_recvd_res -> od_frame_data.motion_score;

        if (curr_motion_score > low_bound_motion_score) {
		if (low_bound_motion_score == 0.0) {
			low_bound_motion_score = curr_motion_score;
                        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","Low bound motion score updated: %f", low_bound_motion_score);
                }

                //vai od frame data to the stack and sort.
                push_od_frame_data(&top, &low_bound_motion_score, curr_motion_score, vai_recvd_res);
                sort_od_frame_data();
        }
}

/** @description: get motion statics information
 *  @param[in] p_cvr_frame - RDKC_FrameInfo structure or frameInfoH264 structure in case xStreamer is enabled
 *  @param[in] p_frame_num_count - unsigned int pointer
 *  @param[in] p_event_type_raw -  uint8_t pointer
 *  @param[in] p_motion_level_raw_sum - float pointer
 *  @return: int
 */
int CVR::get_motion_statistics_info(frameInfoH264 *p_cvr_frame, unsigned int *p_frame_num_count,uint8_t *p_event_type_raw, float *p_motion_level_raw_sum)
{
        vai_result_t *p_od_result = NULL;
        pop_msg(&vai_result_recved);
        p_od_result = &vai_result_recved;
	
        if(od_frame_upload_enabled) {
		if(first_frame_of_clip) {
			memcpy(va_engine_version, p_od_result -> od_frame_data.va_engine_version, sizeof(va_engine_version));
			first_frame_of_clip = false;
		}
		update_od_frame_data(p_od_result);
	}
		
        /* Range of Motion Level                        Type of Motion
	----------------------- 			----------------
      	0 ~ 12						No Motion
	12 ~ 35						Low Motion
	35 ~ 60						Medium Motion
	60 ~ 100					High Motion	*/

	if( p_od_result->motion_level >=0 && p_od_result->motion_level <= 12 ) {
                count_no++;
        }
        if( p_od_result->motion_level >12 && p_od_result->motion_level <= 35 ) {
                count_low++;
        }
        if( p_od_result->motion_level >35 && p_od_result->motion_level <= 60 ) {
                count_med++;
        }
        if( p_od_result->motion_level >60 && p_od_result->motion_level <= 100 ) {
                count_high++;
        }

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): p_od_result->timestamp : %llu, p_cvr_frame->arm_pts: %llu, abs(p_cvr_frame->arm_pts - p_od_result->timestamp): %lld!!!\n", __FILE__, __LINE__, (unsigned long long)p_od_result->timestamp, (unsigned long long)p_cvr_frame->arm_pts, abs((long long)(p_cvr_frame->arm_pts - p_od_result->timestamp)));

        if (0 == p_od_result->timestamp || abs((long long)(p_cvr_frame->arm_pts - p_od_result->timestamp)) > (45000 * 2) ) {
            if (0 == count_motion_mismatch) {
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d):All event are disabled, needn't get motion statistics\n ", __FILE__, __LINE__);
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): MotionTimestamps p_od_result p_cvr_frame abs_diff: %llu, %llu, %lld\n", __FILE__, __LINE__, (unsigned long long)p_od_result->timestamp, (unsigned long long)p_cvr_frame->arm_pts, abs((long long)(p_cvr_frame->arm_pts - p_od_result->timestamp)));
            }
            count_motion_mismatch++;
            return 0;
        }

        (*p_frame_num_count)++;
        (*p_motion_level_raw_sum) = (*p_motion_level_raw_sum) + p_od_result->motion_level_raw;
        
        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): frame_num = %d, motion_level_raw = %f od_event_type = %02x\n", __FILE__, __LINE__,*p_frame_num_count, p_od_result->motion_level_raw, p_od_result->event_type);
       RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): cvr_event_seconds[%d]: %d curr_time: %llu timestamp: %llu\n", __FILE__, __LINE__, EVENT_TYPE_MOTION, cvr_event_seconds[EVENT_TYPE_MOTION], p_od_result-> curr_time, p_od_result->timestamp);
 
        if (p_od_result->event_type & (1 << EVENT_TYPE_PEOPLE)) {
                (*p_event_type_raw)= (*p_event_type_raw) | CVR_EVENT_TYPE_PEOPLE_MASK;
            if( 0 == cvr_event_seconds[EVENT_TYPE_PEOPLE] ) {
                cvr_event_seconds[EVENT_TYPE_PEOPLE] = p_od_result-> curr_time;
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): People detected, Setting the object timestamp %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_PEOPLE]);
		    }
            else {
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): People detected, but object timestamp has already set to %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_PEOPLE]);
            }
        }

        if (p_od_result->event_type & (1 << EVENT_TYPE_TAMPER)) {
                (*p_event_type_raw) = (*p_event_type_raw) | CVR_EVENT_TYPE_TAMPER_MASK;
            if( 0 == cvr_event_seconds[EVENT_TYPE_TAMPER] ) {
                cvr_event_seconds[EVENT_TYPE_TAMPER] = p_od_result-> curr_time;
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Tamper detected, Setting the tamper timestamp %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_TAMPER]);
		    }
            else {
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Tamper detected, but tamper timestamp has already set to %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_TAMPER]);
            }
        }

        if (p_od_result->event_type & (1 << EVENT_TYPE_MOTION)) {
            if( 0 == cvr_event_seconds[EVENT_TYPE_MOTION] ) {
                cvr_event_seconds[EVENT_TYPE_MOTION] = p_od_result-> curr_time;
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Motion detected, Setting the motion timestamp %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_MOTION]);
		    }
            else {
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Motion detected, but motion timestamp has already set to %d\n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_MOTION]);
            }
        }

        return 0;
}

/** @description: retrieve event quiet interval
 *  @param[in] : void
 *  @return: event quiet interval
 */
int CVR::get_quiet_interval()
{
	int quiet_interval =event_quiet_time;
	events_provision_info_t *eventsCfg = NULL;

	// Allocate memory for event config
	eventsCfg = (events_provision_info_t*) malloc(sizeof(events_provision_info_t));

	if (NULL == eventsCfg) {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Error allocating memory. Use existing quiet interval %d\n", __FILE__, __LINE__, quiet_interval);
		return quiet_interval;
	}

	if (RDKC_SUCCESS != readEventConfig(eventsCfg)) {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Error reading EVENTS Config. Use existing quiet interval %d\n", __FILE__, __LINE__, quiet_interval);
		return quiet_interval;
	}

	// get event quiet interval
	if (strlen(eventsCfg->quite_interval) > 0) {
		quiet_interval = atoi(eventsCfg->quite_interval);
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Invalid Quiet Interval. Use existing quiet interval %d\n", __FILE__, __LINE__, quiet_interval);
		return quiet_interval;

	}

	if (event_quiet_time != quiet_interval) {
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Retrieved New Quiet Interval: %d %d\n", __FILE__, __LINE__, event_quiet_time, quiet_interval);
	}

	if (eventsCfg) {
		free(eventsCfg);
		eventsCfg = NULL;
	}

	return quiet_interval;
}

/** @description: getting cvr event info
 *  @param[in] : event_type - EventType pointer
 *  @param[in] : event_datetime - time_t pointer
 *  @param[in] : cvr_starttime - time_t struct
 *  @return: int
 */
int CVR::cvr_get_event_info( EventType *event_type,time_t *event_datetime,time_t cvr_starttime)
{
	event_quiet_time = get_quiet_interval();

	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR",",event_quiet_time=%d",event_quiet_time);

	if (cvr_event_seconds[EVENT_TYPE_MOTION] >= cvr_starttime - 1) {
		/* Update event only if exceeds quiet time */
		if( (cvr_event_seconds[EVENT_TYPE_MOTION] >= (prev_cvr_event_seconds[EVENT_TYPE_MOTION] + event_quiet_time)) ||
				(0 == prev_cvr_event_seconds[EVENT_TYPE_MOTION]) ) {
			*event_type = EVENT_TYPE_MOTION;
			*event_datetime = cvr_event_seconds[EVENT_TYPE_MOTION];
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Setting event_datetime during the start - %ld \n", __FILE__, __LINE__, *event_datetime);
			prev_cvr_event_seconds[EVENT_TYPE_MOTION] = cvr_event_seconds[EVENT_TYPE_MOTION];
		}
		else {
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Skipping Motion events! curr ev time %d prev ev time %d \n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_MOTION], prev_cvr_event_seconds[EVENT_TYPE_MOTION]);
		}

		cvr_event_seconds[EVENT_TYPE_MOTION] = 0;
	}
#ifdef _SUPPORT_TAMPER_DETECTION_
	else if (cvr_event_seconds[EVENT_TYPE_TAMPER] >= cvr_starttime - 1) {
		/* Update event only if exceeds quiet time */
		if( (cvr_event_seconds[EVENT_TYPE_TAMPER] >= (prev_cvr_event_seconds[EVENT_TYPE_TAMPER] + event_quiet_time)) ||
				(0 == prev_cvr_event_seconds[EVENT_TYPE_TAMPER]) ) {
			*event_type = EVENT_TYPE_TAMPER;
			*event_datetime = cvr_event_seconds[EVENT_TYPE_TAMPER];
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Setting event_datetime when the event exceeded the quite time - %ld \n", __FILE__, __LINE__, *event_datetime);
			prev_cvr_event_seconds[EVENT_TYPE_TAMPER] = cvr_event_seconds[EVENT_TYPE_TAMPER];
		}
		else {
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Skipping Motion events! curr ev time %d prev ev time %d \n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_TAMPER], prev_cvr_event_seconds[EVENT_TYPE_TAMPER]);
		}
		cvr_event_seconds[EVENT_TYPE_TAMPER] = 0;
	}
#endif

#ifdef _SUPPORT_OBJECT_DETECTION_
	else if (cvr_event_seconds[EVENT_TYPE_PEOPLE] >= cvr_starttime - 1) {
		/* Update event only if exceeds quiet time */
		if( (cvr_event_seconds[EVENT_TYPE_PEOPLE] >= (prev_cvr_event_seconds[EVENT_TYPE_PEOPLE] + event_quiet_time)) ||
				(0 == prev_cvr_event_seconds[EVENT_TYPE_PEOPLE]) ) {
			*event_type = EVENT_TYPE_PEOPLE;
			*event_datetime = cvr_event_seconds[EVENT_TYPE_PEOPLE];
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Setting event_datetime when the event exceeded the quite time (OD case)- %ld \n", __FILE__, __LINE__, *event_datetime);
			prev_cvr_event_seconds[EVENT_TYPE_PEOPLE] = cvr_event_seconds[EVENT_TYPE_PEOPLE];
		}
		else {
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Skipping Motion events! curr ev time %d prev ev time %d \n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_PEOPLE], prev_cvr_event_seconds[EVENT_TYPE_PEOPLE]);
		}
		cvr_event_seconds[EVENT_TYPE_PEOPLE] = 0;
	}
	else {
			RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): No valid event detection during the timeframe starting from %d, Present detection timestamps P:%d T:%d M:%d \n", __FILE__, __LINE__, cvr_starttime, cvr_event_seconds[EVENT_TYPE_PEOPLE], cvr_event_seconds[EVENT_TYPE_TAMPER], cvr_event_seconds[EVENT_TYPE_MOTION] );
	}
#endif

	// Reset all the event detection timestamps
	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): Reseting all the detection timestamps, Present detection timestamps P:%d T:%d M:%d \n", __FILE__, __LINE__, cvr_event_seconds[EVENT_TYPE_PEOPLE], cvr_event_seconds[EVENT_TYPE_TAMPER], cvr_event_seconds[EVENT_TYPE_MOTION] );
	memset(&cvr_event_seconds, 0, sizeof(cvr_event_seconds));

	return 0;
}

/** @description: read configuration
 *  @param[in] pCloudRecorderInfo : cvr_provision_info_t pointer
 *  @return: int
 */
int CVR::cvr_read_config(cvr_provision_info_t *pCloudRecorderInfo)
{
        // Read cloud recorder server info
        memset(pCloudRecorderInfo, 0, sizeof(cvr_provision_info_t));
        if(readCloudRecorderConfig(pCloudRecorderInfo))
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read cloud recorder configuration error!\n", __FILE__, __LINE__);
                return -1;
	}
    
        return 0;
}

/** @description: Initialize audio stream
 *  @param[in] void
 *  @return: void
 */
void CVR::cvr_init_audio_stream()
{
    cvr_flag |= XSTREAM_AUDIO_FLAG;

    _streamFd = objConsumer.StreamInit( m_streamid, FORMAT_H264, XSTREAM_AUDIO_FLAG ); //Audio
    if ( _streamFd < 0 ) {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Invalid socket fd 0x%x, stream initialization failed \n", __FILE__, __LINE__, _streamFd);
        cvr_flag &= ~XSTREAM_AUDIO_FLAG;
    }
    else {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream(%d) initialization success \n", __FILE__, __LINE__, _streamFd);
    }
}

/** @description: set cvr audio
 *  @param[in] void
 *  @return: int
 */
int CVR::cvr_check_rfcparams()
{
	/* set cvr audio through RFC files */
        char value[MAX_SIZE] = {0};
        char usr_value[8] = {0};
        char *configParam = NULL;
        int prev_cvr_audio_status =  cvr_audio_status;
        if (RDKC_SUCCESS != rdkc_get_user_setting(CVR_AUDIO_STATUS, usr_value)) {
                configParam = (char*)rdkc_envGet(CVR_AUDIO_STATUS);
        }
	else {
                configParam = usr_value;
        }
	
        if(strcmp(configParam, RDKC_TRUE) == 0) {
                cvr_audio_status = CVR_AUDIO_ENABLED;
                RDK_LOG( RDK_LOG_DEBUG1, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to enable.\n", __FILE__, __LINE__);
        }
	else {
                cvr_audio_status = CVR_AUDIO_DISABLED;
                RDK_LOG( RDK_LOG_DEBUG1, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to disable.\n", __FILE__, __LINE__);
        }

        //RFC check for kvs smartrc
        memset(value,0,MAX_SIZE);
        if(RDKC_SUCCESS == GetValueFromRFCFile(RFCFILE, KVS_SMARTRC, value)) {
                if( strcmp(value, RDKC_TRUE) == 0 ) {
                            kvsclip_highmem = 1;
                    }
        }

        if( prev_cvr_audio_status != cvr_audio_status ) {
                if( CVR_AUDIO_ENABLED == cvr_audio_status ) {
			  kvsclip_audio=1;
			  if(init_flag == 1){
			  	cvr_init_audio_stream();
			  }
                }
                else if( CVR_AUDIO_DISABLED == cvr_audio_status ) {
                        RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): CVR audio is disabled.\n", __FILE__, __LINE__);
                        kvsclip_audio=0;
                        if(init_flag == 1){
                            if(cvr_flag & XSTREAM_AUDIO_FLAG)
                            {
                               	if (_streamFd > 0) {
                                    if( 0 != objConsumer.StreamClose(_streamFd, XSTREAM_AUDIO_FLAG)) {
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Stream Close failed\n", __FILE__, __LINE__);
                                    }
                                    else {
                                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream Close success\n", __FILE__, __LINE__);
                                    }
                                    //_streamFd = -1;
                                }
                                cvr_flag &= ~XSTREAM_AUDIO_FLAG;
                            }
                        }
                }
        }
}

/** @description: Checks if the feature is enabled via RFC
 *  @param[in] rfc_feature_fname: RFC feature filename
 *  @param[in] plane1: RFC parameter name
 *  @return: bool
 */
bool CVR::check_enabled_rfc_feature(char*  rfc_feature_fname, char* feature_name)
{
    /* set cvr audio through RFC files */
    char value[MAX_SIZE] = {0};

    if((NULL == rfc_feature_fname) ||
       (NULL == feature_name)) {
        return false;
    }

    /* Check if RFC configuration file exists */
    if( RDKC_SUCCESS == IsRFCFileAvailable(rfc_feature_fname)) {
        /* Get the value from RFC file */
        if( RDKC_SUCCESS == GetValueFromRFCFile(rfc_feature_fname, feature_name, value) ) {
            if( strcmp(value, RDKC_TRUE) == 0) {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s is enabled via RFC.\n",__FILE__, __LINE__, feature_name);
                return true;
            } else {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s is disabled via RFC.\n",__FILE__, __LINE__, feature_name);
                return false;
            }
        }else {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s is not available in rfc file.\n",__FILE__, __LINE__, feature_name);
                return false;
        }
        /* If RFC file is not present, disable the featur */
    } else {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): rfc feature file %s is not present hence %s is disabled via RFC\n",__FILE__, __LINE__, rfc_feature_fname,feature_name);
        return false;
    }
    return false;
}

int CVR::getCVRStreamId()
{
  return m_streamid;
}

void CVR::setCVRStreamId(int streamid)
{
  m_streamid = streamid;
}


/** @description: initialize cvr
 *  @param[in] argv - char pointer
 *  @param[in] pCloudRecorderInfo - CloudRecorderConf pointer
 *  @return: CVR_Failure is failed , CVR_SUCCESS if success
 */
int CVR::cvr_init(unsigned short isAudio,cvr_provision_info_t *pCloudRecorderInfo,uint64_t storageMemory=0)
{
	int rdkc_ret = 1;
        int iscvrenabled = 0;

	if (RDKC_FAILURE == polling_config_init()) {
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error initializing polling config\n", __FILE__, __LINE__);
                return RDKC_FAILURE;
        }

        // Init hwtimer, we need a timestamp which from the same source of video/audio frame timestamp
        hwtimer_fd = CVR::amba_hwtimer_init();
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): amba_hwtimer_init success\n", __FILE__, __LINE__);

        if (hwtimer_fd < 0)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): amba_hwtimer_init error\n", __FILE__, __LINE__);
                return CVR_FAILURE;
        }

        kvsclip_audio = isAudio;/* audio enable flag */
        m_storageMem = storageMemory;

        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsclip_audio : %d \n", __FILE__, __LINE__, kvsclip_audio);
        
        // Init cvr flag
        cvr_flag = XSTREAM_VIDEO_FLAG ;       // Video is must
#ifdef _SUPPORT_AAC_
        cvr_flag |= XSTREAM_AUDIO_FLAG;      // Audio is necessary if AAC enabled
        m_streamid |= 0x10;                      //Default audio channel is 1.
#endif
        cvr_flag |= RDKC_STREAM_FLAG_PADDING;    // Padding is must
        
        cvr_flag |= RDKC_STREAM_FLAG_ABSTIMESTAMP;    //disable abs timestamp - 1 means disable absolute timestamp - 0  means reset timestamp

#if DEBUG_DUMP_H264
        unlink("/opt/cvr_frame.H264");
        fp = fopen("/opt/cvr_frame.H264", "wo+" );

        if ( NULL == fp ) {
                RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d) :open video file open error\n", __FILE__, __LINE__ );
        }

        unlink("/opt/aac_test.aac");

        fpAudioAAC = fopen("/opt/aac_test.aac", "wo+" );

        if ( NULL == fpAudioAAC ) {
                RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d) :open audio file open error\n", __FILE__, __LINE__ );
        }

#endif //DEBUG_DUMP_H264

        if( RDKC_SUCCESS != config_init())
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Config Manager init fails\n",__FILE__,__LINE__);
        }

        if( RDKC_SUCCESS != RFCConfigInit() )
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): RFC config Init fails\n", __FILE__, __LINE__);
                return CVR_FAILURE;
        }

	//Check is OD frame upload is enabled via RFC
	od_frame_upload_enabled = check_enabled_rfc_feature(RFCFILE, OD_FRAMES_UPLOAD);
	
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): od_frame_upload_enabled is :  %d \n", __FILE__, __LINE__, od_frame_upload_enabled);

#ifdef RTMSG
        rtLog_SetLevel(RT_LOG_INFO);
        rtLog_SetOption(rdkLog);

        rtConnection_Create(&connectionSend, "CVR_SEND", "tcp://127.0.0.1:10001");
        rtConnection_Create(&connectionRecv, "CVR_RECV", "tcp://127.0.0.1:10001");
        rtConnection_AddListener(connectionRecv, "RDKC.CVR", on_message_cvr, NULL);
        rtConnection_AddListener(connectionRecv, "RDKC.SMARTTN.STATUS", on_message_smt_TN, NULL);
        rtConnection_AddListener(connectionRecv, "RDKC.ENABLE_DYNAMIC_LOG", on_message_dyn_log, connectionRecv);

        std::thread rtMessage_recv_thread (receive_rtmessage);
        pthread_setname_np(rtMessage_recv_thread.native_handle(),"rt_recmessage");
        rtMessage_recv_thread.detach();
#endif

        // Check camera has polling cvr config from server?
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Wait for xfinity polling config done.Timeout is %d seconds.\n", __FILE__, __LINE__, check_polling_config_timeout);
        while (check_polling_config_timeout > 0 && !term_flag)
        {
                if (0 == access(XFINITY_POLLING_SEQ_FILE, F_OK))
                {
                        break;
                }
                sleep(1);
                check_polling_config_timeout --;
        }
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): xfinity polling config done.\n", __FILE__, __LINE__);

        rdkc_ret = cvr_read_config(pCloudRecorderInfo);
	if (0 == rdkc_ret)
	{
                iscvrenabled = atoi(pCloudRecorderInfo->enable);
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is successful, enable=%d!\n", __LINE__, iscvrenabled);
	}
	else
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, iscvrenabled);
	}
}

/**
 * @description: This function is used to notify smart thumbnail about cvr upload status
 * @param[in]: status, uploadedclip_filename
 * @return: None
 */
void CVR::notify_smt_TN_uploadStatus(cvr_upload_status status, char* upload_fname)
{
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): upload file base name:%s\n",__FILE__, __LINE__, upload_fname);

	rtMessage msg;
	rtMessage_Create(&msg);
	rtMessage_SetInt32(msg, "status", status);
	rtMessage_SetString(msg, "uploadFileName", upload_fname);
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVRUPLOAD","%s(%d): Sending CVR upload status to smart thumbnail, status code:%d\n",__FILE__, __LINE__, status);
	rtError err = rtConnection_SendMessage(connectionSend, msg, "RDKC.CVR.UPLOAD.STATUS");
	rtLog_Debug("SendRequest:%s", rtStrError(err));
	if (err != RT_OK)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) Error sending msg via rtmessage\n", __FILE__,__LINE__);
	}
	rtMessage_Release(msg);
}

void CVR::notify_smt_TN_clipStatus(cvr_clip_status_t status, const char* clip_name, unsigned int event_ts = DEFAULT_EVT_TSTAMP)
{
	char  str_event_ts[256];

	memset(str_event_ts, 0 , sizeof(str_event_ts));
	if(DEFAULT_EVT_TSTAMP != event_ts) {
		snprintf(str_event_ts, sizeof(str_event_ts), "%lu",  event_ts);
	} else {
		strcpy(str_event_ts, "TIMESTAMP_NOT_AVAILABLE");
	}

	if(!clip_name) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Empty clip name!!\n", __FILE__, __LINE__);
		return;
	}

	rtMessage msg;
	rtMessage_Create(&msg);
	rtMessage_SetInt32(msg, "clipStatus", status);
	rtMessage_SetString(msg, "clipname", clip_name);
	rtMessage_SetString(msg, "eventTimeStamp", str_event_ts);

	rtError err = rtConnection_SendMessage(connectionSend, msg, "RDKC.CVR.CLIP.STATUS");
	rtLog_Debug("SendRequest:%s", rtStrError(err));

	if (err != RT_OK)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) Error sending msg via rtmessage\n", __FILE__,__LINE__);
	}
	rtMessage_Release(msg);
}

/** @description: deflates the queue with the frame data
 *                and pushes to the KVS API
 *  @param[in]: -NIL-
 *  @return: bool - SUCCESS or FAILURE
 */
bool CVR::pushFrames(frameInfoH264* frameInfo,
					char* fileName,
					int stream_id,
					unsigned short kvsclip_audio,
					EventType eventType = EVENT_TYPE_MAX,
					bool isEOF = false)
{
    int ret_kvs = RDKC_SUCCESS;
    unsigned short contentchangestatus = 0;

    if(!fileName)
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Empty clip name!! frame upload failure\n", __FILE__, __LINE__);
        return false;
    }
    
    //kvs init
    if(!iskvsInitDone)
    {
        do
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Invoking kvsInit with storage %lu\n", __FILE__, __LINE__,m_storageMem);
            ret_kvs = kvsInit(this, stream_id, m_storageMem);

            static int retry = 0;
            if (0 != ret_kvs)
            {
                retry++;
                if ( retry > KVSINITMAXRETRY )
                {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) : FATAL : Max retry reached in kvsInit exit process %d\n", __FILE__, __LINE__);
                    exit(1);
                }
                iskvsInitDone = false;
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) : kvsInit Failed retry number : %d \n", __FILE__, __LINE__,retry);
                sleep(2);
            }
            else if ( 0 == ret_kvs )
            {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsInit success \n", __FILE__, __LINE__);
                iskvsInitDone = true;
            }
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): After Invoking kvsInit\n", __FILE__, __LINE__);
        } while ( ret_kvs != 0);
    }

    //kvs stream init
    if ( false == iskvsStreamInitDone )
    {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Invoking kvsStreamInit \n", __FILE__, __LINE__ );
        contentchangestatus = 0;
        ret_kvs = kvsStreamInit(kvsclip_audio, kvsclip_highmem, contentchangestatus);
        if ( true == ret_kvs )
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsStreamInit success \n", __FILE__, __LINE__);
            iskvsStreamInitDone = true;
        }
        else
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): FATAL : Max retry reached in kvsStreamInit exit process \n", __FILE__, __LINE__);
            exit(1);
        }
    }

    bool cvr_upload_retry = false;

    do
    {
        RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR","%s(%d): check values : %u\n", __FILE__, __LINE__, kvsclip_audio);
		frameInfoH264 frameInfoTmp;
		memset(&frameInfoTmp, 0, sizeof(frameInfoH264));

		frameInfoTmp.stream_id = frameInfo->stream_id;
		frameInfoTmp.stream_type = frameInfo->stream_type;
		frameInfoTmp.pic_type = frameInfo->pic_type;
		frameInfoTmp.frame_ptr = frameInfo->frame_ptr;
		frameInfoTmp.frame_num = frameInfo->frame_num;
		frameInfoTmp.frame_size = frameInfo->frame_size;
		frameInfoTmp.jpeg_quality = frameInfo->jpeg_quality;
		frameInfoTmp.width = frameInfo->width;
		frameInfoTmp.height = frameInfo->height;
		frameInfoTmp.frame_timestamp = frameInfo->frame_timestamp;
		frameInfoTmp.arm_pts = frameInfo->arm_pts;
		frameInfoTmp.dsp_pts = frameInfo->dsp_pts;

#if DEBUG_DUMP_H264
		// 3 is PCM, 10 is AAC data
		if( XSTREAM_STREAM_TYPE_AAC == frameInfoTmp.stream_type ){

			if ( 512 >= frame_num_audio ) {

				if ( XSTREAM_STREAM_TYPE_AAC == frameInfoTmp.stream_type ) {
					write_bytes = fwrite( ( char * ) frameInfoTmp.frame_ptr, frameInfoTmp.frame_size, 1, fpAudioAAC );

					if ( 512 == frame_num_audio ) {
						printf("RDKC-XS %s(%d): ------ frame_num_audio = %d ------ \n", __FUNCTION__, __LINE__, frame_num_audio);
					}
					if ( write_bytes <= 0 ) {
						RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR","%s(%d) :CVR:: errmsg(%d)=%s!\n", __FILE__, __LINE__, errno, strerror( errno ) );
						break;
					}
					RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) :xStream CVR:: streamid %d stream_type %d pic_type %ld frame_num %ld frame_size %ld width %ld height %ld arm_pts %lld dsp_pts %lld \n", __FILE__, __LINE__, frameInfoTmp.stream_id, frameInfoTmp.stream_type, frameInfoTmp.pic_type, frameInfoTmp.frame_num, frameInfoTmp.frame_size, frameInfoTmp.width, frameInfoTmp.height, frameInfoTmp.arm_pts, frameInfoTmp.dsp_pts );
					RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : AUDIO timestamp:%lu kvsclip_audio = %d\n",__FILE__, __LINE__,frameInfoTmp.frame_timestamp, kvsclip_audio);
				}
				frame_num_audio++;
			}
			else {
				if ( NULL != fpAudioAAC ) {
					fclose( fpAudioAAC );
					fpAudioAAC = NULL;
				}
			}
		}
		else { //Dump Video frames
			if ( 512 >= frame_num ) {
				if ( 512 == frame_num ) {
					printf("RDKC-XS %s(%d): ------ frame_num = %d ------ \n", __FUNCTION__, __LINE__, frame_num);
				}
				write_bytes = fwrite( ( char * ) frameInfoTmp.frame_ptr, frameInfoTmp.frame_size, 1, fp );
				if ( write_bytes <= 0 ) {
					RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR","%s(%d) :CVR:: errmsg(%d)=%s!\n", __FILE__, __LINE__, errno, strerror( errno ) );
					break;
				}
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) :xStream CVR:: streamid %d stream_type %d pic_type %ld frame_num %ld frame_size %ld width %ld height %ld arm_pts %lld dsp_pts %lld \n", __FILE__, __LINE__, frameInfoTmp.stream_id, frameInfoTmp.stream_type, frameInfoTmp.pic_type, frameInfoTmp.frame_num, frameInfoTmp.frame_size, frameInfoTmp.width, frameInfoTmp.height, frameInfoTmp.arm_pts, frameInfoTmp.dsp_pts );
				RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d) : VIDEO timestamp:%lu, kvsclip_audio = %d\n",__FILE__, __LINE__,frameInfoTmp.frame_timestamp, kvsclip_audio);
				frame_num++;
			}
			else {
				if ( NULL != fp ) {
					fclose( fp );
					fp = NULL;
				}
			}
		}
#endif //DEBUG_DUMP_H264

        ret_kvs = kvsUploadFrames(kvsclip_audio, kvsclip_highmem, frameInfoTmp, fileName, isEOF);

        RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR","Pushing the Frames to KVS \n");
        if(isEOF)
        {
            RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR","EOF Frame pushed. Marking the End of a clip %s\n", fileName);
            break;
        }

        if (-1 == ret_kvs)
        { //failure
            cvr_upload_retry = false;
            RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d):  kvsUploadFrames : upload failed %s\n", __FILE__,__LINE__, fileName);
          //notify_smt_TN_uploadStatus(CVR_UPLOAD_FAIL, fileName);
        }
        else if (0 == ret_kvs)
        { //success
            cvr_upload_retry = false;
            RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): kvsUploadFrames : upload success : %s\n",__FILE__, __LINE__, fileName);

            if (event_type == EVENT_TYPE_MOTION)
            {
                RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): kvs Frame push successful with Motion\n",__FUNCTION__, __LINE__);
            }
            else 
            {
                RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): kvs Frame push Successful without Motion\n", __FUNCTION__, __LINE__);
            }

        }
        else if (1 == ret_kvs)
        {
            iskvsStreamInitDone = false;
            cvr_upload_retry	= true;
            contentchangestatus = 1;
            if (false == iskvsStreamInitDone)
            {
                RDK_LOG(RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): Re-Invoking kvs_stream_init \n", __FILE__,__LINE__);

                ret_kvs = kvsStreamInit(kvsclip_audio, kvsclip_highmem, contentchangestatus);
                if ( true == ret_kvs )
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Re-Invoking kvs_stream_init success \n", __FILE__, __LINE__);
                    iskvsStreamInitDone = true;
                }
                else
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): FATAL : Max retry reached in Re-Invoking kvsStreamInit exit process \n", __FILE__, __LINE__);
                    exit(1);
                }
            }
        }
        else 
        {
            RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): Unknown return status for clip %s\n", __FILE__,__LINE__, fileName);
        }

    }while(cvr_upload_retry);


    return true;
}

/** @description: do cvr process
 *  @param[in]: pCloudRecorderInfo - void pointer
 *  @return: void
 */

void CVR::do_cvr(void * pCloudRecorderInfo)
{
    int ret = RDKC_FAILURE;
    int rdkc_ret = 1;
    unsigned short lessMemoryCount = 0; // to restrict the logging of low memory
    unsigned short streamInitErrorCount = 0; // to restrict the logging of Stream Init Error Count
    unsigned short streamNotreadyErrorCount = 0; // to restrict the logging of Stream Init Error Count
    unsigned short idrFrameCount = 0;
    unsigned short pFrameCount = 0;
    unsigned short bFrameCount = 0;
    unsigned short audioFrameCount = 0;
    unsigned short totalFramesinClip = 0;
    int ret_videoStreamConfig = -1;
    cvr_provision_info_t *CloudRecorderInfo  = (cvr_provision_info_t*)pCloudRecorderInfo ;
    int iscvrenabled = atoi(CloudRecorderInfo->enable);
    //file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
    file_len = CVR_CLIP_DURATION;

    while (!term_flag)
    {
        /* reset all motion level(no motion, low motion, medium motion, high motion) counters */
        count_no = 0;
        count_low = 0;
        count_med = 0;
        count_high = 0;

        /* Enable/Disable DEBUG logs */
        if( access( ENABLE_CVR_RDK_DEBUG_LOG_FILE, F_OK ) != -1 ) {
            enable_debug = 1;
        }
        else {
            enable_debug = 0;
        }

        if (reload_cvr_flag)
        {
            rdkc_ret = cvr_read_config(CloudRecorderInfo);
            if (0 == rdkc_ret)
            {
                iscvrenabled = atoi(CloudRecorderInfo->enable);
                //file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
                file_len = CVR_CLIP_DURATION;
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is successful, enable=%d, file_len=%d!\n", __LINE__, iscvrenabled,file_len);
            }
            else
            {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__,iscvrenabled);
            }
            reload_cvr_flag = 0;
        }

        // Need close session if disconnect flag was set
        if (init_flag && local_stream_err)
        {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Local Stream Error- Close Video/Audio Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);

            if (_streamFd > 0) {
                if( 0 != objConsumer.StreamClose(_streamFd, (cvr_flag & XSTREAM_AV_FLAG_MASK)) ){
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Stream Close failed\n", __FILE__, __LINE__);
                }
                else {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream Close success\n", __FILE__, __LINE__);
                }
                _streamFd = -1;

                cvr_flag &= ~XSTREAM_AUDIO_FLAG;
                cvr_flag &= ~XSTREAM_VIDEO_FLAG ;
            }
            init_flag = 0;
            local_stream_err = 0;
        }

        //check cvr rfc params
        cvr_check_rfcparams();
        
        // CVR not enabled
        if (iscvrenabled == 0)
        {
            if (init_flag)
            {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): CVR disabled, Close Video/Audio Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
                if (_streamFd > 0) {
                    if( 0 != objConsumer.StreamClose(_streamFd, (cvr_flag & XSTREAM_AV_FLAG_MASK))) {
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Stream Close failed\n", __FILE__, __LINE__);
                    }
                    else {
                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream Close success\n", __FILE__, __LINE__);
                    }
                    _streamFd = -1;

                    cvr_flag &= ~XSTREAM_AUDIO_FLAG;
                    cvr_flag &= ~XSTREAM_VIDEO_FLAG ;
                }
            }
            init_flag = 0;
            sleep(1);
            continue; //whine first continue
        }
        
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): init_flag : %d\n", __FILE__, __LINE__,init_flag);

        // Init conn for clip building
        if (0 == init_flag)
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): cvr init start\n", __FILE__, __LINE__ );

            cvr_flag = XSTREAM_VIDEO_FLAG ;       // Video is must
#ifdef _SUPPORT_AAC_
            cvr_flag |= XSTREAM_AUDIO_FLAG;      // Audio is necessary if AAC enabled
#endif
            cvr_flag |= RDKC_STREAM_FLAG_PADDING;    // Padding is must

            cvr_flag |= RDKC_STREAM_FLAG_ABSTIMESTAMP;

            int avflag = XSTREAM_VIDEO_FLAG;
            if (CVR_AUDIO_ENABLED == cvr_audio_status) {
                avflag = (XSTREAM_VIDEO_FLAG | XSTREAM_AUDIO_FLAG);
            }
            else {
                cvr_flag &= ~XSTREAM_AUDIO_FLAG;
            }
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsclip_audio : %d : cvr_flag : %d\n", __FILE__, __LINE__, kvsclip_audio, cvr_flag );

            _streamFd = objConsumer.StreamInit( m_streamid, FORMAT_H264, avflag );
            if ( _streamFd < 0 ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Invalid socket fd 0x%x, stream initialization failed \n", __FILE__, __LINE__, _streamFd);
                ret = RDKC_FAILURE;
            }
            else {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream(%d) initialization success avflag = %d \n", __FILE__, __LINE__, _streamFd, avflag);
                ret = RDKC_SUCCESS;
            }

            cvr_frame = NULL;
            cvr_frame = objConsumer.GetH264FrameContainer();
            if(NULL == cvr_frame) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Error in retrieving the H264 frame container \n", __FILE__, __LINE__);
                return false;
            }

            if( RDKC_FAILURE == ret)
            {
                streamInitErrorCount++;
                if( 0 == (streamInitErrorCount % 5) ) {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): init local stream for read video frame error. Retry Count %d\n", __FILE__, __LINE__, streamInitErrorCount);
                }
                sleep(1);
                continue;
            }

            memset(&_videoConfig, 0, sizeof (stream_hal_stream_config));
            ret_videoStreamConfig = objConsumer.GetStreamConfig((m_streamid & VIDEO_MASK), &_videoConfig);
            if( ret_videoStreamConfig < 0 ) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Error in retreving video stream configuration for stream %d \n", __FILE__, __LINE__, (m_streamid & 0x0F) );
            }

            streamInitErrorCount = 0; //Resetting the streamInitErrorCount as StreamInit is success
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Init local stream for read video frame success, cvr_flag: %d!\n", __LINE__, cvr_flag);

            init_flag = 1;
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): CVR init succesfull!\n", __LINE__);

            // wait for I frame
            while (!term_flag)
            {
                ccode = objConsumer.ReadFrame( m_streamid, FORMAT_H264, cvr_frame );

                if (ccode < 0)
                {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                    local_stream_err = 1;
                    break;
                }
                else if (ccode)
                {
                    usleep(10000);
                    streamNotreadyErrorCount++;
                    // if we dont get the Iframe within 3 seconds, then exit the loop
                    if ( streamNotreadyErrorCount > (3*1000) ) {
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read first I frame error!\n", __FILE__, __LINE__);
                        local_stream_err = 1;
                        break;
                    }
                    continue;
                }
                else
                {
                    if(IAV_PIC_TYPE_IDR_FRAME == cvr_frame->pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame->pic_type)
                    {

                        //RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR","%s(%d): Got the first I Frame, _streamFd %d \n", __FILE__, __LINE__, _streamFd);
                        has_an_iframe = 1;
                    }
                    else {
                        RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR","Frame obtained is not an I Frame \n");
                        usleep(10000);
                        continue;
                    }

                    //gettimeofday(&start_t,NULL);
                    clock_gettime(CLOCK_REALTIME, &start_t);
                    start_msec = cvr_frame->frame_timestamp;
                    memcpy(cvr_key_frame, cvr_frame, sizeof(frameInfoH264));

                    RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): start_msec=%lu\n", __FILE__, __LINE__, start_msec);
                    break;
                }
            }
            streamNotreadyErrorCount = 0; // Resetting the error count as we got the Iframe
        }

        // If local stream error, continue
        if(1 == local_stream_err) {
            sleep(1);
            continue;
        }

        clipStatus = CVR_CLIP_GEN_START;
        frame_num_count = 0;
        motion_level_raw_sum = 0.0;
        event_type_raw = 0;
        memset(motion_statistics_info,0,sizeof(motion_statistics_info));
        motion_statistics_info[0] = 0;//first byte represent version.
        motion_level_idx = 1;

        // Generate the file name base on the time
        tv = gmtime(&start_t.tv_sec);
        snprintf(file_name, sizeof(file_name), "%04d%02d%02d%02d%02d%02d", (tv->tm_year+1900), tv->tm_mon+1, tv->tm_mday, tv->tm_hour, tv->tm_min, tv->tm_sec);
        // Generate the file name which used to save CVR clip
        cvr_starttime = start_t.tv_sec;

        //notify smart thumbnail clip creation started
        if(smartTnEnabled)
        {
            RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "Notifying the Clip Start to SMT with filename - %s\n", file_name);
            notify_smt_TN_clipStatus(CVR_CLIP_GEN_START, file_name);
        }

        if (has_an_iframe)
        {
            //write 367 bytes PAT-PMT pair and the first I frame to clip file.
            cvr_flag |= RDKC_STREAM_FLAG_ABSTIMESTAMP;

            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","(%d): Initialize clip creation, ts_fd=%d, cvr_flag=%d!\n", __LINE__, ts_fd, cvr_flag);
            pushFrames(cvr_key_frame, file_name, ( m_streamid & 0x0F ), kvsclip_audio);

            idrFrameCount++;

            has_an_iframe = 0;
            //Get the motion statistics information
            get_motion_statistics_info(cvr_key_frame, &frame_num_count, &event_type_raw, &motion_level_raw_sum);
        }

        while ((long)(file_len*1000 - compareTimestamp(amba_hwtimer_msec(hwtimer_fd), start_msec)) > 20)
        {
            if (term_flag || local_stream_err)
            {
                break;
            }

            // reload cvr config
            if (reload_cvr_flag)
            {
                rdkc_ret = cvr_read_config(CloudRecorderInfo);
                if (0 == rdkc_ret)
                {
                    iscvrenabled = atoi(CloudRecorderInfo->enable);
                    //file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
                    file_len = CVR_CLIP_DURATION;
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is successful, enable=%d, file_len=%d!\n", __LINE__, iscvrenabled,file_len);
                }
                else
                {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, iscvrenabled);
                }
                reload_cvr_flag = 0;
            }

            // CVR disabled
            if (iscvrenabled == 0)
            {
                RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVR","%s(%d): CVR is disabled.\n", __FILE__, __LINE__);
                break;
            }

            ccode = objConsumer.ReadFrame( m_streamid, FORMAT_H264, cvr_frame );

            if (ccode < 0)
            {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                local_stream_err = 1;
                break;
            }
            else if (0 == ccode)
            {
                if ((IAV_PIC_TYPE_IDR_FRAME == cvr_frame->pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame->pic_type)
                        && (long)(file_len*1000 - compareTimestamp(amba_hwtimer_msec(hwtimer_fd), start_msec) < 150))
                {
                    //This I frame is almost at the end of clip file, we'll keep it, to write into next clip file.
                    has_an_iframe = 1;
                    //gettimeofday(&start_t,NULL);
                    clock_gettime(CLOCK_REALTIME, &start_t);
                    start_msec = cvr_frame->frame_timestamp;
                    memcpy(cvr_key_frame, cvr_frame, sizeof(frameInfoH264));
                    RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): start_msec_iframe_clipend=%lu\n", __FILE__, __LINE__, start_msec);
                    break;
                }

                if (!has_an_iframe)
                {
                    pushFrames(cvr_frame, file_name, ( m_streamid & 0x0F ), kvsclip_audio);

                    if ((IAV_PIC_TYPE_IDR_FRAME == cvr_frame->pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame->pic_type) ) {
                        idrFrameCount++;
                    } else if (IAV_PIC_TYPE_P_FRAME == cvr_frame->pic_type) {
                        pFrameCount++;
                    } else if(IAV_PIC_TYPE_B_FRAME == cvr_frame->pic_type) {
                        bFrameCount++;
                    }
                    else if (XSTREAM_STREAM_TYPE_AAC == cvr_frame->stream_type) {
                        audioFrameCount++;
                    }

                    if (XSTREAM_STREAM_TYPE_AAC != cvr_frame->stream_type) {
                        //Get the timestamp of first frame of every second
                        if (compareTimestamp(cvr_frame->frame_timestamp,start_msec) <= 1000 * motion_level_idx)
                        {
                            get_motion_statistics_info(cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                        }
                        else if (motion_level_idx <= VIDEO_DURATION_MAX)
                        {
                            //caculate average motion level of every seconds.
                            if (frame_num_count > 0)
                            {
                                motion_statistics_info[motion_level_idx] = calculate_motion_level(motion_level_raw_sum,frame_num_count,event_type_raw);
                            }

                            //first frame of next second.
                            motion_level_idx++;
                            frame_num_count = 0;
                            motion_level_raw_sum = 0.0;
                            event_type_raw = 0;
                            get_motion_statistics_info(cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                        }
                        else
                        {
                            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):ERROR:buff of motion_statistics_info is full start_msec : %lu, cvr frame timestamp : %lu,  motion_level_idx : %d \n", __FILE__, __LINE__,start_msec,cvr_frame->frame_timestamp,motion_level_idx);
                            local_stream_err=1;
                            break;
                        }
                    }
                }
            }
            else if (ccode)      // 1 means no data to read
            {
                usleep(10000);
                continue;
            }
        }

        if (!has_an_iframe)     //file length reached but we haven't got an I frame. This is to look for a coming I frame.
        {
            if (term_flag || local_stream_err)
            {
                break;
            }

            while (!term_flag)
            {
                ccode = objConsumer.ReadFrame(m_streamid, FORMAT_H264, cvr_frame);
                if (ccode < 0)
                {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                    local_stream_err = 1;
                    break;
                }
                else if (ccode)
                {
                    usleep(10000);
                    continue;
                }
                else
                {
                    if (IAV_PIC_TYPE_IDR_FRAME == cvr_frame->pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame->pic_type)
                    {
                        has_an_iframe = 1;
                        //gettimeofday(&start_t,NULL);
                        clock_gettime(CLOCK_REALTIME, &start_t);
                        start_msec = cvr_frame->frame_timestamp;
                        memcpy(cvr_key_frame, cvr_frame, sizeof(frameInfoH264));
                        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): start_msec_iframe=%lu\n", __FILE__, __LINE__, start_msec);
                        break;
                    }
                    if (!has_an_iframe)
                    {
                        pushFrames(cvr_frame, file_name, ( m_streamid & 0x0F ), kvsclip_audio);

                        //save audio frame with timestamp less than start_msec, else continue
                        if ((XSTREAM_STREAM_TYPE_AAC == cvr_frame->stream_type) && (cvr_frame->frame_timestamp + AUDIO_DURATION > start_msec)) {
                            continue;
                        }

                        if ((IAV_PIC_TYPE_IDR_FRAME == cvr_frame->pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame->pic_type) ) {
                            idrFrameCount++;
                        } else if (IAV_PIC_TYPE_P_FRAME == cvr_frame->pic_type) {
                            pFrameCount++;
                        } else if(IAV_PIC_TYPE_B_FRAME == cvr_frame->pic_type) {
                            bFrameCount++;
                        } else if (XSTREAM_STREAM_TYPE_AAC == cvr_frame->stream_type) {
                            audioFrameCount++;
                        }

                        if (XSTREAM_STREAM_TYPE_AAC != cvr_frame->stream_type) {
                            //Get the timestamp of first frame of every second
                            if (compareTimestamp(cvr_frame->frame_timestamp,start_msec) <= 1000 * motion_level_idx)
                            {
                                get_motion_statistics_info(cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                            }
                            else if (motion_level_idx <= VIDEO_DURATION_MAX)
                            {
                                //caculate average motion level of every seconds.
                                if (frame_num_count > 0)
                                {
                                    motion_statistics_info[motion_level_idx] = calculate_motion_level(motion_level_raw_sum,frame_num_count,event_type_raw);
                                }
                                //first frame of next second.
                                motion_level_idx++;
                                frame_num_count = 0;
                                motion_level_raw_sum = 0.0;
                                event_type_raw = 0;
                                get_motion_statistics_info(cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                            }
                            else
                            {
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):ERROR:buff of motion_statistics_info is full start_msec : %lu, cvr frame timestamp : %lu,  motion_level_idx : %d \n", __FILE__, __LINE__,start_msec,cvr_frame->frame_timestamp,motion_level_idx);
                                local_stream_err=1;
                                break;
                            }
                        }
                    }
                }
            }
        }

        //caculate the last second of  clip
        if ( (XSTREAM_STREAM_TYPE_AAC != cvr_frame->stream_type) && (frame_num_count > 0 && motion_level_idx <= VIDEO_DURATION_MAX) )
        {
            motion_statistics_info[motion_level_idx] = calculate_motion_level(motion_level_raw_sum,frame_num_count,event_type_raw);
        }

        if (!term_flag)
        {
            // Call app to send out to server
            //gettimeofday(&end_t,NULL);
            clock_gettime(CLOCK_REALTIME, &end_t);
            snprintf(endtime, sizeof(endtime), "%lu.%lu", end_t.tv_sec, end_t.tv_nsec);
            RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): endtime=[%s]\n", __FILE__, __LINE__, endtime);
            event_type = EVENT_TYPE_MAX;

            //resetting event date time
            memset(&event_datetime, 0, sizeof(time_t));
            event_datetime = DEFAULT_EVT_TSTAMP;
            cvr_get_event_info(&event_type,&event_datetime,cvr_starttime);

            if(RDKC_FAILURE != ret_videoStreamConfig) {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): CVR configuration: StreamID Resolution(W*H) FrameRate BitRate %d %d*%d %d %d \n", __FILE__, __LINE__, (m_streamid & VIDEO_MASK), _videoConfig.width, _videoConfig.height, _videoConfig.frame_rate, _videoConfig.bit_rate);
            }

            totalFramesinClip = idrFrameCount + pFrameCount + bFrameCount + audioFrameCount;
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s clip frame stats :  idrFrameCount : %d, pFrameCount : %d, bFrameCount : %d, audioFrameCount : %d, totalFramesinClip : %d\n"
                                , __FILE__, __LINE__,file_name, idrFrameCount,pFrameCount,bFrameCount,audioFrameCount, totalFramesinClip);
            idrFrameCount = 0;
            pFrameCount = 0;
            bFrameCount = 0;
            audioFrameCount = 0;
            totalFramesinClip = 0;
            strcpy(fileName, file_name);

            //if local stream error is met while creating clip discard the clip sine it's an incomplete ts clip
            if( 0 == local_stream_err ) {
                long fileIndex = atol(file_name);
                //Uploading  data to the server
                if((od_frame_upload_enabled) &&
                        (EVENT_TYPE_MOTION == event_type) &&
                        (top >= 0))
                {
                    stringify_od_frame_data();
                }
                if(smartTnEnabled)
                {
                    //send notification to smart thumbnail clip creation end.
                    if(DEFAULT_EVT_TSTAMP == event_datetime)
                    {
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Notified clip end to smt with default timestamp - filename (%s) \n", __FILE__, __LINE__, file_name);
                        notify_smt_TN_clipStatus(CVR_CLIP_GEN_END, file_name);
                    }
                    else
                    {
                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Notified clip end to smt with valid timestamp - %ld - filename (%s) \n", __FILE__, __LINE__, event_datetime, file_name);
                        notify_smt_TN_clipStatus(CVR_CLIP_GEN_END, file_name, event_datetime);
                    }
                    eventMap.insert(std::pair<long, EventType>(fileIndex, event_type));
                    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Inserted into the eventMap\n", __FILE__, __LINE__);
                    pushFrames(cvr_frame, file_name, ( m_streamid & 0x0F ), kvsclip_audio, event_type, true);
                }
                else
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Smart thumbnail disabled. Pushing frames\n", __FILE__, __LINE__);
                    eventMap.insert(std::pair<long, EventType>(fileIndex, event_type));
                    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Inserted into the eventMap\n", __FILE__, __LINE__);
                    pushFrames(cvr_frame, file_name, ( m_streamid & 0x0F ), kvsclip_audio, event_type, true);
                }

                if(kvsclip_audio)
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s kvs clip has audio\n", __FILE__, __LINE__, file_name);
                }

                RDK_LOG(RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): No Motion: %d, Low Motion: %d, Medium Motion: %d, High Motion: %d\n",__FILE__, __LINE__, count_no,      count_low, count_med, count_high);
                clipStatus = CVR_CLIP_GEN_END;

                if (count_motion_mismatch > 0) {
                    RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Motion Mismatch count: %d \n", __FILE__, __LINE__, count_motion_mismatch);
                    count_motion_mismatch = 0;
                }

                if(od_frame_upload_enabled)
                {
                    reset_od_frame_data();
                }
            }
        }
        else
        {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Ending after term_flag enabled \n", __FILE__, __LINE__);
        }
    }
}

/** @description: close cvr
 *  @param[in] argv - char pointer
 *  @return: int
 */
int CVR::cvr_close()
{
	//rtmessageCVRThreadExit = true;
        RFCRelease();
        config_release();
        polling_config_exit();

#ifdef RTMSG
        rtmessageCVRThreadExit = true;
        rtConnection_Destroy(connectionRecv);
#endif
        // Close local stream session
        if (init_flag) {
            if (_streamFd > 0) {

                if(cvr_key_frame) {
                    free(cvr_key_frame);
                    cvr_key_frame = NULL;
                }

                if( 0 != objConsumer.StreamClose(_streamFd, (cvr_flag & XSTREAM_AV_FLAG_MASK))) {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR", "%s(%d): Stream Close failed\n", __FILE__, __LINE__);
                }
                else {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR", "%s(%d): Stream Close success\n", __FILE__, __LINE__);
                }
                _streamFd = -1;
                cvr_frame = NULL;

                cvr_flag &= ~XSTREAM_AUDIO_FLAG;
                cvr_flag &= ~XSTREAM_VIDEO_FLAG;
            }
        }

        // Close hwtimer
        CVR::amba_hwtimer_exit(hwtimer_fd);
		hwtimer_fd = -1;

        unlink(LOCK_FILENAME_CVR_DAEMON);
        return 0;

}

volatile sig_atomic_t CVR::term_flag = 0;
void CVR::self_term(int sig)
{
        CVR::term_flag = 1;
//	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Received signal SIGTER!!\n", __FILE__, __LINE__);
}

volatile sig_atomic_t CVR::reload_cvr_flag = 1;
void CVR::reload_config(int dummy)
{
        CVR::reload_cvr_flag = 1;
        CVR::reload_cvr_config = 1;
}

/** @description: main
 *  @return: int
 */
int main(int argc, char *argv[])
{
    cvr_provision_info_t CloudRecorderInfo;
    const char* debugConfigFile = NULL;
    const char* configFilePath = NULL;
    int itr=0;
    // Init signal handler
    (void) signal(SIGTERM, CVR::self_term);
    (void) signal(SIGUSR1, CVR::reload_config);
    unsigned short kvsclip_audio =0;/* audio enable flag */
    int streamId = DEF_CVR_CHANNEL;
    uint64_t storageMemory=0;
    while (itr < argc)
    {
        if(strcmp(argv[itr],"--debugconfig")==0)
        {
            itr++;
            if (itr < argc)
            {
                debugConfigFile = argv[itr];
            }
            else
            {
                break;
            }
        }

        if(strcmp(argv[itr],"--configFilePath")==0)
        {
            itr++;
            if (itr < argc)
            {
                configFilePath = argv[itr];
            }
            else
            {
                break;
            }
        }

	if(strcmp(argv[itr],"--kvsAudioEnabled")==0)
        {
            itr++;
            if (itr < argc)
            {
                kvsclip_audio = (unsigned short) atoi(argv[itr]);
            }
            else
            {
                break;
            }
        }

	if(strcmp(argv[itr],"--streamId")==0)
        {
            itr++;
            if (itr < argc)
            {
                streamId = (unsigned short) atoi(argv[itr]);
            }
            else
            {
                break;
            }
        }

	if(strcmp(argv[itr],"--sdkMem")==0)
        {
            itr++;
            if (itr < argc)
            {
               storageMemory  = (uint64_t) atoi(argv[itr]);
            }
            else
            {
                break;
            }
        }

        itr++;
    }

    if(rdk_logger_init(debugConfigFile) == 0) 
    {
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s rdk logger enabled \n", __FILE__);;
    }
    memset(&CloudRecorderInfo, 0, sizeof(cvr_provision_info_t));

    /* Registering callback function for Breakpadwrap Function */
#ifdef BREAKPAD
    sleep(1);
    BreakPadWrapExceptionHandler eh;
    eh = newBreakPadWrapExceptionHandler();
#endif
    
    CVR cvr_object;
    cvr_object.setCVRStreamId(streamId);

    int ret = cvr_object.cvr_init(kvsclip_audio,&CloudRecorderInfo,storageMemory);
    if(CVR_FAILURE == ret) {
	goto error_exit;
    }

    cvr_object.do_cvr(&CloudRecorderInfo);

error_exit:
    cvr_object.cvr_close();
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s exit! - sigterm flag - %d \n", __FILE__, __LINE__, argv[0],CVR::term_flag);
    return 0;
}
