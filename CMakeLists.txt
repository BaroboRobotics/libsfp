# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 3.2.1)

set(target sfp)
project(${target})

if(TARGET ${target})
  # FIXME make sure that the current target's version or configuration matches?
  # this feels rather dangerous. find_package would solve this conundrum :/
  message(FATAL_ERROR "sfp already configured")
endif()

option(SFP_BUILD_TESTS "Build libsfp tests" OFF)
option(BUILD_SHARED_LIBS "build shared libraries" OFF)

option(SFP_CONFIG_DEBUG "Enable SFP debugging messages" OFF)
option(SFP_CONFIG_WARN "Enable SFP warnings" OFF)
option(SFP_CONFIG_ERROR "Enable SFP errors" OFF)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 3)
set(VERSION_PATCH 0)
set(VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

# libsfp has a library target, and optional tests

set(sources src/serial_framing_protocol.cpp )

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("int main () { throw int(); }" HAVE_EXCEPTIONS)
if(HAVE_EXCEPTIONS)
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost 1.54.0 REQUIRED COMPONENTS system filesystem log thread)
    list(APPEND sources src/system_error.cpp)
endif()

add_library(${target} ${sources})
set_target_properties(${target}
    PROPERTIES CXX_STANDARD 14
               CXX_STANDARD_REQUIRED ON
               )

if(HAVE_EXCEPTIONS)
    target_include_directories(${target} PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${Boost_LIBRARIES})
endif()

if(SFP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# target properties

set_target_properties(${target} PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
  SOVERSION ${VERSION_MAJOR}
  MACOSX_RPATH ON
)

configure_file(config.h.in include/sfp/config.h)
target_include_directories(${target}
    PUBLIC include
           ${PROJECT_BINARY_DIR}/include
           $<TARGET_PROPERTY:cxx-util,INTERFACE_INCLUDE_DIRECTORIES>
)

# Staging

if(WIN32)
  set(dest .)
elseif(APPLE)
  set(dest ../MacOS)
else()
  set(dest lib)
endif()

#install(TARGETS ${target} DESTINATION ${dest})
