cmake_minimum_required(VERSION 3.10)
project(hdmi_simple_display VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Main executable
add_executable(hdmi_simple_display hdmi_simple_display.cpp)

# Link pthread for std::thread
find_package(Threads REQUIRED)
target_link_libraries(hdmi_simple_display PRIVATE Threads::Threads)

# Install target
install(TARGETS hdmi_simple_display DESTINATION bin)
install(FILES service/hdmi-display.service DESTINATION lib/systemd/system)