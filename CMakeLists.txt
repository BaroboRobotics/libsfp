# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.8)
project(LIBSFP)

##############################################################################
# Platform Specific Setup

if(NOT CMAKE_HOST_WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif(NOT CMAKE_HOST_WIN32)

##############################################################################
# Targets

set(SRCS serial_framing_protocol.c net_byte_order.c)
set(HEADERS serial_framing_protocol.h)

add_library(sfpObjects OBJECT ${SRCS})
add_library(sfp SHARED $<TARGET_OBJECTS:sfpObjects>)

install(TARGETS sfp RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
install(FILES ${HEADERS} DESTINATION include)
