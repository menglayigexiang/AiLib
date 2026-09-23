find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(libmcp_stdio_test_server stdio_echo_server.cpp)
else()
    add_executable(libmcp_stdio_test_server stdio_echo_server.cpp)
endif()

target_link_libraries(libmcp_stdio_test_server
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(libmcp_interop_server interop_server.cpp)
else()
    add_executable(libmcp_interop_server interop_server.cpp)
endif()

target_link_libraries(libmcp_interop_server
    PRIVATE
        LibMcp::LibMcp
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(libmcp_interop_client interop_client.cpp)
else()
    add_executable(libmcp_interop_client interop_client.cpp)
endif()

target_link_libraries(libmcp_interop_client
    PRIVATE
        LibMcp::LibMcp
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(tst_LibMcp tst_LibMcp.cpp)
else()
    add_executable(tst_LibMcp tst_LibMcp.cpp)
endif()

target_link_libraries(tst_LibMcp
    PRIVATE
        LibMcp::LibMcp
        Qt${QT_VERSION_MAJOR}::Test
)

target_compile_definitions(tst_LibMcp
    PRIVATE
        LIBMCP_STDIO_TEST_SERVER="$<TARGET_FILE:libmcp_stdio_test_server>"
)
add_dependencies(tst_LibMcp libmcp_stdio_test_server)

add_test(NAME LibMcp COMMAND tst_LibMcp)
set_tests_properties(LibMcp PROPERTIES TIMEOUT 30)

file(GLOB libmcp_public_headers CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/LibMcp/include/LibMcp/*.h"
)

set(libmcp_header_checks)
foreach(header IN LISTS libmcp_public_headers)
    file(RELATIVE_PATH include_name
        "${CMAKE_SOURCE_DIR}/LibMcp/include"
        "${header}"
    )
    string(MAKE_C_IDENTIFIER "${include_name}" check_name)
    set(check_file "${CMAKE_CURRENT_BINARY_DIR}/header_checks/${check_name}.cpp")
    file(WRITE "${check_file}" "#include <${include_name}>\n")
    list(APPEND libmcp_header_checks "${check_file}")
endforeach()

add_library(libmcp_header_checks OBJECT ${libmcp_header_checks})
target_link_libraries(libmcp_header_checks PRIVATE LibMcp::LibMcp)
