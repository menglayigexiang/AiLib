#pragma once

namespace LibMcp {
class McpServer;
}

// 注册 TestApp Server 用于验证典型 MCP Tool 场景的固定工具集。
bool registerTestServerTools(LibMcp::McpServer& server);  // 返回全部工具是否成功注册
