project(TestApp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Resolve the same Qt major as the library without relying on sibling variables.
get_target_property(QT_VERSION_MAJOR AiLib::Core AILIB_QT_MAJOR)
if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets)
endif()

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
endif()
include(GNUInstallDirs)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(TestApp WIN32 MACOSX_BUNDLE src/main.cpp src/DemoWindow.cpp src/DemoWindow.h)
else()
    add_executable(TestApp WIN32 MACOSX_BUNDLE src/main.cpp src/DemoWindow.cpp src/DemoWindow.h)
endif()
target_link_libraries(TestApp PRIVATE AiLib::Core ailib_demo_support Qt${QT_VERSION_MAJOR}::Widgets)
set_target_properties(TestApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<CONFIG>"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.ailib.TestApp"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

if(WIN32)
    add_custom_command(TARGET TestApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:TestApp>"
        VERBATIM
    )
elseif(APPLE)
    set_property(TARGET TestApp PROPERTY INSTALL_RPATH "@loader_path/../Frameworks;@loader_path/../../../${CMAKE_INSTALL_LIBDIR}")
else()
    set_property(TARGET TestApp PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS TestApp
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(COMMAND qt_generate_deploy_app_script)
    if(APPLE AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
        get_target_property(qt_qmake Qt6::qmake IMPORTED_LOCATION)
        execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_LIBS
            OUTPUT_VARIABLE qt_lib_dir OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        # Modular SDKs such as Homebrew keep optional frameworks outside QtCore's directory.
        qt_generate_deploy_app_script(TARGET TestApp OUTPUT_SCRIPT deploy_script
            DEPLOY_TOOL_OPTIONS "-libpath=${qt_lib_dir}")
    else()
        qt_generate_deploy_app_script(TARGET TestApp OUTPUT_SCRIPT deploy_script)
    endif()
    install(SCRIPT ${deploy_script})
else()
    get_target_property(qt_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_BINS
        OUTPUT_VARIABLE qt_bin_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE qt_plugin_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_LIBS
        OUTPUT_VARIABLE qt_lib_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(APPLE)
        find_program(qt_deploy_tool macdeployqt HINTS "${qt_bin_dir}" REQUIRED NO_DEFAULT_PATH)
    elseif(WIN32)
        find_program(qt_deploy_tool windeployqt HINTS "${qt_bin_dir}" REQUIRED NO_DEFAULT_PATH)
    endif()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployLegacyQt.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/DeployLegacyQt.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployLegacyQt.cmake")
endif()
