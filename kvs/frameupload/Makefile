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
USE_BREAKPAD = yes
USE_HTTPCLIENT = yes
USE_CONFIGUTILS = yes
USE_SYSUTILS = yes
USE_KINESIS_PRODUCER = yes
USE_LIBSYSWRAPPER = yes

ifeq ($(XCAM_MODEL), XHB1)
USE_DEWARP = yes
CFLAGS +=-DOSI
else
SUPPORT_MXML = yes
USE_STREAMUTILS = yes
endif

ifeq ($(XCAM_MODEL), SCHC2)
USE_DEWARP = yes
endif

ifneq ($(ENABLE_XSTREAMER), true)
USE_PLUGINS = yes
else
USE_CONSUMER = yes
endif

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak
LIBS = $(LIBFLAGS)

CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/frameupload/upload/
CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/frameupload/core/
CFLAGS  += -DRTMSG
CFLAGS  += -g -std=c++14
CFLAGS  += -Wno-error
CFLAGS	+= -pthread
CFLAGS  += -fpermissive

ifeq ($(ENABLE_XSTREAMER), true)
$(info( '*************XSTREAMER ENABLED**************')
CFLAGS  += -D_HAS_XSTREAM_
LIBS    += -Wl,-rpath-link=$(PROJ_ROOT)/vendor/img/fs/shadow_root/usr/lib
endif

ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS +=-DXCAM2
endif

#FRAME_CVR
INSTALL = install
RM = rm
ifeq ($(XCAM_MODEL), XHB1)
CFLAGS +=-DXHB1
SRC  += upload/kvsupload.cpp core/osi_cvr_daemon.cpp
else
SRC  += upload/kvsupload.cpp core/cvr_daemon.cpp
endif
OBJ = $(SRC:.cpp=.o)

RELEASE_TARGET = cvr_daemon_kvs2
DEBUG_TARGET = cvr_daemon_kvs2_debug

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
