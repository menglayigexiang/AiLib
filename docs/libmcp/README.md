# LibMcp

LibMcp 是一个使用 C++17 和 Qt 构建的实验性 MCP Client/Server 库。工程支持
Qt 5.15 和 Qt 6.2+，并提供一个按需构建的 Qt Widgets 测试应用。

## 已提供的功能

- `McpResult<T>` 与统一错误模型。
- JSON-RPC 请求、响应、通知、超时和连接关闭处理。
- MCP 初始化与能力发现。
- Tools、Resources、Resource Templates 和 Prompts。
- 列表请求自动 cursor 分页，包含循环与最大页数保护。
- Streamable HTTP Client/Server Transport。
- 用于单元测试的内存 Transport。
- `McpClientManager` 配置集合及 JSON 序列化/事务式反序列化。
- 统一 Qt Widgets `TestApp` 中的 LibMcp 页面，可连接远端 MCP 或启动本地 MCP Server。

## 构建

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="E:/Qt/installer/6.11.2/mingw_64"
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt 5.15 使用对应 Kit 的 `CMAKE_PREFIX_PATH` 即可。LibMcp 固定构建为动态库，
可通过顶层选项关闭或开启人工测试应用：

```powershell
cmake -S . -B build -DAILIB_BUILD_MCP=ON
```

## 最小 Server 示例

```cpp
auto transport = std::make_unique<LibMcp::StreamableHttpServerTransport>(
    QHostAddress::LocalHost, 8080, QStringLiteral("/mcp"));

LibMcp::McpServer server(
    std::move(transport),
    {QStringLiteral("example"), QStringLiteral("1.0.0"), QStringLiteral("Example")});

LibMcp::McpTool echo;
echo.name = QStringLiteral("echo");
echo.inputSchema = {
    {QStringLiteral("type"), QStringLiteral("object")}
};

server.addTool(echo, [](const QJsonArray &input) {
    return input;
});

server.start();
```

## 最小 Client 示例

```cpp
auto transport = std::make_unique<LibMcp::StreamableHttpClientTransport>(
    QUrl(QStringLiteral("http://127.0.0.1:8080/mcp")));

LibMcp::McpClient client(std::move(transport));

auto startFuture = client.start();
auto toolsFuture = client.listTools();
```

公共异步 API 返回 `QFuture<McpResult<T>>`，可以使用 `QFutureWatcher` 接收完成结果。
不要在 GUI 线程中阻塞等待 Future。

架构及模块职责参见 [architecture.md](architecture.md)。
