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

# Add dependent libraries
USE_CONFIGMGR = yes
USE_RTMESSAGE = yes
USE_HTTPCLIENT = yes
USE_GST = yes
USE_KINESIS_PRODUCER = yes
USE_CJSON = yes
USE_SYSUTILS= yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak

SRCS  += kvsupload.cpp memorystats.cpp cvrUploadAPI.cpp

CFLAGS  += -g -std=c++14 -fPIC
CFLAGS+= -I$(RDK_PROJECT_ROOT_PATH)/cvr/clipupload/upload/
CFLAGS+= -DXFINITY_SUPPORT

ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS += -DUSE_MFRLIB
endif

OBJS  := $(SRCS:.cpp=.o)

RELEASE_TARGET   =   libcvrupload_kvs.so
DEBUG_TARGET   =   libcvrupload_kvs_debug.so

all: $(RELEASE_TARGET) $(DEBUG_TARGET) install

INSTALL = install
RM = rm

$(OBJS): %.o: %.cpp
	$(CXX) -c $(CFLAGS) $< -o $@

$(RELEASE_TARGET): $(OBJS)
	$(CXX) $(CFLAGS) -shared -o $@ $(OBJS) $(LIBFLAGS)
	$(STRIP) $(RELEASE_TARGET)

$(DEBUG_TARGET): $(OBJS)
	$(CXX) $(CFLAGS) -shared -o $@ $(OBJS) $(LIBFLAGS)

clean:
	$(RM) -rf $(OBJS) *~ $(RELEASE_TARGET) $(DEBUG_TARGET)
	
install:
	$(PLATFORM_BREAKPAD_BINARY) $(DEBUG_TARGET) > $(RELEASE_TARGET).sym
	mv -f $(RELEASE_TARGET).sym  $(PLATFORM_SYMBOL_PATH)
	echo "kvs clip upload debug symbol created for $(RELEASE_TARGET)"

	$(INSTALL) -D $(RELEASE_TARGET) ${RDK_FSROOT_PATH}/usr/lib/$(RELEASE_TARGET)

uninstall:
	$(RM) -f $(RDK_FSROOT_PATH)/usr/lib/$(RELEASE_TARGET) $(RDK_FSROOT_PATH)/usr/lib/$(DEBUG_TARGET)
