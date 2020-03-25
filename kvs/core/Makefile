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

XFINITY_SUPPORT := yes

# Add dependent libraries
USE_CONFIGMGR = yes
USE_RFCCONFIG = yes
USE_RTMESSAGE = yes
USE_PLUGINS = yes
USE_BREAKPAD = yes
USE_HTTPCLIENT = yes
USE_STREAMUTILS = yes
USE_SYSUTILS = yes
USE_GST = yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak
LIBS = $(LIBFLAGS)

CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/upload/
CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/core/
CFLAGS  += -DRTMSG
CFLAGS  += -std=c++11
CFLAGS  +=  -std=c++11
CFLAGS  += -Wno-error
CFLAGS	+= -pthread
CFLAGS  += -fpermissive

LIBS    += -L$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/upload -lcvrupload_kvs
ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS +=-DXCAM2
endif

ifeq ($(XFINITY_SUPPORT), yes)
CFLAGS +=-DXFINITY_SUPPORT
endif

#GST_CVR
INSTALL = install
RM = rm

SRC  += cvr_daemon.cpp 

OBJ = $(SRC:.cpp=.o)

TARGET = cvr_daemon_kvs

all: $(OBJ) $(TARGET) install

$(TARGET): $(OBJ)
	$(CXX) $(CFLAGS) -o $(@) $^ $(LIBS)  #-Wall -Wno-error

%.o:%.cpp
	 $(CXX) -c $< $(CFLAGS)  -o $@  # -Wall -Wno-error


clean: uninstall
	$(RM) -f $(OBJ) $(TARGET)

install:
	$(INSTALL) -D $(TARGET) ${RDK_SDROOT}/usr/local/bin/$(TARGET)

uninstall:
	$(RM) -f $(RDK_SDROOT)/usr/local/bin/$(TARGET)
