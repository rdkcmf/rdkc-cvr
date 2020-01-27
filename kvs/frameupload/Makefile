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
USE_KINESIS_PRODUCER = yes

include ${RDK_PROJECT_ROOT_PATH}/utility/AppsRule.mak
LIBS = $(LIBFLAGS)

CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/frameupload/upload/
CFLAGS  += -I$(RDK_PROJECT_ROOT_PATH)/cvr/kvs/frameupload/core/
CFLAGS  += -DRTMSG
CFLAGS  += -std=c++14
CFLAGS  += -Wno-error
CFLAGS	+= -pthread
CFLAGS  += -fpermissive

ifeq ($(XCAM_MODEL), SCHC2)
CFLAGS +=-DXCAM2
endif

ifeq ($(XFINITY_SUPPORT), yes)
CFLAGS +=-DXFINITY_SUPPORT
endif

#GST_CVR
INSTALL = install
RM = rm

SRC  += upload/kvsupload.cpp upload/memorystats.cpp core/cvr_daemon.cpp

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
