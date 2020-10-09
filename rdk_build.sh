#!/bin/bash

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

#######################################
#
# Build Framework standard script for
#
# webrtc source code

# use -e to fail on any shell issue
# -e is the requirement from Build Framework
set -e

# default PATHs - use `man readlink` for more info
# the path to combined build
export RDK_PROJECT_ROOT_PATH=${RDK_PROJECT_ROOT_PATH-`readlink -m ..`}
export COMBINED_ROOT=$RDK_PROJECT_ROOT_PATH

# path to build script (this script)
export RDK_SCRIPTS_PATH=${RDK_SCRIPTS_PATH-`readlink -m $0 | xargs dirname`}

# path to components sources and target
export RDK_SOURCE_PATH=${RDK_SOURCE_PATH-`readlink -m .`}
export RDK_TARGET_PATH=${RDK_TARGET_PATH-$RDK_SOURCE_PATH}

#default component name
export RDK_COMPONENT_NAME=${RDK_COMPONENT_NAME-`basename $RDK_SOURCE_PATH`}
export BUILDS_DIR=$RDK_PROJECT_ROOT_PATH
export RDK_DIR=$BUILDS_DIR
export ENABLE_RDKC_LOGGER_SUPPORT=true
export DCA_PATH=$RDK_SOURCE_PATH

if [ "$XCAM_MODEL" == "SCHC2" ]; then
. ${RDK_PROJECT_ROOT_PATH}/build/components/amba/sdk/setenv2
else  
. ${RDK_PROJECT_ROOT_PATH}/build/components/sdk/setenv2
fi

export PLATFORM_SDK=${RDK_TOOLCHAIN_PATH}
export FSROOT=$RDK_FSROOT_PATH

export KVS_FRAMEUPLOAD=yes

if [ "$XCAM_MODEL" == "SCHC2" ] || [ "$XCAM_MODEL" == "XHB1" ]; then
    echo "Enable xStreamer by default for xCam2 and DBC"
    export ENABLE_XSTREAMER=true
else
    echo "Disable xStreamer by default for xCam and iCam2"
    export ENABLE_XSTREAMER=false
fi

# parse arguments
INITIAL_ARGS=$@

function usage()
{
    set +x
    echo "Usage: `basename $0` [-h|--help] [-v|--verbose] [action]"
    echo "    -h    --help                  : this help"
    echo "    -v    --verbose               : verbose output"
    echo
    echo "Supported actions:"
    echo "      configure, clean, build (DEFAULT), rebuild, install"
}

# This Function to perform pre-build configurations before building plugin code
function configure()
{
   true
}

# This Function to perform clean the build if any exists already
function clean()
{
    echo "Start Clean"
    cd $RDK_SOURCE_PATH

    make clean
    make uninstall

    cd $RDK_SOURCE_PATH/kvs
    make clean
    make uninstall

}

# This Function peforms the build to generate the webrtc.node
function build()
{
    cd $RDK_SOURCE_PATH

    echo "RDK_SOURCE_PATH :::::: $RDK_SOURCE_PATH"
    make all

    echo "CVR build is done"

    echo "Building kvs application"
    cd $RDK_SOURCE_PATH/kvs
    make all

    echo "KVS build is done"
    echo "Starting install... $RDK_COMPONENT_NAME"
    install
}

# This Function peforms the rebuild to generate the webrtc.node
function rebuild()
{
    clean
    build
    cd $RDK_SOURCE_PATH/kvs
    clean
    build
}

# This functions performs installation of webrtc-streaming-node output created into sercomm firmware binary
function install()
{
    echo "Start CVR Installation"

    cd $RDK_SOURCE_PATH
   
    make install

    cd $RDK_SOURCE_PATH/kvs

    make install

    if [ -f $RDK_SOURCE_PATH/kvs/clipupload/upload/kvs_log_configuration ] ; then
        echo "Installing amazon-kinesis-video-streams-producer-sdk log configuration"
        cp -vf $RDK_SOURCE_PATH/kvs/clipupload/upload/kvs_log_configuration $RDK_FSROOT_PATH/etc
    fi
 
    echo "CVR Installation is done"
}

function setHydra()
{
    echo "setHydra - Disable xStreamer"
    export ENABLE_XSTREAMER=false
}

# run the logic
#these args are what left untouched after parse_args
HIT=false

for i in "$@"; do
    echo "$i"
    case $i in
        enableHydra)  HIT=true; setHydra ;;
        configure)  HIT=true; configure ;;
        clean)      HIT=true; clean ;;
        build)      HIT=true; build ;;
        rebuild)    HIT=true; rebuild ;;
        install)    HIT=true; install ;;
        *)
            #skip unknown
        ;;
    esac
done

# if not HIT do build by default
if ! $HIT; then
  build
fi

