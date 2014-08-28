# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)
project(LIBSFP CXX C)

option(SFP_BUILD_TESTS "Build libsfp tests" OFF)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 2)
set(VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

configure_file(config.h.in include/sfp/config.h)

# Dependencies

if(NOT DEFINED DEPS_ROOT)
    if(DEFINED ENV{DEPS_ROOT})
        set(DEPS_ROOT "$ENV{DEPS_ROOT}")
    else()
        message(FATAL_ERROR "Please set DEPS_ROOT to the location of your dependencies (libsfp, ribbon-bridge, etc.).")
    endif()
endif()

function(add_dependency _name)
    add_subdirectory(${DEPS_ROOT}/${_name} ${_name})
endfunction()

add_dependency(cxx-util)

##############################################################################
# Targets

# Sources need to be listed manually so CMake knows to regenerate the
# build system when the list of sources changes.
set(SOURCES src/serial_framing_protocol.cpp )
set_source_files_properties(${SOURCES} PROPERTIES COMPILE_FLAGS -std=c++11)

# By default, we'll build a shared library, but the robot firmware needs to be
# able to build a static library to link with. Providing a separate sfp-static
# target isn't that useful, because the firmware uses an entirely separate
# toolchain, so it will have a separate build directory.
option(BUILD_SHARED_LIBS "build shared libraries" OFF)
add_library(sfp ${SOURCES})

target_include_directories(sfp
    PUBLIC include
           ${PROJECT_BINARY_DIR}/include
           $<TARGET_PROPERTY:cxx-util,INTERFACE_INCLUDE_DIRECTORIES>
           )

set_target_properties(sfp PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
  SOVERSION ${VERSION_MAJOR})

##############################################################################
# Installation

if(UNIX)
  set(INSTALL_LIB_DIR lib)
  set(INSTALL_BIN_DIR bin)
else()
  set(INSTALL_LIB_DIR .)
  set(INSTALL_BIN_DIR .)
endif()

install(TARGETS sfp
  ARCHIVE DESTINATION ${INSTALL_LIB_DIR}
  LIBRARY DESTINATION ${INSTALL_LIB_DIR}
  RUNTIME DESTINATION ${INSTALL_BIN_DIR})

# Fix install-name for Mac libraries
if(APPLE)
  set_target_properties(sfp PROPERTIES INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib")
endif()

if(SFP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
