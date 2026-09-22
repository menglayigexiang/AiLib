# MCP 同步 API 方案调研

> 调研日期：2026-09-21  
> 目标协议：MCP `2026-07-28`  
> 资料范围：MCP 官方规范以及官方 TypeScript、Python、C#、Java/Kotlin、Rust SDK 仓库。

## 结论

不建议 LibMcp 采用“同步公共 API + 同步协议内核”的纯同步设计。

更适合 Qt 5.15.2/Qt 6.11 的方案是：

1. 协议内核、Transport、请求调度和 Server dispatcher 全部采用事件驱动的异步实现。
2. 第一阶段可以只向普通调用者主推同步 facade，例如 `McpBlockingClient::callTool()`。
3. 同步 facade 只能在非 GUI、非 Transport 所属线程调用；检测到同线程调用时立即返回错误，不能靠嵌套 `QEventLoop` 等待。
4. 内部必须保留请求 ID、超时、主动取消、并发请求、通知及订阅处理能力。Server handler 也必须能够调度到工作线程。
5. 公共类型从一开始保留 `CancellationToken`（或等价对象）和明确的超时参数，否则以后无法兼容完整取消语义。

换句话说，可以只先发布“同步使用方式”，但不应该只实现“同步机制”。Java 官方 SDK 是最接近这种产品形态的参考：它公开同步与异步两套 facade，但同步 facade 建立在 Reactor 异步基础设施之上。

## 为什么协议本身要求异步内核

### 并发和订阅

MCP `2026-07-28` 的基础消息模式包括普通请求/响应、MRTR 和订阅/通知；这些模式是所有实现都必须支持的基础层。Streamable HTTP 中，一个请求可能对应一个持续的 SSE 响应流，而 `subscriptions/listen` 是长期保持的通知流。纯串行阻塞模型无法在等待一个结果时继续处理取消、进度和通知。[基础协议](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/index.mdx)；[Streamable HTTP](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/streamable-http.mdx)

### 取消必须在请求仍执行时被处理

两种标准 Transport 的取消方式不同：

- stdio：客户端发送引用原请求 ID 的 `notifications/cancelled`；Server 应尽快停止工作，并且不能再发送该请求的消息。
- Streamable HTTP：客户端关闭该请求自己的响应流；Server 必须把它视为取消。

因此，Server 在 handler 执行期间仍要持续驱动 I/O 和分发取消事件。若 Transport 与 handler 在同一线程同步阻塞，取消消息没有机会被读取。[Transport 总览](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/index.mdx)；[stdio 取消](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/stdio.mdx)；[Streamable HTTP 取消](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/streamable-http.mdx)

### MRTR 虽然无状态，但仍然是多轮异步流程

`2026-07-28` 用 MRTR 替代旧版的 Server 主动 JSON-RPC 请求。Server 返回 `InputRequiredResult`，Client 完成 sampling、elicitation 或 roots 请求后，携带 `inputResponses` 和 `requestState` 重试原请求。它降低了 Transport 会话耦合，但仍可能等待用户输入、模型生成或文件系统结果，不能把它理解为“所有调用都适合阻塞当前线程”。[版本变更](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx)；[官方发布说明](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/blog/content/posts/2026-07-28-spec-ga/index.md)

## 官方 SDK 对比

| SDK | Client 请求 | Server handler | 取消/并发 | 判断 |
|---|---|---|---|---|
| TypeScript | `Promise` / `await` | 异步 handler，可返回 Promise | `AbortSignal`，请求级 signal；HTTP 请求级流 | 异步内核 |
| Python | `async def` / `await` | 异步 handler | AnyIO/任务取消，并继续处理 Transport I/O | 异步内核 |
| C# | `CreateAsync`、`CallToolAsync` 等 | 可写同步业务方法，但框架 dispatcher/过滤器异步 | `CancellationToken` 注入每个 handler | 异步内核，允许同步业务函数 |
| Java | `McpSync*` 与 `McpAsync*` 两套 | sync 直接返回；async 返回 `Mono` | 内部基于 Reactor | 异步内核 + 同步 facade |
| Kotlin | `suspend` / coroutine | coroutine handler | `CancellationException` 和并发 handler | 异步内核 |
| Rust | `async fn` / Future / `.await` | handler 返回 Future | timeout/cancel 与异步 session | 异步内核 |

### TypeScript

官方 v2 客户端方法是 Promise/await 模型；Transport 的 `start()` 和发送流程为异步。Streamable HTTP Transport 明确公开 `hasPerRequestStream`，并将请求级 `AbortSignal` 组合到每个 POST/SSE 流上。Server handler context 同样携带请求 signal，因此业务执行可以协作取消。[客户端文档](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/packages/client/README.md)；[Streamable HTTP Client 源码](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/packages/client/src/client/streamableHttp.ts)；[流式交互示例](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/examples/streaming/README.md)

### Python

官方客户端的协议操作（例如 `call_tool`）是 `async def`，通过 `await` 获取结果；Server handler 也是异步函数。顶层 Server 虽然提供同步的 `run()` 入口，但该入口只是调用 `anyio.run(...)` 启动异步运行时，并不代表协议处理采用同步内核。[ClientSession 源码](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/client/session.py)；[客户端示例](https://github.com/modelcontextprotocol/python-sdk/blob/main/examples/snippets/clients/parsing_tool_results.py)；[Server 源码](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/server/mcpserver/server.py)

### C#

官方客户端使用 `CreateAsync`、`ListToolsAsync`、`CallToolAsync` 等 Task API。Server 可以让简单工具直接返回普通值，但框架请求管线、过滤器和完整 handler 使用 async/Task，并把 `CancellationToken` 传给 handler。官方取消文档还区分了普通请求取消与 Tasks 扩展的 `tasks/cancel`。[工具文档](https://github.com/modelcontextprotocol/csharp-sdk/blob/main/docs/concepts/tools/tools.md)；[取消与无状态模式](https://github.com/modelcontextprotocol/csharp-sdk/blob/main/docs/concepts/stateless/stateless.md)；[Tasks 扩展](https://github.com/modelcontextprotocol/csharp-sdk/blob/main/docs/concepts/tasks/tasks.md)

### Java

官方 Java SDK 明确同时提供 `McpSyncClient`/`McpSyncServer` 和 `McpAsyncClient`/`McpAsyncServer`。同步 handler 直接返回结果，异步 handler 返回 `Mono<...>`；同步 API 是面向阻塞用例的 facade，底层异步模型采用 Reactive Streams/Project Reactor。它证明“同步易用 API”可以存在，但并不支持“只写同步内核”的结论。[Java SDK](https://github.com/modelcontextprotocol/java-sdk)；[Server 同步/异步 handler 文档](https://github.com/modelcontextprotocol/java-sdk/blob/main/docs/server.md)

需注意：当前 Java SDK 的版本进度不应被单独当作 `2026-07-28` 全特性的权威实现依据；这里主要借鉴其同步 facade 架构。[Java SDK 变更记录](https://github.com/modelcontextprotocol/java-sdk/blob/main/CHANGELOG.md)

### Kotlin

官方 Kotlin SDK 使用 coroutine-first API。Client 操作在协程内执行；示例中的 `runBlocking` 只是应用入口对 suspend API 的同步桥接。Streamable HTTP server 的核心请求处理函数为 `suspend fun handleRequest(...)`，Transport 关闭时会取消等待中的响应。[Kotlin SDK](https://github.com/modelcontextprotocol/kotlin-sdk)；[Streamable HTTP Server Transport 源码](https://github.com/modelcontextprotocol/kotlin-sdk/blob/main/kotlin-sdk-server/src/commonMain/kotlin/io/modelcontextprotocol/kotlin/sdk/server/StreamableHttpServerTransport.kt)

### Rust

官方 Rust SDK 基于 Tokio。Client 请求通过 `.await` 完成，Server handler 使用 `async fn`/Future，完整 session 也以异步方式启动、等待和取消。官方实现还自动执行 MRTR 多轮处理，进一步说明调用的表面“返回最终结果”并不等于内部同步。[Rust SDK](https://github.com/modelcontextprotocol/rust-sdk)；[ServerHandler 源码](https://github.com/modelcontextprotocol/rust-sdk/blob/main/crates/rmcp/src/handler/server.rs)

## 建议的 LibMcp API 分层

### 1. 必须存在的异步内核（可先不作为主推 API）

```cpp
class McpRequest : public QObject
{
    Q_OBJECT
public:
    RequestId id() const;
    void cancel();

signals:
    void finished(const McpResult &result);
    void failed(const McpError &error);
    void progress(const McpProgress &progress);
};

class McpClient : public QObject
{
    Q_OBJECT
public:
    McpRequest *callTool(const CallToolRequest &request,
                         const RequestOptions &options = {});
};
```

这里的 `McpRequest` 管理请求 ID、超时和取消。Transport 永远不因等待业务结果而停止事件处理。

### 2. 面向用户的同步 facade

```cpp
class McpBlockingClient
{
public:
    Result<CallToolResult> callTool(
        const CallToolRequest &request,
        std::chrono::milliseconds timeout,
        CancellationToken cancellation = {});
};
```

实现要求：

- facade 把请求投递到专用 I/O 线程中的 `McpClient`；
- 调用线程使用条件变量等待，不运行嵌套 `QEventLoop`；
- 超时或 token 取消时，向异步内核发出取消并解除等待；
- 若调用发生在 I/O 对象所属线程或 GUI 主线程，立即返回 `BlockingCallOnEventThread`；
- MRTR 可以由 facade 自动循环到最终结果，也应提供轮数上限；
- `subscriptions/listen`、日志、进度等持续事件不能伪装成“返回一个值”的同步方法，应使用线程安全回调或独立订阅对象。

### 3. Server handler

可允许用户注册看起来同步的函数：

```cpp
using ToolHandler = std::function<Result<CallToolResult>(
    const CallToolRequest &,
    const RequestContext &,
    CancellationToken)>;
```

但 dispatcher 必须把它调度到受控工作线程池，Transport 线程继续接收取消和其他请求。需要提供最大并发数、排队上限、handler deadline 和关闭时等待策略。

## 最终建议

如果“只做同步”是指使用者不想处理信号槽、回调或 Future，可以接受，并采用 `McpBlockingClient` 作为第一阶段主要公共入口。

如果“只做同步”是指整个库只使用阻塞 I/O、一次只处理一个请求、handler 执行时暂停消息循环，则不适合完整 MCP SDK，也不适合 Qt GUI 和移动端。它会直接削弱取消、并发、订阅、进度、Streamable HTTP SSE 和 MRTR，并给未来扩展制造 ABI/API 包袱。
