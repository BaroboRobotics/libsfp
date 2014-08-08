# Make sure TBB is available.
find_path(TBB_INCLUDE_DIR tbb/tbb.h)
if(NOT TBB_INCLUDE_DIR)
    message(FATAL_ERROR "libsfp tests requires Intel's Threaded Building Blocks.
    Debian-like: apt-get install libtbb-dev
    Gentoo: emerge dev-cpp/tbb")
endif()

set_source_files_properties(
    sfp-echo.cpp
    PROPERTIES
    COMPILE_FLAGS "-std=c++11")

add_executable(sfp-echo sfp-echo.cpp)
target_include_directories(sfp-echo
    PRIVATE $<TARGET_PROPERTY:sfp,INCLUDE_DIRECTORIES>
    PRIVATE ${TBB_INCLUDE_DIR})
target_link_libraries(sfp-echo sfp tbb)

add_test(NAME sfp-echo-clean COMMAND sfp-echo 0 0)
# 5% chance of octet drop, 5% chance of bit flip
add_test(NAME sfp-echo-fuzzy COMMAND sfp-echo 5 5)
