# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)
project(LIBSFP C)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 0)

##############################################################################
# Headers

# Install our public headers.
file(GLOB HEADERS include/*)
install(FILES ${HEADERS} DESTINATION include)

# Expose our public headers to dependent sibling subprojects in CMake.
set(${PROJECT_NAME}_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
  CACHE INTERNAL "${PROJECT_NAME}: public headers" FORCE)

include_directories(${${PROJECT_NAME}_INCLUDE_DIRS})

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
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR} SOVERSION ${VERSION_MAJOR})

install(TARGETS ${TARGET}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
