cmake_minimum_required(VERSION 3.25)
project(synchrony CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(synchrony-core
  src/wire.cpp
  src/net.cpp
  src/peer.cpp
)
target_include_directories(synchrony-core PUBLIC src)
target_compile_options(synchrony-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(synchrony-node src/main.cpp)
target_link_libraries(synchrony-node PRIVATE synchrony-core)
target_compile_options(synchrony-node PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
foreach(test wire peer)
  add_executable(${test}_test tests/${test}_test.cpp)
  target_link_libraries(${test}_test PRIVATE synchrony-core)
  target_compile_options(${test}_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${test} COMMAND ${test}_test)
endforeach()

install(TARGETS synchrony-node RUNTIME DESTINATION bin)
