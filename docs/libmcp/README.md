# LibMcp

LibMcp 是一个 C++17、Qt 5.15/Qt 6.2+ 的 MCP Client/Server 动态库，只实现
MCP `2026-07-28`。它不包含旧版初始化握手、协议级 Session 或版本兼容层。

## 已实现能力

- 每个请求携带协议版本、ClientInfo 和 ClientCapabilities 的无状态协议模型。
- Tools、Resources、Resource Templates、Prompts、Completion 和自动 cursor 分页。
- Tool input/output JSON Schema 2020-12 注册与运行时校验。
- MRTR `input_required`、请求内 Progress、`subscriptions/listen` 和取消。
- STDIO 与无状态 Streamable HTTP；HTTP 支持请求级 SSE 增量事件。
- `QSharedPointer<McpOperation<T>>` 异步结果、进度、输入请求和取消。
- 内存 Transport、真实 STDIO 子进程测试和本地 HTTP 协议测试。
- TestApp Client 配置列表、连接开关、能力发现、工具浏览、JSON 参数调用、
  Progress/MRTR 输入与结果诊断，以及 Server 运行观测。

Client 列表中的开关表示“用户希望启用”，而不是“TCP/HTTP 已连接”。启用流程依次为
`Starting → Discovering → LoadingCapabilities → Ready`；只有固定版本发现和 Server 声明的
Tools、Resources、Resource Templates、Prompts 基础列表全部加载成功才显示
“可用”。Endpoint 可访问但不支持 MCP `2026-07-28` 时进入 `Error` 并显示“不可用”，不会回退
到旧版 `initialize` 或把 Transport 连通误报为协议可用。未声明任何可列表基础能力的现代
Server 可直接进入 `Ready`；只声明部分能力时，Manager 只加载其明确声明的列表。

Manager 另外公开独立的 `TransportState` 和 `ProtocolState`。Transport 只有实际收到当前
Endpoint 的响应后才标记为 `Reachable`；协议状态独立区分未验证、验证中、兼容、不兼容和
响应无效。因此 TestApp 可以准确显示“Endpoint 可达，但不兼容 MCP 2026-07-28”，而不是用
单一“已连接/未连接”掩盖真实原因。

官方 Schema 固定保存在 `LibMcp/protocol/mcp-2026-07-28.schema.json`。普通构建
不下载协议文件，也不运行代码生成器。jsoncons 作为 LibMcp 私有的 header-only
源码依赖，不产生额外库，也不进入公共 API。

## 构建与测试

```sh
cmake -S . -B build -G Ninja -DAILIB_BUILD_MCP=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

LibMcp 固定构建为动态库。测试只使用代码、Qt Test、本地 HTTP、受控子进程和
协议消息，不依赖截图或视频分析。

公共异步 API 返回 `QSharedPointer<McpOperation<T>>`。调用方可连接 `finished`、
`progressChanged`、`inputRequired` 信号，并在完成后继续读取状态、结果或错误。

详细边界见 [架构说明](architecture.md) 和
[MCP 2026-07-28 设计文档](mcp-2026-07-28-design.md)。面向集成者和 TestApp
使用者的后续工作见 [使用体验优化计划](usability-optimization-plan.md)。官方 SDK 的四向实测结果见
[互操作验证](interoperability.md)。
