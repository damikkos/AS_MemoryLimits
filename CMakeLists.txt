cmake_minimum_required(VERSION 3.30)
project(AS_MemoryLimits VERSION 4.0.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "AS_MemoryLimits requires clang-cl and the Windows x64 SDK.")
endif()

set(GRAFT_SOURCE_DIR "" CACHE PATH "Path to the KR_GRAFTED source checkout")
if(NOT EXISTS "${GRAFT_SOURCE_DIR}/cmake/graft_plugin.cmake")
    message(FATAL_ERROR "Set GRAFT_SOURCE_DIR to the KR_GRAFTED source checkout.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory("${GRAFT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/graft" EXCLUDE_FROM_ALL)
include("${GRAFT_SOURCE_DIR}/cmake/graft_plugin.cmake")

set(GRAFT_OUT "${CMAKE_CURRENT_BINARY_DIR}/out")
add_subdirectory(source/AS_MemoryLimits)
