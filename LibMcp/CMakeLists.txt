cmake_minimum_required(VERSION 3.21)

project(LibMcp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOGEN_PARALLEL 1)

option(LIBMCP_BUILD_TESTS "Build LibMcp tests and test application" ON)

add_subdirectory(src)

if(LIBMCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
