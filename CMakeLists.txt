# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 3.5)
project(sfp VERSION 0.3.0)

option(SFP_BUILD_TESTS "Build libsfp tests" OFF)
option(BUILD_SHARED_LIBS "build shared libraries" OFF)

option(SFP_CONFIG_DEBUG "Enable SFP debugging messages" OFF)
option(SFP_CONFIG_WARN "Enable SFP warnings" OFF)
option(SFP_CONFIG_ERROR "Enable SFP errors" OFF)

# libsfp has a library target, and optional tests

set(sources src/serial_framing_protocol.cpp )

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("int main () { throw int(); }" HAVE_EXCEPTIONS)
if(HAVE_EXCEPTIONS)
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost 1.54.0 REQUIRED COMPONENTS system filesystem log thread)
    list(APPEND sources src/system_error.cpp)
endif()

find_package(cxx-util)

add_library(sfp ${sources})
set_target_properties(sfp
    PROPERTIES CXX_STANDARD 14
               CXX_STANDARD_REQUIRED ON
               )

if(HAVE_EXCEPTIONS)
    target_include_directories(sfp PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(sfp PRIVATE ${Boost_LIBRARIES} PUBLIC barobo::cxx-util)
else()
    target_link_libraries(sfp PUBLIC barobo::cxx-util)
endif()

if(SFP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# target properties

set_target_properties(sfp PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  MACOSX_RPATH ON
)

configure_file(config.h.in include/sfp/config.h)
target_include_directories(sfp
    PRIVATE include
            ${PROJECT_BINARY_DIR}/include
)


install(TARGETS sfp EXPORT barobo
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY ${PROJECT_BINARY_DIR}/include/ DESTINATION include)

# Boilerplate follows

export(EXPORT barobo
    FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-targets.cmake"
    NAMESPACE barobo::
)

install(EXPORT barobo
    FILE ${PROJECT_NAME}-targets.cmake
    NAMESPACE barobo::
    DESTINATION cmake
)

file(WRITE "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/${PROJECT_NAME}-targets.cmake\")"
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-configVersion.cmake"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
    "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-configVersion.cmake"
    DESTINATION cmake
)
