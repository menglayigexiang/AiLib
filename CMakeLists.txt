cmake_minimum_required(VERSION 3.21)
project(AiLib VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use cmake -S . -B build; in-source configuration is unsupported.")
endif()

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AILIB_BUILD_MCP "Build the experimental LibMcp component" ON)

set(ailib_qt_components Core Network)
if(AILIB_BUILD_MCP)
    list(APPEND ailib_qt_components Concurrent)
endif()

find_package(Qt6 6.2 QUIET COMPONENTS ${ailib_qt_components})
if(Qt6_FOUND)
    set(QT_VERSION_MAJOR 6)
    qt_standard_project_setup()
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS ${ailib_qt_components})
    set(QT_VERSION_MAJOR 5)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    include(GNUInstallDirs)
endif()

add_subdirectory(LibAiCore)

if(AILIB_BUILD_MCP)
    add_subdirectory(LibMcp)
endif()

add_subdirectory(examples)
add_subdirectory(TestApp)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
