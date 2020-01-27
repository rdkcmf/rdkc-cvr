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
#ifndef __CVR_GST_H__
#define __CVR_GST_H__

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h> 
#include <unistd.h>
#include<gst/gst.h>
#include<stdio.h>
#include<gst/app/gstappsink.h>
#include<iostream>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include<sys/time.h>
#include <assert.h>
#include <sys/syscall.h>
#include "RFCCommon.h"
#include "rdk_debug.h"
#include "cvr.h"
#include "cvr_daemon.h"


#define MPEG_PKT_SIZE 188
#define TS_PACKET_SIZE 188
#define PIDTYPE_PMT 0xFF00
#define PIDTYPE_PCR 0xFF01
#define IS_AUDIO_STREAM_ID(id)  ((id)==0xBD || ((id) >= 0xC0 && (id) <= 0xDF))
#define IS_VIDEO_STREAM_ID(id)  ((id) >= 0xE0 && (id) <= 0xEF)
#define MAX_PTS_VALUE 0x1FFFFFFFFLL
#define  TRUE 1


char *sys_time=NULL;
int bitrate=0;
int tsheader_length=0;  //Additional ts header length.
int buffer_position=0;
int peswithpts=TRUE; // true means,we have to send PES with pts and PCR
typedef uint8_t  byte;
FILE *pat_pmt_ts_file = NULL;
int pes_count =0xff;
int pat_count =0xff;
int pmt_count =0xff;
int sdt_count =0xff;
int cc_restflag=1;
unsigned char pat_version=0x01;
unsigned short programNumber=01;
unsigned short pmt_pid=0x1000;
uint64_t pts_value=0;//121650;
unsigned char* sdtbuffer;
int sdtbufsize;
int patbufsize;
unsigned char* patbuffer;
int pmtbufsize;
unsigned char* pmtbuffer;
int result;
FILE * TFile;
int max_file_count_flag=0;

int PES_hdr_len;
int next_iframe=0;
int create_chunk=0;//This flag will set if the stream length is greater than 'CHUNK_LENGTH'.
int flag_1=0;
int count_1=0;
int padding_pstn=0;
int full_pes_ok=0;
uint8_t *pesbuffer=NULL;
uint8_t *iframebuf = ( uint8_t * ) malloc( sizeof( uint8_t ) * 4096 );
int iframebuf_len =0;
bool withpcr=0;
int pesbuffer_len=0;
int exit_flag=FALSE;
int pesbuffer_len_cpy=0;
int buffer_full_position=0;

static unsigned long crc32_table[256];
static int crc32_initialized = 0;
//int CHUNK_LENGTH=20;
/*struct cam_pid_desc
{
       uint pid;
        uint streamType;
};
cam_pid_desc* ppiddesc;*/
#define DEFLT_KEYVALUE 7
#define DEFLT_FILECOUNT -1
#define DEFLT_FRAMERATE 10.00
#define DEFLT_CHUNKLENGTH 15
#define DEFLT_BITRATE 100
struct cam_config
{
int keyvalue;
int max_file_count;
float frame_rate;
int CHUNK_LENGTH;
int has_audio;
int bitrate;
}config = {DEFLT_KEYVALUE,DEFLT_FILECOUNT,DEFLT_FRAMERATE,DEFLT_CHUNKLENGTH,0,DEFLT_BITRATE}; //Assigning defualt values

uint32_t numPidDesc=3;
uint16_t pcr_pid=0x01FF;
unsigned char pmt_version=0x01;
static unsigned char* sdt_packet( int * dataSize);
int frame_count=5;
int iframe=0;
int total_mapsize=0;

/*vraiable declaration for AAC to TS encoder*/

struct aac_config
{
int syncword	;
int ID			;
int layer		;
int protection_absent ;
int profile	;
int home;
int sampling_freq_idx ;
int private_bit		;
int channel_cfg		;
int original_copy 	;
int copyright_id_bit  ;
int copyright_id_start;
int frame_length      ;
int adts_buf_fullness ;
int num_rawdata_blocks;
}aacconfig;

int prev_frm_len=0;
int aud_buffer_size=0;
int streamtype=0xc0; // ac3:bd
uint8_t *aud_buffer = ( uint8_t * ) malloc( sizeof( uint8_t ) * ( 200 ) );
uint8_t *aud_pes    =NULL;
uint8_t *aac_peshdr = NULL;
int aacaud_framecount=0;
uint64_t aacaud_pts_value=0;//121650;
uint64_t dd=11520;//(1024 *90000)/8000;

//#define CVR_AUDIO "RFC_DATA_RDKC_GST_CVR_AUDIO"
//#define RFC_CVR ".RFC_RDKC_GST.ini"
//#define GST   "RFC_ENABLE_RDKC_GST"
#define MAX_CONFIG_VARIABLE_LEN 20
#define CONFIG_LINE_BUFFER_SIZE 100
#define ABSOLUTETIME_LENGTH 8
#define MAPLENGTH 4
#define AACFRAMELENGTH  128
char file_path[50]="tmp/cvr";//path of the chunk files.
char file_path_cpy[50];
char filename[50];
char prev_filename[50];
int f_seq = 0;
static volatile unsigned long long map_size = 0;
uint8_t *TS_packet = ( uint8_t * ) malloc( sizeof( uint8_t ) * 188 );
uint8_t *PES_hdr = ( uint8_t * ) malloc( sizeof( uint8_t ) * 29 );
char *pesbuffer_full = ( char * ) malloc( sizeof( char ) * ( 4096 ) );
uint8_t *remainingbuf = ( uint8_t * ) malloc( sizeof( uint8_t ) * 8 );
uint8_t *aremainingbuf = ( uint8_t * ) malloc( sizeof( uint8_t ) * 8 );
int audiook =0;
static unsigned long long  atot_len = 0;
static volatile int threadsync=1;
volatile int req_audthread = 1;
static unsigned long long v_absolutetime =0;
static unsigned long long prev_vabsolutetime = 0;
unsigned int frame_duration =0;//in milliseconds
static unsigned long long a_absolutetime =0;
unsigned long long a_firstpts =0;
unsigned long long v_firstpts =0;
static signed  long long  vtot_len = 0;
int got_ptsdif =0;
uint64_t video_initial_pts =0;
uint64_t aac_initial_pts = 0;
volatile int stop_cvr = 0;
int  pts_offset = 40;//assuming first frame length
volatile int waitingforsig=0;
volatile int waitingforsig1=0;
volatile int  audconversion_atwait=0;
volatile int videoreq_retry=0;
volatile int videoreq_success =0;

pthread_t getgstvideo;
pthread_t getgstaudio ;
pthread_t convert_video;
pthread_t convert_aacauduio;
sem_t s1; //to sync audio and video conversion process
sem_t s2; //to sync the file write operation
sem_t s3;
sem_t s4;
sem_t s5;

pthread_attr_t attr;

pthread_cond_t cv_hasaudio;
pthread_mutex_t mutex_hasaudio;

pthread_cond_t cvfor_threadsync;
pthread_mutex_t mutexfor_threadsync;

pthread_cond_t cvfor_threadsync_1;
pthread_mutex_t mutexfor_threadsync_1;

pthread_cond_t cvfor_waitingaudbuf;
pthread_mutex_t mutexfor_waitingaudbuf;

pthread_cond_t cvfor_waitingvideobuf;
pthread_mutex_t mutexfor_waitingvideobuf;

pthread_cond_t cvfor_gotptsdif;
pthread_mutex_t mutexfor_gotptsdif;


/*End of variable declaration*/

/*function declaratioon of h264 to TS encoder*/
void pes();
void pat();
void pmt();
void sdt();
void pdt();
void ts_converter(uint8_t *pesbuffer,int pesbuffer_len,int with_pcr, int streamtype, uint32_t pid);
char * current_time ();
void pat_packet(unsigned char version, unsigned short prognum, unsigned short pmtpid, unsigned char *pat, int size, int &count);
extern void PES_header(int with_PTS,uint64_t pts, int streamtype, int data_len );
static int write_TS_packet_parts(uint8_t TS_packet[TS_PACKET_SIZE],int TS_hdr_len,uint8_t pes_hdr[],int pes_hdr_len,uint8_t data[],int data_len,uint32_t pid, int with_PTS, int with_PCR);				   
extern void encode_pts_dts(uint8_t data[],int guard_bits,uint64_t value);
static unsigned char* form_pmt_packet( int * dataSize);		   
//static unsigned char* form_pat_packet( int * pSize);
void write_chunk();
void file_append();
void read_config();
int read_int_from_config_line(char* config_line);
void read_string_from_config_line(char* config_line);
int check_folder_exist(char * f_path);
static void mkdir_p(const char *dir);
void send_request(char *request,char *response);
void Stop_streaming(cam_config &config_data);
gboolean fetch_response(GstElement *elt, gpointer user_data);
void start_videostreaming(cam_config &config_data);
int video_map(unsigned int buffer_full_length,char *pesbuffer_full);
void *start_getgstvideo(void *vargp);
void *start_convertvideo( void *vargp );
void *start_convertaacaudio( void *vargp );
static unsigned long get_crc32(unsigned char *data, int size);
static void init_crc32();

gboolean on_sample_audio(GstElement *elt);
gboolean on_message_audio(GstBus *bus,GstMessage *message);
void get_audio(int audio_format, cam_config &config_data);
void *start_getgstaudio( void *vargp );

/*AAC to Ts encoder function declaration*/
int audio_PES_header( int with_PTS, uint64_t pts_value, int streamtype, int pes_pckt_len );
void audio_pes(int header_selector,int stuffing_byte_length, int pid,uint8_t PES_hdr[], int peshdr_len, uint8_t data[], int data_len);
int audio_to_ts_converter(uint8_t *audiobuf, int frame_len, int streamtype, uint32_t pid);
int adts_framelen_cal(uint8_t *hdr, int total_len);
void adts_parse(uint8_t *hdr, int audiobuf_position);
void get_audio(int audio_format, cam_config &config_data);
/*end of function declaration*/
void pts_adjust();
void gettime();

#endif
