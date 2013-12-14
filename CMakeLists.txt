# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.8)
project(LIBSFP)

set(SFP_MAJOR_VERSION 0)
set(SFP_MINOR_VERSION 0)
set(SFP_VERSION ${SFP_MAJOR_VERSION}.${SFP_MINOR_VERSION})

##############################################################################
# Targets

set(SRCS serial_framing_protocol.c net_byte_order.c)
set(HEADERS serial_framing_protocol.h)

add_library(sfpObjects OBJECT ${SRCS})
add_library(sfp SHARED $<TARGET_OBJECTS:sfpObjects>)

set_target_properties(sfp PROPERTIES VERSION ${SFP_VERSION} SOVERSION ${SFP_MAJOR_VERSION})

install(TARGETS sfp RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
install(FILES ${HEADERS} DESTINATION include)

##############################################################################
# Platform Specific Setup

if(NOT CMAKE_HOST_WIN32)
  set_target_properties(sfpObjects PROPERTIES COMPILE_FLAGS "-fPIC")
endif(NOT CMAKE_HOST_WIN32)
