cmake_minimum_required(VERSION 3.16)
project(hdmi_simple_display)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIR} ${GLEW_INCLUDE_DIRS})

add_executable(hdmi_simple_display hdmi_simple_display.cpp)

target_link_libraries(hdmi_simple_display ${GLEW_LIBRARIES} ${SDL2_LIBRARIES} GL pthread)
