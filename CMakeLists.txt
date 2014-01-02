# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)
project(LIBSFP C)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)

# Boilerplate stuff
string(TOLOWER ${PROJECT_NAME} project_name)
set(EXPORT_TARGET_SET ${project_name}-targets)

##############################################################################
# Headers

set(INCLUDE_DIR include)
install(DIRECTORY ${INCLUDE_DIR} DESTINATION .)
include_directories(${INCLUDE_DIR})

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

install(TARGETS ${TARGET} EXPORT ${EXPORT_TARGET_SET}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

##############################################################################
# Configuration for dependent projects

set(INSTALL_CMAKE_DIR lib/cmake/${project_name})

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/${project_name}-config-version.cmake"
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
  COMPATIBILITY SameMajorVersion)

# This must happen after INCLUDE_DIR and TARGET are set. The config file will
# use these two variables.
# Configure once for projects which are part of the same build as libsfp.
configure_file(inbuildconfig.cmake.in
  "${PROJECT_BINARY_DIR}/${project_name}-config.cmake" @ONLY)

# Add ${PROJECT_BINARY_DIR} to the find_package config.cmake search path.
export(PACKAGE ${PROJECT_NAME})

# And configure again for the final installation directory.
configure_package_config_file(config.cmake.in
  "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/${project_name}-config.cmake"
  INSTALL_DESTINATION "${INSTALL_CMAKE_DIR}"
  PATH_VARS INCLUDE_DIR)

install(FILES
  "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/${project_name}-config.cmake"
  "${PROJECT_BINARY_DIR}/${project_name}-config-version.cmake"
  DESTINATION "${INSTALL_CMAKE_DIR}")

# Generate a target export cmake script for the final installation directory.
install(EXPORT ${EXPORT_TARGET_SET} DESTINATION "${INSTALL_CMAKE_DIR}")
