find_package(Qt${LIBMCP_QT_MAJOR} REQUIRED COMPONENTS Core Widgets)

add_executable(LibMcpTests unit/TestMain.cpp)
target_link_libraries(LibMcpTests PRIVATE
    LibMcp::LibMcp
    Qt${LIBMCP_QT_MAJOR}::Core
)
add_test(NAME LibMcpTests COMMAND LibMcpTests)

add_subdirectory(testapp)
