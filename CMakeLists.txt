# Build generator.

# Declare the minimum CMake version for the project.
cmake_minimum_required(VERSION 3.16)

# Declare the project name and that it uses C++ language (CXX).
project(itch_order_book CXX)

# Optimize with -O2 flag. Enable warnings.
add_compile_options(-O2 -Wall -Wextra)

# Set the target C++ standard to C++17.
set(CMAKE_CXX_STANDARD 17)

# Make C++17 mandatory.
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define a build target named itch_model_cli from itch_model_cli.cpp
add_executable(itch_model_cli itch_model_cli.cpp)

# Add the repo root (where this CMakeLists.txt lives) to the include path.
target_include_directories(itch_model_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Turn on CTest support for the project.
enable_testing()

# Add a second target test_order_table from test_order_table.cpp
add_executable(test_order_table book/tests/test_order_table.cpp)

# Add a dir with CMakeLists.txt to the include path for test_order_table.
target_include_directories(test_order_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/book)

# Register a CTest test named order_table that runs test_order_table executable
add_test(NAME order_table COMMAND test_order_table)
# Running "ctest" in the build directory will execute it and report pass/fail based
# on its exit code.

# Add a third target test_price_book from test_price_book.cpp
add_executable(test_price_book book/tests/test_price_book.cpp)

target_include_directories(test_price_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/book)

add_test(NAME price_book COMMAND test_price_book)

# Add test_order_book from test_order_book.cpp
add_executable(test_order_book book/tests/test_order_book.cpp)
target_include_directories(test_order_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/book)
add_test(NAME order_book COMMAND test_order_book)

# Add order_book_cli as a target
add_executable(order_book_cli book/order_book_cli.cpp)
target_include_directories(order_book_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Add order_book_latency as a target
add_executable(order_book_latency book/order_book_latency.cpp)
target_include_directories(order_book_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})