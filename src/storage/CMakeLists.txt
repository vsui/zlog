add_library(test_backend OBJECT test_backend.cc)
target_include_directories(test_backend
  PUBLIC ${Boost_INCLUDE_DIRS}
  PRIVATE $<TARGET_PROPERTY:gtest,INTERFACE_INCLUDE_DIRECTORIES>)

add_subdirectory(ceph)
add_subdirectory(lmdb)
add_subdirectory(ram)
add_subdirectory(bench)
