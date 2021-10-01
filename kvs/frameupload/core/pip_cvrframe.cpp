/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the License);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/***** HEADER FILE *****/

#ifdef ENABLE_CVR_WITH_PIPEWIRE

#include "pip_cvrframe.h"

#include "spa/param/video/format-utils.h"
#include "spa/debug/types.h"
#include "spa/param/video/type-info.h"
#include "pipewire/pipewire.h"

#include <netinet/in.h>

/***** MACROS *****/
#define RDKC_FAILURE            -1
#define RDKC_SUCCESS            0
#define MAX_NAL_SIZE            30

#define PORT                8090
#define MAXLINE             30
#define MAX_QUEUESIZE       3
#define QUEUE_FRAME_SIZE    10

/***** Global Variable Declaration *****/
pthread_t       pip_cvrgetFrame;
pthread_mutex_t pip_cvrqueueLock;

guint64 pip_cvrframe_timestamp  = 0;
guint8* pip_cvrframe_buffer     = NULL;
guint32 pip_cvrframe_size	= 0;

volatile static int cvrpushIndex = 0;

static bool FrameDebug(false);


int pip_cvrbuffer_width = 1280;
int pip_cvrbuffer_height  = 720;

int pip_cvrstreamId       = 4;
int pip_cvrstream_type    = 1;

char acvrfilename[50] = { 0 };

/***** Structure Declaration *****/

struct cvr_data {
        struct pw_main_loop *loop;
        struct pw_stream *stream;
        struct spa_video_info format;
};

typedef struct
{
    guint8* buffer;
    guint64  timestamp;
    unsigned long long size;
}CVR_BUFFERQUEUE;  //same struct definition

CVR_BUFFERQUEUE pip_cvr_bufferqueue[MAX_QUEUESIZE];

/***** PROTOTYPE *****/
int PIP_CVR_pushBuffer( unsigned long long frameSize, guint8 * frame, guint64 timestamp );
int PIP_CVR_popBuffer( PIP_CVR_FrameInfo * frame_info );
void *PIP_CVR_start_Video_Streaming( void *vargp );
static void PIP_CVR_on_process(void *userdata);
static void PIP_CVR_on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param);
void PIP_CVR_current_time(  char *sys_time);

/***** Function Definition *****/

/* {{{ PIP_AllocateBufferForQueue() */
int PIP_AllocateBufferForQueue()
{
    int i=0;

    for(i=0; i<MAX_QUEUESIZE; i++)
    {
        pip_cvr_bufferqueue[i].buffer = ( guint8 * ) ( malloc( QUEUE_FRAME_SIZE ) );

	if( NULL == pip_cvr_bufferqueue[i].buffer )
	{
            printf( "%s(%d): Error : memory allocation failed \n", __FILE__, __LINE__ );
            return RDKC_FAILURE;
	}
    }

    return RDKC_SUCCESS;
}
/* }}} */

/* {{{ PIP_CVR_pushBuffer() */
int PIP_CVR_pushBuffer( unsigned long long frameSize, guint8 * frame, guint64 timestamp )
{
    if ( pthread_mutex_lock( &pip_cvrqueueLock ) != 0 )
    {
        return RDKC_FAILURE;
    }

    if ( cvrpushIndex >= MAX_QUEUESIZE )
    {
        if ( FrameDebug )
            printf( "%s(%d): Maximum queue size reached \n", __FILE__,
                   __LINE__ );
        cvrpushIndex = 0;
    }

    pip_cvr_bufferqueue[cvrpushIndex].buffer = ( guint8 * ) ( realloc( pip_cvr_bufferqueue[cvrpushIndex].buffer , frameSize ) );
    memcpy( pip_cvr_bufferqueue[cvrpushIndex].buffer, frame, frameSize );

    pip_cvr_bufferqueue[cvrpushIndex].timestamp = timestamp;
    pip_cvr_bufferqueue[cvrpushIndex].size = frameSize;
    cvrpushIndex++;
    pthread_mutex_unlock( &pip_cvrqueueLock );

    return RDKC_SUCCESS;
}
/* }}} */

/* {{{ PIP_CVR_popBuffer() */
int PIP_CVR_popBuffer( PIP_CVR_FrameInfo * frame_info )
{
    static int popIndex = 0;

    if ( pthread_mutex_lock( &pip_cvrqueueLock ) != 0 )
    {
        printf( "%s(%d): Error : mutex lock failed \n", __FILE__, __LINE__ );
        return RDKC_FAILURE;
    }

    if ( cvrpushIndex > 0 )
    {

        frame_info->frame_size = pip_cvr_bufferqueue[popIndex].size;
        frame_info->frame_timestamp = pip_cvr_bufferqueue[popIndex].timestamp;

        frame_info->frame_ptr = pip_cvr_bufferqueue[popIndex].buffer;
    }
    else
    {
        if ( FrameDebug )
            printf( "%s(%d): Queue is empty \n", __FILE__, __LINE__ );
    }

    pthread_mutex_unlock( &pip_cvrqueueLock );

    return RDKC_SUCCESS;
}
/* }}} */

/* {{{ PIP_CVR_deleteBuffer() */
int PIP_CVR_deleteBuffer(  )
{
    if ( pthread_mutex_lock( &pip_cvrqueueLock ) != 0 )
    {
        printf( "%s(%d): Mutex lock failed \n", __FILE__, __LINE__ );
        return RDKC_FAILURE;
    }

    int i;

    if ( 1 == cvrpushIndex )
    {
        cvrpushIndex--;
    }
    else if ( cvrpushIndex > 1 )
    {
        for ( i = 0; i < ( cvrpushIndex - 1 ); i++ )
        {
            pip_cvr_bufferqueue[i].buffer =
                ( guint8* ) ( realloc( pip_cvr_bufferqueue[i].buffer,
                                 pip_cvr_bufferqueue[i + 1].size ) );

            memcpy( pip_cvr_bufferqueue[i].buffer, pip_cvr_bufferqueue[i + 1].buffer,
                    pip_cvr_bufferqueue[i + 1].size );
            pip_cvr_bufferqueue[i].timestamp = pip_cvr_bufferqueue[i + 1].timestamp;
            pip_cvr_bufferqueue[i].size = pip_cvr_bufferqueue[i + 1].size;
        }

        cvrpushIndex--;
    }
    else
    {
        if ( FrameDebug )
            printf( "%s(%d): Nothing to delete \n", __FILE__, __LINE__ );
    }
    pthread_mutex_unlock( &pip_cvrqueueLock );

    return RDKC_SUCCESS;
}
/* }}} */

/* {{{ PIP_CVR_ReadFrameData() */
int PIP_CVR_ReadFrameData( PIP_CVR_FrameInfo *frame_info )
{
        int ret=0;

        if(FrameDebug) 
	printf("%s(%d): Entering pip_ReadFrameData() fn \n",__FILE__, __LINE__);

        if( cvrpushIndex > 0 )
        {
                ret = PIP_CVR_popBuffer(frame_info );

                if ( RDKC_SUCCESS != ret)
                {
                   printf( "%s(%d): Failed to read data from queue \n",__FILE__, __LINE__);
                }

                frame_info->stream_id = pip_cvrstreamId;
                frame_info->stream_type = pip_cvrstream_type;  // Video or Audio frame
                frame_info->width = pip_cvrbuffer_width;
                frame_info->height = pip_cvrbuffer_height;

                if(FrameDebug) 
			printf("%s(%d): RMS frame data Read \n",__FILE__, __LINE__);
        }
        else
        {
                return 1; // No Frame data
        }

        return 0;
}
/* }}} */

/* {{{ PIP_CVR_InitFrame() */
int PIP_CVR_InitFrame()
{
	int returnval = 0;
	
	if(FrameDebug)
        printf("%s(%d): Entering Pipewire based rmsframe enable...\n", __FILE__, __LINE__);

        pip_cvrframe_buffer = ( guint8 * ) ( malloc( MAX_NAL_SIZE ) );

        if(pip_cvrframe_buffer == NULL)
        {
                printf("%s(%d): Malloc for frameBuffer failed...\n", __FILE__, __LINE__);
                return -1;
        }

        pw_init(NULL, NULL);

        if(pthread_mutex_init(&pip_cvrqueueLock, NULL) != 0)
        {
                printf("%s(%d): Mutex init for queue failed...\n", __FILE__, __LINE__);
                free (pip_cvrframe_buffer);
                return -1;
        }

	returnval = PIP_AllocateBufferForQueue();

	if(returnval != 0)
        {
		return -1;
	}

        returnval = pthread_create( &pip_cvrgetFrame, NULL, PIP_CVR_start_Video_Streaming, NULL );

        if( 0 != returnval )  
        {
                printf("%s(%d): Failed to create start_Streaming thread,error_code: %d\n",
                        __FILE__, __LINE__,returnval);
                pthread_mutex_destroy(&pip_cvrqueueLock);
                free (pip_cvrframe_buffer);
                return -1;
        }

        if(FrameDebug) printf("%s(%d): Exiting Pipewire based rmsframe enabled...\n", __FILE__, __LINE__);

	return 0;
}
/* }}} */

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .param_changed = PIP_CVR_on_param_changed,
        .process = PIP_CVR_on_process,
};

/* {{{ PIP_CVR_start_Video_Streaming() */
void *PIP_CVR_start_Video_Streaming( void *vargp )
{
        struct cvr_data cvr_data = { 0, };
        const struct spa_pod *params[1];
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        char sys_time[25] = { 0 };

        PIP_CVR_current_time( sys_time );

        sprintf( acvrfilename, "/tmp/pipewire%s.h264", &sys_time );

        if(FrameDebug) 	
	printf("%s(%d): Entering rmsframe start streaming thread...\n",
                __FILE__, __LINE__);

        cvr_data.loop = pw_main_loop_new(NULL);

        cvr_data.stream = pw_stream_new_simple(
                                        pw_main_loop_get_loop(cvr_data.loop),
                                        "video-capture",
                                        pw_properties_new(
                                  	PW_KEY_MEDIA_TYPE, "Video",
                                        PW_KEY_MEDIA_CATEGORY, "Capture",
                                        PW_KEY_MEDIA_ROLE, "Camera",
                                        NULL),
                                        &stream_events,
                                        &cvr_data);


	params[0] = spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_Format,   SPA_PARAM_EnumFormat,
                        SPA_FORMAT_mediaType,     SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype,  SPA_POD_Id(SPA_MEDIA_SUBTYPE_h264),
                        SPA_FORMAT_VIDEO_format,  SPA_POD_Id(SPA_VIDEO_FORMAT_ENCODED),
                        SPA_FORMAT_VIDEO_size,    SPA_POD_Rectangle(&SPA_RECTANGLE(pip_cvrbuffer_width, pip_cvrbuffer_height)),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                                              &SPA_FRACTION(25, 1),
                                                              &SPA_FRACTION(0, 1),
                                                              &SPA_FRACTION(1000, 1)));

        pw_stream_connect(cvr_data.stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1);

        pw_main_loop_run(cvr_data.loop);

        pw_stream_destroy(cvr_data.stream);
        pw_main_loop_destroy(cvr_data.loop);

        return 0;
}
/* }}} */

/* {{{ PIP_CVR_on_process() */
static void PIP_CVR_on_process(void *userdata)
{
        struct cvr_data *cvr_data = userdata;
        struct pw_buffer *b;
        struct spa_buffer *buf;
	struct timeb timer_msec;
        int    ret=0;


        if ((b = pw_stream_dequeue_buffer(cvr_data->stream)) == NULL) {
            pw_log_warn("out of buffers: %m");
            return;
        }

        buf = b->buffer;

        if (buf->datas[0].data == NULL)
            return;

        if (!ftime(&timer_msec))
        {
            pip_cvrframe_timestamp = ((long long int) timer_msec.time) * 1000ll +
                                    (long long int) timer_msec.millitm;
        }
        else {
            pip_cvrframe_timestamp = -1;
        }

#if 0
	FILE* pFile;

        if( '\0' != acvrfilename[0] )
        {
	    pFile = fopen(acvrfilename, "a+");

	    fwrite(buf->datas[0].data,buf->datas[0].chunk->size,1,pFile);

	    fclose(pFile);
        }
#endif

        pip_cvrframe_size = buf->datas[0].chunk->size;
        pip_cvrframe_buffer = (guint8 *)realloc(pip_cvrframe_buffer, sizeof(guint8)* pip_cvrframe_size);

        memcpy(pip_cvrframe_buffer, buf->datas[0].data, pip_cvrframe_size );

	ret = PIP_CVR_pushBuffer( pip_cvrframe_size, pip_cvrframe_buffer , pip_cvrframe_timestamp );

        if ( RDKC_SUCCESS != ret)
        {
            printf( "%s(%d): Pushing to the queue failed \n",__FILE__, __LINE__);
        }

          pw_stream_queue_buffer(cvr_data->stream, b);

	  CVR_Mutex_unlock();
}
/* }}} */

/* {{{ PIP_CVR_on_param_changed() */
static void PIP_CVR_on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
        struct cvr_data *cvr_data = userdata;
        struct pw_stream *stream = cvr_data->stream;
        uint8_t params_buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
        const struct spa_pod *params[5];

        if (param == NULL || id != SPA_PARAM_Format)
                return;

        if (spa_format_parse(param,
                        &cvr_data->format.media_type,
                        &cvr_data->format.media_subtype) < 0)
                return;

        if (cvr_data->format.media_type != SPA_MEDIA_TYPE_video )
                return;

        if( cvr_data->format.media_subtype == SPA_MEDIA_SUBTYPE_raw )
        {
            if( spa_format_video_raw_parse(param, &cvr_data->format.info.raw) >= 0 )
            {
                printf("got video format:\n");
                printf("  format: %d (%s)\n", cvr_data->format.info.raw.format,
                        spa_debug_type_find_name(spa_type_video_format,
                                cvr_data->format.info.raw.format));
                printf("  size: %dx%d\n", cvr_data->format.info.raw.size.width,
                        cvr_data->format.info.raw.size.height);
                printf("  framerate: %d/%d\n", cvr_data->format.info.raw.framerate.num,
                        cvr_data->format.info.raw.framerate.denom);
            }
        }

	       if( cvr_data->format.media_subtype == SPA_MEDIA_SUBTYPE_h264 )
        {
            if( spa_format_video_h264_parse(param, &cvr_data->format.info.h264) >= 0 )
            {
                printf("got video format: H264\n");
/*                printf("  format: %d (%s)\n", cvr_data->format.info.h264.stream_format,
                        spa_debug_type_find_name(spa_type_video_format,
                                cvr_data->format.info.h264.stream_format));*/
                printf("  size: %dx%d\n", cvr_data->format.info.h264.size.width,
                        cvr_data->format.info.h264.size.height);
                printf("  framerate: %d/%d\n", cvr_data->format.info.h264.framerate.num,
                        cvr_data->format.info.h264.framerate.denom);
            }
        }
               /* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
         * number, stride etc of the buffers */

        params[0] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr)));

                /* we are done */
        pw_stream_update_params(stream, params, 1);

}
/* }}} */

/* {{{ PIP_CVR_current_time() */
void PIP_CVR_current_time(  char *sys_time)
{
    time_t time_now;
    struct tm *timeinfo;

    if ( NULL != sys_time )
    {
        time( &time_now );

        timeinfo = localtime( &time_now );

        strftime( sys_time, 21, "%F:%T", timeinfo );  //Setting format of time
    }
}
/* }}} */

/* {{{ PIP_CVR_TerminateFrame() */
int PIP_CVR_TerminateFrame ( )
{
	int i=0;

        if(FrameDebug) printf("%s(%d): Entering Gstreamer based rmsframe Disable...\n", __FILE__, __LINE__);

        if(pip_cvrframe_buffer)
        {
                free(pip_cvrframe_buffer);
                pip_cvrframe_buffer = NULL;
        }

	for( i=0; i<MAX_NAL_SIZE; i++)
	{
	    free( pip_cvr_bufferqueue[i].buffer );
	    pip_cvr_bufferqueue[i].buffer = 0;
	}

        pthread_mutex_destroy(&pip_cvrqueueLock);

        return 0;
}
/* }}} */

#endif /* ENABLE_CVR_WITH_PIPEWIRE */
