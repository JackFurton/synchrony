cmake_minimum_required(VERSION 3.25)
project(synchrony CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(synchrony-node
  src/main.cpp
  src/wire.cpp
  src/net.cpp
)
target_include_directories(synchrony-node PRIVATE src)
target_compile_options(synchrony-node PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS synchrony-node RUNTIME DESTINATION bin)
