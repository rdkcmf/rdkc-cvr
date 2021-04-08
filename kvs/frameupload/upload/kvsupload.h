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
#ifndef _KVS_UPLOAD_API_H
#define _KVS_UPLOAD_API_H

#include "kvsuploadCallback.h"

int kvsInit(kvsUploadCallback* callback, int stream_id, uint64_t storageMem = 0);
int kvsStreamInit(unsigned short& audioenabled, unsigned short& kvshighmem, unsigned short& contentchangestatus);
#ifdef _HAS_XSTREAM_
int kvsUploadFrames(unsigned short& kvshighmem, frameInfoH264 frameData, char* filename, bool isEOF = false );
#else
int kvsUploadFrames(unsigned short& kvshighmem, RDKC_FrameInfo frameData, char* fileName, bool isEOF = false );
#endif //_HAS_XSTREAM_

#endif
