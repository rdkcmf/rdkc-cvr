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
#ifndef _CVR_UPLOAD_API_H
#define _CVR_UPLOAD_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include "system_config.h"

	/* initialise cvr upload */
        int cvr_upload_init();

 	/* @description: This function is used to call the function which upload data to the http server.
	 *
	 * @param[in]: File path,start time,end time, event type,event date time, m file path, motion level, num of arguments.
	 *
	 * @return: Error code.
	 */
	int cvr_upload(char *fpath, char *stime, char *etime, int eventType, unsigned int eventDatetime, char *m_fpath , int motion_level_idx, char* str_od_data, char* va_engine_version, bool smartTnEnabled);

	/* close cvr upload */
        void cvr_upload_close();

#ifdef __cplusplus
}
#endif
#endif
