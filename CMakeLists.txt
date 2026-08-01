cmake_minimum_required(VERSION 3.20)

project(advanced-cpp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EnTT and Taskflow at the commits the course text is pinned to. See docs/CLAUDE.md.
include(cmake/dependencies.cmake)

file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE "src" ${CMAKE_BINARY_DIR})

# One executable per exercise, so several main()s can coexist.
#   acpp_exercise(<name> <source>...)
# Shared code that later modules reuse lives in src/ and is on the include path.
function(acpp_exercise name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endfunction()

add_subdirectory(modules/00-setup)
