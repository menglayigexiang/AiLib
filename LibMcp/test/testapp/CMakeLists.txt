add_executable(LibMcpTestApp main.cpp MainWindow.cpp MainWindow.h)
target_link_libraries(LibMcpTestApp PRIVATE
    LibMcp::LibMcp
    Qt${LIBMCP_QT_MAJOR}::Widgets
)
set_target_properties(LibMcpTestApp PROPERTIES WIN32_EXECUTABLE TRUE)
