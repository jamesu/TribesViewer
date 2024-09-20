cmake_minimum_required(VERSION 3.10)

# Set the project name
project(TribesViewer)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Specify the include directories
include_directories(include imgui include/slm)

# Define a variable to optionally hold the SDL3 static library path
set(SDL3_LIB_DIR "" CACHE FILEPATH "Path to SDL3 static library (.a)")

# Try to use pkg-config to find SDL3 (only if static lib not provided)
if (SDL3_LIB_DIR STREQUAL "")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)

    if (SDL3_FOUND)
        message(STATUS "Using SDL3 from pkg-config")
        include_directories(${SDL3_INCLUDE_DIRS})
        link_directories(${SDL3_LIBRARY_DIRS})
        set(SDL3_LIBS ${SDL3_LIBRARIES})
    else()
        message(FATAL_ERROR "SDL3 not found via pkg-config and no static library provided")
    endif()
else()
    message(STATUS "Using static SDL3 library")
    
    # Allow the user to provide include directory if using the static lib
    set(SDL3_INCLUDE_DIR "" CACHE PATH "Path to SDL3 include directory")

    if (SDL3_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR "SDL3_INCLUDE_DIR must be set when using SDL3 static library")
    endif()

    # Add SDL3 include directory and link the static SDL3 lib
    include_directories(${SDL3_INCLUDE_DIR})
    link_directories(${SDL3_LIB_DIR})
    set(SDL3_LIBS SDL3)
endif()

# Collect source files
file(GLOB TRIBESVIEWER_SRC
    "TribesViewer/*.cpp"
    "slm/*.cpp"
    "imgui/*.cpp"
)

# Add executable
add_executable(TribesViewer ${TRIBESVIEWER_SRC})

# Link SDL3
target_link_libraries(TribesViewer ${SDL3_LIBS} -lm -pthread)
target_compile_definitions(TribesViewer PRIVATE NO_BOOST)
