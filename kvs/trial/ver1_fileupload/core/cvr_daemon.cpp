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
#include "cvr_daemon.h"

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
#ifdef RTMSG
rtConnection CVR::connection;
rtError CVR::err;
#endif
std::queue<vai_result_t, std::list<vai_result_t> > CVR::rtmessageCVRQ;
std::mutex CVR::rtmessageCVRMutex;
#ifdef RTMSG
bool CVR::rtmessageCVRThreadExit;
volatile bool CVR::smartTnEnabled = false;
#endif
std::condition_variable CVR::msgCv;
RdkCPluginFactory* CVR::temp_factory = CreatePluginFactoryInstance(); //creating plugin factory instance
RdkCVideoCapturer* CVR::recorder = ( RdkCVideoCapturer* )temp_factory->CreateVideoCapturer();
int CVR::local_stream_err = 0;
volatile sig_atomic_t CVR::reload_cvr_config = 1;

using namespace std;

//std::thread rtMessage_recv_thread;

/** @description: Constructor
 *  @param[in] void
 */
CVR::CVR(): init_flag(0),
            load_config_flag(0),
	    check_polling_config_timeout( XFINITY_POLLING_CONFIG_TIMEOUT ),
	    event_type( EVENT_TYPE_MAX),
            m_fd (-1),
	    frame_num_count(0),
	    motion_level_raw_sum(0),
	    event_type_raw(0),
 	    motion_level_idx (1),
	    pid(0),
	    file_fd(0),
 	    i(0),
	    ccode(0),
	    ts_fd(0),
	    cvr_flag(0),
	    hwtimer_fd(0),
	    //local_stream_err(0),
	    file_len(CVR_CLIP_DURATION),
	    file_num(CVR_CLIP_NUMBER),
	    file_format(CLOUD_RECORDER_VIDEO_TYPE_TS),
	    stream_id(DEF_CVR_CHANNEL),
	    has_an_iframe(0),
 	    target_duration(0),
	    sequence(0),
	    isIPAcquired(0),
	    start_msec(0),
	    count_no(0),
	    count_low(0),
	    count_med(0),
	    count_high(0)
{
#ifdef RTMSG
	rtmessageCVRThreadExit = false;
#endif
	memset(&cvr_key_frame,0,sizeof(RDKC_FrameInfo));
}

CVR::~CVR()
{
        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","Destructor called \n", __FILE__, __LINE__);
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

# if 0
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

        //Handle the rtmessage request
        if (rtMessageHeader_IsRequest(hdr))
        {
                char* buff = NULL;
                uint32_t buff_length = 0;

                rtMessage_ToString(req, &buff, &buff_length);
                rtLog_Info("Req : %.*s", buff_length, buff);
                free(buff);

                rtMessage_GetString(req, "status", &status);

                RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","(%s):%d smart thumbnail status: %s\n", __FUNCTION__, __LINE__, status);

                if(!strcmp(status, "start")) {
                        smartTnEnabled = true;
                } else {
                        smartTnEnabled = false;
                }

                // create response
                rtMessage res;
                rtMessage_Create(&res);
                rtMessage_SetString(res, "reply", "Success");
                rtConnection_SendResponse(con, hdr, res, 1000);
                rtMessage_Release(res);
        }
        rtMessage_Release(req);
}
#endif

/** @description: convert iObject to th_Object
 *  @param[in]  hdr : constant pointer rtMessageHeader
 *  @param[in] buff : constant pointer uint8_t
 *  @param[in]    n : uint32_t
 *  @param[in] closure : void pointer
 *  @return: void
 */
void CVR::on_message_cvr(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure)
{

#ifdef XFINITY_SUPPORT
        static CloudRecorderConf CloudRecorderInfo;
#else
        static RdkCCloudRecorderConf CloudRecorderInfo;
#endif

	if ( reload_cvr_config ) {
#ifdef XFINITY_SUPPORT
        	memset(&CloudRecorderInfo, 0, sizeof(CloudRecorderConf));
		if ( ReadCloudRecorderInfo(&CloudRecorderInfo) ) {
#else
        	memset(&CloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
		if ( RdkCReadCloudRecorderSection(&CloudRecorderInfo) ) {
#endif
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Reload cloud recorder configuration error!\n", __FILE__, __LINE__);
		}
		reload_cvr_config = 0;
	}

	// No need to process the analytics message if cvr is disabled or stream error
	if ( (CloudRecorderInfo.enable == 0) || 
             (local_stream_err == 1) ) {
	
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
	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): timestamp: %llu, od_event_type = %lu, motion_level_raw = %f \n", __FILE__, __LINE__,(unsigned long long)vai_result_recved_rtmsg.timestamp, (unsigned long)vai_result_recved_rtmsg.event_type, vai_result_recved_rtmsg.motion_level_raw);

	push_msg(vai_result_recved_rtmsg);
}

/** @description: receive message
 *  @param[in]  :  void
 *  @return: void
 */
void CVR::receive_rtmessage()
{
    while(!rtmessageCVRThreadExit) {
	    err = rtConnection_Dispatch(connection);
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
 *  @param[in] p_cvr_frame - RDKC_FrameInfo structure 
 *  @param[in] p_frame_num_count - unsigned int pointer
 *  @param[in] p_event_type_raw -  uint8_t pointer
 *  @param[in] p_motion_level_raw_sum - float pointer
 *  @return: int
 */
int CVR::get_motion_statistics_info(RDKC_FrameInfo *p_cvr_frame, unsigned int *p_frame_num_count,uint8_t *p_event_type_raw, float *p_motion_level_raw_sum)
{
        vai_result_t *p_od_result = NULL;
        static unsigned int iCount = 0;

#ifdef RTMSG
        //vai_result_t vai_result_recved;
        //memset(&vai_result_recved, 0, sizeof(vai_result_t));
        pop_msg(&vai_result_recved);
#endif

#ifdef RTMSG
        //p_od_result = &vai_result_recved_rtmsg;
        p_od_result = &vai_result_recved;
	if(od_frame_upload_enabled) {

		if(first_frame_of_clip){
			memcpy(va_engine_version, p_od_result -> od_frame_data.va_engine_version, sizeof(va_engine_version));
			first_frame_of_clip = false;
		}

		update_od_frame_data(p_od_result);
	}
#else
        if (p_cvr_frame->padding_len > 0 && NULL != (unsigned char *)p_cvr_frame->padding_ptr)
        {
                p_od_result = (vai_result_t *)p_cvr_frame->padding_ptr;

#endif
		/* Range of Motion Level                             Type of Motion
		  ----------------------- 			    ----------------
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
                //if (0 == p_od_result->timestamp || abs(p_od_result->timestamp - p_cvr_frame->arm_pts) > 45000) {
                if (0 == p_od_result->timestamp || abs((long long)(p_cvr_frame->arm_pts - p_od_result->timestamp)) > (45000 * 2) ) {
                        iCount++;
                        if(iCount == 600) {  //log every one minute @10fps

                                RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVR","%s(%d):All event are disabled, needn't get motion statistics!!!\n ", __FILE__, __LINE__);
				//RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVR","%s(%d): p_od_result->timestamp : %llu, p_cvr_frame->arm_pts: %llu, abs(p_cvr_frame->arm_pts - p_od_result->timestamp): %d!!!\n", __FILE__, __LINE__, (unsigned long long)p_od_result->timestamp, (unsigned long long)p_cvr_frame->arm_pts, (int)abs(p_cvr_frame->arm_pts - p_od_result->timestamp));
				RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVR","%s(%d): MotionTimestamps p_od_result p_cvr_frame abs_diff: %llu, %llu, %lld\n", __FILE__, __LINE__, (unsigned long long)p_od_result->timestamp, (unsigned long long)p_cvr_frame->arm_pts, abs((long long)(p_cvr_frame->arm_pts - p_od_result->timestamp)));
                                iCount = 0;
                        }
                        return 0;
                }

                (*p_frame_num_count)++;
                (*p_motion_level_raw_sum) = (*p_motion_level_raw_sum) + p_od_result->motion_level_raw;
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): frame_num = %d, motion_level_raw = %f od_event_type = %02x\n", __FILE__, __LINE__,*p_frame_num_count, p_od_result->motion_level_raw, p_od_result->event_type);
                if (p_od_result->event_type & (1 << EVENT_TYPE_PEOPLE)) {
                        (*p_event_type_raw)= (*p_event_type_raw) | CVR_EVENT_TYPE_PEOPLE_MASK;
#ifdef RTMSG
		    if( 0 == cvr_event_seconds[EVENT_TYPE_PEOPLE] ) {
#if 0
			struct tm* tv = NULL;
			tv = gmtime(&start_t.tv_sec);
                        cvr_event_seconds[EVENT_TYPE_PEOPLE] = start_t.tv_sec;
#endif		// #if 0
			cvr_event_seconds[EVENT_TYPE_PEOPLE] = p_od_result-> curr_time;
		    }
#endif
                }

                if (p_od_result->event_type & (1 << EVENT_TYPE_TAMPER)) {
                        (*p_event_type_raw) = (*p_event_type_raw) | CVR_EVENT_TYPE_TAMPER_MASK;
#ifdef RTMSG
		    if( 0 == cvr_event_seconds[EVENT_TYPE_TAMPER] ) {
#if 0
			struct tm* tv;
                        tv = gmtime(&start_t.tv_sec);
                        cvr_event_seconds[EVENT_TYPE_TAMPER] = start_t.tv_sec;
#endif
                        cvr_event_seconds[EVENT_TYPE_TAMPER] = p_od_result-> curr_time;
		    }
#endif
                }
#ifdef RTMSG
              if (p_od_result->event_type & (1 << EVENT_TYPE_MOTION)) {
                        //(*p_event_type_raw) = (*p_event_type_raw) | CVR_EVENT_TYPE_MOTION_MASK;
		    if( 0 == cvr_event_seconds[EVENT_TYPE_MOTION] ) {
#if 0
			struct tm* tv;
                        tv = gmtime(&start_t.tv_sec);
                        cvr_event_seconds[EVENT_TYPE_MOTION] = start_t.tv_sec;
#endif		// #if 0
                        cvr_event_seconds[EVENT_TYPE_MOTION] = p_od_result-> curr_time;
		    }
                }
#else
        }
#endif

        return 0;
}

/** @description: getting cvr event info
 *  @param[in] : event_type - EventType pointer
 *  @param[in] : event_datetime - time_t pointer
 *  @param[in] : cvr_starttime - time_t struct
 *  @return: int
 */
int CVR::cvr_get_event_info( EventType *event_type,time_t *event_datetime,time_t cvr_starttime)
{
#ifdef RTMSG

#else
        time_t cvr_event_seconds[EVENT_TYPE_MAX];

        if (0 != read_cvr_event_seconds_into_file(cvr_event_seconds))
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read cvr event timestamp error!\n", __FILE__, __LINE__);
                return -1;
        }
#endif
        if (cvr_event_seconds[EVENT_TYPE_MOTION] >= cvr_starttime - 1)
        {
                *event_type = EVENT_TYPE_MOTION;
                *event_datetime = cvr_event_seconds[EVENT_TYPE_MOTION];
                cvr_event_seconds[EVENT_TYPE_MOTION] = 0;
        }
#ifdef _SUPPORT_TAMPER_DETECTION_
        else if (cvr_event_seconds[EVENT_TYPE_TAMPER] >= cvr_starttime - 1)
        {
                *event_type = EVENT_TYPE_TAMPER;
                *event_datetime = cvr_event_seconds[EVENT_TYPE_TAMPER];
                cvr_event_seconds[EVENT_TYPE_TAMPER] = 0;
        }
#endif
#ifdef _SUPPORT_OBJECT_DETECTION_
        else if (cvr_event_seconds[EVENT_TYPE_PEOPLE] >= cvr_starttime - 1)
        {
                *event_type = EVENT_TYPE_PEOPLE;
                *event_datetime = cvr_event_seconds[EVENT_TYPE_PEOPLE];
                cvr_event_seconds[EVENT_TYPE_PEOPLE] = 0;
        }
#endif

#ifdef RTMSG

#else
        write_cvr_event_seconds_into_file(cvr_event_seconds);   // Need write cvr event seconds back after we use it
#endif
        return 0;
}

/** @description: lock file
 *  @param[in] fname :char pointer
 *  @return: int
 */
int CVR::cvr_daemon_check_filelock(char *fname)
{
        int fd = -1;
        pid_t pid = -1;

        fd = open(fname, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(fd < 0 && errno == EEXIST) {
                fd = open(fname, O_RDONLY, 0644);
                if (fd >= 0) {
                        read(fd, &pid, sizeof(pid));
                        kill(pid, SIGTERM);
                        close(fd);
                        sleep(1);
                        if (CheckAppsPidAlive("cvr_daemon", pid)) {
                                kill(pid, SIGTERM);
                        }
                        else {
                                unlink(fname);
                        }
                }
                return -2;
        }
        return fd;
}

/** @description: getting audio streamm id
 *  @param[in] audio_index - int 
 *  @return: int
 */
int CVR::get_audio_stream_id(int audio_index)
{
        int i = 0;
        int ret = -1;
        AUD_Conf aconf;

        if (audio_index < AUDIO_STREAM_INDEX_0 || audio_index >= AUDIO_STREAM_INDEX_MAX) {
                return ret;
        }
        if (ODO_OK != AUD_GetConf(&aconf)) {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Get audio configuration error!\n", __FILE__, __LINE__);
                return ret;
        }
        if (0
#ifdef _SUPPORT_AAC_
        || (AUDIO_CODEC_AAC == aconf.aud_stream[audio_index].mic_encoded_type && aconf.aud_stream[audio_index].mic_enable)
#endif
#ifdef _SUPPORT_MP3_
        || (AUDIO_CODEC_MP3 == aconf.aud_stream[audio_index].mic_encoded_type && aconf.aud_stream[audio_index].mic_enable)
#endif
        )
        {
                ret =  audio_index;
        }
        else {
                for (i = AUDIO_STREAM_INDEX_0; i < AUDIO_STREAM_INDEX_MAX; i++)
                {
                        if (0
#ifdef _SUPPORT_AAC_
                        || (AUDIO_CODEC_AAC == aconf.aud_stream[i].mic_encoded_type && aconf.aud_stream[audio_index].mic_enable)
#endif
#ifdef _SUPPORT_MP3_
                        || (AUDIO_CODEC_MP3 == aconf.aud_stream[i].mic_encoded_type && aconf.aud_stream[audio_index].mic_enable)
#endif
                        )
                        {
                                ret = i;
                                break;
                        }
                }
        }

        return ret;

}

/** @description: read configuration
 *  @param[in] pCloudRecorderInfo : CloudRecorderConf pointer OR RdkCCloudRecorderConf pointer
 *  @return: int
 */
#ifdef XFINITY_SUPPORT
int CVR::cvr_read_config(CloudRecorderConf *pCloudRecorderInfo)
{
        // Read cloud recorder server info
        memset(pCloudRecorderInfo, 0, sizeof(CloudRecorderConf));
        if (ReadCloudRecorderInfo(pCloudRecorderInfo))
#else
int CVR::cvr_read_config(RdkCCloudRecorderConf *pCloudRecorderInfo)
{
        // Read cloud recorder server info
        memset(pCloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
        if (RdkCReadCloudRecorderSection(pCloudRecorderInfo))
#endif
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read cloud recorder configuration error!\n", __FILE__, __LINE__);
                return -1;
        }
        return 0;

}

/** @description: enable cvr audio
 *  @param[in] val : bool
 *  @return: int
 */
int CVR::cvr_enable_audio(bool val)
{
        int ret = 0;
        int cfg_chg_flag = 0;
        char cvrAudioMsg[CGI_ERR_MSG_SIZE_MAX];

        // Read all config settings
	g_pconf = (All_Conf*)malloc(8*1024*7); // allocating more memory as a work around to fix RDKC-3002
	if(NULL == g_pconf) {
                RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Memory allocation error for All Conf!!\n", __FUNCTION__, __LINE__);
		return -1;
	}
	memset(g_pconf, 0, 8*1024*7);
	if (ReadAllConf(g_pconf, cvrAudioMsg) != 0)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): ReadAllConf failed!\n", __FILE__, __LINE__);
		if(g_pconf) {
			free(g_pconf);
		}
                return -1;
        }

        if( true == val )
        {
                char value_enable[2] = "1";

                if(set_stream_audio_enable_in(value_enable,1,cvrAudioMsg,g_pconf,NULL,VIDEO_CODEC_H264,H264_STREAM_INDEX_3) != 0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio of H264[%d] enable!\n", __FUNCTION__, __LINE__,H264_STREAM_INDEX_3);
			if(g_pconf) {
				free(g_pconf);
			}
                        return -1;
                }
#ifndef XCAM2 
                if(set_audio_mic_enable_2(value_enable, 1, cvrAudioMsg, (void*) g_pconf, (LIST *)NULL) != 0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio enable!\n", __FUNCTION__, __LINE__);
			if(g_pconf) {
				free(g_pconf);
			}
                        return -1;
                }
#else
		if (set_audio_mic_enable(value_enable, 1, cvrAudioMsg, (void*) g_pconf, NULL) != 0)
		{
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio mic enable!\n", __FUNCTION__, __LINE__);
                        return -1;
		}
		//set in_audio_type is AAC
		if (set_audio_mic_encoded_type_cvr(1,(void*) g_pconf,NULL)!=0)
		{
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio encode type to AAC!\n", __FUNCTION__, __LINE__);
                        return -1;
		}
#endif
        }
        else if( false == val )
        {
                char value_disable[2] = "0";

                if(set_stream_audio_enable_in(value_disable,1,cvrAudioMsg,g_pconf,NULL,VIDEO_CODEC_H264,H264_STREAM_INDEX_3) != 0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio of H264[%d] disable!\n", __FUNCTION__, __LINE__,H264_STREAM_INDEX_3);
			if(g_pconf) {
				free(g_pconf);
			}
                        return -1;
                }
#ifndef XCAM2
                if(set_audio_mic_enable_2(value_disable, 1, cvrAudioMsg, (void*) g_pconf, (LIST *)NULL) != 0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio disable!\n", __FUNCTION__, __LINE__);
			if(g_pconf) {
				free(g_pconf);
			}
                        return -1;
                }
#else
		RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): disabling audio\n", __FUNCTION__, __LINE__);
		if (set_audio_mic_enable(value_disable, 1, cvrAudioMsg, (void*) g_pconf, NULL) != 0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio mic enable!\n", __FUNCTION__, __LINE__);
                        return -1;
                }
                //set in_audio_type is AAC
                if (set_audio_mic_encoded_type_cvr(0,(void*) g_pconf,NULL)!=0)
                {
                        RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set audio encode type to AAC!\n", __FUNCTION__, __LINE__);
                        return -1;
                }

#endif
        }

        if (SaveAllConf(g_pconf, cvrAudioMsg, 1, &cfg_chg_flag) != 0)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): SaveAllConf failed!\n", __FILE__, __LINE__);
		if(g_pconf) {
			free(g_pconf);
		}
                return -1;
        }

        // Make the new settings work
        if (cfg_chg_flag)
        {
                RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): All function react!\n", __FILE__, __LINE__);
                AllFuncReact(g_pconf, cvrAudioMsg);
        }
	if(g_pconf) {
		free(g_pconf);
	}

        return 0;

}

/** @description: set cvr audio
 *  @param[in] void
 *  @return: int
 */
int CVR::cvr_set_audio()
{
        /* set cvr audio through RFC files */
        char value[MAX_SIZE] = {0};
        char usr_value[8] = {0};
        char *configParam = NULL;
        int prev_cvr_audio_status =  cvr_audio_status;

        /* Check if RFC configuration file exists */
        if( RDKC_SUCCESS == IsRFCFileAvailable(RFCFILE) ) {
                /* Get the value from RFC file */
                if( RDKC_SUCCESS == GetValueFromRFCFile(RFCFILE, CVR_AUDIO, value) ) {
                        if( strcmp(value, RDKC_TRUE) == 0 ) {
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
                        }
                        else if( strcmp(value, RDKC_FALSE) == 0 ) {
                                cvr_audio_status = CVR_AUDIO_DISABLED;
                                RDK_LOG( RDK_LOG_DEBUG1, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to disable.\n", __FILE__, __LINE__);
                        }
                }
		else
		{	RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): cvr_set_audio : cvr audio feature is not available in rfc file.\n",__FILE__, __LINE__);
			cvr_audio_status = CVR_AUDIO_DISABLED;
		}
                /* if the get value from rfc fails, dont do anything. use defult settings done by polling config. */
        }
	else {
                /* if the rfc files are not available, disable CVR audio */
                cvr_audio_status = CVR_AUDIO_DISABLED;
                RDK_LOG( RDK_LOG_DEBUG1, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to disable since rfc file is not available\n", __FILE__, __LINE__);
        }

        if( prev_cvr_audio_status != cvr_audio_status ) {
                if( CVR_AUDIO_ENABLED == cvr_audio_status && !cvr_enable_audio(true) ) {
                        RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): CVR audio is enabled.\n", __FILE__, __LINE__);
                }
                else if( CVR_AUDIO_DISABLED == cvr_audio_status && !cvr_enable_audio(false) ) {
                        RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): CVR audio is disabled.\n", __FILE__, __LINE__);
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
                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): %s is not available in rfc file.\n",__FILE__, __LINE__, feature_name);
                return false;
        }
        /* If RFC file is not present, disable the featur */
    } else {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.XCV","%s(%d): rfc feature file %s is not present.\n",__FILE__, __LINE__, rfc_feature_fname);
        return false;
    }
    return false;
}

/** @description: initialize cvr
 *  @param[in] argv - char pointer
 *  @param[in] pCloudRecorderInfo - CloudRecorderConf pointer
 *  @return: CVR_Failure is failed , CVR_SUCCESS if success
 */
int CVR::cvr_init(int argc, char* argv[],CloudRecorderConf *pCloudRecorderInfo)
{
	int rdkc_ret = 1;
        /* process control */
        file_fd = CVR::cvr_daemon_check_filelock(LOCK_FILENAME_CVR_DAEMON);
        if (-2 == file_fd)
        {
                file_fd = CVR::cvr_daemon_check_filelock(LOCK_FILENAME_CVR_DAEMON);
        }
        if (file_fd < 0)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): %s run error!\n", __FILE__, __LINE__, argv[0]);
                return CVR_FAILURE;
        }
        pid = getpid();
        write(file_fd, &pid, sizeof(pid));
        close(file_fd);

        // Init hwtimer, we need a timestamp which from the same source of video/audio frame timestamp
        hwtimer_fd = CVR::amba_hwtimer_init();
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): amba_hwtimer_init success\n", __FILE__, __LINE__);

        if (hwtimer_fd < 0)
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): amba_hwtimer_init error\n", __FILE__, __LINE__);
                return CVR_FAILURE;
        }
        // Init cvr flag
        cvr_flag = RDKC_STRAM_FLAG_VIDEO;       // Video is must
#ifdef _SUPPORT_AAC_
        cvr_flag |= RDKC_STRAM_FLAG_AUDIO;      // Audio is necessary if AAC enabled
        stream_id |= 0x10;                      //Default audio channel is 1.
#endif
        cvr_flag |= RDKC_STRAM_FLAG_PADDING;    // Padding is must
	conf = new camera_resource_config_t;
	//conf->KeyValue = stream_id + MAX_ENCODE_STREAM_NUM;

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

        rtConnection_Create(&connection, "CVR", "tcp://127.0.0.1:10001");
        rtConnection_AddListener(connection, "RDKC.CVR", on_message_cvr, NULL);
        //rtConnection_AddListener(connection, "RDKC.SMARTTN.STATUS", on_message_smt_TN, connection);

        std::thread rtMessage_recv_thread (receive_rtmessage);
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
        rdkc_ret = cvr_read_config(pCloudRecorderInfo);
	if (0 == rdkc_ret)
	{
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is successful, enable=%d!\n", __LINE__, pCloudRecorderInfo->enable);
	}
	else
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, pCloudRecorderInfo->enable);
	}

	/* initialise cvr upload */
        cvr_upload_init(argc,argv);

        if (access(CVR_CLIP_PATH, F_OK))
        {
                sprintf(cmd, "mkdir %s", CVR_CLIP_PATH);
                system(cmd);
        }
}

/** @description: do cvr process
 *  @param[in]: pCloudRecorderInfo - void pointer
 *  @return: void
 */
void CVR::do_cvr(void * pCloudRecorderInfo)
{
	int ret = RDKC_FAILURE;
	int rdkc_ret = 1;
	int lessMemoryCount = 0; // to restrict the logging of low memory 
	int streamInitErrorCount = 0; // to restrict the logging of Stream Init Error Count 
	int streamNotreadyErrorCount = 0; // to restrict the logging of Stream Init Error Count 

#ifdef XFINITY_SUPPORT
        CloudRecorderConf *CloudRecorderInfo  = (CloudRecorderConf*)pCloudRecorderInfo ;
#else
        RdkCCloudRecorderConf *CloudRecorderInfo = (RdkCCloudRecorderConf*)pCloudRecorderInfo;
#endif
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
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is successful, enable=%d!\n", __LINE__, CloudRecorderInfo->enable);
			}
			else
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, CloudRecorderInfo->enable);
			}
                        reload_cvr_flag = 0;
                }

                // Need close session if disconnect flag was set
                if (init_flag && local_stream_err)
                {
			if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                        {
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Local Stream Error- Close Audio Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
				recorder->StreamClose(cvr_flag);
				cvr_flag &= ~RDKC_STRAM_FLAG_AUDIO;
                        }
			if(cvr_flag & RDKC_STRAM_FLAG_VIDEO)
                        {
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Local Stream Error- Close Video Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
				recorder->StreamClose(cvr_flag);
				cvr_flag &= ~RDKC_STRAM_FLAG_VIDEO;
                        }
                        init_flag = 0;
                        local_stream_err = 0;
                }

                //set CVR audio
                CVR::cvr_set_audio();

                // CVR not enabled
                if (CloudRecorderInfo->enable == 0)
                {
                        if (init_flag)
                        {
				if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                                {
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): CVR disabled, Close Audio Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
					recorder->StreamClose(cvr_flag);
					cvr_flag &= ~RDKC_STRAM_FLAG_AUDIO;
                                }
				if(cvr_flag & RDKC_STRAM_FLAG_VIDEO)
                                {
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): CVR disabled, Close Video Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
					recorder->StreamClose(cvr_flag);
					cvr_flag &= ~RDKC_STRAM_FLAG_VIDEO;
                                }
                        }
                        init_flag = 0;
                        sleep(1);
                        continue; //whine first continue
                }

                // Init conn for clip building
                if (0 == init_flag)
                {

			cvr_flag = RDKC_STRAM_FLAG_VIDEO;       // Video is must
#ifdef _SUPPORT_AAC_
			cvr_flag |= RDKC_STRAM_FLAG_AUDIO;      // Audio is necessary if AAC enabled
#endif
			cvr_flag |= RDKC_STRAM_FLAG_PADDING;    // Padding is must

                        // request connection, inti ts
			conf->KeyValue = stream_id + MAX_ENCODE_STREAM_NUM;
			ret = recorder->StreamInit(conf, cvr_flag&(~RDKC_STRAM_FLAG_AUDIO));
			if( RDKC_FAILURE == ret)
                        {
				streamInitErrorCount++;
				if( 0 == (streamInitErrorCount % 5) ) {
                                	RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): init local stream for read video frame error. Retry Count %d\n", __FILE__, __LINE__, streamInitErrorCount);
				}
                                sleep(1);
                                continue;
                        }
			streamInitErrorCount = 0; //Resetting the streamInitErrorCount as StreamInit is success
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Init local stream for read video frame success, cvr_flag: %d!\n", __LINE__, cvr_flag);

                        if (cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                        {
                                i = get_audio_stream_id(stream_id >> 4);
                                if (i < 0)
                                {
					recorder->StreamClose(cvr_flag);
					RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Audio Stream ID error, Close Audio Streaming Session, audio id: %d and cvr_flag: %d!\n", __LINE__, i, cvr_flag);
					cvr_flag &= ~RDKC_STRAM_FLAG_AUDIO;
                                }
                                else
                                {
                                        stream_id &= 0x0F;
                                        stream_id |= i << 4;
					conf->KeyValue = stream_id + MAX_ENCODE_STREAM_NUM;
					ret = recorder->StreamInit(conf, RDKC_STRAM_FLAG_AUDIO); //Audio
					if( RDKC_FAILURE == ret)
                                        {
					    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): init local stream for read audio frame error.\n", __FILE__, __LINE__);
                                            sleep(1);
                                                //continue; if audio failed, ignore audio
					    cvr_flag &= ~RDKC_STRAM_FLAG_AUDIO;
                                        }
					else {
					    	cvr_flag |= RDKC_STRAM_FLAG_AUDIO;
						RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): init local stream for read audio frame success, cvr_flag: %d!\n", __FILE__, __LINE__, cvr_flag);
					}
                                }
                        }
                        init_flag = 1;
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): CVR init succesfull!\n", __LINE__);

                        // wait for I frame
                        while (!term_flag)
                        {
                                ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_VIDEO);
                                if (ccode < 0)
                                {
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                        local_stream_err = 1;
                                        break;
                                }
                                else if (ccode)
                                {
                                        usleep(1000);
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
                                        has_an_iframe = 1;
                                        //gettimeofday(&start_t,NULL);
					clock_gettime(CLOCK_REALTIME, &start_t);
                                        start_msec = cvr_frame.frame_timestamp;
                                        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): start_msec=%lu\n", __FILE__, __LINE__, start_msec);
                                        memcpy(&cvr_key_frame, &cvr_frame, sizeof(RDKC_FrameInfo));
                                        break;
                                }
                        }
			streamNotreadyErrorCount = 0; // Resetting the error count as we got the Iframe
                }

		// If local stream error, continue
		if(1 == local_stream_err) {
			continue;
		}

                file_len = CloudRecorderInfo->video_duration;
                file_format = CloudRecorderInfo->video_format;
                frame_num_count = 0;
                motion_level_raw_sum = 0.0;
                event_type_raw = 0;
                memset(motion_statistics_info,0,sizeof(motion_statistics_info));
                motion_statistics_info[0] = 0;//first byte represent version.
                motion_level_idx = 1;

                // Generate the file name base on the time
                tv = gmtime(&start_t.tv_sec);
                snprintf(file_name, sizeof(file_name), "%s/%04d%02d%02d%02d%02d%02d", CVR_CLIP_PATH,(tv->tm_year+1900), tv->tm_mon+1, tv->tm_mday, tv->tm_hour, tv->tm_min, tv->tm_sec);
                // Generate the file name which used to save CVR clip
                snprintf(fpath, sizeof(fpath), "%s.ts", file_name);
                // Generate the file name which used to save motion level infomation base on the time
                snprintf(m_fpath, sizeof(m_fpath), "%s.m_level", file_name);
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): fname=[%s]\n", __FILE__, __LINE__, fpath);
                snprintf(starttime, sizeof(starttime), "%lu.%lu", start_t.tv_sec, start_t.tv_nsec);
                RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): starttime=[%s]\n", __FILE__, __LINE__, starttime);
                cvr_starttime = start_t.tv_sec;

                //create clip file to write.
                ts_fd = open(fpath, O_TRUNC | O_RDWR | O_CREAT, 0666);
                if (ts_fd < 0)
                {
                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): open %s failed\n", __FILE__, __LINE__, fpath);
                        unlink(fpath);
                        continue;
                }
		else
		{
			RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): ts_fd = %d, open %s successful\n", __FILE__, __LINE__, ts_fd, fpath);
		}

                if (has_an_iframe)
                {
                        //write 367 bytes PAT-PMT pair and the first I frame to clip file.
			if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                        {
				RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","(%d): Initialize clip creation, ts_fd=%d, cvr_flag=%d!\n", __LINE__, ts_fd, cvr_flag);
				rdkc_ret = recorder->CVRBuildInit(ts_fd, &cvr_key_frame, cvr_flag);
				if (-1 == rdkc_ret)
				{
					RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): Initialize clip creation failed\n", __FILE__, __LINE__);
				}
				else
				{
					RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","(%d): Initialize clip creation success!\n", __LINE__);
				}
                        }
                        else
                        {
				RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","(%d): Initialize clip creation, ts_fd=%d, cvr_flag=%d!\n", __LINE__, ts_fd, cvr_flag);
				rdkc_ret = recorder->CVRBuildInit(ts_fd, &cvr_key_frame, cvr_flag&(~RDKC_STRAM_FLAG_AUDIO));
				if (-1 == rdkc_ret)
				{
					RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): Initialize clip creation failed\n", __FILE__, __LINE__);
				}
				else
				{
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Initialize clip creation success!\n", __LINE__);
				}
                        }
			rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_key_frame);
			if (-1 == rdkc_ret)
			{
				RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
			}

                        has_an_iframe = 0;
                        //Get the motion statistics information
                        get_motion_statistics_info(&cvr_key_frame, &frame_num_count, &event_type_raw, &motion_level_raw_sum);
                }
                while ((long)(file_len*1000 - compare_timestamp(amba_hwtimer_msec(hwtimer_fd), start_msec)) > 20)
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
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is successful, enable=%d!\n", __LINE__, CloudRecorderInfo->enable);
				}
				else
				{
					RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Reload Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, CloudRecorderInfo->enable);
				}
                                reload_cvr_flag = 0;
                        }
                        // CVR disabled
                        if (CloudRecorderInfo->enable == 0)
                        {
                                RDK_LOG( RDK_LOG_WARN,"LOG.RDK.CVR","%s(%d): CVR is disabled.\n", __FILE__, __LINE__);
                                break;
                        }
                        file_len = CloudRecorderInfo->video_duration;
			if(cvr_flag & RDKC_STRAM_FLAG_VIDEO)
                        {
                                ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_VIDEO);
                                if (ccode < 0)
                                {
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                        local_stream_err = 1;
                                        break;
                                }
                                else if (0 == ccode)
                                {
                                        if ((IAV_PIC_TYPE_IDR_FRAME == cvr_frame.pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame.pic_type)
                                        && (long)(file_len*1000 - compare_timestamp(amba_hwtimer_msec(hwtimer_fd), start_msec) < 150))
                                        {
                                                //This I frame is almost at the end of clip file, we'll keep it, to write into next clip file.
                                                has_an_iframe = 1;
                                                //gettimeofday(&start_t,NULL);
						clock_gettime(CLOCK_REALTIME, &start_t);
                                                start_msec = cvr_frame.frame_timestamp;
                                                RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): start_msec_iframe_clipend=%lu\n", __FILE__, __LINE__, start_msec);
                                                memcpy(&cvr_key_frame, &cvr_frame, sizeof(RDKC_FrameInfo));
                                                break;
                                        }
                                        if (!has_an_iframe)
                                        {
						rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_frame);
						if (-1 == rdkc_ret)
						{
							RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
						}

                                                //Get the timestamp of first frame of every second
                                                if (compare_timestamp(cvr_frame.frame_timestamp,start_msec) <= 1000 * motion_level_idx)
                                                {
                                                        get_motion_statistics_info(&cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
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
                                                        get_motion_statistics_info(&cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                                                }
                                                else
                                                {
                                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):ERROR:buff of motion_statistics_info is full!!! \n", __FILE__, __LINE__);
                                                }
                                        }
                                }
                        }

			if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                        {
                                ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_AUDIO);
                                if (ccode < 0)
                                {
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                        local_stream_err = 1;
                                        break;
                                }
                                else if (0 == ccode)
                                {
					rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_frame);
					if (-1 == rdkc_ret)
					{
						RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
					}
                                }
                        }

                        if (ccode)      // 1 means no data to read
                        {
                                usleep(1000);
                                continue;
                        }

                        //rest memory is quite low. shouldn't write more frames.sleep and check.
                        while(sc_get_sys_free_mem() < 5000000)
                        {
				if(lessMemoryCount < 10) {
					RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Available system memory is %ld which is less than 5MB and hence suspending CVR \n", __FILE__, __LINE__, sc_get_sys_free_mem());
					lessMemoryCount++;
				}
                                sleep(2);
                        }
			lessMemoryCount = 0;
                }

                if (!has_an_iframe)     //file length reached but we haven't got an I frame. This is to look for a coming I frame.
                {
                        while (!term_flag)
                        {
				if(cvr_flag & RDKC_STRAM_FLAG_VIDEO)
                                {
					ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_VIDEO);
                                        if (ccode < 0)
                                        {
                                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                                local_stream_err = 1;
                                                break;
                                        }
                                        else if (ccode)
                                        {
                                                usleep(1000);
                                                continue;
                                        }
                                        else
                                        {
                                                if (IAV_PIC_TYPE_IDR_FRAME == cvr_frame.pic_type || IAV_PIC_TYPE_I_FRAME == cvr_frame.pic_type)
                                                {
                                                        has_an_iframe = 1;
                                                        //gettimeofday(&start_t,NULL);
							clock_gettime(CLOCK_REALTIME, &start_t);
                                                        start_msec = cvr_frame.frame_timestamp;
                                                        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): start_msec_iframe=%lu\n", __FILE__, __LINE__, start_msec);
                                                        memcpy(&cvr_key_frame, &cvr_frame, sizeof(RDKC_FrameInfo));
                                                        break;
                                                }
                                                if (!has_an_iframe)
                                                {
							rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_frame);
							if (-1 == rdkc_ret)
							{
								RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
							}

	                                             	//Get the timestamp of first frame of every second
                                                        if (compare_timestamp(cvr_frame.frame_timestamp,start_msec) <= 1000 * motion_level_idx)
                                                        {
                                                                get_motion_statistics_info(&cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
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
                                                                get_motion_statistics_info(&cvr_frame, &frame_num_count,&event_type_raw, &motion_level_raw_sum);
                                                        }
                                                        else
                                                        {
                                                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):ERROR:buff of motion_statistics_info is full!!! \n", __FILE__, __LINE__);
                                                        }
                                                }
                                        }
                                }
				if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                                {
					ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_AUDIO);
                                        if (ccode < 0)
                                        {
                                                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                                local_stream_err = 1;
                                                break;
                                        }
                                        else if (ccode)
                                        {
                                                usleep(1000);
                                                continue;
                                        }
                                        else
                                        {
						rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_frame);
						if (-1 == rdkc_ret)
						{
							RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
						}
                                        }
                                }
                        }
                }
                //Need save all audio frame which timestamp less than start_msec
		if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                {
                        while(!term_flag)
                        {
                                ccode = recorder->GetStream(&cvr_frame,RDKC_STRAM_FLAG_AUDIO);
                                if (ccode < 0)
                                {
                                        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Read frame error!\n", __FILE__, __LINE__);
                                        local_stream_err = 1;
                                        break;
                                }
                                else if (ccode)
                                {
                                        usleep(1000);
                                        continue;
                                }
                                else
                                {
					rdkc_ret = recorder->CVRBuildWriteFrame(ts_fd, &cvr_frame);
					if (-1 == rdkc_ret)
					{
						RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): ts_fd=%d, Clip Building Error\n", __FILE__, __LINE__, ts_fd);
					}

                                        if (cvr_frame.frame_timestamp + AUDIO_DURATION > start_msec)
                                        {
                                                break;
                                        }
                                }
                        }
                }


                //caculate the last second of  clip
                if (frame_num_count > 0 && motion_level_idx <= VIDEO_DURATION_MAX)
                {
                        motion_statistics_info[motion_level_idx] = calculate_motion_level(motion_level_raw_sum,frame_num_count,event_type_raw);
                }
                //write motion statistics info to file.
                m_fd = open(m_fpath, O_TRUNC | O_WRONLY | O_CREAT, 0666);
                if (-1 != m_fd)
                {
                        write(m_fd,motion_statistics_info,motion_level_idx+1);
                        close(m_fd);
                }
		else
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): open %s failed\n", __FILE__, __LINE__, m_fpath);	
		}

                if (!term_flag)
                {
                        // Call app to send out to server
                        close(ts_fd);
                        //gettimeofday(&end_t,NULL);
			clock_gettime(CLOCK_REALTIME, &end_t);
                        snprintf(endtime, sizeof(endtime), "%lu.%lu", end_t.tv_sec, end_t.tv_nsec);
                        RDK_LOG( RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): endtime=[%s]\n", __FILE__, __LINE__, endtime);
                        event_type = EVENT_TYPE_MAX;

                        /*for(int i = 0; i<motion_level_idx+1; i++) {
                            if(motion_statistics_info[i] != 0) {
                                cvr_get_event_info(&event_type,&event_datetime,cvr_starttime);
                                break;
                            }
                        }*/
			cvr_get_event_info(&event_type,&event_datetime,cvr_starttime);

                        //Uploading  data to the server
			if((od_frame_upload_enabled) &&
			   (EVENT_TYPE_MOTION == event_type) &&
			   (top >= 0)) {
				stringify_od_frame_data();
				if(smartTnEnabled) {
                                	cvr_upload( fpath, starttime, endtime, event_type, event_datetime,m_fpath,motion_level_idx+1,str_od_data,va_engine_version,true);
				} else {
                                	cvr_upload( fpath, starttime, endtime, event_type, event_datetime,m_fpath,motion_level_idx+1,str_od_data,va_engine_version,false);
				}
			}
			else
			{
				if (smartTnEnabled) {
                                	cvr_upload( fpath, starttime, endtime, event_type, event_datetime,m_fpath,motion_level_idx+1,NULL,NULL,true);
				} else {
                                	cvr_upload( fpath, starttime, endtime, event_type, event_datetime,m_fpath,motion_level_idx+1,NULL,NULL,false);
				}
			}
			RDK_LOG(RDK_LOG_DEBUG1,"LOG.RDK.CVR","%s(%d): No Motion: %d, Low Motion: %d, Medium Motion: %d, High Motion: %d\n",__FILE__, __LINE__, count_no, count_low, count_med, count_high);

			if(od_frame_upload_enabled) {
				reset_od_frame_data();
			}

                }
                else
                {
                        unlink(fpath);
                        unlink(m_fpath);
                }
        }

}

/** @description: close cvr
 *  @param[in] argv - char pointer
 *  @return: int
 */
int CVR::cvr_close(char *argv[])
{
	//rtmessageCVRThreadExit = true;
        RFCRelease();
        config_release();

#ifdef RTMSG
        rtmessageCVRThreadExit = true;
        rtConnection_Destroy(connection);
#endif
        // Close local stream session
        if (init_flag)
        {
		if(cvr_flag & RDKC_STRAM_FLAG_AUDIO)
                {
			recorder->StreamClose(cvr_flag);
			cvr_flag &= ~RDKC_STRAM_FLAG_AUDIO;
                }
		if(cvr_flag & RDKC_STRAM_FLAG_VIDEO)
                {
			recorder->StreamClose(cvr_flag);
			cvr_flag &= ~RDKC_STRAM_FLAG_VIDEO;
                }
        }

       /* close cvr upload */
        cvr_upload_close();

        // Close hwtimer
        CVR::amba_hwtimer_exit(hwtimer_fd);

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

#ifdef XFINITY_SUPPORT
        CloudRecorderConf CloudRecorderInfo;
#else
        RdkCCloudRecorderConf CloudRecorderInfo;
#endif

        rdk_logger_init("/etc/debug.ini");
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s start ...\n", __FILE__);


#ifdef XFINITY_SUPPORT
        memset(&CloudRecorderInfo, 0, sizeof(CloudRecorderConf));
#else
        memset(&CloudRecorderInfo, 0, sizeof(RdkCCloudRecorderConf));
#endif

        // Init signal handler
        (void) signal(SIGTERM, CVR::self_term);
        (void) signal(SIGUSR1, CVR::reload_config);

	CVR cvr_object;

	int ret = cvr_object.cvr_init(argc,argv,&CloudRecorderInfo);

	if(CVR_FAILURE == ret) {
		goto error_exit;
	}

	cvr_object.do_cvr(&CloudRecorderInfo);

error_exit:
	cvr_object.cvr_close(argv);
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s exit! - sigterm flag - %d \n", __FILE__, __LINE__, argv[0],CVR::term_flag);
	return 0;
}
