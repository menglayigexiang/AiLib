find_package(Threads REQUIRED)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_library(LibAiCore SHARED
        src/agent/Agent.cpp
        src/tools/SchemaValidator.cpp
        src/tools/ToolRegistry.cpp
        src/tools/ToolExecutor.cpp
        src/core/Cancellation.cpp
        src/core/MediaResource.cpp
        src/core/Message.cpp
        src/stream/StreamSession.cpp
        src/protocol/openai/OpenAIStreamDecoder.cpp
        src/protocol/openai/OpenAIResponsesStreamDecoder.cpp
        src/protocol/anthropic/AnthropicStreamDecoder.cpp
        src/network/QtHttpTransport.cpp
        src/protocol/openai/OpenAIChatCompatibleAdapter.cpp
        src/protocol/openai/OpenAIResponsesAdapter.cpp
        src/protocol/anthropic/AnthropicMessagesAdapter.cpp
        src/client/LLMClient.cpp
        src/client/LLMClientFactory.cpp
        src/provider/ModelRegistry.cpp)

else()
    add_library(LibAiCore SHARED
        src/agent/Agent.cpp
        src/tools/SchemaValidator.cpp
        src/tools/ToolRegistry.cpp
        src/tools/ToolExecutor.cpp
        src/core/Cancellation.cpp
        src/core/MediaResource.cpp
        src/core/Message.cpp
        src/stream/StreamSession.cpp
        src/protocol/openai/OpenAIStreamDecoder.cpp
        src/protocol/openai/OpenAIResponsesStreamDecoder.cpp
        src/protocol/anthropic/AnthropicStreamDecoder.cpp
        src/network/QtHttpTransport.cpp
        src/protocol/openai/OpenAIChatCompatibleAdapter.cpp
        src/protocol/openai/OpenAIResponsesAdapter.cpp
        src/protocol/anthropic/AnthropicMessagesAdapter.cpp
        src/client/LLMClient.cpp
        src/client/LLMClientFactory.cpp
        src/provider/ModelRegistry.cpp)
endif()
add_library(AiLib::Core ALIAS LibAiCore)
target_link_libraries(LibAiCore PUBLIC Qt${QT_VERSION_MAJOR}::Core PRIVATE Qt${QT_VERSION_MAJOR}::Network Threads::Threads)
target_compile_features(LibAiCore PUBLIC cxx_std_17)
target_compile_definitions(LibAiCore PRIVATE AILIB_CORE_BUILD)
target_include_directories(LibAiCore PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

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
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT SDK
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT SDK
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT SDK
)

install(DIRECTORY include/AiLib DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT SDK
    FILES_MATCHING PATTERN "*.h"
)
