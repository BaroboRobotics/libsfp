# Make sure TBB is available.
find_path(TBB_INCLUDE_DIR tbb/tbb.h)
if(NOT TBB_INCLUDE_DIR)
    message(FATAL_ERROR "libsfp tests requires Intel's Threaded Building Blocks.
    Debian-like: apt-get install libtbb-dev
    Gentoo: emerge dev-cpp/tbb")
endif()

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.54.0 REQUIRED system filesystem log thread coroutine context)

set_source_files_properties(
    sfp-echo.cpp
    sfp-asio-echo.cpp
    PROPERTIES
    COMPILE_FLAGS "-std=c++11")

add_executable(sfp-echo sfp-echo.cpp)
target_include_directories(sfp-echo
    PRIVATE $<TARGET_PROPERTY:sfp,INCLUDE_DIRECTORIES>
            ${TBB_INCLUDE_DIR}
            ${Boost_INCLUDE_DIRS})
target_link_libraries(sfp-echo sfp tbb)

add_executable(sfp-asio-echo sfp-asio-echo.cpp)
target_include_directories(sfp-asio-echo
	PRIVATE $<TARGET_PROPERTY:sfp,INCLUDE_DIRECTORIES>
			${Boost_INCLUDE_DIRS})
target_link_libraries(sfp-asio-echo sfp pthread rt ${Boost_LIBRARIES})

add_test(NAME sfp-echo-clean COMMAND sfp-echo 0 0)
# 0.1% chance of octet drop, 0% chance of bit flip
add_test(NAME sfp-echo-fuzzy COMMAND sfp-echo 0.001 0)
add_test(NAME sfp-asio-echo COMMAND sfp-asio-echo)
