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

/************* Included Header FIle **************/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbasesrc.h>

/*RDK Logging */
#include "rdk_debug.h"
#include <sys/wait.h>
#include <unistd.h>

/************ Static / Glocal variable ****************/
static int    exit_flag;        /* Program termination flag     */

GstElement *pstcamerasrc = NULL;

typedef enum camerasrc
{
    CAMERA_V4L2_SRC = 1,
    CAMERA_LIBCAMERA_SRC,
    CAMERA_PIPEWIRE_SRC
}CAMERA_SRC;

typedef struct cvr_gst_rpi
{
    char avideotype[50];

    int width;
    int height;
    int framerate;

}CVR_GST_RPI;

CVR_GST_RPI stCvrGstRpi = { 0 };

CAMERA_SRC encamerasrc = CAMERA_V4L2_SRC;

/************ Prototype *************/

void load_default_cvr_gstreamer_value();

static void signal_handler(int sig_num);

int main(int argc, char *argv[]);

void start_stream();

gboolean on_message(GstBus *bus,GstMessage *message, gpointer userData);

/************* API *****************/

/* load_default_cvr_gstreamer_value() */
void load_default_cvr_gstreamer_value()
{
        stCvrGstRpi.width = 1280;
        stCvrGstRpi.height = 720;

        stCvrGstRpi.framerate  = 30;

    strcpy( stCvrGstRpi.avideotype, "video/x-raw" );
}
/* }}} */

/* {{{ signal_handler() */
static void signal_handler(int sig_num)
{
    if (sig_num == SIGCHLD)
    {
        do
        {
        } while (waitpid(-1, &sig_num, WNOHANG) > 0);
    }
    else if( sig_num == 2 )
    {
        gst_element_send_event(pstcamerasrc, gst_event_new_eos());
	exit_flag = sig_num;
    }
    else
    {
        exit_flag = sig_num;
    }
}
/* }}} */

/* {{{ main() */
int main(int argc, char *argv[])
{
    /* RDK logger initialization */
    rdk_logger_init("/etc/debug.ini");

    gst_init(&argc,&argv);

    (void) signal(SIGCHLD, signal_handler);

    (void) signal(SIGTERM, signal_handler);

    (void) signal(SIGINT, signal_handler);

    if( NULL != argv[1] )
    {
        if( 0 == strcmp( argv[1], "v4l2src") )
        {
            encamerasrc = CAMERA_V4L2_SRC;
        }
        else if( 0 == strcmp( argv[1], "libcamerasrc") )
        {
            encamerasrc = CAMERA_LIBCAMERA_SRC;
        }
	else if( 0 == strcmp( argv[1], "pipewiresrc") )
        {
            encamerasrc = CAMERA_PIPEWIRE_SRC;
        }
    }

    load_default_cvr_gstreamer_value();

    start_stream();

    fflush(stdout);

    while (exit_flag == 0)
        sleep(1);

    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.GSTREAMER","%s(%d): Exiting on signal %d waiting for all threads to finish...",__FILE__, __LINE__,exit_flag);

    fflush(stdout);

    RDK_LOG( RDK_LOG_INFO,"LOG.RDK.GSTREAMER","%s(%d): done.\n", __FILE__, __LINE__);

    return (EXIT_SUCCESS);

}
/* }}} */

/* {{{ start_stream() */
void start_stream()
{
    GstElement *pipeline,*camerasrc,*filter,*h264enc,*h264parse,*kvssink,*videoconvert;
    GMainLoop  *loop;
    GstBus     *bus;
    GstCaps    *filtercaps;

    char const *stream_name;
    char const *accessKey;
    char const *secretKey;
    char const *defaultRegion;

    char bgstelementlink = 0;

    pipeline = gst_element_factory_make("pipeline","pipeline");

    if( CAMERA_V4L2_SRC == encamerasrc )
    {
        pstcamerasrc = camerasrc = gst_element_factory_make("v4l2src","v4l2src");
    }
    else if( CAMERA_LIBCAMERA_SRC == encamerasrc )
    {
        pstcamerasrc = camerasrc = gst_element_factory_make("libcamerasrc","libcamerasrc");
    }
    else if( CAMERA_PIPEWIRE_SRC == encamerasrc )
    {
        pstcamerasrc = camerasrc = gst_element_factory_make("pipewiresrc","pipewiresrc");
    }

    videoconvert = gst_element_factory_make("videoconvert","videoconvert");
	    
    filter = gst_element_factory_make("capsfilter","filter");

    h264enc = gst_element_factory_make("omxh264enc","omxh264enc");

    h264parse = gst_element_factory_make("h264parse","h264parse");

    kvssink = gst_element_factory_make("kvssink","kvssink");

    if ( !pipeline || !camerasrc || !videoconvert || !filter || !h264enc || !h264parse || !kvssink)
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.GSTREAMER","%s(%d): Unable to make elements\n", __FILE__, __LINE__);
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));

    loop = g_main_loop_new(NULL,FALSE);

    gst_bus_add_watch(bus,(GstBusFunc) on_message, loop);

    if( CAMERA_PIPEWIRE_SRC == encamerasrc )
    {
        gst_bin_add_many(GST_BIN(pipeline),camerasrc,videoconvert,filter,h264enc,h264parse,kvssink,NULL);

        bgstelementlink = gst_element_link_many(camerasrc,videoconvert,filter,h264enc,h264parse,kvssink,NULL);
    }
    else
    {
        gst_bin_add_many(GST_BIN(pipeline),camerasrc,filter,h264enc,h264parse,kvssink,NULL);

        bgstelementlink = gst_element_link_many(camerasrc,filter,h264enc,h264parse,kvssink,NULL);
    }

    if( bgstelementlink )
    {
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.GSTREAMER","%s(%d):Element linking success for pipeline\n", __FILE__, __LINE__);

    } else
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.GSTREAMER","%s(%d):Element linking failure for pipeline\n", __FILE__, __LINE__);

    }

    filtercaps = gst_caps_new_simple (stCvrGstRpi.avideotype,
                                      "width", G_TYPE_INT, stCvrGstRpi.width,
                                      "height", G_TYPE_INT, stCvrGstRpi.height,
                                      "framerate", GST_TYPE_FRACTION, stCvrGstRpi.framerate, 1,
                                      NULL);

    g_object_set (G_OBJECT (filter), "caps", filtercaps, NULL);


  if (NULL==(accessKey = getenv("ACCESS_KEY"))) {
    accessKey = "AccessKey";
  }

  if (NULL==(secretKey = getenv("SECRET_KEY"))) {
    secretKey = "SecretKey";
  }

  if (NULL==(defaultRegion = getenv("AWS_DEFAULT_REGION"))) {
    defaultRegion = "defaultRegion";
  }

  if (NULL==(stream_name = getenv("STREAM_NAME"))) {
    stream_name = "stream_name";
  }

  g_object_set(G_OBJECT (kvssink),
                  "stream-name", stream_name,
                  "storage-size", 100,
                  "access-key", accessKey,
                  "secret-key", secretKey,
                  "aws-region", defaultRegion,
		  NULL );

    gst_caps_unref (filtercaps);

    gst_element_set_state(pipeline,GST_STATE_PLAYING);

    if( gst_element_get_state(pipeline,NULL,NULL,GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_SUCCESS)
    {
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.GSTREAMER","%s(%d):The state of pipeline changed to GST_STATE_PLAYING successfully\n", __FILE__, __LINE__);

    } else
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.GSTREAMER","%s(%d):The state of pipeline changed to GST_STATE_PLAYING failed\n", __FILE__, __LINE__);
    }

    g_main_loop_run(loop);

    gst_element_set_state(kvssink,GST_STATE_READY);

    gst_element_set_state(h264parse,GST_STATE_READY);

    gst_element_set_state(filter,GST_STATE_READY);

    gst_element_set_state(h264enc,GST_STATE_READY);

    gst_element_set_state(videoconvert,GST_STATE_READY);

    gst_element_set_state(camerasrc,GST_STATE_READY);


    gst_element_set_state(kvssink,GST_STATE_NULL);

    gst_element_set_state(h264parse,GST_STATE_NULL);

    gst_element_set_state(filter,GST_STATE_READY);

    gst_element_set_state(h264enc,GST_STATE_NULL);

    gst_element_set_state(videoconvert,GST_STATE_NULL);

    gst_element_set_state(camerasrc,GST_STATE_NULL);


    gst_element_set_state(pipeline,GST_STATE_NULL);


    if( gst_element_get_state(pipeline,NULL,NULL,GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_SUCCESS)
    {
        RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.GSTREAMER","%s(%d):The state of pipeline changed to GST_STATE_NULL successfully\n", __FILE__, __LINE__);

    } else
    {
        RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.GSTREAMER","%s(%d):Changing the state of pipeline to GST_STATE_NULL failed\n", __FILE__, __LINE__);

    }

    if( CAMERA_PIPEWIRE_SRC == encamerasrc )
    {
        gst_element_unlink_many(camerasrc,videoconvert,filter,h264enc,h264parse,kvssink,NULL);
    }
    else
    {
        gst_element_unlink_many(camerasrc,filter,h264enc,h264parse,kvssink,NULL);
    }

    gst_object_ref(camerasrc);

    if( CAMERA_PIPEWIRE_SRC == encamerasrc )
    {
         gst_bin_remove_many(GST_BIN(pipeline),kvssink,h264parse,h264enc,filter,videoconvert,camerasrc,NULL);
    }
    else
    {
        gst_bin_remove_many(GST_BIN(pipeline),kvssink,h264parse,h264enc,filter,camerasrc,NULL);
    }

    gst_object_unref(pipeline);

    RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.GSTREAMER","%s(%d):pipeline deleted\n", __FILE__, __LINE__);

}
/* }}} */

/* {{{ on_message() */
gboolean on_message(GstBus *bus,GstMessage *message, gpointer userData)
{
    GMainLoop *loop = (GMainLoop *) userData;

    switch(GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_EOS:
        {
            RDK_LOG( RDK_LOG_DEBUG,"LOG.RDK.GSTREAMER","%s(%d): EOS has reached\n", __FILE__, __LINE__);

            g_main_loop_quit(loop);
        }
            break;

        case GST_MESSAGE_ERROR:
        {
            RDK_LOG( RDK_LOG_ERROR,"LOG.RDK.GSTREAMER","%s(%d): Error has occured\n", __FILE__, __LINE__);

            g_main_loop_quit(loop);
        }
            break;

        default:
            break;
    }

    return TRUE;

}
/* }}} */
