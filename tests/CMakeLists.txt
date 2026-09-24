project(AiLibTests LANGUAGES CXX)

if(AILIB_BUILD_MCP)
    add_subdirectory(libmcp)
endif()

get_target_property(QT_VERSION_MAJOR AiLib::Core AILIB_QT_MAJOR)
if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test Network)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test Network)
endif()
find_package(Threads REQUIRED)

foreach(test_name PublicModels Message MediaResource Cancellation Chat AnthropicMessages OpenAIResponses ModelRegistry Streaming Tools Agent AgentControl Retry QtHttpTransport)
    set(test_target tst_${test_name})
    if(test_name STREQUAL "QtHttpTransport")
        set(test_source transport/${test_target}.cpp)
    else()
        set(test_source unit/${test_target}.cpp)
    endif()
    if(QT_VERSION_MAJOR EQUAL 6)
        qt_add_executable(${test_target} ${test_source})
    else()
        add_executable(${test_target} ${test_source})
    endif()
    set_target_properties(${test_target} PROPERTIES AUTOMOC ON)
    target_link_libraries(${test_target} PRIVATE
        AiLib::Core Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Network Threads::Threads
    )
    if(WIN32)
        add_custom_command(TARGET ${test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:${test_target}>"
            VERBATIM
        )
    endif()
    add_test(NAME ${test_name} COMMAND ${test_target})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
endforeach()

# Compile each public header alone to catch hidden include-order dependencies.
file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../LibAiCore/include/AiLib/*.h"
)
set(header_checks)
foreach(header IN LISTS public_headers)
    file(RELATIVE_PATH include_name
        "${CMAKE_CURRENT_SOURCE_DIR}/../LibAiCore/include" "${header}"
    )
    string(MAKE_C_IDENTIFIER "${include_name}" check_name)
    set(check_file "${CMAKE_CURRENT_BINARY_DIR}/header_checks/${check_name}.cpp")
    file(WRITE "${check_file}" "#include <${include_name}>\n")
    list(APPEND header_checks "${check_file}")
endforeach()
add_library(ailib_header_checks OBJECT ${header_checks})
target_link_libraries(ailib_header_checks PRIVATE AiLib::Core)

# 手动外部 Provider 测试不注册到 CTest，不要求 CI 配置凭据。
option(AILIB_BUILD_MANUAL_TESTS "Build optional real-provider checks" OFF)
if(AILIB_BUILD_MANUAL_TESTS)
    if(QT_VERSION_MAJOR EQUAL 6)
        qt_add_executable(manual_kimi_messages manual/kimi_messages.cpp)
        qt_add_executable(manual_deepseek_chat manual/deepseek_chat.cpp)
        qt_add_executable(manual_agent_acceptance manual/agent_acceptance.cpp)
    else()
        add_executable(manual_kimi_messages manual/kimi_messages.cpp)
        add_executable(manual_deepseek_chat manual/deepseek_chat.cpp)
        add_executable(manual_agent_acceptance manual/agent_acceptance.cpp)
    endif()
    target_link_libraries(manual_kimi_messages PRIVATE AiLib::Core)
    target_link_libraries(manual_deepseek_chat PRIVATE AiLib::Core)
    target_link_libraries(manual_agent_acceptance PRIVATE AiLib::Core ailib_demo_support)
    if(WIN32)
        foreach(manual_target manual_kimi_messages manual_deepseek_chat manual_agent_acceptance)
            add_custom_command(TARGET ${manual_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:${manual_target}>"
                VERBATIM
            )
        endforeach()
    endif()
endif()

# 示例层线程与 UI 桥接测试；Qt Widgets 不进入 SDK 依赖。
set(demo_test_sources
    unit/tst_Demo.cpp
    ../TestApp/MainWindow.cpp
    ../TestApp/MainWindow.h
    ../TestApp/LogWidget.cpp
    ../TestApp/LogWidget.h
    ../TestApp/LibAiCore/LibAiCorePage.cpp
    ../TestApp/LibAiCore/LibAiCorePage.h
)

if(AILIB_BUILD_MCP)
    list(APPEND demo_test_sources
        ../TestApp/LibMcp/LibMcpPage.cpp
        ../TestApp/LibMcp/LibMcpPage.h
        ../TestApp/LibMcp/McpClientConfigDialog.cpp
        ../TestApp/LibMcp/McpClientConfigDialog.h
        ../TestApp/LibMcp/McpClientWorkbench.cpp
        ../TestApp/LibMcp/McpClientWorkbench.h
    )
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
    qt_add_executable(tst_Demo ${demo_test_sources})
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
    add_executable(tst_Demo ${demo_test_sources})
endif()
set_target_properties(tst_Demo PROPERTIES AUTOMOC ON)
add_dependencies(tst_Demo ailib_cli)
target_compile_definitions(tst_Demo PRIVATE AILIB_CLI_PATH="$<TARGET_FILE:ailib_cli>")
target_link_libraries(tst_Demo PRIVATE AiLib::Core Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(tst_Demo PRIVATE ailib_demo_support)
if(AILIB_BUILD_MCP)
    target_compile_definitions(tst_Demo PRIVATE AILIB_TESTAPP_HAS_MCP)
    target_link_libraries(tst_Demo PRIVATE LibMcp::LibMcp)
endif()
if(WIN32)
    add_custom_command(TARGET tst_Demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:tst_Demo>"
        VERBATIM
    )
endif()
add_test(NAME WidgetsDemo COMMAND tst_Demo)
set_tests_properties(WidgetsDemo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

if(AILIB_BUILD_MANUAL_TESTS)
    if(QT_VERSION_MAJOR EQUAL 6)
        qt_add_executable(manual_gui_acceptance manual/gui_acceptance.cpp
            ../TestApp/LibAiCore/LibAiCorePage.cpp
            ../TestApp/LibAiCore/LibAiCorePage.h)
    else()
        add_executable(manual_gui_acceptance manual/gui_acceptance.cpp
            ../TestApp/LibAiCore/LibAiCorePage.cpp
            ../TestApp/LibAiCore/LibAiCorePage.h)
    endif()
    set_target_properties(manual_gui_acceptance PROPERTIES AUTOMOC ON)
    target_link_libraries(manual_gui_acceptance PRIVATE AiLib::Core ailib_demo_support Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
    if(WIN32)
        add_custom_command(TARGET manual_gui_acceptance POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:manual_gui_acceptance>"
            VERBATIM
        )
    endif()
endif()

if(AILIB_BUILD_MANUAL_TESTS)
    if(QT_VERSION_MAJOR EQUAL 6)
        qt_add_executable(manual_kimi_tool_probe manual/kimi_tool_probe.cpp)
    else()
        add_executable(manual_kimi_tool_probe manual/kimi_tool_probe.cpp)
    endif()
    target_link_libraries(manual_kimi_tool_probe PRIVATE AiLib::Core)
    if(WIN32)
        add_custom_command(TARGET manual_kimi_tool_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:LibAiCore>" "$<TARGET_FILE_DIR:manual_kimi_tool_probe>"
            VERBATIM
        )
    endif()
endif()
