find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Concurrent)

set(LIBMCP_QT_MAJOR ${QT_VERSION_MAJOR} CACHE INTERNAL "Qt major version used by LibMcp")

add_library(LibMcp
    core/JsonRpcCodec.cpp
    core/McpProtocolCodec.cpp
    core/JsonSchemaValidator.cpp
    client/McpClient.cpp
    client/McpClientManager.cpp
    server/McpServer.cpp
    transport/InMemoryTransport.cpp
    transport/StreamableHttpClientTransport.cpp
    transport/StreamableHttpServerTransport.cpp
    include/LibMcp/LibMcpGlobal.h
    include/LibMcp/McpResult.h
    include/LibMcp/McpTypes.h
    include/LibMcp/McpClientTransport.h
    include/LibMcp/McpServerTransport.h
    include/LibMcp/InMemoryTransport.h
    include/LibMcp/StreamableHttpTransport.h
    include/LibMcp/McpClient.h
    include/LibMcp/McpServer.h
    include/LibMcp/McpClientManager.h
)

add_library(LibMcp::LibMcp ALIAS LibMcp)

target_compile_definitions(LibMcp PRIVATE LIBMCP_LIBRARY)
target_include_directories(LibMcp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(LibMcp
    PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network
    PRIVATE Qt${QT_VERSION_MAJOR}::Concurrent
)

set_target_properties(LibMcp PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DEBUG_POSTFIX d
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(LibMcp PUBLIC LIBMCP_SHARED)
endif()
