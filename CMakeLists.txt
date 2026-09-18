cmake_minimum_required(VERSION 3.21)
project(AiLib LANGUAGES NONE)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use cmake -S . -B build; in-source configuration is unsupported.")
endif()

include(CTest)

add_subdirectory(LibAiCore "${CMAKE_CURRENT_SOURCE_DIR}/LibAiCore/build")
add_subdirectory(examples)
add_subdirectory(TestApp "${CMAKE_CURRENT_SOURCE_DIR}/TestApp/build")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
