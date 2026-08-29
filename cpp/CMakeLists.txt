cmake_minimum_required(VERSION 3.16)
project(itch_model_cli CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(itch_model_cli itch_model_cli.cpp)
target_include_directories(itch_model_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
