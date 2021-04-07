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
USE_RFCCONFIG = yes
USE_RTMESSAGE = yes
USE_PLUGINS = yes
USE_BREAKPAD = yes
USE_HTTPCLIENT = yes
USE_STREAMUTILS = yes
USE_SYSUTILS = yes
USE_GST = yes
USE_LIBSYSWRAPPER = yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak
LIBS = $(LIBFLAGS)

CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/clipupload/upload/
CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/clipupload/core/
CFLAGS  += -DRTMSG
CFLAGS  += -g -std=c++11
CFLAGS  += -Wno-error
CFLAGS	+= -pthread
CFLAGS  += -fpermissive

LIBS    += -L$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/upload -lcvrupload_kvs
ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS +=-DXCAM2
endif

#GST_CVR
INSTALL = install
RM = rm

SRC  += cvr_daemon.cpp 

OBJ = $(SRC:.cpp=.o)

RELEASE_TARGET = cvr_daemon_kvs
DEBUG_TARGET = cvr_daemon_kvs_debug

all: $(OBJ) $(RELEASE_TARGET) $(DEBUG_TARGET) install

$(RELEASE_TARGET): $(OBJ)
	$(CXX) $(CFLAGS) -o $(@) $^ $(LIBS)  #-Wall -Wno-error
	$(STRIP) $(RELEASE_TARGET)

$(DEBUG_TARGET): $(OBJ)
	$(CXX) $(CFLAGS) -o $(@) $^ $(LIBS)  #-Wall -Wno-error

%.o:%.cpp
	 $(CXX) -c $< $(CFLAGS)  -o $@  # -Wall -Wno-error

clean: uninstall
	$(RM) -f $(OBJ) $(RELEASE_TARGET) $(DEBUG_TARGET)

install:
	$(PLATFORM_BREAKPAD_BINARY) $(DEBUG_TARGET) > $(RELEASE_TARGET).sym
	sed -i "1s/$(DEBUG_TARGET)/$(RELEASE_TARGET)/" $(RELEASE_TARGET).sym
	mv -f $(RELEASE_TARGET).sym  $(PLATFORM_SYMBOL_PATH)
	echo "kvs frame upload debug symbol created for $(RELEASE_TARGET)"
	$(INSTALL) -D $(RELEASE_TARGET) ${RDK_SDROOT}/usr/local/bin/$(RELEASE_TARGET)

uninstall:
	$(RM) -f $(RDK_SDROOT)/usr/local/bin/$(RELEASE_TARGET)
