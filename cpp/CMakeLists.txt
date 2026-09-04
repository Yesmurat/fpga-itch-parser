# Build generator.

# Declare the minimum CMake version for the project.
cmake_minimum_required(VERSION 3.16)

# Declare the project name and that it uses C++ language (CXX).
# This sets up internal variables and enables the C++ compiler.
project(itch_model_cli CXX)

# Set the target C++ standard to C++17.
set(CMAKE_CXX_STANDARD 17)

# Make C++17 mandatory.
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define  a build target named itch_model_cli from itch_model_cli.cpp
add_executable(itch_model_cli itch_model_cli.cpp)

# Add a dir with CMakeLists.txt (cpp/) to the include path for itch_model_cli.
target_include_directories(itch_model_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Turn on CTest support for the project.
enable_testing()

# Add a second target test_order_table from test_order_table.cpp
add_executable(test_order_table book/test_order_table.cpp)

# Add a dir with CMakeLists.txt to the include path for test_order_table.
target_include_directories(test_order_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Register a CTest test named order_table that runs test_order_table executable
add_test(NAME order_table COMMAND test_order_table)
# Running "ctest" in the build directory will execute it and report pass/fail based
# on its exit code.
