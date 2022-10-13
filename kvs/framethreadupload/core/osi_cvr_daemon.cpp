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
#define KVSINITMAXRETRY         5
#define EVENT_THRESHHOLD_COUNT  4
#define KVS_SMARTRC "RFC_ENABLE_XCAM2_SMARTRC"
int CVR::cvr_audio_status = CVR_AUDIO_UNKNOWN;
char fileName[256] = {0};

#ifdef RTMSG
	rtConnection CVR::connectionRecv;
	rtConnection CVR::connectionSend;
	rtError CVR::err;
	bool CVR::rtmessageCVRThreadExit;
	volatile bool CVR::smartTnEnabled = true;
#endif

int CVR::local_stream_err = 0;
unsigned long CVR::lastUploadedFrame_ts = 0;
using namespace std;

#define VIDEO_MASK 0x0F

void CVR::onUploadSuccess(char* cvrRecName)
{
    RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): kvs Upload Successful - %s\n", __FUNCTION__, __LINE__, cvrRecName);
    RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): Notifying the upload status for the file %s\n",__FUNCTION__, __LINE__, cvrRecName);
    notify_smt_TN_uploadStatus(CVR_UPLOAD_OK, cvrRecName);
}

void CVR::onUploadError(char* cvrRecName, const char* streamStatus)
{
    RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): kvs Upload Failed - %s\n", __FUNCTION__, __LINE__, cvrRecName);
    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):  kvsclip upload error %s, %s\n",__FILE__, __LINE__, cvrRecName, streamStatus);
    notify_smt_TN_uploadStatus(CVR_UPLOAD_FAIL, cvrRecName);
}

/** @description: Constructor
 *  @param[in] void
 */
CVR::CVR(): init_flag(0),
	check_polling_config_timeout( XFINITY_POLLING_CONFIG_TIMEOUT ),
	_streamFd(-1),
#ifdef RTMSG
	event_quiet_time(DEFAULT_EV_QUIET_TIME),
#endif
	clipStatus(CVR_CLIP_GEN_UNKNOWN),
	ccode(0),
	cvr_flag(0),
	hwtimer_fd(-1),
	file_len(CVR_CLIP_DURATION),
	m_streamid(DEF_CVR_CHANNEL),
	has_an_iframe(0),
	start_msec(0),
	iskvsInitDone(false),
	iskvsStreamInitDone(false),
	kvsclip_audio(0),
	m_storageMem(0),
	m_streamdump(0),
	dumpfp(NULL),
	cvrDumpEnabled(false),
	m_skipAudioFrameCount(0),
	m_kvsUploadThreadExit(false)
{
#ifdef RTMSG
	rtmessageCVRThreadExit = false;
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

/** @description:Callback function when the cvr configuration message is received
 *  @param[in] hdr : pointer to rtMessage Header
 *  @param[in] buff : buffer for data received via rt message
 *  @param[in] n : number of bytes received
 *  @return: void
 */
void CVR::on_message_cvrconf(rtMessageHeader const* hdr, uint8_t const* buff, uint32_t n, void* closure)
{
	char const*  status = NULL;
	char const*  config = NULL;

	rtConnection con = (rtConnection) closure;

	rtMessage req;
	rtMessage_FromBytes(&req, buff, n);

	char* tempbuff = NULL;
	uint32_t buff_length = 0;

	rtMessage_ToString(req, &tempbuff, &buff_length);
	rtLog_Debug("Req : %.*s", buff_length, tempbuff);
	free(tempbuff);

	rtMessage_GetString(req, "status", &status);
	rtMessage_GetString(req, "config", &config);

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVR","(%s):%d status:%s, config:%s\n", __FUNCTION__, __LINE__, status,config);

	if( (!strcmp(status, "refresh")) && (!strcmp(config, CLOUDRECORDER_CONFIG_FILE)) )
	{
		reload_config();
	}

	rtMessage_Release(req);
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
                RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to enable.\n", __FILE__, __LINE__);
        }
	else {
                cvr_audio_status = CVR_AUDIO_DISABLED;
                RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d): setting CVR audio status to disable.\n", __FILE__, __LINE__);
        }

        if( prev_cvr_audio_status != cvr_audio_status ) {
                if( CVR_AUDIO_ENABLED == cvr_audio_status ) {
                          RDK_LOG( RDK_LOG_INFO, "LOG.RDK.CVR", "%s(%d): CVR audio is enabled.\n", __FILE__, __LINE__);
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

int CVR::getCVRStreamId()
{
  return m_streamid;
}

void CVR::setCVRStreamId(int streamid)
{
  m_streamid = streamid;
}

/** @description: initialize cvr
 *  @param[in] isAudio - audio status
 *  @param[in] pCloudRecorderInfo - CloudRecorderConf pointer
 *  @param[in] storageMemory - kvs storage in bytes
 *  @return: CVR_Failure is failed , CVR_SUCCESS if success
 */
int CVR::cvr_init(unsigned short isAudio,cvr_provision_info_t *pCloudRecorderInfo,const int& streamdump,uint64_t storageMemory=0)
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
        m_streamdump = streamdump;
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsclip_audio : %d m_storageMem : %llu, m_streamdump : %d\n", __FILE__, __LINE__, kvsclip_audio,m_storageMem,m_streamdump);
        
        // Init cvr flag
        cvr_flag = XSTREAM_VIDEO_FLAG ;       // Video is must
#ifdef _SUPPORT_AAC_
        cvr_flag |= XSTREAM_AUDIO_FLAG;      // Audio is necessary if AAC enabled
        m_streamid |= 0x10;                      //Default audio channel is 1.
#endif
        cvr_flag |= RDKC_STREAM_FLAG_PADDING;    // Padding is must
        
        cvr_flag |= RDKC_STREAM_FLAG_ABSTIMESTAMP;    //disable abs timestamp - 1 means disable absolute timestamp - 0  means reset timestamp

        if( RDKC_SUCCESS != config_init())
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Config Manager init fails\n",__FILE__,__LINE__);
        }

        if( RDKC_SUCCESS != RFCConfigInit() )
        {
                RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): RFC config Init fails\n", __FILE__, __LINE__);
                return CVR_FAILURE;
        }

#ifdef RTMSG
        rtLog_SetLevel(RT_LOG_INFO);
        rtLog_SetOption(rdkLog);

        rtConnection_Create(&connectionSend, "CVR_SEND", "tcp://127.0.0.1:10001");
        rtConnection_Create(&connectionRecv, "CVR_RECV", "tcp://127.0.0.1:10001");
        rtConnection_AddListener(connectionRecv, "RDKC.SMARTTN.STATUS", on_message_smt_TN, NULL);
        rtConnection_AddListener(connectionRecv, "RDKC.CONF.REFRESH", on_message_cvrconf, NULL);
        rtConnection_AddListener(connectionRecv, "RDKC.ENABLE_DYNAMIC_LOG", on_message_dyn_log, connectionRecv);

        std::thread rtMessage_recv_thread (receive_rtmessage);
        pthread_setname_np(rtMessage_recv_thread.native_handle(),"rt_recmessage");
        rtMessage_recv_thread.detach();
#endif

        // Check camera has polling cvr config from server?
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Wait for xfinity polling config done.Timeout is %d seconds.\n", __FILE__, __LINE__, check_polling_config_timeout);
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
                iscvrenabled = atoi(pCloudRecorderInfo->enable);
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is successful, enable=%d!\n", __LINE__, iscvrenabled);
	}
	else
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","(%d): Read Cloud Recorder Info is unsuccessful, enable=%d!\n", __LINE__, iscvrenabled);
	}
	createkvsstream((m_streamid & 0x0F),0);

	/* Create upload thread */
	std::thread pushFrameTh(&CVR::pushFramesToKVS, this);
	pthread_setname_np(pushFrameTh.native_handle(), "pushFrames_to_KVS");
	pushFrameTh.detach();

	// CVR optimization Debug - dump files
	if (!access("/tmp/.cvrdump", 0) )
	{
		cvrDumpEnabled = true;
	}
	else
	{
		cvrDumpEnabled = false;
	}
}

/**
 * @description: This function is used to notify smart thumbnail about cvr upload status
 * @param[in]: status, uploadedclip_filename
 * @return: None
 */
void CVR::notify_smt_TN_uploadStatus(cvr_upload_status status, char* upload_fname)
{
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): upload file base name:%s\n",__FILE__, __LINE__, upload_fname);

	rtMessage msg;
	rtMessage_Create(&msg);
	rtMessage_SetInt32(msg, "status", status);
	rtMessage_SetString(msg, "uploadFileName", upload_fname);
	RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): Sending CVR upload status to smart thumbnail, status code:%d\n",__FILE__, __LINE__, status);
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

	if(!clip_name) {
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Empty clip name!!\n", __FILE__, __LINE__);
		return;
	}

	rtMessage msg;
	rtMessage_Create(&msg);
	rtMessage_SetInt32(msg, "clipStatus", status);
	rtMessage_SetString(msg, "clipname", clip_name);

	rtError err = rtConnection_SendMessage(connectionSend, msg, "RDKC.CVR.CLIP.STATUS");
	rtLog_Debug("SendRequest:%s", rtStrError(err));

	if (err != RT_OK)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) Error sending msg via rtmessage\n", __FILE__,__LINE__);
	}
	rtMessage_Release(msg);
}

/** @description: creates kvs stream
 *  @param[in]: stream_id and recreateflag
 *  @return: bool - SUCCESS or FAILURE
 */
bool CVR::createkvsstream(int stream_id, unsigned short recreateflag)
{
    int ret_kvs = RDKC_SUCCESS;
    
    //kvs init
    if(!iskvsInitDone)
    {
        do
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Invoking kvsInit\n", __FILE__, __LINE__);
            ret_kvs = kvsInit(this, stream_id, m_storageMem);

            static int retry = 0;
            if (0 != ret_kvs)
            {
                retry++;
                if ( retry > KVSINITMAXRETRY )
                {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d) : FATAL : Max retry reached in kvsInit exit process %d\n", __FILE__, __LINE__);
                    cvr_close();
                    exit(0);
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
        ret_kvs = kvsStreamInit(kvsclip_audio, recreateflag);
        if ( true == ret_kvs )
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): kvsStreamInit success \n", __FILE__, __LINE__);
            iskvsStreamInitDone = true;
        }
        else
        {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): FATAL : Max retry reached in kvsStreamInit exit process \n", __FILE__, __LINE__);
            cvr_close();
            exit(0);
        }
    }

    return true;
}


/** @description: push message into queue
 *  @param[in] kvsUploadData_t struct
 *  @return: void
 */
void CVR::push_KvsFrameInfo(kvsUploadData_t kvsFrameInfo) {

    //Start - code to check hang in upload thread
    static unsigned long prevUploadedFrame_ts = 0;
    unsigned long tempFrame_ts = 0;
    static uint32_t counterForSample(0);
    static uint32_t counterHang(0);
    uint32_t noOfSamples = 4;
    uint32_t sampleSet = KVSUPLOAD_Q_SIZE_LIMIT/noOfSamples;

    // compare ts approx after every 280 frames
    if ( counterForSample++ % sampleSet == 0 && 0 != lastUploadedFrame_ts ) {

        // In case of End of segment, we ignore the current element
        // and to ensure following element is considered for ts comparision, we decrement the counter
        if ( kvsDataQ.front().isEndofSegment == true ) {
            --counterForSample;
        }
        else {
            // TODO: Ideally acquire lock, store lastUploadedFrame_ts to local variable and release the lock.
            tempFrame_ts = lastUploadedFrame_ts;
            if ( prevUploadedFrame_ts >= tempFrame_ts ) {
                ++counterHang;
                if ( counterHang >= noOfSamples) {
                    RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Hang detected in upload thread\n", __FUNCTION__, __LINE__);
                    fflush(stdout);
                    kill(getpid(), SIGTERM);
                }
            }
            else {
                prevUploadedFrame_ts = tempFrame_ts;
                counterHang = 0;
                counterForSample = 0;
            }
        }
    }
    //End - code to check hang in upload thread

    std::unique_lock<std::mutex> lock(kvsFrameCVRMutex);

    // Limiting the queue size, by discarding the oldest element in the queue
    if( KVSUPLOAD_Q_SIZE_LIMIT <= kvsDataQ.size()) {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Maximum limit for the KVS data queue reached. Queue size is %d. Popping out one element from the queue!!!\n", __FILE__, __LINE__, kvsDataQ.size() );
        kvsDataQ.pop();
    }

    kvsDataQ.push(kvsFrameInfo);
    lock.unlock();
    msgCv_kvs.notify_one();
}

/** @description: pop message from queue
 *  @param[in] *kvsFrameInfo - kvsUploadData_t pointer
 *  @return: void
 */
void CVR::pop_KvsFrameInfo(kvsUploadData_t *kvsFrameInfo) {

    std::unique_lock<std::mutex> lock(kvsFrameCVRMutex);
    msgCv_kvs.wait(lock,[this]{return (m_kvsUploadThreadExit || !kvsDataQ.empty());});

    if(m_kvsUploadThreadExit) {
        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","Notification from cvr_close\n", __FILE__, __LINE__);
    }
    else if(!kvsDataQ.empty()) {
        *kvsFrameInfo = kvsDataQ.front();
        kvsDataQ.pop();
    }

    lock.unlock();
}

void CVR::pushFramesToKVS() {

    char pcFilename[CVR_FILE_PATH_LEN] = "/tmp/cvr_video_upload.H264";
    char pcAudioFilename[CVR_FILE_PATH_LEN] = "/tmp/cvr_audio_upload.aac";
    FILE *fpFramesParse = NULL, *fpAudioFramesParse = NULL;
    unlink(pcFilename);
    unlink(pcAudioFilename);

    if(cvrDumpEnabled) {
        fpFramesParse = fopen( pcFilename, "wo+" );
        if ( NULL == fpFramesParse ) {
            RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d) :open video file open error\n", __FILE__, __LINE__ );
        }

        fpAudioFramesParse = fopen( pcAudioFilename, "wo+" );
        if ( NULL == fpAudioFramesParse ) {
            RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d) :open audio file open error\n", __FILE__, __LINE__ );
        }
    }

    unsigned short idrFrameCount = 0;
    unsigned short pFrameCount = 0;
    unsigned short bFrameCount = 0;
    unsigned short audioFrameCount = 0;
    unsigned short totalFramesinClip = 0;
    m_skipAudioFrameCount = 0;
    char fileName[CVR_FILE_PATH_LEN];
    kvsUploadData_t topKvsUploadData;

    frameInfoH264 frameInfoTmp;
    pthread_setname_np(pthread_self(), __FUNCTION__);

    while (!term_flag && !m_kvsUploadThreadExit) {

        std::lock_guard<std::mutex> lockGuard(kvsFramePushMutex);

        topKvsUploadData = {};
        pop_KvsFrameInfo(&topKvsUploadData);

        strcpy(fileName, topKvsUploadData.fileName.c_str());

        memset(&frameInfoTmp, 0, sizeof(frameInfoH264));

        frameInfoTmp.stream_id = topKvsUploadData.stream_id;
        frameInfoTmp.stream_type = topKvsUploadData.stream_type;
        frameInfoTmp.pic_type = topKvsUploadData.pic_type;
        frameInfoTmp.frame_num = topKvsUploadData.frame_num;
        frameInfoTmp.frame_size = topKvsUploadData.frame_size;
        frameInfoTmp.jpeg_quality = topKvsUploadData.jpeg_quality;
        frameInfoTmp.width = topKvsUploadData.width;
        frameInfoTmp.height = topKvsUploadData.height;
        frameInfoTmp.frame_timestamp = topKvsUploadData.frame_timestamp;
        frameInfoTmp.arm_pts = topKvsUploadData.arm_pts;
        frameInfoTmp.dsp_pts = topKvsUploadData.dsp_pts;
        frameInfoTmp.frame_ptr = (unsigned char*)topKvsUploadData.frame_ptr.c_str();

       if (m_streamdump) {
           if( XSTREAM_STREAM_TYPE_AAC != frameInfoTmp.stream_type ) {
	       int data_bytes = 0;
               if( NULL != dumpfp ) {
                   data_bytes = fwrite( ( char * ) frameInfoTmp.frame_ptr, 1, frameInfoTmp.frame_size, dumpfp );
                   if ( data_bytes <= 0 ) {
                       RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR","%s(%d) errmsg(%d)=%s!\n", __FILE__, __LINE__, errno, strerror( errno ) );
                       return;
                   } else {
                       RDK_LOG( RDK_LOG_TRACE1, "LOG.RDK.CVR","%s(%d) Written bytes %d...\n", __FILE__, __LINE__, data_bytes );
                   }
               }
           }
        }

        int ret_kvs = kvsUploadFrames(frameInfoTmp, fileName, topKvsUploadData.isEndofSegment);

        RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR","Pushing the Frames to KVS \n");
        if(topKvsUploadData.isEndofSegment) {
            RDK_LOG( RDK_LOG_DEBUG, "LOG.RDK.CVR","EOF Frame pushed. Marking the End of a clip %s\n", fileName);

            audioFrameCount = audioFrameCount - m_skipAudioFrameCount;

            totalFramesinClip = idrFrameCount + pFrameCount + bFrameCount + audioFrameCount;
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s clip frame stats :  idrFrameCount : %d, pFrameCount : %d, bFrameCount : %d, audioFrameCount : %d, totalFramesinClip : %d\n"
                , __FILE__, __LINE__, fileName, idrFrameCount, pFrameCount, bFrameCount, audioFrameCount, totalFramesinClip);
            idrFrameCount = 0;
            pFrameCount = 0;
            bFrameCount = 0;
            audioFrameCount = 0;
            totalFramesinClip = 0;
            m_skipAudioFrameCount = 0;

        }

        if (-1 == ret_kvs) {
            RDK_LOG(RDK_LOG_DEBUG, "LOG.RDK.CVR", "%s(%d):  kvsUploadFrames : upload failed %s\n", __FILE__,__LINE__, fileName);
        }
        else if (0 == ret_kvs) {
            RDK_LOG(RDK_LOG_TRACE1, "LOG.RDK.CVR", "%s(%d): kvsUploadFrames : upload success : %s\n",__FILE__, __LINE__, fileName);

            if(false == topKvsUploadData.isEndofSegment) {
                if ((IAV_PIC_TYPE_IDR_FRAME == frameInfoTmp.pic_type || IAV_PIC_TYPE_I_FRAME == frameInfoTmp.pic_type) ) {
                    idrFrameCount++;
                } else if (IAV_PIC_TYPE_P_FRAME == frameInfoTmp.pic_type) {
                    pFrameCount++;
                } else if(IAV_PIC_TYPE_B_FRAME == frameInfoTmp.pic_type) {
                    bFrameCount++;
                }
                else if (XSTREAM_STREAM_TYPE_AAC == frameInfoTmp.stream_type) {
                    audioFrameCount++;
                }
            }

            // Capturing ts for monitoring hang
            lastUploadedFrame_ts = frameInfoTmp.frame_timestamp;
        }
        else if (1 == ret_kvs) {
            iskvsStreamInitDone = false;
            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","(%d): stream error in pushFrames\n", __LINE__);
            if (topKvsUploadData.isEndofSegment) {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : isstreamerror reported as TRUE : %d \n", __FILE__, __LINE__, ret_kvs);
                createkvsstream(( m_streamid & 0x0F ),1);
            }
        }
        else {
            RDK_LOG(RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d): Unknown return status for clip %s\n", __FILE__,__LINE__, fileName);
        }

        //Dump Video and audio frames
        if(cvrDumpEnabled) {
            if(false == topKvsUploadData.isEndofSegment && fpFramesParse != NULL) {
                if( fpAudioFramesParse != NULL && XSTREAM_STREAM_TYPE_AAC == frameInfoTmp.stream_type ) {
                    if ( fwrite((char *)frameInfoTmp.frame_ptr, frameInfoTmp.frame_size, 1, fpAudioFramesParse ) <= 0 ) {
                        RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR","%s(%d) :CVR:: errmsg(%d)=%s!\n", __FILE__, __LINE__, errno, strerror( errno ) );
                    }
                }
                else {
                    if ( fwrite((char *)frameInfoTmp.frame_ptr, frameInfoTmp.frame_size, 1, fpFramesParse ) <= 0 ) {
                        RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR","%s(%d) :CVR:: errmsg(%d)=%s!\n", __FILE__, __LINE__, errno, strerror( errno ) );
                    }
                }
            }
        }
    } // end of while block - !term_flag

    if ( NULL != fpFramesParse ) {
        fclose( fpFramesParse );
        fpFramesParse = NULL;
    }

    if ( NULL != fpAudioFramesParse ) {
        fclose( fpAudioFramesParse );
        fpAudioFramesParse = NULL;
    }
    return;
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
    int ret_videoStreamConfig = -1;
    cvr_provision_info_t *CloudRecorderInfo  = (cvr_provision_info_t*)pCloudRecorderInfo ;
    int iscvrenabled = atoi(CloudRecorderInfo->enable);
    file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
    unsigned short lenableaudio = kvsclip_audio;
    kvsUploadData_t currentFrameInfo;

    pthread_setname_np(pthread_self(), __FUNCTION__);
    while (!term_flag)
    {
        if (reload_cvr_flag)
        {
            rdkc_ret = cvr_read_config(CloudRecorderInfo);
            if (0 == rdkc_ret)
            {
                iscvrenabled = atoi(CloudRecorderInfo->enable);
                file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
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
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","(%d): CVR disabled, Close Video/Audio Streaming Session, cvr_flag: %d!\n", __LINE__, cvr_flag);
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
            lastUploadedFrame_ts = 0; // Resetting - as cvr is disabled
            sleep(1);
            continue; //whine first continue
        }
        
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.CVR","%s(%d): init_flag : %d\n", __FILE__, __LINE__,init_flag);

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

                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): start_msec=%lu\n", __FILE__, __LINE__, start_msec);
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

        if ( lenableaudio != kvsclip_audio) {
            RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Recreating stream : prior audio enable flag : %d , current audio enable flag : %d\n", __FILE__, __LINE__, lenableaudio, kvsclip_audio);
            lenableaudio = kvsclip_audio;
            iskvsStreamInitDone = false;
            createkvsstream(( m_streamid & 0x0F ),1);
        }

        clipStatus = CVR_CLIP_GEN_START;

        // Generate the file name base on the time
        memset(file_name, '\0', sizeof(file_name));
        tv = gmtime(&start_t.tv_sec);
        snprintf(file_name, sizeof(file_name), "%04d%02d%02d%02d%02d%02d", (tv->tm_year+1900), tv->tm_mon+1, tv->tm_mday, tv->tm_hour, tv->tm_min, tv->tm_sec);

        currentFrameInfo = {};
        if(m_streamdump) {
            memset(fpath, '\0', sizeof(fpath));
            snprintf(fpath, sizeof(fpath), "%s/%s.es", CVR_CLIP_PATH, file_name);
            dumpfp = fopen(fpath, "wo+" );
            if ( NULL == dumpfp ) {
                RDK_LOG( RDK_LOG_ERROR, "LOG.RDK.CVR", "%s(%d) :open video file open error\n", __FILE__, __LINE__ );
             }
        }

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

            currentFrameInfo = {}; currentFrameInfo.frame_ptr = "";
            string str(file_name);
            string currentFrame_ptr( reinterpret_cast< char * >(cvr_key_frame->frame_ptr), cvr_key_frame->frame_size );

            currentFrameInfo.stream_id = cvr_key_frame->stream_id;
            currentFrameInfo.stream_type = cvr_key_frame->stream_type;
            currentFrameInfo.pic_type = cvr_key_frame->pic_type;
            currentFrameInfo.frame_ptr = currentFrame_ptr;
            currentFrameInfo.frame_num = cvr_key_frame->frame_num;
            currentFrameInfo.frame_size = cvr_key_frame->frame_size;
            currentFrameInfo.jpeg_quality = cvr_key_frame->jpeg_quality;
            currentFrameInfo.width = cvr_key_frame->width;
            currentFrameInfo.height = cvr_key_frame->height;
            currentFrameInfo.frame_timestamp = cvr_key_frame->frame_timestamp;
            currentFrameInfo.arm_pts = cvr_key_frame->arm_pts;
            currentFrameInfo.dsp_pts = cvr_key_frame->dsp_pts;
            currentFrameInfo.isEndofSegment = false;
            currentFrameInfo.fileName = str;
            push_KvsFrameInfo(currentFrameInfo);

            has_an_iframe = 0;
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
                    file_len = atoi(CloudRecorderInfo->cvr_segment_info.duration);
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
                term_flag = 1;
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
                    clock_gettime(CLOCK_REALTIME, &start_t);
                    start_msec = cvr_frame->frame_timestamp;
                    memcpy(cvr_key_frame, cvr_frame, sizeof(frameInfoH264));
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): start_msec_iframe_clipend=%lu\n", __FILE__, __LINE__, start_msec);
                    break;
                }
                if (!has_an_iframe)
                {
                    currentFrameInfo = {}; currentFrameInfo.frame_ptr = "";
                    string str(file_name);
                    string currentFrame_ptr( reinterpret_cast< char * >(cvr_frame->frame_ptr), cvr_frame->frame_size );

                    currentFrameInfo.stream_id = cvr_frame->stream_id;
                    currentFrameInfo.stream_type = cvr_frame->stream_type;
                    currentFrameInfo.pic_type = cvr_frame->pic_type;
                    currentFrameInfo.frame_ptr = currentFrame_ptr;
                    currentFrameInfo.frame_num = cvr_frame->frame_num;
                    currentFrameInfo.frame_size = cvr_frame->frame_size;
                    currentFrameInfo.jpeg_quality = cvr_frame->jpeg_quality;
                    currentFrameInfo.width = cvr_frame->width;
                    currentFrameInfo.height = cvr_frame->height;
                    currentFrameInfo.frame_timestamp = cvr_frame->frame_timestamp;
                    currentFrameInfo.arm_pts = cvr_frame->arm_pts;
                    currentFrameInfo.dsp_pts = cvr_frame->dsp_pts;
                    currentFrameInfo.isEndofSegment = false;
                    currentFrameInfo.fileName = str;
                    push_KvsFrameInfo(currentFrameInfo);
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
                        clock_gettime(CLOCK_REALTIME, &start_t);
                        start_msec = cvr_frame->frame_timestamp;
                        memcpy(cvr_key_frame, cvr_frame, sizeof(frameInfoH264));
                        RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): start_msec_iframe=%lu\n", __FILE__, __LINE__, start_msec);
                        break;
                    }
                    if (!has_an_iframe)
                    {
                        currentFrameInfo = {}; currentFrameInfo.frame_ptr = "";
                        string str(file_name);
                        string currentFrame_ptr( reinterpret_cast< char * >(cvr_frame->frame_ptr), cvr_frame->frame_size );

                        currentFrameInfo.stream_id = cvr_frame->stream_id;
                        currentFrameInfo.stream_type = cvr_frame->stream_type;
                        currentFrameInfo.pic_type = cvr_frame->pic_type;
                        currentFrameInfo.frame_ptr = currentFrame_ptr;
                        currentFrameInfo.frame_num = cvr_frame->frame_num;
                        currentFrameInfo.frame_size = cvr_frame->frame_size;
                        currentFrameInfo.jpeg_quality = cvr_frame->jpeg_quality;
                        currentFrameInfo.width = cvr_frame->width;
                        currentFrameInfo.height = cvr_frame->height;
                        currentFrameInfo.frame_timestamp = cvr_frame->frame_timestamp;
                        currentFrameInfo.arm_pts = cvr_frame->arm_pts;
                        currentFrameInfo.dsp_pts = cvr_frame->dsp_pts;
                        currentFrameInfo.isEndofSegment = false;
                        currentFrameInfo.fileName = str;
                        push_KvsFrameInfo(currentFrameInfo);

                        //save audio frame with timestamp less than start_msec, else continue
                        if ((XSTREAM_STREAM_TYPE_AAC == cvr_frame->stream_type) && (cvr_frame->frame_timestamp + AUDIO_DURATION > start_msec)) {
                            // audioFrameCount gets calculated in the upload thread.
                            // Keep a count of audio frame with timestamp > start_msec,
                            // this value will be subtracted from the audioFrameCount
                            ++m_skipAudioFrameCount;
                            continue;
                        }
                    }
                }
            }
        }

        if (!term_flag)
        {
            if(RDKC_FAILURE != ret_videoStreamConfig) {
                RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): CVR configuration: StreamID Resolution(W*H) FrameRate BitRate %d %d*%d %d %d \n", __FILE__, __LINE__, (m_streamid & VIDEO_MASK), _videoConfig.width, _videoConfig.height, _videoConfig.frame_rate, _videoConfig.bit_rate);
            }

            strcpy(fileName, file_name);

            if(m_streamdump) {
                if ( NULL != dumpfp ) {
                    fclose( dumpfp );
                    dumpfp = NULL;
                }
            }

            //if local stream error is met while creating clip discard the clip sine it's an incomplete ts clip
            if( 0 == local_stream_err ) {
                if(smartTnEnabled)
                {
                    notify_smt_TN_clipStatus(CVR_CLIP_GEN_END, file_name);
                }
                else
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Smart thumbnail disabled. Pushing frames\n", __FILE__, __LINE__);
                }

                currentFrameInfo = {}; currentFrameInfo.frame_ptr = "";
                string str(file_name);

                currentFrameInfo.stream_id = cvr_frame->stream_id;
                currentFrameInfo.pic_type = cvr_frame->pic_type;
                currentFrameInfo.stream_type = cvr_frame->stream_type;
                currentFrameInfo.frame_num = cvr_frame->frame_num;
                currentFrameInfo.frame_size = cvr_frame->frame_size;
                currentFrameInfo.frame_timestamp = cvr_frame->frame_timestamp;
                currentFrameInfo.isEndofSegment = true;
                currentFrameInfo.fileName = str;
                push_KvsFrameInfo(currentFrameInfo);

                if(kvsclip_audio)
                {
                    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s kvs clip has audio\n", __FILE__, __LINE__, file_name);
                }
                clipStatus = CVR_CLIP_GEN_END;
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

        m_kvsUploadThreadExit = true;
        msgCv_kvs.notify_all();

        return 0;

}

volatile sig_atomic_t CVR::term_flag = 0;
void CVR::self_term(int sig)
{
        CVR::term_flag = 1;
//	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Received signal SIGTER!!\n", __FILE__, __LINE__);
}

volatile sig_atomic_t CVR::reload_cvr_flag = 0;
void CVR::reload_config()
{
        CVR::reload_cvr_flag = 1;
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
    int streamdump=0;
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

        if(strcmp(argv[itr],"--dump")==0)
        {
            itr++;
            if (itr < argc)
            {
               streamdump  = (int) atoi(argv[itr]);
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
    pthread_setname_np(pthread_self(), __FUNCTION__);

    CVR cvr_object;
    cvr_object.setCVRStreamId(streamId);

    int ret = cvr_object.cvr_init(kvsclip_audio,&CloudRecorderInfo,streamdump,storageMemory);
    if(CVR_FAILURE == ret) {
	goto error_exit;
    }

    cvr_object.do_cvr(&CloudRecorderInfo);

error_exit:
    cvr_object.cvr_close();
    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s exit! - sigterm flag - %d \n", __FILE__, __LINE__, argv[0],CVR::term_flag);
    return 0;
}
