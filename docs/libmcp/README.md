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
- TestApp Client 配置列表、添加/编辑表单、连接开关和 Server 运行观测。

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
[MCP 2026-07-28 设计文档](mcp-2026-07-28-design.md)。官方 SDK 的四向实测结果见
[互操作验证](interoperability.md)。
