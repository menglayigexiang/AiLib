# LibMcp 架构说明

LibMcp 只实现 MCP `2026-07-28`。公共 Qt/C++ API、协议分发、Wire 编解码、Schema
校验、Transport 和内部 HTTP facade 采用单向依赖，不保存协议级 Client Session。

```text
Public API: McpClient / McpServer / McpOperation / Models
             |                         |
          Client                    Server registries
             +---------- Protocol core ----------+
                       Wire / metadata / schema
                                  |
                  STDIO / Streamable HTTP Transport
                                  |
                       private HttpClient/HttpServer
```

核心边界：

- 公共 API 使用 Qt 类型，不暴露 jsoncons、Qt Network Reply、Socket 或 HTTP parser。
- Transport 只负责完整 MCP 消息、framing 和请求级流，不理解 Tool/Resource/Prompt。
- HTTP facade 只负责通用 HTTP 请求、响应、chunked 流和连接生命周期。
- 每个请求自行携带协议版本、Client 能力和 ClientInfo。
- Progress 与订阅通知通过请求级 SSE 或 STDIO 同一消息通道传输。
- JSON Schema 校验是内部函数级模块，不形成公共 Validator 抽象。
- TestApp 只调用公共 API，不穿透 Transport 或 Wire 私有实现。

目录职责、能力矩阵、错误原则和验收要求详见
[MCP 2026-07-28 设计文档](mcp-2026-07-28-design.md)。
