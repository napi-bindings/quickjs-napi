cmake_minimum_required(VERSION 3.24)
project(quickjs-napi C)

set(CMAKE_C_STANDARD 23)

include_directories(include)

add_executable(quickjs-napi main.c)
target_link_libraries(quickjs-napi ${CMAKE_SOURCE_DIR}/libqjs.a)
