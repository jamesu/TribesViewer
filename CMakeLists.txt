cmake_minimum_required(VERSION 3.10)

# Set the project name
project(TribesViewer)

function(generate_escaped_string_header INPUT_FILE OUTPUT_HEADER CONST_NAME)
    # Check if the output header exists
    if(EXISTS "${OUTPUT_HEADER}")
        return()
    endif()

    # Read the content of the input file
    file(READ "${INPUT_FILE}" FILE_CONTENT)

    # Escape special characters in the file content
    string(REPLACE "\\" "\\\\" ESCAPED_FILE_CONTENT "${FILE_CONTENT}")
    string(REPLACE "\"" "\\\"" ESCAPED_FILE_CONTENT "${ESCAPED_FILE_CONTENT}")
    string(REPLACE "\n" "\\n\"\n\"" ESCAPED_FILE_CONTENT "${ESCAPED_FILE_CONTENT}")

    # Write the header file with the escaped string
    file(WRITE "${OUTPUT_HEADER}" "// Auto-generated header file\n")
    file(APPEND "${OUTPUT_HEADER}" "const char* ${CONST_NAME} = \"${ESCAPED_FILE_CONTENT}\";\n")

    message(STATUS "Generated header file '${OUTPUT_HEADER}' from '${INPUT_FILE}'")
endfunction()


generate_escaped_string_header("TribesViewer/lineShader.wgsl" "${CMAKE_BINARY_DIR}/lineShader.wgsl.h" "sLineShaderCode")
generate_escaped_string_header("TribesViewer/modelShader.wgsl" "${CMAKE_BINARY_DIR}/modelShader.wgsl.h" "sModelShaderCode")


# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Specify the include directories
include_directories(include imgui include/slm "${CMAKE_BINARY_DIR}")

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

set(TARGET_DEFINES NO_BOOST)

if (USE_WGPU_NATIVE)
set(TARGET_DEFINES ${TARGET_DEFINES} WGPU_NATIVE IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
set(TARGET_HEADER_SEARCH_PATHS
	${TARGET_HEADER_SEARCH_PATHS}
	${WGPU_NATIVE_PATH}/ffi/webgpu-headers
	)
set(LIBRARY_SEARCH_PATHS ${LIBRARY_SEARCH_PATHS} ${WGPU_NATIVE_PATH}/target/debug)

find_library(WEBGPU_NATIVE_LIB_PATH
    NAMES libwgpu_native.a libwgpu_native.lib
    PATHS ${LIBRARY_SEARCH_PATHS}
    NAMES_PER_DIR
)

set(TARGET_LIBS ${TARGET_LIBS} ${WEBGPU_NATIVE_LIB_PATH})
else()
set(TARGET_DEFINES ${TARGET_DEFINES} WGPU_NATIVE IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
set(TARGET_LIBS ${TARGET_LIBS} webgpu)
endif()

include_directories(${TARGET_HEADER_SEARCH_PATHS})


# Collect source files
file(GLOB TRIBESVIEWER_SRC
    "TribesViewer/*.cpp"
    "slm/*.cpp"
    "imgui/*.cpp"
)

# Add executable
add_executable(TribesViewer ${TRIBESVIEWER_SRC})

# Link SDL3
target_link_libraries(TribesViewer ${SDL3_LIBS} -lm -pthread ${TARGET_LIBS})
target_compile_definitions(TribesViewer PRIVATE ${TARGET_DEFINES})

# Link frameworks
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)

    target_link_libraries(TribesViewer
        ${COREFOUNDATION_LIBRARY}
        ${METAL_LIBRARY}
        ${QUARTZCORE_LIBRARY}
    )
endif()

