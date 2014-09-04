# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)

set(target sfp)
if(TARGET ${target})
  # FIXME make sure that the current target's version or configuration matches?
  # this feels rather dangerous. find_package would solve this conundrum :/
  message(FATAL_ERROR "libsfp already configured")
endif()

project(${target})

option(SFP_BUILD_TESTS "Build libsfp tests" OFF)
option(BUILD_SHARED_LIBS "build shared libraries" OFF)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 2)
set(VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

# libsfp has a library target, and optional tests

set(sources src/serial_framing_protocol.cpp )
set_source_files_properties(${SOURCES} PROPERTIES COMPILE_FLAGS -std=c++11)

add_library(${target} ${sources})

if(SFP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# target properties

set_target_properties(${target} PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
  SOVERSION ${VERSION_MAJOR})
if(APPLE)
  set_target_properties(${target} PROPERTIES INSTALL_NAME_DIR "@executable_path")
endif()

target_include_directories(${target}
    PUBLIC include
           ${PROJECT_BINARY_DIR}/include
           $<TARGET_PROPERTY:cxx-util,INTERFACE_INCLUDE_DIRECTORIES>
)


# Staging

if(WIN32)
  set(INSTALL_LIB_DIR .)
  set(INSTALL_BIN_DIR .)
elseif(APPLE)
  set(INSTALL_LIB_DIR Contents/MacOS)
  set(INSTALL_BIN_DIR Contents/MacOS)
else()
  set(INSTALL_LIB_DIR lib)
  set(INSTALL_BIN_DIR bin)
endif()

install(TARGETS ${target}
  ARCHIVE DESTINATION ${INSTALL_LIB_DIR}
  LIBRARY DESTINATION ${INSTALL_LIB_DIR}
  RUNTIME DESTINATION ${INSTALL_BIN_DIR})
