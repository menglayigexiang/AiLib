include(CMakePackageConfigHelpers)

if(QT_VERSION_MAJOR EQUAL 6)
    set(LIBMCP_QT_MIN_VERSION 6.2)
else()
    set(LIBMCP_QT_MIN_VERSION 5.15)
endif()

set(libmcp_sources
    core/JsonRpcCodec.cpp
    core/McpProtocolCodec.cpp
    core/McpOperation.cpp
    core/ProtocolMetadata.cpp
    core/ProtocolSchemaValidator.cpp
    core/JsonSchemaValidator.cpp
    http/HttpClient.cpp
    http/HttpServer.cpp
    client/McpClient.cpp
    client/McpClientManager.cpp
    server/McpServer.cpp
    transport/InMemoryTransport.cpp
    transport/StdioClientTransport.cpp
    transport/StdioServerTransport.cpp
    transport/StreamableHttpClientTransport.cpp
    transport/StreamableHttpServerTransport.cpp
)

set(libmcp_public_headers
    ../include/LibMcp/InMemoryTransport.h
    ../include/LibMcp/LibMcpGlobal.h
    ../include/LibMcp/McpClient.h
    ../include/LibMcp/McpClientManager.h
    ../include/LibMcp/McpOperation.h
    ../include/LibMcp/McpClientTransport.h
    ../include/LibMcp/McpResult.h
    ../include/LibMcp/McpServer.h
    ../include/LibMcp/McpServerTransport.h
    ../include/LibMcp/McpTypes.h
    ../include/LibMcp/StreamableHttpTransport.h
    ../include/LibMcp/StdioTransport.h
    ../protocol/protocol.qrc
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_library(LibMcp SHARED
        ${libmcp_sources}
        ${libmcp_public_headers}
    )
else()
    add_library(LibMcp SHARED
        ${libmcp_sources}
        ${libmcp_public_headers}
    )
endif()

add_library(LibMcp::LibMcp ALIAS LibMcp)

target_compile_definitions(LibMcp
    PRIVATE
        LIBMCP_LIBRARY
    PUBLIC
        LIBMCP_SHARED
)

target_include_directories(LibMcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/jsoncons/include
)

target_link_libraries(LibMcp
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Concurrent
)

set_target_properties(LibMcp PROPERTIES
    AUTORCC ON
    AILIB_QT_MAJOR ${QT_VERSION_MAJOR}
    OUTPUT_NAME Mcp
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DEBUG_POSTFIX d
)

install(TARGETS LibMcp
    EXPORT LibMcpTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LibMcp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LibMcp
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LibMcp
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../include/LibMcp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT LibMcp
    FILES_MATCHING PATTERN "*.h"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LibMcpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/LibMcpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LibMcpConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LibMcp"
)

install(EXPORT LibMcpTargets
    FILE LibMcpTargets.cmake
    NAMESPACE LibMcp::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LibMcp"
    COMPONENT LibMcp
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LibMcpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LibMcpConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LibMcp"
    COMPONENT LibMcp
)
