cmake_minimum_required(VERSION 2.8.8)
project(LIBSFP)

include_directories(${DOF_FIRMWARE_SOURCE_DIR}/inc)

if(NOT CMAKE_HOST_WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif(NOT CMAKE_HOST_WIN32)

set(SRCS serial_framing_protocol.c net_byte_order.c)

add_library(sfpObjects OBJECT ${SRCS})
add_library(sfp SHARED $<TARGET_OBJECTS:sfpObjects>)
