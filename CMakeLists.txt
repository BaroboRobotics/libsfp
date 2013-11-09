cmake_minimum_required(VERSION 2.8)
project(LIBSFP)

include_directories(${DOF_FIRMWARE_SOURCE_DIR}/inc)

add_library(sfpStatic STATIC net_byte_order.c serial_framing_protocol.c)
add_library(sfp SHARED net_byte_order.c serial_framing_protocol.c)
