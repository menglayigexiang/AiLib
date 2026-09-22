# LibMcp 架构

## 模块

```text
Application / TestApp / LibMcpPage
        |
        +-- McpClientManager
        +-- McpClient
        +-- McpServer
                 |
             MCP Protocol
                 |
       JSON-RPC / RequestManager
                 |
              Transport
        +--------+---------+
        |                  |
   In-Memory         Streamable HTTP
```

项目按 `core`、`client`、`server` 和 `transport` 组织，但只生成一个 `LibMcp`
库。目录边界为将来拆分准备，当前不增加额外 DLL。

## Core

Core 包含：

- `McpResult<T>`、`McpError` 和公共协议类型。
- JSON-RPC 消息构造与校验。
- MCP 类型与 JSON 之间的转换。
- Qt 5/Qt 6 共用的 Future 完成工具。

Core 不依赖 Client、Server 或具体 Transport。

## Client

`McpClient` 独占一个 `McpClientTransport`。`start()` 依次启动 Transport、发送
`initialize`、检查协议版本、保存 Server 能力并发送 initialized 通知。

所有 JSON-RPC 请求由 Client 内部请求管理器统一分配 ID、设置超时并匹配响应。
Tools、Resources 和 Prompts 只是该请求机制上的薄封装。

列表接口在内部追踪不透明 cursor，自动聚合页面，同时限制最多 64 页并检测重复
cursor。调用者不会接触分页状态。

## Client Manager

`McpClientManager` 保存一组 `McpClientConfig`，并按需创建 Client。配置格式具有独立的
`version`，与 MCP 协议版本无关。

反序列化首先完整解析到临时集合，全部成功后再替换当前配置。存在运行中的 Client 时
拒绝替换，防止连接和配置失配。

## Server

`McpServer` 包含三个独立注册表：

- Tool Registry
- Resource Registry
- Prompt Registry

注册函数接收普通 C++ 回调，不向业务函数暴露 Transport、Session 或 JSON-RPC ID。
Server 负责请求分发、协议结果构造和错误映射。

## Transport

Transport 只传送 JSON-RPC 消息，不解释 Tools、Resources 或 Prompts。

`InMemoryTransport` 用于确定性的自动测试。`StreamableHttpTransport` 支持 HTTP POST、
MCP Session ID、JSON 响应和通知的 HTTP 202 响应。

## 线程规则

- LibMcp 不创建 `QThread`。
- Client 和 Server 的异步公开操作可以跨线程调用，并排队到对象所属线程。
- Transport 和网络 QObject 只在自己的 thread affinity 中使用。
- `QFuture` 仅表达操作完成，不代表调用线程。
- 应用移动 Client/Server 时，应保证其 Transport 与所有者位于同一线程。
- GUI 线程不得同步阻塞等待网络 Future。

## TestApp 的 LibMcp 页面

LibMcpPage 位于 `TestApp/LibMcp`，只使用 LibMcp 公共 API：

- Client 页面管理 Streamable HTTP MCP 地址并连接外部 Server。
- Server 页面配置监听 IP、端口和路径，启动本地 MCP Server。
- Log 页面显示启动、连接及错误信息。

自动验证不操作 LibMcp 页面；统一 TestApp 参与命令行编译和链接检查。
