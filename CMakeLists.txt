cmake_minimum_required(VERSION 3.10)
project(hdmi_simple_display VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add the executable
add_executable(hdmi_simple_display hdmi_simple_display.cpp)

# Include directory for stb_image.h
target_include_directories(hdmi_simple_display PRIVATE ${CMAKE_SOURCE_DIR})

# Find required packages
find_package(PkgConfig)

# Optional: EGL and OpenGL ES (if available)
if(PkgConfig_FOUND)
    pkg_check_modules(EGL egl)
    pkg_check_modules(GLES glesv2)
    
    if(EGL_FOUND AND GLES_FOUND)
        message(STATUS "EGL and GLES found, enabling OpenGL support")
        target_compile_definitions(hdmi_simple_display PRIVATE HAVE_EGL)
        target_include_directories(hdmi_simple_display PRIVATE 
            ${EGL_INCLUDE_DIRS} 
            ${GLES_INCLUDE_DIRS})
        target_link_libraries(hdmi_simple_display 
            ${EGL_LIBRARIES} 
            ${GLES_LIBRARIES})
    else()
        message(STATUS "EGL/GLES not found, building without OpenGL support")
    endif()
endif()

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(hdmi_simple_display pthread)
endif()

# Copy shaders to build directory
file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR})

# Install target
install(TARGETS hdmi_simple_display DESTINATION bin)
install(DIRECTORY shaders/ DESTINATION share/hdmi_simple_display/shaders)