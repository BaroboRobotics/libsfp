# Serial Framing Protocol library
# Barobo, Inc.

cmake_minimum_required(VERSION 2.8.9)
project(LIBSFP C)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 0)
set(VERSION ${VERSION_MAJOR}.${VERSION_MINOR})

# Expose our public interface to other subprojects in CMake.
set(${PROJECT_NAME}_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
  CACHE INTERNAL "${PROJECT_NAME}: public include directories" FORCE)

include_directories(${${PROJECT_NAME}_INCLUDE_DIRS})

##############################################################################
# Targets

set(TARGET sfp)

file(GLOB SOURCES src/*.c)
file(GLOB HEADERS include/*.h)

add_library(${TARGET}-objs OBJECT ${SOURCES})
add_library(${TARGET} SHARED $<TARGET_OBJECTS:${TARGET}-objs>)

# Although -fPIC is implied for SHARED targets, it is not for OBJECT targets,
# and we must set this manually. TODO sfp needs to use PIC so we can link it
# into libbarobo.so, but this will probably cause code bloat in the Linkbot
# firmware. Set two different targets with an option to switch them.
set_target_properties(${TARGET}-objs
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

set_target_properties(${TARGET}
  PROPERTIES VERSION ${VERSION} SOVERSION ${VERSION_MAJOR})

install(TARGETS ${TARGET}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES ${HEADERS} DESTINATION include)
