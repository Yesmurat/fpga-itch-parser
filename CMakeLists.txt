# Build configuration for the ITCH decoder, order book, CLIs, and tests.

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

# ------------------------------------------------------------------------------------
# Targets in book/ find their sibling headers directly.
# They need the root only for itch_decoder.hpp.

# Create "itch_model_cli" target and add path to its include files.
add_executable(itch_model_cli itch_model_cli.cpp)
target_include_directories(itch_model_cli PRIVATE ${CMAKE_SOURCE_DIR})

# Create "order_book_cli" target and add path to its include files.
add_executable(order_book_cli book/order_book_cli.cpp)
target_include_directories(order_book_cli PRIVATE ${CMAKE_SOURCE_DIR})

# Create "order_book_latency" target and add path to its include files.
add_executable(order_book_latency book/order_book_latency.cpp)
target_include_directories(order_book_latency PRIVATE ${CMAKE_SOURCE_DIR})

# ------------------------------------------------------------------------------------
# Tests live in tests/, so they need book/ explicitly too.

# Turn on CTest support for the project.
enable_testing()

# Create a test_order_table target, add path to its include files, and treat it as a test.
add_executable(test_order_table tests/test_order_table.cpp)
target_include_directories(test_order_table PRIVATE ${CMAKE_SOURCE_DIR}/book)
add_test(NAME order_table COMMAND test_order_table)

# Create a test_price_book target, add path to its include files, and treat it as a test.
add_executable(test_price_book tests/test_price_book.cpp)
target_include_directories(test_price_book PRIVATE ${CMAKE_SOURCE_DIR}/book)
add_test(NAME price_book COMMAND test_price_book)

# Create a test_order_book target, add paths to its include files, and treat it as a test.
add_executable(test_order_book tests/test_order_book.cpp)
target_include_directories(test_order_book PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/book)
add_test(NAME order_book COMMAND test_order_book)