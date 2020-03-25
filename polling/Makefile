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
USE_HTTPCLIENT = yes
USE_LIBXML = yes
SUPPORT_MXML = yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak

CFLAGS  += -DXFINITY_SUPPORT

ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS   += -DUSE_MFRLIB
endif

SRCS	+= xmlParser.cpp
SRCS	+= xfinity_polling_config.cpp

OBJS	= $(SRCS:.cpp=.o)

BIN   =   xfinity_polling_config

all: $(BIN) install
RM = rm
INSTALL = install

$(OBJS): %.o: %.cpp
	$(CXX) -c $(CFLAGS) $< -o $@

$(BIN): $(OBJS)
	$(CXX) $(CFLAGS) -o $@ $(OBJS) $(LIBFLAGS)

clean:
	$(RM) -rf $(OBJS) *~ $(BIN)

install:
	$(INSTALL) -D $(BIN) ${RDK_SDROOT}/usr/local/bin/$(BIN)

uninstall:
	$(RM) -f $(RDK_SDROOT)/usr/local/bin/$(BIN)
