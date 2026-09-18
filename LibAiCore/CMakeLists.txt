project(LibAiCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 QUIET COMPONENTS Core)
if(Qt6_FOUND)
    set(QT_VERSION_MAJOR 6)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core)
    set(QT_VERSION_MAJOR 5)
endif()

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
endif()
include(GNUInstallDirs)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_library(LibAiCore SHARED src/LibAiCore.cpp)
else()
    add_library(LibAiCore SHARED src/LibAiCore.cpp)
endif()
add_library(AiLib::Core ALIAS LibAiCore)
target_link_libraries(LibAiCore PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# An otherwise empty Windows DLL needs an export to produce its import library.
if(WIN32)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/empty_library.cpp"
        "extern \"C\" __declspec(dllexport) void ailib_placeholder() {}\n")
    target_sources(LibAiCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/empty_library.cpp")
endif()

set_target_properties(LibAiCore PROPERTIES
    AILIB_QT_MAJOR ${QT_VERSION_MAJOR}
    OUTPUT_NAME AiCore
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<CONFIG>"
)

install(TARGETS LibAiCore
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
