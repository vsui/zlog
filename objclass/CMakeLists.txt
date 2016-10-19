cmake_minimum_required(VERSION 2.8.11)
project(ceph-object-class-example)

# require c++11
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if (COMPILER_SUPPORTS_CXX11)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
  message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support.")
endif()

#include_directories(../../src/include)

# cls_zlog
add_library(cls_zlog SHARED cls_zlog.cc)
set_target_properties(cls_zlog PROPERTIES VERSION "1.0.0" SOVERSION "1")


