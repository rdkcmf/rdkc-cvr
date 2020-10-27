cmake_minimum_required(VERSION 1.6.3)

PROJECT(cvr-gst-rpi)

#make the list with all the source files
FILE(GLOB SOURCES "cvr_gst_rpi.c")

#prepare the include directories
INCLUDE_DIRECTORIES( "." )

find_package(PkgConfig REQUIRED)
pkg_check_modules(PKG_gstreamer REQUIRED gstreamer-1.0)
pkg_check_modules(PKG_gstapp REQUIRED gstreamer-app-1.0)

ADD_EXECUTABLE(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${PKG_gstreamer_INCLUDE_DIRS} )

TARGET_LINK_LIBRARIES(${PROJECT_NAME}
                      rdkloggers
                      ${PKG_gstreamer_LIBRARIES}
                      ${PKG_gstapp_LIBRARIES}        
                     )

INSTALL(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

