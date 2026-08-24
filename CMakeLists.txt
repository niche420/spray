cmake_minimum_required(VERSION 3.24)
project(spray LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    option(SPRAY_ENABLE_D3D12  "Build the Direct3D 12 backend"  OFF)
    option(SPRAY_ENABLE_VULKAN "Build the Vulkan backend" ON)
elseif(UNIX || APPLE)
    set(SPRAY_ENABLE_VULKAN ON CACHE BOOL "Vulkan is the only backend on Linux and Mac" FORCE)
else()
    message(FATAL_ERROR "spray: unsupported platform")
endif()

if(NOT SPRAY_ENABLE_VULKAN AND NOT SPRAY_ENABLE_D3D12)
    message(FATAL_ERROR "spray: no backend enabled — enable at least one of "
                         "SPRAY_ENABLE_VULKAN / SPRAY_ENABLE_D3D12")
endif()

if(SPRAY_ENABLE_VULKAN)
    # Need the glslc compiler
    find_package(Vulkan REQUIRED COMPONENTS glslc)
endif()

set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL)

add_subdirectory(spray)