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
#include "cvr_gst.h"

/*GST based CVR function declaration start*/


static gboolean on_new_sample( GstElement * elt )
{

	videoreq_success = 1;
	static int flag_1 = 0;
	GstSample *sample;
	GstBuffer *buffer;
	GstMapInfo map;
	sample = gst_app_sink_pull_sample( GST_APP_SINK( elt ) );
	buffer = gst_sample_get_buffer( sample );
	gst_buffer_map( buffer, &map, GST_MAP_READ );
	if(flag_1 == 0)
	{
		got_ptsdif++;
		if(got_ptsdif == 2)
		{
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Got both audio video PTS\n", __FILE__, __LINE__);
			pthread_mutex_lock(&mutexfor_gotptsdif);
			pthread_cond_broadcast(&cvfor_gotptsdif);
			pthread_mutex_unlock(&mutexfor_gotptsdif);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): gotptsdif mutex released\n", __FILE__, __LINE__);
		}

		memcpy(&v_firstpts,map.data,ABSOLUTETIME_LENGTH);
		prev_vabsolutetime = v_firstpts;
		flag_1 = 1;
	}

	sem_wait(&s4);
	vtot_len = map.size + vtot_len;
	remainingbuf =( uint8_t * ) realloc(remainingbuf,sizeof( uint8_t ) * (vtot_len ));
	if( NULL == remainingbuf )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): realloc failed for remainingbuf\n", __FILE__, __LINE__);
	}
	memcpy(&remainingbuf[vtot_len - map.size] , map.data,map.size);
	sem_post(&s4);
	gst_buffer_unmap( buffer, &map );
	gst_sample_unref( sample );

	if( waitingforsig1 == 1)
	{
		pthread_mutex_lock(&mutexfor_waitingvideobuf);
		waitingforsig1=0;
		pthread_cond_broadcast(&cvfor_waitingvideobuf);
		pthread_mutex_unlock(&mutexfor_waitingvideobuf);
	}
	sleep(0);
	return FALSE;
}

static gboolean on_message( GstBus * bus, GstMessage * message, gpointer user_data )
{
	GError *err = NULL;
	gchar *dbg_info = NULL;

	switch ( GST_MESSAGE_TYPE( message ) )
	{
		case GST_MESSAGE_EOS:
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): EOS of startstream request reached\n",__FILE__, __LINE__);
			sem_post(&s4);
			g_main_loop_quit( ( GMainLoop * ) user_data );
			break;
		case GST_MESSAGE_ERROR:
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stream request ERROR has occured,error_code: %d\n",
					__FILE__, __LINE__,GST_MESSAGE_ERROR);
			gst_message_parse_error (message, &err, &dbg_info);
			//g_printerr ("ERROR from element %s: %s\n",
			//GST_OBJECT_NAME (message->src), err->message);
			//g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
			g_error_free (err);
			g_free (dbg_info);
			videoreq_retry=1;
			g_main_loop_quit( ( GMainLoop * ) user_data );
			break;
		default:
			break;
	}
	return TRUE;
}


static gboolean on_message_for_stoprequest( GstBus * bus, GstMessage * message, gpointer user_data )
{
	GError *err = NULL;
	gchar *dbg_info = NULL;
	switch ( GST_MESSAGE_TYPE( message ) )
	{
		case GST_MESSAGE_EOS:
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stop request EOS reached\n",__FILE__, __LINE__);
			// sem_post(&s4);
			g_main_loop_quit( ( GMainLoop * ) user_data );
			break;
		case GST_MESSAGE_ERROR:
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stop request ERROR has occured,error_code: %d\n",
					__FILE__, __LINE__,GST_MESSAGE_ERROR);
			g_main_loop_quit( ( GMainLoop * ) user_data );
			break;
		default:
			break;
	}
	return TRUE;
}


/*GST based CVR funcion declaration end*/

int do_cvr(void * pCloudRecorderInfo, bool isGSTEnabled, int argc, char* argv[])
{

#ifdef XFINITY_SUPPORT
	CloudRecorderConf *CloudRecorderInfo  = (CloudRecorderConf*)pCloudRecorderInfo ;
#else
	RdkCCloudRecorderConf *CloudRecorderInfo = (RdkCCloudRecorderConf*)pCloudRecorderInfo;
#endif

	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Gstreamer based CVR enabled...\n", __FILE__, __LINE__);
	read_config(  ); /* Reading cvr configuration data */
	//config.CHUNK_LENGTH = CloudRecorderInfo.video_duration;/*assigning cloud recorder info*/
	config.CHUNK_LENGTH = CloudRecorderInfo->video_duration;/*assigning cloud recorder info*/
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Video duration: %d\n", __FILE__, __LINE__,config.CHUNK_LENGTH);

	gst_init( &argc, &argv ); /*Initializing Gsstreamer*/

	// CVR not enabled
	//while (!cvr_object.term_flag)
	while (!CVR::term_flag)
	{
		//if (CloudRecorderInfo.enable == 0)
		if (CloudRecorderInfo->enable == 0)
		{
			sleep(1);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): cvr not enabled...\n", __FILE__, __LINE__);
			continue;
		}



		int ret;
		size_t stacksize = PTHREAD_STACK_MIN;  
		ret = pthread_attr_init(&attr);
		if(ret != 0 )
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize attributes,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}

		ret  = pthread_attr_setstacksize(&attr, stacksize);
		if(ret != 0 )
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to set stack size,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}

		sem_init(&s1, 0, 1);
		sem_init(&s2, 0, 1);
		sem_init(&s3, 0, 0);
		sem_init(&s4, 0, 1);
		sem_init(&s5, 0, 1);
		ret = pthread_cond_init(&cv_hasaudio, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cv_hasaudio,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}
		ret = pthread_cond_init(&cvfor_threadsync, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cvfor_threadsync,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}
		ret = pthread_cond_init(&cvfor_threadsync_1, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cvfor_threadsync_1,error_code: %d\n",							    __FILE__, __LINE__,ret);         
		}
		ret = pthread_cond_init(&cvfor_waitingaudbuf, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cvfor_waitingaudbuf,error_code: %d\n",							 __FILE__, __LINE__,ret);
		}
		ret = pthread_cond_init(&cvfor_waitingvideobuf, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cvfor_waitingvideobuf,error_code: %d\n",							 __FILE__, __LINE__,ret);
		}
		ret = pthread_cond_init(&cvfor_gotptsdif, NULL);
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to initialize cvfor_gotptsdif,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}
		ret = pthread_create( &getgstvideo, &attr, start_getgstvideo, NULL );
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create getgstvideo thread,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}

		//	for ( int jj =0;jj<150;jj++)
		//	usleep(1000);
		ret = pthread_create (&getgstaudio,&attr,start_getgstaudio, NULL );
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create getgstaudio thread,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}
		config.has_audio = IsGSTEnabledInRFC(RFC_CVR,CVR_AUDIO_GST);
		if( config.has_audio == 1 )
		{	
			for ( int jj =0;jj<150;jj++)
				usleep(1000);

			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio enabled through RFC...\n",__FILE__, __LINE__);
			pthread_mutex_lock(&mutex_hasaudio);
			pthread_cond_broadcast(&cv_hasaudio);
			pthread_mutex_unlock(&mutex_hasaudio);

			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Waiting to get PTS difference...\n",__FILE__, __LINE__);
			pthread_mutex_lock(&mutexfor_gotptsdif);
			pthread_cond_wait(&cvfor_gotptsdif, &mutexfor_gotptsdif);
			pthread_mutex_unlock(&mutexfor_gotptsdif);
			a_firstpts = a_firstpts/1000000;// CONVERTING TO MSESCONDS
			v_firstpts = v_firstpts/1000000;
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): First audio PTS; %llu First video PTS: %llu\n",
					__FILE__, __LINE__,a_firstpts,v_firstpts);

			if(a_firstpts > v_firstpts)
			{
				aacaud_pts_value =(uint64_t) ( (a_firstpts - v_firstpts) * 90 );
				pts_value =0;
				video_initial_pts = pts_value;
				aac_initial_pts = aacaud_pts_value;
			}
			else
			{
				pts_value =(uint64_t)( (v_firstpts - a_firstpts) * 90 );
				aacaud_pts_value = 0;
				video_initial_pts = pts_value;
				aac_initial_pts = aacaud_pts_value;
			}
		}

		ret = pthread_create( &convert_video, &attr,start_convertvideo, NULL );
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create convert_video thread,error_code: %d\n",
					__FILE__, __LINE__,ret);
		}
		ret = pthread_create( &convert_aacauduio, &attr,start_convertaacaudio, NULL );
		if(NULL != ret)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create convert_aacaudio thread,error_code: %d\n"							   ,__FILE__, __LINE__,ret);
		}
		pthread_join( getgstvideo, NULL );
		pthread_join( getgstaudio, NULL );
		pthread_join( convert_video, NULL );
		pthread_join( convert_aacauduio, NULL );
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): CVR recording completed...\n",
				__FILE__, __LINE__);
		cvr_upload_close();
	}
}

/* function definitions for GST based cvr */

/* The function will collect data from gst buffer and pass to ts_converter once it receive one complete buffer*/
static int video_map( unsigned int buffer_full_length, char *pesbuffer_full )
{
	char *temp_buf       = NULL;
	count_1		     = 0;
	buffer_full_position = 0;
	while(1)
	{      
		if(buffer_full_length==0) 
		{
			return(0);
		}
		if(buffer_full_length < 4 )/* if there is no complete  frame in the buffer, the function will return the complete buffer size.						     so the next buffer will append at the end of the existing buffer*/
		{
			buffer_full_length=buffer_full_length+count_1;
			return(buffer_full_length);
		}
		if(buffer_full_length>=4)
		{ 
			if(buffer_full_position > 0) //to avoid initial sync bytes
			{
				if ( pesbuffer_full[buffer_full_position] == 0 && pesbuffer_full[buffer_full_position + 1] == 0
						&& pesbuffer_full[buffer_full_position + 2] == 0 && pesbuffer_full[buffer_full_position + 3] == 1
						&& ( pesbuffer_full[buffer_full_position + 4] == 0x67 || pesbuffer_full[buffer_full_position + 4] == 0x61 ) )
				{
					pesbuffer_len = count_1;
					pesbuffer =( uint8_t * ) malloc( sizeof( uint8_t ) * pesbuffer_len );
					if ( pesbuffer == NULL )
					{
						RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): malloc failed for pesbuffer\n", __FILE__, __LINE__);
					}
					memcpy( pesbuffer, pesbuffer_full, pesbuffer_len );
					full_pes_ok = 1;
					if ( buffer_full_length != 0 )
					{
						temp_buf =( char * ) malloc( sizeof( char ) *  buffer_full_length );
						if ( temp_buf == NULL )
						{
							RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): malloc failed for temp_buf\n",
									__FILE__, __LINE__);
						}
						memcpy( &temp_buf[0],&pesbuffer_full[buffer_full_position],buffer_full_length );
						memcpy( &pesbuffer_full[0], &temp_buf[0],buffer_full_length);
						buffer_full_position = 0;
						free(temp_buf);
						count_1=0;
					}		
				}
			}
			if(full_pes_ok==0)
			{
				buffer_full_position++;
				buffer_full_length--;
				count_1++;
			}
			/*full_pes_ok flag will be 1 if the pesbuffer having one complete I or P frame*/
			if ( full_pes_ok )
			{
				full_pes_ok = 0;
				if ( pesbuffer[0] == 0 && pesbuffer[1] == 0 && pesbuffer[2] == 0 && pesbuffer[3] == 1 && ( pesbuffer[4] == 0x67 || 
							pesbuffer[4] == 0x61 ) )
				{
					if ( pesbuffer[4] == 0x67 )//PSP with I_FRAME
					{
						iframebuf_len = pesbuffer_len;
						iframebuf = (uint8_t *) realloc(iframebuf, sizeof(uint8_t) * iframebuf_len );
						if ( iframebuf == NULL )
						{
							RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): realloc failed for iframebuf\n",
									__FILE__, __LINE__);
						}

						memcpy(iframebuf,pesbuffer,iframebuf_len);
						iframe=1;// iframe++;
						frame_count++;
						if(frame_duration != 0)	
							pts_value += (frame_duration * 90); // ( 1 / config.frame_rate ) * 90000;
						withpcr = TRUE;
						if ( frame_count >= ( config.frame_rate * config.CHUNK_LENGTH ) )
						{
							RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Total frame count is %d recorded clip duration is %.2f\n",__FILE__, __LINE__,frame_count,float(frame_count/config.frame_rate));
							create_chunk = 1;
							static int mm =0;
							//config.has_audio = 1;
							config.has_audio = (int )IsGSTEnabledInRFC(RFC_CVR,CVR_AUDIO_GST);
							if(config.has_audio == 1 && mm==0)
							{
								mm=1;
								RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio enabled through RFC...\n",__FILE__, __LINE__);
								pthread_mutex_lock(&mutex_hasaudio);
								pthread_cond_broadcast(&cv_hasaudio);
								pthread_mutex_unlock(&mutex_hasaudio);
								pthread_mutex_lock(&mutexfor_gotptsdif);
								pthread_cond_wait(&cvfor_gotptsdif, &mutexfor_gotptsdif);
								pthread_mutex_unlock(&mutexfor_gotptsdif);
								pts_adjust();
							}
						}
					}
					else if ( pesbuffer[4] == 0x61 )//P_FRAME
					{
						frame_count++;
						if(frame_duration != 0)
							pts_value += (frame_duration * 90); // ( 1 / config.frame_rate ) * 90000;
					}
				}
				else
				{
					RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): H264 header error!!!\n", __FILE__, __LINE__);
				}
				/*Each frame will be written to the file after converting to ts.*/
				ts_converter( pesbuffer, pesbuffer_len, withpcr , 0xe0, 0x0100 );
				free( pesbuffer );
				/* create_chunk=1 means, the length of the stream is >= CHUNK_LENGTH*/
				return(buffer_full_length);
			}

		}

	}//while(1) end
}

static unsigned char *form_pat_packet( int *pSize )
{
	int packetSize 	        = MPEG_PKT_SIZE;
	int ttsSize		= 0;
	unsigned char *data_ptr = ( unsigned char * ) malloc( packetSize );
	if ( NULL == data_ptr )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): malloc failed for data_ptr\n", __FILE__, __LINE__);
		return NULL;
	}
	pat_packet( pat_version, programNumber, pmt_pid, data_ptr+ttsSize, packetSize-ttsSize, pat_count);
	*pSize = packetSize;
	return data_ptr;
}

static void pat_packet( unsigned char version, unsigned short prognum,
		unsigned short pmtpid, unsigned char *pat, int size,
		int &count )
{
	int i = 0;

	pat_count++;
	pat[0] = 0x47;
	pat[1] = 0x40;
	pat[2] = 0x00;
	pat[3] = 0x10;
	pat[3] |= ( pat_count & 0x0f );
	pat[4] = 0x00;
	pat[5] = 0x00;
	pat[6] = 0xb0;
	pat[7] = 0x0d;
	pat[8] = 0x00;
	pat[9] = 0x01;
	pat[10] = 0xc1;
	pat[11] = 0x00;
	pat[12] = 0x00;
	pat[13] = 0x00;
	pat[14] = 0x01;
	pat[15] = 0xf0;
	pat[16] = 0x00;
	pat[17] = 0x2a;
	pat[18] = 0xb1;
	pat[19] = 0x04;
	pat[20] = 0xb2;;
	for ( i = 21; i < size; i++ )
	{
		pat[i] = 0xFF;
	}

}

/*Create PMT packet*/
static unsigned char *form_pmt_packet( int *dataSize )
{
	int pes_pid 	  = 0x0100; //video pid
	int aac_pid 	  = 0x1f;  //AAC audio pid
	int i 		  = 0;
	int temp	  = 0;
	uint16_t pmtPid	  = 0x1000, pcrPid = 0x0100;
	int pmtSectionLen = 0;
	int pi		  = 17;
	unsigned char* pmtPacket;
	pmtPacket	  = (unsigned char*)malloc( MPEG_PKT_SIZE );
	if ( NULL == pmtPacket )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d):malloc failed for pmtPacket\n", __FILE__, __LINE__);
		return NULL;
	}

	pmtPacket[pi++] = 0x1B; /*h264*/
	pmtPacket[pi++] = (0xE0 | (unsigned char) ((pes_pid >> 8) & 0x1F));
	pmtPacket[pi++] = (unsigned char) (0xFF & pes_pid);
	pmtPacket[pi++] = 0xF0 ;
	pmtPacket[pi++] = 0x00 ;
	pmtPacket[pi++] = 0x0f; /*AAC audio*/
	pmtPacket[pi++] = (0xE0 | (unsigned char) ((aac_pid >> 8) & 0x1F));
	pmtPacket[pi++] = (unsigned char) (0xFF & aac_pid);
	pmtPacket[pi++] = 0xF0 ;
	pmtPacket[pi++] = 0x00 ;
	pmtPacket[0]= 0x47;
	pmtPacket[1]= 0x60;
	pmtPacket[1] |= (unsigned char) ((pmtPid >> 8) & 0x1F);
	pmtPacket[2]= (unsigned char) (0xFF & pmtPid);
	pmtPacket[3]= 0x10; // 2 bits Scrambling = no; 2 bits adaptation field = no adaptation; 4 bits continuity counter
	pmt_count++;
	pmtPacket[3] |= (pmt_count & 0x0F);
	pmtSectionLen= pi-4;
	pmtPacket[4]= 0x00;
	pmtPacket[5]= 0x02;
	pmtPacket[6]= (0xB0 | ((pmtSectionLen>>8)&0xF));
	pmtPacket[7]= (pmtSectionLen & 0xFF); //lower 8 bits of Section length
	pmtPacket[8]= ((programNumber >> 8)&0xFF); // TSID : Don't care
	pmtPacket[9]= (programNumber)&0xFF; ; // TSID : Don't care
	temp= pmt_version << 1;
	temp= temp & 0x3E; //Masking first 2 bits and last one bit : 0011 1110 (3E)
	pmtPacket[10]= 0xC1 | temp; //C1 : 1100 0001 : setting reserved bits as 1, current_next_indicator as 1
	pmtPacket[11]= 0x00;
	pmtPacket[12]= 0x00;
	pmtPacket[13]= 0xE0;
	pmtPacket[13] |= (unsigned char) ((pcrPid >> 8) & 0x1F);
	pmtPacket[14]= (unsigned char) (0xFF & pcrPid);
	pmtPacket[15] = 0xF0;
	pmtPacket[16] = 0x00; //pgm info length.  No DTCP descr here..
	unsigned long crc = get_crc32(&pmtPacket[5], pi -5);
	// 4 bytes of CRC
	pmtPacket[pi++] = (crc >> 24) & 0xFF;
	pmtPacket[pi++] = (crc >> 16) & 0xFF;
	pmtPacket[pi++] = (crc >> 8) & 0xFF;
	pmtPacket[pi++] = crc & 0xFF;
	// Fill stuffing bytes for rest of TS packet
	for (i = pi; i < MPEG_PKT_SIZE; i++)
	{
		pmtPacket[i] = 0xFF;
	}
	*dataSize = MPEG_PKT_SIZE;
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Created PMT packet successfully\n",__FILE__, __LINE__);
	return pmtPacket;
}

/*****************************************************/
/*pes_header() and write_TS_packet_parts(), both these functions creating the ful pes packet*/

extern void PES_header( int with_PTS, uint64_t pts, int streamtype, int data_len )
{

	int pts1, pts2, pts3;
	int guard_bits = 2;
	if ( with_PTS )
	{
		PES_hdr[0] = 0x00;
		PES_hdr[1] = 0x00;
		PES_hdr[2] = 0x01;
		PES_hdr[3] = streamtype;      //stream_id; 0xe0 for raw h264 video
		PES_hdr[4] = ((data_len & 0xFF00) >> 8);
		PES_hdr[5] = ((data_len & 0x00FF));
		PES_hdr[6] = 0x80;
		PES_hdr[7] = 0x80;
		PES_hdr[8] = 0x05;
		//encode_pts_dts(&(PES_hdr[9]),2,pts);
		pts1 = ( int ) ( ( pts >> 30 ) & 0x07 );
		pts2 = ( int ) ( ( pts >> 15 ) & 0x7FFF );
		pts3 = ( int ) ( pts & 0x7FFF );
		PES_hdr[9] = ( guard_bits << 4 ) | ( pts1 << 1 ) | 0x01;
		PES_hdr[10] = ( pts2 & 0x7F80 ) >> 7;
		PES_hdr[11] = ( ( pts2 & 0x007F ) << 1 ) | 0x01;
		PES_hdr[12] = ( pts3 & 0x7F80 ) >> 7;
		PES_hdr[13] = ( ( pts3 & 0x007F ) << 1 ) | 0x01;
		PES_hdr[14] = 0x00;
		PES_hdr[15] = 0x00;
		PES_hdr[16] = 0x00;
		PES_hdr[17] = 0x01;
		PES_hdr[18] = 0x09;
		PES_hdr[19] = 0xf0;

	}
}

static int write_TS_packet_parts( uint8_t TS_packet[TS_PACKET_SIZE],
		int TS_hdr_len, uint8_t pes_hdr[],
		int pes_hdr_len, uint8_t data[],
		int data_len, uint32_t pid, int with_PTS,
		int with_PCR )
{
	int err;
	if ( !with_PTS )
	{
		pes_count++;
		if ( tsheader_length == 0 )
		{
			TS_packet[0] = 0x47;
			TS_packet[1] = 0x01;
			//TS_packet[1] |= (unsigned char) ((pid >> 8) & 0x1F);
			TS_packet[2] = 0x00;    //(unsigned char) (0xFF & pid);
			TS_packet[3] = 0x10;    // 2 bits Scrambling = no; 2 bits adaptation field = no adaptation; 4 bits continuity counter
			TS_packet[3] |= ( pes_count & 0x0F );
		}
		else
		{
			TS_packet[0] = 0x47;
			TS_packet[1] = 0x01;
			//TS_packet[1] |= (unsigned char) ((pid >> 8) & 0x1F);
			TS_packet[2] = 0x00;    //(unsigned char) (0xFF & pid);
			TS_packet[3] = 0x30;    // 2 bits Scrambling = no; 2 bits adaptation field = no adaptation; 4 bits                      continuity counter
			TS_packet[3] |= ( pes_count & 0x0F );
			TS_packet[4] = tsheader_length - 1;
			TS_packet[5] = 0;
			for ( int yy = 1; yy < tsheader_length; yy++ )
				TS_packet[yy + 5] = 0xff;
		}
	}
	else
	{
		if ( with_PCR )
		{
			//  if PCR_extn is > 0, pcr = (pcr * 300 + pcr_ext % 300) / 300.
			//pcr_extn should be 0 to 299.
			int PCR_extn = 0;
			uint64_t PCR_base = pts_value;
			pes_count++;
			TS_packet[0] = 0x47;
			TS_packet[1] = 0x41;
			//TS_packet[1] |= (unsigned char) ((pid >> 8) & 0x1F);
			TS_packet[2] = 0x00;    //(unsigned char) (0xFF & pid);
			TS_packet[3] = 0x30;    // 2 bits Scrambling = no; 2 bits adaptation field = no adaptation; 4 bits continuity                               counter
			TS_packet[3] |= ( pes_count & 0x0F );
			//pes_count++;
			TS_packet[4] = 0x07;    //adaption field length
			TS_packet[5] = 0x50;    //enabled random access indicator and PCR.   
			TS_packet[6] = ( uint8_t ) ( PCR_base >> 25 );
			TS_packet[7] = ( uint8_t ) ( ( PCR_base >> 17 ) & 0xFF );
			TS_packet[8] = ( uint8_t ) ( ( PCR_base >> 9 ) & 0xFF );
			TS_packet[9] = ( uint8_t ) ( ( PCR_base >> 1 ) & 0xFF );
			TS_packet[10] =( uint8_t ) ( ( ( PCR_base & 0x1 ) << 7 ) | 0x7E | ( PCR_extn >> 8 ) );
			TS_packet[11] = ( uint8_t ) ( PCR_extn >> 1 );
		}
		else
		{
			pes_count++;
			TS_packet[0] = 0x47;
			TS_packet[1] = 0x41;
			//TS_packet[1] |= (unsigned char) ((pid >> 8) & 0x1F);
			TS_packet[2] = 0x00;    //(unsigned char) (0xFF & pid);
			TS_packet[3] = 0x10;    // 2 bits Scrambling = no; 2 bits adaptation field = no adaptation; 4 bits continuity                               counter
			TS_packet[3] |= ( pes_count & 0x0F );
			TS_packet[4] = 0x07;    //adaption field length
		}

	}
	sem_wait(&s2); 
	if ( pes_hdr_len > 0 )
	{
		if(TS_hdr_len+pes_hdr_len > 188) assert(0);
		memcpy( &( TS_packet[TS_hdr_len] ), pes_hdr, pes_hdr_len );
	}
	if ( data_len > 0 )
	{
		if(pes_hdr_len+TS_hdr_len + data_len > 188 ) assert(0);
		memcpy( &( TS_packet[TS_hdr_len + pes_hdr_len] ), data, data_len );
		err = fwrite( TS_packet, sizeof( uint8_t ), 188, pat_pmt_ts_file );
	}
	sem_post(&s2);	
}

extern void encode_pts_dts( uint8_t data[], int guard_bits, uint64_t value )
{
	int pts1, pts2, pts3;
	if ( value > MAX_PTS_VALUE )
	{
		char *what;
		uint64_t temp = value;
		while ( temp > MAX_PTS_VALUE )
			temp -= MAX_PTS_VALUE;
		/* switch (guard_bits)
		 * {
		 * case 2:  what = "PTS alone"; break;
		 * case 3:  what = "PTS before DTS"; break;
		 * case 1:  what = "DTS after PTS"; break;
		 * default: what = "PTS/DTS/???"; break;
		 * }
		 */
		value = temp;
	}

	pts1 = ( int ) ( ( value >> 30 ) & 0x07 );
	pts2 = ( int ) ( ( value >> 15 ) & 0x7FFF );
	pts3 = ( int ) ( value & 0x7FFF );
	data[0] = ( guard_bits << 4 ) | ( pts1 << 1 ) | 0x01;
	data[1] = ( pts2 & 0x7F80 ) >> 7;
	data[2] = ( ( pts2 & 0x007F ) << 1 ) | 0x01;
	data[3] = ( pts3 & 0x7F80 ) >> 7;
	data[4] = ( ( pts3 & 0x007F ) << 1 ) | 0x01;
}

/*********************************************/

static void pat(  )
{
	patbuffer = form_pat_packet( &patbufsize );
	sem_wait(&s2);
	fwrite( patbuffer, 1, patbufsize, pat_pmt_ts_file );
	free( patbuffer );
	sem_post(&s2);
}

static void pmt(  )
{
	sem_wait(&s2);
	pmtbuffer = form_pmt_packet( &pmtbufsize );
	fwrite( pmtbuffer, 1, pmtbufsize, pat_pmt_ts_file );
	free( pmtbuffer );
	sem_post(&s2);
}

static unsigned char *sdt_packet( int *sdtdataSize )
{
	unsigned char *sdtPacket;
	sdtPacket = ( unsigned char * ) malloc( MPEG_PKT_SIZE );
	if ( NULL == sdtPacket )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): malloc failed for sdtPacket\n", __FILE__, __LINE__);
		return NULL;
	}
	sdt_count++;

	sdtPacket[0] = 0x47;
	sdtPacket[1] = 0x40;
	sdtPacket[2] = 0x11;
	sdtPacket[3] = 0x10;
	sdtPacket[3] |= ( sdt_count & 0x0f );
	sdtPacket[4] = 0x00;
	sdtPacket[5] = 0x42;
	sdtPacket[6] = 0xf0;
	sdtPacket[7] = 0x25;
	sdtPacket[8] = 0x00;
	sdtPacket[9] = 0x01;
	sdtPacket[10] = 0xc1;
	sdtPacket[11] = 0x00;
	sdtPacket[12] = 0x00;
	sdtPacket[13] = 0xff;
	sdtPacket[14] = 0x01;
	sdtPacket[15] = 0xff;
	sdtPacket[16] = 0x00;
	sdtPacket[17] = 0x01;
	sdtPacket[18] = 0xfc;
	sdtPacket[19] = 0x80;
	sdtPacket[20] = 0x14;
	sdtPacket[21] = 0x48;
	sdtPacket[22] = 0x12;
	sdtPacket[23] = 0x01;
	sdtPacket[24] = 0x06;
	sdtPacket[25] = 0x46;
	sdtPacket[26] = 0x46;
	sdtPacket[27] = 0x6d;
	sdtPacket[28] = 0x70;
	sdtPacket[29] = 0x65;
	sdtPacket[30] = 0x67;
	sdtPacket[31] = 0x09;
	sdtPacket[32] = 0x53;
	sdtPacket[33] = 0x65;
	sdtPacket[34] = 0x72;
	sdtPacket[35] = 0x76;
	sdtPacket[36] = 0x69;
	sdtPacket[37] = 0x63;
	sdtPacket[38] = 0x65;
	sdtPacket[39] = 0x30;
	sdtPacket[40] = 0x31;
	sdtPacket[41] = 0x77;
	sdtPacket[42] = 0x7c;
	sdtPacket[43] = 0x43;
	sdtPacket[44] = 0xca;
	// Fill stuffing bytes for rest of TS packet
	for ( int i = 45; i < MPEG_PKT_SIZE; i++ )
	{
		sdtPacket[i] = 0xFF;
	}
	*sdtdataSize = MPEG_PKT_SIZE;
	return sdtPacket;
}

static void sdt(  )
{
	sem_wait(&s2);
	sdtbuffer = sdt_packet( &sdtbufsize );
	fwrite( sdtbuffer, 1, sdtbufsize, pat_pmt_ts_file );
	sem_post(&s2);
	free( sdtbuffer );
}

static void write_chunk(  )
{

	char m_fpath[CVR_FILE_PATH_LEN];
	char starttime[200]="1210";
	char endtime[200] = "1210";
	char fpath[100];

	next_iframe = 0;
	frame_count = 0;
	pes_count = 0xff;
	pat_count = 0xff;
	pmt_count = 0xff;
	sdt_count = 0xff;
	fclose( pat_pmt_ts_file );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): TS file generated successfully..\n",__FILE__, __LINE__);
	sprintf( fpath, "/tmp/cvr/%s",filename);
	sprintf( m_fpath, "/tmp/cvr/%s","hello.m_level");
	cvr_upload( fpath, starttime, endtime,0, 0,m_fpath,1,NULL,NULL);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Creting new chunk\n",__FILE__, __LINE__);
	//cvr_upload( fpath, starttime, endtime, event_type, event_datetime,m_fpath,motion_level_idx+1);
	strcpy( prev_filename, filename );
	sys_time = current_time(  );
	sprintf( filename, "%d_%d_%sts", f_seq++, bitrate, sys_time );
	free( sys_time );
	strcpy( file_path_cpy, file_path );
	strcat( file_path, "/" );
	strcat( file_path, filename );
	pat_pmt_ts_file = fopen( file_path, "w+" );
	if(NULL==pat_pmt_ts_file)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create %s chunk\n", __FILE__, __LINE__,filename);
	}
	strcpy( file_path, file_path_cpy );
	sdt(  );
	pat(  );
	pmt(  );
}

static void file_append(  )
{
	int f_length;
	char *buffer = NULL;
	FILE *filecopy = NULL;
	FILE *File = NULL;
	File = fopen( filename, "r+" ); //curent file
	if ( File != NULL )
	{
		fseek( File, 0, SEEK_END );
		f_length = ftell( File );
		fseek( File, 0, SEEK_SET );
		buffer = ( char * ) malloc( f_length );
		fread( buffer, 1, f_length, File );
		//sprintf(filename,"pat_pmt_ts_file_%d.ts",--f_seq);
		filecopy = fopen( prev_filename, "a+" );
		fwrite( buffer, sizeof( char ), f_length, filecopy );
		fclose( File );
		free( buffer );
		fclose( filecopy );
	}
	else
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): %s chunk not created\n",__FILE__, __LINE__,filename);
	}
}
/*It will convert the raw h264 frame to TS format*/
static void ts_converter( uint8_t * pesbuffer, int pesbuffer_len, int with_pcr, int streamtype, uint32_t pid )
{
	int data_len=0;
	pesbuffer_len_cpy = pesbuffer_len;
	int buffer_position = 0;
	if ( with_pcr )
	{
		//data_len=188-12-6;
		withpcr = FALSE;
		PES_header( TRUE, pts_value, streamtype, data_len );
		result = write_TS_packet_parts( TS_packet, 12, PES_hdr, 20,&( pesbuffer[buffer_position] ), 156,pid, TRUE, TRUE );
		buffer_position += 156;
		pesbuffer_len -= 156;
	}
	else
	{
		//data_len=188-4-6;
		PES_header( TRUE, pts_value ,streamtype, data_len);
		result =
			write_TS_packet_parts( TS_packet, 4, PES_hdr, 20, &( pesbuffer[buffer_position] ), 164,pid, TRUE, FALSE );
		buffer_position += 164;
		pesbuffer_len -= 164;
	}
	while ( pesbuffer_len >= 184 )
	{
		tsheader_length = 0;
		//data_len=0;
		PES_header( FALSE, 0 ,streamtype, data_len);
		result = write_TS_packet_parts( TS_packet, tsheader_length + 4, PES_hdr, 0,&( pesbuffer[buffer_position] ), 184,pid, FALSE, FALSE );
		buffer_position += 184;
		pesbuffer_len -= 184;

	}
	if ( pesbuffer_len < 184 && pesbuffer_len > 0 )
	{
		//data_len=0;
		tsheader_length = 188 - ( pesbuffer_len + 4 );  //tsheader_length means, addition stuffing bytes only
		PES_header( FALSE, 0,streamtype, data_len );
		result = write_TS_packet_parts( TS_packet, tsheader_length + 4, PES_hdr, 0, &( pesbuffer[buffer_position] ),
				pesbuffer_len, pid, FALSE, FALSE );
	}
}


/*To get the current system time*/
static char *current_time(  )
{
	time_t time_now;
	struct tm *timeinfo;
	char *tm_buffer = ( char * ) malloc( sizeof( char ) * 21 );
	if(NULL==tm_buffer)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to allocate memory for tm_buffer\n", __FILE__, __LINE__);
	}
	time( &time_now );
	timeinfo = localtime( &time_now );
	strftime( tm_buffer, 21, "%F:%T.", timeinfo );//Setting format of time
	return tm_buffer;
}

/*Reading the configuration file */
static void read_config(  )
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Reading configuration data...\n",__FILE__, __LINE__);	
	char confbuf[CONFIG_LINE_BUFFER_SIZE];
	FILE *conffile = fopen( "/opt/cvr_config.txt", "r" );///opt/cvr_config.txt
	if ( conffile != NULL )
	{
		while ( !feof( conffile ) )
		{
			fgets( confbuf, CONFIG_LINE_BUFFER_SIZE, conffile );
			if ( confbuf[0] == '#' || strlen( confbuf ) < 4 )
			{
				continue;
			}
			if ( strstr( confbuf, "FRAMERATE" ) )
			{
				config.frame_rate =  ( float ) read_int_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Framerate: %f\n",__FILE__, __LINE__,config.frame_rate);
			}
			if ( strstr( confbuf, "KEYVALUE" ) )
			{
				config.keyvalue = read_int_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Keyvalue: %d\n",__FILE__, __LINE__,config.keyvalue);
			}
			if ( strstr( confbuf, "CHUNK_LENGTH" ) )
			{
				config.CHUNK_LENGTH = read_int_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Chunk length: %d\n",__FILE__, __LINE__,config.CHUNK_LENGTH);
			}
			if ( strstr( confbuf, "FILE_PATH" ) )
			{
				read_string_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): File path: %s\n",__FILE__, __LINE__,file_path);
			}
			if ( strstr( confbuf, "MAX_FILE_COUNT" ) )
			{
				config.max_file_count = read_int_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Maximum file count: %d\n",__FILE__, __LINE__,config.max_file_count);
			}

			if ( strstr( confbuf, "BITRATE" ) )
			{
				config.bitrate = read_int_from_config_line( confbuf );
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Bitrate: %d\n",__FILE__, __LINE__,config.bitrate);
			}

		}
		fclose( conffile );
	}
	else
	{
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Could not open configuration file /opt/cvr_config.txt\n",__FILE__, __LINE__);
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Trying to read configuration file /etc/cvr_config.txt\n",__FILE__, __LINE__);
		FILE *conffile = fopen( "/etc/cvr_config.txt", "r" );
		if ( conffile != NULL )
		{
			while ( !feof( conffile ) )
			{
				fgets( confbuf, CONFIG_LINE_BUFFER_SIZE, conffile );
				if ( confbuf[0] == '#' || strlen( confbuf ) < 4 )
				{
					continue;
				}
				if ( strstr( confbuf, "FRAMERATE" ) )
				{
					config.frame_rate =( float ) read_int_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Framerate: %f\n",__FILE__, __LINE__,config.frame_rate);
				}
				if ( strstr( confbuf, "KEYVALUE" ) )
				{
					config.keyvalue = read_int_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Keyvalue: %d\n",__FILE__, __LINE__,config.keyvalue);   
				}
				if ( strstr( confbuf, "CHUNK_LENGTH" ) )
				{
					config.CHUNK_LENGTH =
						read_int_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Chunk length: %d\n",__FILE__, __LINE__,config.CHUNK_LENGTH);
				}
				if ( strstr( confbuf, "FILE_PATH" ) )
				{
					read_string_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): File path: %s\n",__FILE__, __LINE__,file_path);
				}
				if ( strstr( confbuf, "MAX_FILE_COUNT" ) )
				{
					config.max_file_count =read_int_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Maximum file count: %d\n",__FILE__, __LINE__,config.max_file_count);
				}
				if ( strstr( confbuf, "BITRATE" ) )
				{
					config.bitrate = read_int_from_config_line( confbuf );
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Bitrate: %d\n",__FILE__, __LINE__,config.bitrate);
				}

			}
			fclose( conffile );
		}
		else
		{       
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Could not open the configuration file etc/cvr_config.txt\n",__FILE__, __LINE__);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Assigning default values\n",__FILE__, __LINE__);

		}
	}

}


static int read_int_from_config_line( char *config_line )
{
	char prm_name[MAX_CONFIG_VARIABLE_LEN];
	int val;
	sscanf( config_line, "%s %d\n", prm_name, &val );
	return val;
}

static void read_string_from_config_line( char *config_line )
{
	char prm_name[MAX_CONFIG_VARIABLE_LEN];
	sscanf( config_line, "%s %s\n", prm_name, file_path );
}

static int check_folder_exist( char *f_path )
{
	struct stat st;
	return ( stat( f_path, &st ) == 0 );
}



static void mkdir_p( const char *dir )
{
	char tmp[256];
	char *p = NULL;
	size_t len;

	snprintf( tmp, sizeof( tmp ), "%s", dir );
	len = strlen( tmp );
	if ( tmp[len - 1] == '/' )
		tmp[len - 1] = 0;
	for ( p = tmp + 1; *p; p++ )
		if ( *p == '/' )
		{
			*p = 0;
			mkdir( tmp, S_IRWXU );
			*p = '/';
		}
	mkdir( tmp, S_IRWXU );
}

/* functions for stream stop operation*/
/*Stopping Pipeline via EOS signal*/
static void Stop_streaming( cam_config & config_data )
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stopping Pipeline via EOS signal!!\n",__FILE__, __LINE__);
	char setrequest_1[100];
	char response_1[100];
	response_1[0] = '\0';
	if( config.has_audio == 1 )
	{
		pthread_mutex_lock(&mutexfor_waitingaudbuf);
		pthread_cond_signal(&cvfor_waitingaudbuf);
		pthread_mutex_unlock(&mutexfor_waitingaudbuf);
		pthread_mutex_lock(&mutexfor_threadsync_1);
		pthread_cond_signal(&cvfor_threadsync_1);
		pthread_mutex_unlock(&mutexfor_threadsync_1);


		sprintf( setrequest_1,"http://127.0.0.1:8085/stopaudiostream&keyvalue=0&format=1&" );
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stop request URL:%s\n",__FILE__, __LINE__,setrequest_1);
		send_request( setrequest_1, response_1 );
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio stop request end\n",__FILE__, __LINE__);
		sem_wait(&s3); 
	}
	usleep(1000);

	sprintf( setrequest_1,"http://127.0.0.1:8085/stopstream&keyvalue=%d&", config_data.keyvalue );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stop request URL:%s\n",__FILE__, __LINE__,setrequest_1);    
	send_request( setrequest_1, response_1 );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Streaming stopped!!!\n",__FILE__, __LINE__);
}

static void send_request( char *request, char *response )
{	
	GstElement *req_souphttpsrc, *req_pipeline, *req_appsink;
	GMainLoop *loop2;
	loop2 = g_main_loop_new( NULL, FALSE );
	GstBus *req_bus;
	req_pipeline = gst_element_factory_make( "pipeline", NULL );
	req_appsink = gst_element_factory_make( "appsink", NULL );
	req_souphttpsrc = gst_element_factory_make( "souphttpsrc", NULL );
	g_object_set( G_OBJECT( req_appsink ), "emit-signals", TRUE, "sync", FALSE,
			NULL );
	req_bus = gst_pipeline_get_bus( GST_PIPELINE(req_pipeline ) );
	gst_bus_add_watch( req_bus, ( GstBusFunc ) on_message_for_stoprequest, ( gpointer ) loop2 );
	if ( !req_souphttpsrc )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Faled to create httpsrc\n",__FILE__, __LINE__);
	}
	gst_bin_add_many( GST_BIN( req_pipeline ), req_souphttpsrc, req_appsink, NULL );
	gst_element_link_many( req_souphttpsrc, req_appsink, NULL );
	g_signal_connect( req_appsink, "new-sample", G_CALLBACK( fetch_response ),
			( gpointer ) response );
	g_object_set( G_OBJECT( req_souphttpsrc ), "location", request, NULL );
	gst_element_set_state( req_appsink, GST_STATE_PLAYING );
	gst_element_set_state( req_pipeline, GST_STATE_PLAYING );
	gst_element_set_state( req_pipeline, GST_STATE_PLAYING );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): GST_STATE_PLAYING\n",__FILE__, __LINE__);
	g_main_loop_run( loop2 );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Exited from loop2\n",__FILE__, __LINE__);
	//gst_element_set_state(req_pipeline,GST_STATE_NULL);//commented
	gst_element_set_state( req_appsink, GST_STATE_READY );
	gst_element_set_state( req_souphttpsrc, GST_STATE_READY );
	gst_element_set_state( req_appsink, GST_STATE_NULL );
	gst_element_set_state( req_souphttpsrc, GST_STATE_NULL );

	gst_element_set_state( req_pipeline, GST_STATE_NULL );

	if ( gst_element_get_state( req_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE ) ==
			GST_STATE_CHANGE_SUCCESS )
	{	
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): The state of pipeline changed to GST_STATE_NULL successfully\n",__FILE__, __LINE__);
	}
	else
	{
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Changing the state of pipeline to GST_STATE_NULL failed\n",__FILE__, __LINE__);
	}


	gst_element_unlink_many( req_souphttpsrc, req_appsink, NULL );
	gst_object_ref( req_souphttpsrc );
	gst_object_ref( req_appsink );
	gst_bin_remove_many( GST_BIN( req_pipeline ), req_appsink, req_souphttpsrc, NULL );

	gst_object_unref( req_pipeline );
	gst_object_unref( req_souphttpsrc );
	gst_object_unref( req_appsink );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Pipeline deleted\n",__FILE__, __LINE__);
}

static gboolean fetch_response( GstElement * elt, gpointer user_data )
{
	char *response;
	response = ( char * ) user_data;
	cout << response;
	char *str;
	GstSample *sample;
	GstBuffer *buffer;
	GstMapInfo map;
	sample = gst_app_sink_pull_sample( GST_APP_SINK( elt ) );
	buffer = gst_sample_get_buffer( sample );
	gst_buffer_map( buffer, &map, GST_MAP_READ );
	str = ( char * ) malloc( map.size );
	strncpy( str, ( char * ) map.data, map.size );
	str = strtok( str, "!" );
	strcat( response, str );
	gst_buffer_unmap( buffer, &map );
	gst_sample_unref( sample );
	return FALSE;

}

static void start_videostreaming( cam_config & config_data )
{
returntoreq:
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Sending video streaming request...\n",__FILE__, __LINE__);
	GstElement *souphttpsrc, *pipeline, *appsink;
	//gst_init(&argc,&argv);
	pipeline = gst_element_factory_make( "pipeline", NULL );
	appsink = gst_element_factory_make( "appsink", NULL );
	souphttpsrc = gst_element_factory_make( "souphttpsrc", NULL );
	GstBus *bus;
	g_object_set( G_OBJECT( appsink ), "emit-signals", TRUE, "sync", FALSE,
			NULL );
	bus = gst_pipeline_get_bus( GST_PIPELINE( pipeline ) );
	GMainLoop *loop;
	loop = g_main_loop_new( NULL, FALSE );//gstreamer loop functon will call the videomap function.
	gst_bus_add_watch( bus, ( GstBusFunc ) on_message, ( gpointer ) loop );
	if ( !souphttpsrc )
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create httpsrc\n",__FILE__, __LINE__);
	}




	char startrequest[100];
	sprintf( startrequest,
			"http://127.0.0.1:8085/startstream&keyvalue=%d&framerate=%d&dotimestamp=1&bitrate=%d&",
			config_data.keyvalue, ( int ) config_data.frame_rate,config.bitrate );
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Video Requested URL is: %s\n",__FILE__, __LINE__,startrequest);
	g_object_set( G_OBJECT( souphttpsrc ), "location", startrequest, NULL );
	g_object_set( G_OBJECT( souphttpsrc ), "blocksize", 4096 *2 , NULL );//new property
	g_object_set( G_OBJECT( appsink), "blocksize", 4096 * 2, NULL );
	gst_bin_add_many( GST_BIN( pipeline ), souphttpsrc, appsink, NULL );
	gst_element_link_many( souphttpsrc, appsink, NULL );
	gst_element_set_state( pipeline, GST_STATE_PLAYING );
	g_signal_connect( appsink, "new-sample", G_CALLBACK( on_new_sample ),
			NULL );

	g_main_loop_run( loop );
	if ( (videoreq_retry == 1) && (videoreq_success == 0)  )
	{
		sleep(5);
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Resending video stream request\n",__FILE__, __LINE__);
		videoreq_retry =0;
		goto returntoreq;
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Exited from video loop\n",__FILE__, __LINE__);

}


static void *start_getgstvideo( void *vargp )
{
	if ( check_folder_exist( file_path ) )
	{  
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s folder already exist\n",__FILE__, __LINE__,file_path);
	}
	else
	{
		mkdir_p( file_path );
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Created %s folder successfully\n",__FILE__, __LINE__,file_path);
	}
	sys_time = current_time(  );
	sprintf( filename, "%d_%d_%sts", f_seq++, bitrate, sys_time );
	strcpy( prev_filename, filename );
	free( sys_time );
	strcpy( file_path_cpy, file_path );
	strcat( file_path, "/" );
	strcat( file_path, filename );
	pat_pmt_ts_file = fopen( file_path, "a+" );
	if(NULL==pat_pmt_ts_file)
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create first chunk %s\n",__FILE__, __LINE__,filename);
	}
	strcpy( file_path, file_path_cpy );
	sdt(  );
	pat(  );
	pmt(  );

	start_videostreaming( config );

	free( TS_packet );
	free( PES_hdr );
	free( pesbuffer_full );
	if(pat_pmt_ts_file !=NULL)
		fclose(pat_pmt_ts_file ); 

	/*f_seq should be 1 if we have only one chunk.So no need to call file_append function */
	/*if the last chunk length less than 5seconds and f_seq > 1, then it will be appended with the last chunk. */
	if ( frame_count <= ( config.frame_rate * 5 ) && ( f_seq > 1 ) )
	{
		file_append(  );
		strcpy( file_path_cpy, file_path );
		strcat( file_path, "/" );
		strcat( file_path, filename );
		int error = remove( file_path );    //
		if ( error != 0 )
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): %s file already removed\n",__FILE__, __LINE__,filename);
	}
	strcpy( file_path, file_path_cpy );
	free(remainingbuf);
	free(iframebuf);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): getgstvideo thread exited\n",__FILE__, __LINE__);
	pthread_exit(NULL);
}

static void *start_convertvideo( void *vargp )
{

	static unsigned long long v_mapsize=0;
	static int vabsolutetime_flag = 1;
	static int vmapsize_flag = 0;
	static int vmapdata_flag = 0;
	static int synccount = -1;
	while(stop_cvr == 0)
	{

		sem_wait(&s4);
#if 1
		if( (create_chunk == 1) && (threadsync == 1))
		{
			create_chunk = 0;
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Creating new chunk...\n",__FILE__, __LINE__);
			write_chunk(  );//this function will create a new chunk
			cc_restflag=1; //this flag is used to reset the audio pes counter
			/*Adding the last i frame of the prev chunk to the beginning of the next chunk.*/
			ts_converter( iframebuf, iframebuf_len, withpcr , 0xe0,0x0100 );
		}
		if ( ( ( f_seq > config.max_file_count ) && ( max_file_count_flag == 0 )&& (config.max_file_count != -1) && (threadsync == 1) ) ||  (CVR::term_flag) )
		{
			max_file_count_flag = 1;
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Maximum file count reached\n",__FILE__, __LINE__);
			strcpy( file_path_cpy, file_path );
			strcat( file_path, "/" );
			strcat( file_path, filename );
			remove( file_path );
			strcpy( file_path, file_path_cpy );
			stop_cvr = 1;   // GST termination flag
			//term_flag = 1;  // hrydra termination flag	
			Stop_streaming( config );		  

			pthread_mutex_lock(&mutexfor_threadsync);
			pthread_cond_signal(&cvfor_threadsync);
			pthread_mutex_unlock(&mutexfor_threadsync);


			pthread_mutex_lock(&mutexfor_waitingvideobuf);
			pthread_cond_signal(&cvfor_waitingvideobuf);
			pthread_mutex_unlock(&mutexfor_waitingvideobuf);

			pthread_mutex_lock(&mutex_hasaudio);
			pthread_cond_broadcast(&cv_hasaudio);
			pthread_mutex_unlock(&mutex_hasaudio);
			//sending stop request
			// Stop_streaming( config );
		}

#endif
		if( (vtot_len < ABSOLUTETIME_LENGTH)  && ( vabsolutetime_flag == 1 ) && (stop_cvr==0) && (threadsync == 1) )
		{ 
			sem_post(&s4);
			pthread_mutex_lock(&mutexfor_waitingvideobuf);
			waitingforsig1 = 1;
			pthread_cond_wait(&cvfor_waitingvideobuf, &mutexfor_waitingvideobuf);
			pthread_mutex_unlock(&mutexfor_waitingvideobuf);
			sem_wait(&s4);
		}
		if( (vtot_len >= ABSOLUTETIME_LENGTH ) && ( vabsolutetime_flag == 1 ) && (threadsync == 1) && (stop_cvr==0))
		{
			memcpy(&v_absolutetime,remainingbuf,ABSOLUTETIME_LENGTH);
			memmove(&remainingbuf[0],&remainingbuf[ABSOLUTETIME_LENGTH], vtot_len - ABSOLUTETIME_LENGTH);
			vtot_len = vtot_len - ABSOLUTETIME_LENGTH ;
			vabsolutetime_flag = 0;
			vmapsize_flag = 1;
		}
		if( (vtot_len < MAPLENGTH)  && ( vmapsize_flag == 1 ) && (stop_cvr==0) && (threadsync == 1) )
		{
			sem_post(&s4);
			pthread_mutex_lock(&mutexfor_waitingvideobuf);
			waitingforsig1 = 1;
			pthread_cond_wait(&cvfor_waitingvideobuf, &mutexfor_waitingvideobuf);
			pthread_mutex_unlock(&mutexfor_waitingvideobuf);
			sem_wait(&s4);
		}
		if( ( vtot_len >= MAPLENGTH ) && ( vmapsize_flag == 1) && (threadsync == 1) && (stop_cvr==0) )
		{
			memcpy(&v_mapsize,remainingbuf, MAPLENGTH);
			memmove(&remainingbuf[0],&remainingbuf[MAPLENGTH], vtot_len - MAPLENGTH);
			vtot_len = vtot_len - MAPLENGTH ;
			vmapsize_flag = 0;
			vmapdata_flag = 1;
		} 
		if( (vtot_len < v_mapsize)  && ( vmapdata_flag == 1 ) && (stop_cvr==0) && (threadsync == 1) )
		{
			sem_post(&s4);
			pthread_mutex_lock(&mutexfor_waitingvideobuf);
			waitingforsig1 = 1;
			pthread_cond_wait(&cvfor_waitingvideobuf, &mutexfor_waitingvideobuf);
			pthread_mutex_unlock(&mutexfor_waitingvideobuf);
			sem_wait(&s4);
		}
		if( ( vtot_len >= v_mapsize ) && ( vmapdata_flag == 1) && (threadsync == 1) && (stop_cvr==0) )
		{
			pts_offset += (v_absolutetime/1000000 - prev_vabsolutetime/1000000);
			frame_duration = v_absolutetime/1000000 - prev_vabsolutetime/1000000;
			synccount ++;
			sem_wait(&s1);
			pesbuffer_full =(char * ) realloc( pesbuffer_full,sizeof( char ) *  v_mapsize + map_size);	
			memmove(&pesbuffer_full[map_size],&remainingbuf[0], v_mapsize );
			memmove(&remainingbuf[0],&remainingbuf[v_mapsize], vtot_len - v_mapsize );
			map_size += v_mapsize;
			vtot_len = vtot_len - v_mapsize;
			map_size=video_map(map_size, pesbuffer_full);
			vabsolutetime_flag =1;
			vmapdata_flag =0;
			prev_vabsolutetime = v_absolutetime;
			if( (pts_offset >= AACFRAMELENGTH) && (config.has_audio == 1) ) 
			{
				threadsync = 0;
				if( audconversion_atwait==1)
				{
					audconversion_atwait=0;
					pthread_mutex_lock(&mutexfor_threadsync_1);
					//threadsync = 0;
					pthread_cond_signal(&cvfor_threadsync_1);
					pthread_mutex_unlock(&mutexfor_threadsync_1);

				}
				//threadsync = 0;
			}
			else    {threadsync = 1;}
			sem_post(&s1);
		} 
		sem_post(&s4);
		if( (threadsync == 0) && (config.has_audio == 1) && (stop_cvr==0) )
		{
			pthread_mutex_lock(&mutexfor_threadsync);
			pthread_cond_wait(&cvfor_threadsync, &mutexfor_threadsync);
			pthread_mutex_unlock(&mutexfor_threadsync);
		}
		sleep(0);
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): start_convertvideo thread exited\n",__FILE__, __LINE__);
}

static void *start_convertaacaudio( void *vargp )
{

	static unsigned long long a_mapsize=0;
	static int aabsolutetime_flag = 1;
	static int amapsize_flag = 0;
	static int amapdata_flag = 0;
	pthread_mutex_lock(&mutex_hasaudio);
	pthread_cond_wait(&cv_hasaudio, &mutex_hasaudio);
	pthread_mutex_unlock(&mutex_hasaudio);

	while(stop_cvr == 0)
	{
		if( (threadsync == 1) && (config.has_audio == 1) )
		{
			pthread_mutex_lock(&mutexfor_threadsync_1);
			audconversion_atwait=1;
			pthread_cond_wait(&cvfor_threadsync_1, &mutexfor_threadsync_1);
			pthread_mutex_unlock(&mutexfor_threadsync_1);

		}

		sem_wait(&s5);
		if( (atot_len < ABSOLUTETIME_LENGTH)  && ( aabsolutetime_flag == 1 ) )
		{
			//waitingforsig=1;
			sem_post(&s5);
			pthread_mutex_lock(&mutexfor_waitingaudbuf);
			waitingforsig=1;
			pthread_cond_wait(&cvfor_waitingaudbuf, &mutexfor_waitingaudbuf);
			pthread_mutex_unlock(&mutexfor_waitingaudbuf);
			sem_wait(&s5);
		}

		if( (atot_len >= ABSOLUTETIME_LENGTH ) && ( aabsolutetime_flag == 1 ) && ( threadsync == 0) )
		{
			memcpy(&a_absolutetime,aremainingbuf,ABSOLUTETIME_LENGTH);
			memmove(&aremainingbuf[0],&aremainingbuf[ABSOLUTETIME_LENGTH], atot_len - ABSOLUTETIME_LENGTH);
			a_firstpts = a_absolutetime/1000000;
			atot_len = atot_len - ABSOLUTETIME_LENGTH ;
			aabsolutetime_flag = 0;
			amapsize_flag = 1;
		}
		if( (atot_len < MAPLENGTH) && ( amapsize_flag == 1) )
		{
			//waitingforsig=1;
			sem_post(&s5);
			pthread_mutex_lock(&mutexfor_waitingaudbuf);
			waitingforsig=1;
			pthread_cond_wait(&cvfor_waitingaudbuf, &mutexfor_waitingaudbuf);
			pthread_mutex_unlock(&mutexfor_waitingaudbuf);
			sem_wait(&s5);
		}

		if( ( atot_len >= MAPLENGTH ) && ( amapsize_flag == 1) && ( threadsync == 0))
		{
			memcpy(&a_mapsize,aremainingbuf, MAPLENGTH);
			memmove(&aremainingbuf[0],&aremainingbuf[MAPLENGTH], atot_len - MAPLENGTH);
			atot_len = atot_len - MAPLENGTH ;
			amapsize_flag = 0;
			amapdata_flag = 1;
		}

		if( (atot_len <a_mapsize) && ( amapdata_flag == 1) )
		{
			sem_post(&s5);
			pthread_mutex_lock(&mutexfor_waitingaudbuf);
			waitingforsig=1;
			pthread_cond_wait(&cvfor_waitingaudbuf, &mutexfor_waitingaudbuf);
			pthread_mutex_unlock(&mutexfor_waitingaudbuf);
			sem_wait(&s5);
		}

		if( ( atot_len >= a_mapsize ) && ( amapdata_flag == 1) && ( threadsync == 0)   )
		{   
			aabsolutetime_flag = 1;
			amapdata_flag = 0;
			aud_buffer =( uint8_t * ) realloc( aud_buffer,sizeof( uint8_t ) * a_mapsize + aud_buffer_size);
			if ( aud_buffer == NULL )
			{
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Realloc failed for aud_buffer\n", __FILE__, __LINE__);
			}
			memmove( &aud_buffer[aud_buffer_size], &aremainingbuf[0], a_mapsize);
			aud_buffer_size+=a_mapsize;
			memmove(&aremainingbuf[0],&aremainingbuf[a_mapsize], atot_len - a_mapsize );
			atot_len =atot_len -  a_mapsize ;
			sem_wait(&s1);
			int frame_length=adts_framelen_cal(aud_buffer,aud_buffer_size);
			int length= audio_to_ts_converter(aud_buffer,frame_length,streamtype,0x1f);
			memmove( &aud_buffer[0], &aud_buffer[frame_length], aud_buffer_size-frame_length);
			aud_buffer_size = aud_buffer_size-frame_length;
			pts_offset -= AACFRAMELENGTH ;
			if( pts_offset < AACFRAMELENGTH )
			{
				threadsync =1;
				pthread_mutex_lock(&mutexfor_threadsync);
				pthread_cond_signal(&cvfor_threadsync);
				pthread_mutex_unlock(&mutexfor_threadsync);
			}
		}
		sem_post(&s1);
		sem_post(&s5);
		sleep(0);

	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): convertaacaudio thread exited\n",__FILE__, __LINE__);
}


static void init_crc32()
{
	unsigned int k;

	if(crc32_initialized) return;
	for(unsigned int i = 0; i < 256; i++)
	{
		k = 0;
		for(unsigned int j = (i << 24) | 0x800000; j != 0x80000000; j <<= 1)
		{
			k = (k << 1) ^ (((k ^ j) & 0x80000000) ? 0x04c11db7 : 0);
		}
		crc32_table[i] = k;
	}
	crc32_initialized = 1;

}


static unsigned long get_crc32(unsigned char *data, int size)
{
	int i;
	uint32_t result = 0xffffffff;
	init_crc32();
	for(i = 0; i < size; i++)
	{
		result = (result << 8) ^ crc32_table[(result >> 24) ^ data[i]];
	}

	return result;
}/* End of get_crc32 */



/*AAC to TS encoder function definitions*/
static void get_audio(int audio_format, cam_config &config_data)
{
	GMainLoop *audioloop;
	audioloop = g_main_loop_new(NULL,FALSE);
	char startrequest[100];
	sprintf( startrequest,"http://127.0.0.1:8085/startaudiostream&keyvalue=0&format=%d&dotimestamp=1&", audio_format );

	GstElement *audsouphttpsrc,*audpipeline,*audappsink;

	audpipeline = gst_element_factory_make("pipeline",NULL);
	audappsink = gst_element_factory_make("appsink",NULL);
	audsouphttpsrc = gst_element_factory_make("souphttpsrc",NULL);
	GstBus *audbus;
	g_object_set(G_OBJECT(audappsink),"emit-signals",TRUE,"sync",FALSE,NULL);
	audbus = gst_pipeline_get_bus(GST_PIPELINE(audpipeline));
	gst_bus_add_watch( audbus, ( GstBusFunc ) on_message, ( gpointer ) audioloop );
	g_object_set( G_OBJECT( audsouphttpsrc ), "blocksize",4096 , NULL );//new property
	g_object_set( G_OBJECT( audappsink), "blocksize",4096, NULL );

	if(!audsouphttpsrc)
	{  
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create audsouphttpsrc\n",__FILE__, __LINE__);
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio request URL is : %s\n",__FILE__, __LINE__,startrequest);
	g_object_set( G_OBJECT( audsouphttpsrc ), "location", startrequest, NULL );
	//g_object_set(G_OBJECT(audsouphttpsrc),"location","http://192.168.160.84:8085/startaudiostream&keyvalue=0&format=1&",NULL);
	gst_bin_add_many(GST_BIN(audpipeline),audsouphttpsrc,audappsink,NULL);
	gst_element_link_many(audsouphttpsrc,audappsink,NULL);
	gst_element_set_state(audpipeline,GST_STATE_PLAYING);

	g_signal_connect(audappsink,"new-sample",G_CALLBACK(on_sample_audio),NULL);
	g_main_loop_run(audioloop);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio loop exited\n",__FILE__, __LINE__);
	sem_post(&s3);
}



static gboolean on_sample_audio(GstElement *elt)
{
	static int flag_2 =0;
	GstSample *audsample;
	GstBuffer *buffer;
	GstMapInfo map;
	audsample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
	buffer = gst_sample_get_buffer(audsample);
	gst_buffer_map(buffer,&map,GST_MAP_READ);
	if(flag_2 == 0)
	{
		memcpy(&a_firstpts,map.data,ABSOLUTETIME_LENGTH);
		flag_2 = 1;
		got_ptsdif++;
		if(got_ptsdif == 2)
		{
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Got both audio video PTS\n", __FILE__, __LINE__);
			pthread_mutex_lock(&mutexfor_gotptsdif);
			pthread_cond_broadcast(&cvfor_gotptsdif);
			pthread_mutex_unlock(&mutexfor_gotptsdif);
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): gotptsdif mutex released\n", __FILE__, __LINE__);
		}
	}
	sem_wait(&s5);
	atot_len = map.size + atot_len;       
	aremainingbuf =( uint8_t * ) realloc(aremainingbuf,sizeof( uint8_t ) * (atot_len ));
	memcpy(&aremainingbuf[atot_len - map.size] , map.data,map.size);
	gst_buffer_unmap( buffer, &map );
	gst_sample_unref( audsample );
	sem_post(&s5);
	if( waitingforsig == 1)
	{
		waitingforsig=0;
		pthread_mutex_lock(&mutexfor_waitingaudbuf);
		pthread_cond_signal(&cvfor_waitingaudbuf);
		pthread_mutex_unlock(&mutexfor_waitingaudbuf);
	}
	sleep(0);
	return FALSE;
}

static void *start_getgstaudio( void *vargp )
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): getgstaudio thread started\n",__FILE__, __LINE__);
	pthread_mutex_lock(&mutex_hasaudio);
	pthread_cond_wait(&cv_hasaudio, &mutex_hasaudio);
	pthread_mutex_unlock(&mutex_hasaudio);
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): cv_hasaudio got released\n",__FILE__, __LINE__);
	if( config.has_audio == 1 )
	{
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): cv_hasaudio got released...\n",__FILE__, __LINE__);
		get_audio(1,config);
		free(aremainingbuf);
		free(aud_buffer);
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): getgstaudio thread exited\n",__FILE__, __LINE__);
}


static void audio_pes(int header_selector,int stuffing_byte_length, int pid,uint8_t aac_peshdr[], int peshdr_len, uint8_t data[], int data_len)
{
	static int aud_pes_count=0xff;
	int aud_ts_len=0;
	int i=0;
	aud_pes=( uint8_t * ) malloc( sizeof( uint8_t ) * 188 );
	int PCR_extn = 0;
	uint64_t PCR_base = pts_value;

	if(cc_restflag==1)
	{
		aud_pes_count=0xff;
		cc_restflag=0;
	}

	switch(header_selector) 
	{
		case 1: //with pcr
			aud_pes[0]=0x47;
			aud_pes[1]=0x40; //Set when a PES, PSI, or DVB-MIP packet begins immediately following the header.
			aud_pes[1]|= (unsigned char) ((pid >> 8) & 0x1F);
			aud_pes[2]=(unsigned char) (0xFF & pid);
			aud_pes[3]=0x30; //adaptation field followed by payload
			aud_pes_count++;
			aud_pes[3]|= ( aud_pes_count & 0x0F );
			aud_pes[4]= 0x07;//Adaptation Field Length
			aud_pes[5]=0x50;//set pcr and Random access indicator
			PCR_extn = 0;
			PCR_base = pts_value;
			aud_pes[6] = ( uint8_t ) ( PCR_base >> 25 );
			aud_pes[7] = ( uint8_t ) ( ( PCR_base >> 17 ) & 0xFF );
			aud_pes[8] = ( uint8_t ) ( ( PCR_base >> 9 ) & 0xFF );
			aud_pes[9] = ( uint8_t ) ( ( PCR_base >> 1 ) & 0xFF );
			aud_pes[10]= ( uint8_t ) ( ( ( PCR_base & 0x1 ) << 7 ) | 0x7E | ( PCR_extn >> 8 ) );
			aud_pes[11] = ( uint8_t ) ( PCR_extn >> 1 );
			if(stuffing_byte_length !=0)
			{
				for ( i = 1; i < (stuffing_byte_length ) ; i++ )//stuffing byte includes PCR values.So actual stuffing byte siz                                                                                  e is -7.
				{
					aud_pes[i+11] = 0xFF;
				}
			}
			aud_ts_len=i+12; //ts header length
			break;

		case 2: 
			aud_pes[0]=0x47;
			aud_pes[1]=0x00; //Set when a PES, PSI, or DVB-MIP packet begins immediately following the header.
			aud_pes[1]|= (unsigned char) ((pid >> 8) & 0x1F);
			aud_pes[2]=(unsigned char) (0xFF & pid);
			aud_pes[3]=0x10; //no adaptation field, payload only
			aud_pes_count++;
			aud_pes[3]|= ( aud_pes_count & 0x0F );
			aud_ts_len=4;
			break;

		case 3:
			aud_pes[0]=0x47;
			aud_pes[1]=0x00; //Set when a PES, PSI, or DVB-MIP packet begins immediately following the header.
			aud_pes[1]|= (unsigned char) ((pid >> 8) & 0x1F);
			aud_pes[2]=(unsigned char) (0xFF & pid);
			aud_pes[3]=0x30; //adaptation field followed by payload
			aud_pes_count++;
			aud_pes[3]|= ( aud_pes_count & 0x0F );
			aud_pes[4]= stuffing_byte_length;//Adaptation Field Length
			aud_pes[5]=0x00;
			for ( i = 1; i < (stuffing_byte_length) ; i++ )
			{		
				aud_pes[i+5] = 0xFF;
			}		
			if(stuffing_byte_length==1)
				aud_ts_len=6;
			else
				aud_ts_len=i+5; //ts header length
			break;
	}
	sem_wait(&s2);
	if(NULL==pat_pmt_ts_file)
	{
		pat_pmt_ts_file = fopen( file_path, "a+" );
		if(NULL==pat_pmt_ts_file)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Failed to create %s chunk\n",__FILE__, __LINE__,filename);
		}
	} 

	if ( peshdr_len > 0 )
	{
		if(aud_ts_len+peshdr_len > 188)
		{
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): TS packet is greater than 188\n", __FILE__, __LINE__);
		}//for debugging
		memcpy( &( aud_pes[aud_ts_len] ),aac_peshdr, peshdr_len );
	}

	if ( data_len > 0 )
	{	
		int total_length = aud_ts_len + peshdr_len+data_len;
		if(aud_ts_len + peshdr_len+data_len > 188)
		{
			assert(0);
		}
		memcpy( &( aud_pes[aud_ts_len + peshdr_len] ), data, data_len );
		int err = fwrite( aud_pes, sizeof( uint8_t ), 188, pat_pmt_ts_file );
	}

	sem_post(&s2);
	free(aac_peshdr);
	free(aud_pes);

}

static int audio_to_ts_converter(uint8_t *audiobuf, int frame_len, int streamtype, uint32_t pid)
{
	int audbuf_len=frame_len;
	int pes_hdr_length=0;
	int audiobuf_position=0;
	int current_framelen=0;
	int pes_pkt_len=0;
	int stuffing_byte=0;

	while(audbuf_len>0)
	{
		pes_pkt_len=frame_len;
		aacaud_pts_value += dd;
		//	dd= 11520;//(1024 * 90000) / 8000;
		dd= 90 * AACFRAMELENGTH ;
		if(audbuf_len < 162)
		{
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Audio frame length is less than 162...\n",__FILE__, __LINE__); 
			stuffing_byte=188 - (audbuf_len + 26);
			pes_hdr_length=audio_PES_header(1,aacaud_pts_value,streamtype, audbuf_len);
			audio_pes(1,stuffing_byte, pid, aac_peshdr, pes_hdr_length, &(audiobuf[0] ),audbuf_len);
			audbuf_len=0;
			stuffing_byte=0;
		}
		else
		{	
			pes_hdr_length=audio_PES_header(1,aacaud_pts_value,streamtype, pes_pkt_len);
			audio_pes(1,0,pid, aac_peshdr, pes_hdr_length, &( audiobuf[audiobuf_position] ), 188- (pes_hdr_length+12)); 
			audiobuf_position = audiobuf_position + ( 188- (pes_hdr_length+12) );
			audbuf_len=audbuf_len- (188-(pes_hdr_length+12) );
			current_framelen= current_framelen + (188- (pes_hdr_length+12) );

			while( (audbuf_len >= 184) && (current_framelen <= (pes_pkt_len-184) ))
			{
				pes_hdr_length=audio_PES_header(0, 0,streamtype, pes_pkt_len);
				audio_pes(2,0,pid, aac_peshdr, pes_hdr_length, &( audiobuf[audiobuf_position] ), 188- (pes_hdr_length+4)); 
				audiobuf_position = audiobuf_position + ( 188- (pes_hdr_length+4) ); 
				audbuf_len=audbuf_len- (188- ( pes_hdr_length+4 ) );
				current_framelen= current_framelen + (188- (pes_hdr_length+4 ) );
			}
			if( audbuf_len < 184 && audbuf_len > 0 )
			{
				stuffing_byte= (183 - (pes_pkt_len - current_framelen)); //
				if(stuffing_byte==0)//Zero means data length is 183.
				{
					RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Stuffing byte is 0\n",__FILE__, __LINE__);	
					stuffing_byte=183-92;//spliting 183 bytes into two seperate pes
					pes_hdr_length=audio_PES_header(0,0,streamtype, pes_pkt_len);
					audio_pes(3,stuffing_byte, pid, aac_peshdr, pes_hdr_length, &(audiobuf[audiobuf_position] ),92 ); 
					audiobuf_position=audiobuf_position + 92; 
					audbuf_len=audbuf_len-92;//(188-pes_hdr_length+4);
					stuffing_byte=183-91;//spliting 183 bytes into to seperate pes
					pes_hdr_length=audio_PES_header(0,0,streamtype, pes_pkt_len);
					audio_pes(3,stuffing_byte, pid, aac_peshdr, pes_hdr_length, &(audiobuf[audiobuf_position] ),91 );     
					audiobuf_position=audiobuf_position + 91;                
					audbuf_len=audbuf_len-91;//(188-pes_hdr_length+4);
					current_framelen=0;
				}
				else
				{
					pes_hdr_length=audio_PES_header(0,0,streamtype, pes_pkt_len);
					audio_pes(3,stuffing_byte,pid, aac_peshdr, pes_hdr_length, &( audiobuf[audiobuf_position] ), 
							pes_pkt_len-current_framelen); 
					audiobuf_position=audiobuf_position + (pes_pkt_len-current_framelen) ; 
					audbuf_len=audbuf_len- (pes_pkt_len-current_framelen)-8;//(188-pes_hdr_length+4);
					current_framelen=0;
				}
			}
		}	
		return(0);	 
	}

}

static void adts_parse(uint8_t *hdr, int audiobuf_position)
{
	aacconfig.syncword = (hdr[audiobuf_position++] << 4) | (hdr[audiobuf_position] >> 4);
	if (aacconfig.syncword != 0b111111111111)
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Invalid syncword!!!\n",__FILE__, __LINE__);


	aacconfig.ID                 = (hdr[audiobuf_position] >> 3) & 0b1   ;
	aacconfig.layer              = (hdr[audiobuf_position] >> 1) & 0b11  ;
	aacconfig.protection_absent  = (hdr[audiobuf_position++]     ) & 0b1   ;
	aacconfig.profile            = (hdr[audiobuf_position] >> 6) & 0b11  ;
	aacconfig.sampling_freq_idx  = (hdr[audiobuf_position] >> 2) & 0b1111;
	aacconfig.private_bit        = (hdr[audiobuf_position] >> 1) & 0b1   ;
	aacconfig.channel_cfg        = ((hdr[audiobuf_position++] & 0b1) << 2) | (hdr[3] >> 6) ;
	aacconfig.original_copy      = (hdr[audiobuf_position] >> 5) & 0b1 ;
	aacconfig.home               = (hdr[audiobuf_position] >> 4) & 0b1 ;
	aacconfig.copyright_id_bit   = (hdr[audiobuf_position] >> 3) & 0b1 ;
	aacconfig.copyright_id_start = (hdr[audiobuf_position] >> 2) & 0b1 ;
	aacconfig.frame_length       = ((hdr[audiobuf_position++] & 0b11) << 11) | (hdr[audiobuf_position++] << 3) | (hdr[audiobuf_position] >> 5) ;
	aacconfig.adts_buf_fullness  = ((hdr[audiobuf_position++] & 0b11111) << 6) | (hdr[audiobuf_position] >> 2) ;
	aacconfig.num_rawdata_blocks = (hdr[audiobuf_position]     ) & 0b11  ;
}

static int adts_framelen_cal(uint8_t *hdr, int total_len)
{
	aacaud_framecount=0;
	int frame_length=0;
	int i=0;
	if( hdr[i]==0xff && hdr[i+1]==0xf1)
	{
		aacaud_framecount++;
	}
	else
	{
		RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Sync byte error!!!\n",__FILE__, __LINE__);
	}
	frame_length += ((hdr[i+3] & 0b11) << 11) | (hdr[i+4] << 3) | (hdr[i+5] >> 5);
	if(frame_length >= total_len) 
		return(frame_length);
}



static int audio_PES_header( int with_PTS, uint64_t pts_value, int streamtype, int pes_pckt_len )
{
	pes_pckt_len+=8;
	int peshdr_len=0;
	int pts1, pts2, pts3;
	int guard_bits = 2;
	aac_peshdr = ( uint8_t * ) malloc( sizeof( uint8_t ) * 14 );    
	if ( with_PTS )
	{
		aac_peshdr[0] = 0x00;
		aac_peshdr[1] = 0x00;
		aac_peshdr[2] = 0x01;
		aac_peshdr[3] = streamtype;      //stream_id; 0xe0 for raw h264 video
		aac_peshdr[4] = ((pes_pckt_len & 0xFF00) >> 8);
		aac_peshdr[5] = ((pes_pckt_len & 0x00FF));
		aac_peshdr[6] = 0x80;
		aac_peshdr[7] = 0x80;
		aac_peshdr[8] = 0x05;
		//encode_pts_dts(&(PES_hdr[9]),2,pts);
		pts1 = ( int ) ( ( pts_value >> 30 ) & 0x07 );
		pts2 = ( int ) ( ( pts_value >> 15 ) & 0x7FFF );
		pts3 = ( int ) ( pts_value & 0x7FFF );
		aac_peshdr[9] = ( guard_bits << 4 ) | ( pts1 << 1 ) | 0x01;
		aac_peshdr[10] = ( pts2 & 0x7F80 ) >> 7;
		aac_peshdr[11] = ( ( pts2 & 0x007F ) << 1 ) | 0x01;
		aac_peshdr[12] = ( pts3 & 0x7F80 ) >> 7;
		aac_peshdr[13] = ( ( pts3 & 0x007F ) << 1 ) | 0x01;
		peshdr_len=14;
		/* PES_hdr[14] = 0x00;
		   PES_hdr[15] = 0x00;
		   PES_hdr[16] = 0x00;
		   PES_hdr[17] = 0x01;
		   PES_hdr[18] = 0x09;
		   PES_hdr[19] = 0xf0; 
		 */
	} 
	return (peshdr_len);
}

static void gettime()
{
	timeval tim;
	gettimeofday(&tim,NULL);
	double t1 = tim.tv_usec;
}

static void pts_adjust()
{
	pts_offset =0;
	a_firstpts =a_firstpts/1000000;// CONVERTING TO MSESCONDS
	v_firstpts = v_absolutetime/1000000;
	if(a_firstpts > v_firstpts)
	{
		aacaud_pts_value =(uint64_t) (( (a_firstpts - v_firstpts) * 90.00) + pts_value  );
		pts_offset-= (a_firstpts - v_firstpts);
	}
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): audio PTS:%llu video PTS:%llu aacaud_pts_value:%llu pts_value:%llu,pts_offset:%d\n",__FILE__, __LINE__,a_firstpts,v_firstpts,aacaud_pts_value,pts_value,pts_offset);
} 

bool IsGSTEnabledInRFC(char* file,char* name )
{
	RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Reading RFC values\n",__FILE__, __LINE__);
	char value[MAX_SIZE] = {0};
	if( RDKC_SUCCESS == IsRFCFileAvailable(file) ) {
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): RFC file is available\n",__FILE__, __LINE__);
		if( RDKC_SUCCESS == GetValueFromRFCFile(file, name, value) ) {
			RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): Able to get  value from file\n",__FILE__, __LINE__);
			if( strcmp(value, RDKC_TRUE) == 0 ) {
				RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): RFC is set inside the file\n",__FILE__, __LINE__);
				return true;
			}
			else {
				RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): RFC is not set inside the file\n",__FILE__, __LINE__);
				return false;
			}
		}
		else {
			RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.CVR","%s(%d): Unable to get  value from the file\n",__FILE__, __LINE__);
			return false;
		}
	}
	else {
		RDK_LOG( RDK_LOG_INFO,"LOG.RDK.CVR","%s(%d): RFC file is not available\n",__FILE__, __LINE__);
		return false;
	}
}

/**********END***********/

