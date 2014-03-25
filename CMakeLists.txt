# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)
project(LIBSFP C)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 0)

##############################################################################
# Headers

set(PUBLIC_INCLUDE_DIR include)
install(DIRECTORY ${PUBLIC_INCLUDE_DIR} DESTINATION .)
include_directories(${PUBLIC_INCLUDE_DIR})

##############################################################################
# Targets

set(TARGET sfp)

# Sources need to be listed manually so CMake knows to regenerate the
# build system when the list of sources changes.
set(SOURCES src/serial_framing_protocol.c src/net_byte_order.c)

# By default, we'll build a shared library, but the robot firmware needs to be
# able to build a static library to link with. Providing a separate
# ${TARGET}-static target isn't that useful, because the firmware uses an
# entirely separate toolchain, so it will have a separate build directory.
option(BUILD_SHARED_LIBS "build shared libraries" ON)
add_library(${TARGET} ${SOURCES})

set_target_properties(${TARGET} PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
  SOVERSION ${VERSION_MAJOR})

install(TARGETS ${TARGET}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

##############################################################################
# Configuration for dependent parent projects

set(${PROJECT_NAME}_INCLUDE_DIRS ${PUBLIC_INCLUDE_DIR})
set(${PROJECT_NAME}_LIBRARIES ${TARGET})
