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
USE_RFCCONFIG = yes
USE_GST = yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak
LINFLAGS = $(LIBFLAGS)
CPPFLAGS = $(CFLAGS)

SRCS  += cvr_app.cpp

OBJS  := $(SRCS:.cpp=.o)
PROG   =  cvrapp
all: $(PROG) install
RM = rm

$(OBJS): %.o: %.cpp
	$(CXX) -c $(CPPFLAGS) $< -std=c++11 -o $@

#.cpp.o:
#	$(CXX) -v -c $(LINFLAGS) $< -o $(@D)/$(@F)
$(PROG): $(OBJS)
	$(CXX) $(CPPFLAGS) -shared -o $@ $(OBJS) $(LINFLAGS)

clean:
	$(RM) -rf $(OBJS) *~ $(PROG)

#linux: 
#	$(CXX) -v $(LINFLAGS) cvr_app.cpp -s -o $(PROG)

install:
	@echo ${RDK_FSROOT_PATH}
	cp $(PROG) ${RDK_FSROOT_PATH}/usr/bin/
