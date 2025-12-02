cmake_minimum_required(VERSION 3.10)
project(hdmi_simple_display VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Main executable
add_executable(hdmi_simple_display hdmi_simple_display.cpp)

# Install target
install(TARGETS hdmi_simple_display DESTINATION bin)