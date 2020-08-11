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

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak

SRCS  += kvsupload.cpp memorystats.cpp cvrUploadAPI.cpp

CFLAGS  += -fPIC
CFLAGS+= -I$(RDK_PROJECT_ROOT_PATH)/cvr/clipupload/upload/
CFLAGS+= -DXFINITY_SUPPORT

ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS += -DUSE_MFRLIB
endif

OBJS  := $(SRCS:.cpp=.o)

BIN   =   libcvrupload_kvs.so

all: $(BIN) install

INSTALL = install
RM = rm

$(OBJS): %.o: %.cpp
	$(CXX) -c $(CFLAGS) $< -std=c++14 -o $@

$(BIN): $(OBJS)
	$(CXX) $(CFLAGS) -shared -o $@ $(OBJS) $(LIBFLAGS)
clean:
	$(RM) -rf $(OBJS) *~ $(BIN)

install:
	$(INSTALL) -D $(BIN) ${RDK_FSROOT_PATH}/usr/lib/$(BIN)

uninstall:
	$(RM) -f $(RDK_FSROOT_PATH)/usr/lib/$(BIN)

