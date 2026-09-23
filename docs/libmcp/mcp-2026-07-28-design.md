# LibMcp MCP 2026-07-28 设计文档

## 1. 文档状态

本文档定义 LibMcp 面向 MCP `2026-07-28` 的重写边界、模块职责、公共 API
方向、Transport 要求、TestApp 行为和验收标准。本文档获得确认后才进入
代码实现。

实现中以官方 MCP `2026-07-28` Schema 和规范为唯一协议依据。当本文档与官方
规范冲突时，必须修正本文档和实现，不得为保留旧行为而偏离规范。

## 2. 目标

1. 只实现 MCP `2026-07-28`，不支持 `2025-11-25` 及更早协议。
2. 实现该版本全部未弃用的 Client 和 Server 核心功能。
3. 同时实现 STDIO 和无状态 Streamable HTTP Transport。
4. 线协议严格匹配官方 Schema，公共 C++ API 保持清晰的 Qt/C++ 使用习惯。
5. 代码按职责模块化，让审核者能够逐层理解协议、传输、校验和业务行为。
6. 通过 Qt Test、官方 Schema 和官方 TypeScript/Python SDK 互操作证明协议正确性。

## 3. 硬性边界

### 3.1 协议边界

- 不实现 `initialize` 和 `notifications/initialized`。
- 不实现协议级 Session、`Mcp-Session-Id`、Session DELETE 或 Session 超时清理。
- 不实现旧 HTTP+SSE Transport，不实现 Streamable HTTP GET 独立事件流。
- 不实现多协议版本适配器、协议工厂或版本降级逻辑。
- 每个请求独立且自包含，不依赖前一个请求建立的隐式上下文。
- 请求 `_meta` 携带规范要求的 `protocolVersion`、`clientCapabilities` 和可选
  `clientInfo`等元数据。
- Authorization/OAuth 不在本次实现中，在核心协议完成后作为独立安全主题设计。

### 3.2 兼容与构建边界

- 允许破坏性重构现有 LibMcp 公共 API，不保留 deprecated 兼容层。
- 保持 C++17、Qt 5.15 和 Qt 6.2+ 支持。
- LibMcp 继续生成一个动态库，不拆分多个动态库。
- `jsoncons` 以固定版本 header-only 源码纳入仓库，仅作为 LibMcp 私有编译依赖，
  不产生独立动态库或静态库。
- `llhttp` 可以作为 vendored HTTP parser，直接编译进 LibMcp，其类型和 API 不得泄漏。
- 第三方源码、许可证和版本说明放在独立目录，不直接修改第三方文件。

## 4. 总体架构

```text
Public Qt/C++ API
  McpClient / McpServer / McpOperation<T> / Public Models
                         |
Client API --------------+-------------- Server API
    |                                           |
Client capability logic                 Registries and dispatch
    |                                           |
    +------------- Protocol Core ---------------+
                  |  Public/Wire conversion
                  |  JSON-RPC codec
                  |  MCP metadata and validation
                  |  MRTR / subscriptions
                  |  JSON Schema validation
                  |
               Transport
          +--------+---------+
          |                  |
        STDIO        Streamable HTTP
                              |
                   Internal HTTP Facades
                    HttpClient / HttpServer
                   + Qt Network / Qt Socket
                   + private llhttp parser
```

依赖只能沿图中方向向下。Transport 不知道 Tools、Resources、Prompts 等能力语义；
HTTP 模块不知道 MCP Header、JSON-RPC 或 MCP SSE 事件语义。

## 5. 目录与模块职责

下列目录是设计目标，具体文件名可在不改变职责边界的前提下调整。

```text
LibMcp/
  include/LibMcp/        稳定的公共 Qt/C++ API
  protocol/              固定的官方 2026-07-28 Schema
  src/
    core/                错误、Operation 基础、通用内部工具
    wire/                JSON-RPC/MCP Wire 编解码与强校验
    schema/              jsoncons 封装与 Qt JSON 转换
    client/              Client 公共行为与 Client 能力
    server/              Server 分发、注册表与 Server 能力
    transport/           Transport 抽象、STDIO、Streamable HTTP
    http/                内部 HttpClient/HttpServer facade 及实现
  third_party/
    jsoncons/            未修改的固定版本源码与许可证
    llhttp/              未修改的固定版本源码与许可证
```

### 5.1 Wire 协议层

- 官方 Schema 原样保存，不添加 LibMcp 私有字段。
- 内部 Wire 值使用 `QJsonObject`、`QJsonArray` 和 `QJsonValue`。
- JSON-RPC codec 只负责 Request、Response、Notification 和 Error 的结构。
- MCP codec 负责 method、params、result、`_meta` 和标准错误的转换与校验。
- 公共 C++ 模型与 Wire JSON 之间通过显式 encode/decode 函数转换。
- 解码不得默认补齐规范要求的必填字段，不得吞掉未知标准错误。
- 未识别的合法扩展元数据按规范保留，不为假设扩展建立插件架构。

### 5.2 JSON Schema 校验模块

模块只提供少量内部函数级能力：

- 验证 `QJsonObject` Schema 自身是否符合 JSON Schema 2020-12。
- 验证 `QJsonValue` 实例是否符合指定 Schema。
- 将 jsoncons 错误转换为 LibMcp 内部简洁错误，不向其他模块传递第三方类型。
- 默认禁止通过网络解析外部 `$ref`；对无法解析的引用返回明确错误。

校验点：

1. Tool 注册时校验 `inputSchema` 和可选 `outputSchema`。
2. `tools/call` 执行前校验 arguments。
3. Tool 声明 `outputSchema` 时校验结构化输出。

### 5.3 Client 模块

- 不执行连接级 initialize 握手。
- 可选调用 `server/discover` 获取 Server 能力，但不将其结果作为后续请求的
  隐式协议状态。
- 每个请求自行携带协议版本、Client 能力和 Client 身份。
- 通过统一请求调度器生成 ID、匹配结果、取消操作和分发请求内事件。
- 列表 API 必须正确暴露或处理 cursor，不得在未经设计的情况下默默吞掉分页语义。
- MRTR 输入请求、Progress 和 Subscription 事件通过当前 `McpOperation<T>` 传递。

### 5.4 Server 模块

- 实现 `server/discover` 并为每个结果附加规范要求的 Server 元数据。
- 每个请求独立验证协议版本、Client 能力和 method 所需能力。
- 不保存协议级 Client Session 或 connected-client 状态。
- Tools、Resources、Resource Templates 和 Prompts 使用独立注册表。
- Completion、Elicitation/MRTR 和 Subscriptions 分别属于独立职责，不堆入单一分发函数。
- 请求计数、Active Requests 和最近 ClientInfo 是运行观测数据，不参与协议分发。

## 6. 协议能力矩阵

本次实现下列未弃用核心能力：

| 能力 | 方法/事件 | Client 职责 | Server 职责 |
| --- | --- | --- | --- |
| 发现 | `server/discover` | 请求并解码能力 | 返回版本、身份和能力 |
| Tools | `tools/list`, `tools/call` | 分页读取与调用 | 注册、列表、参数校验与执行 |
| Resources | `resources/list`, `resources/read` | 分页读取内容 | 注册、列表和读取 |
| Resource Templates | `resources/templates/list` | 分页读取模板 | 注册与列表 |
| Prompts | `prompts/list`, `prompts/get` | 分页读取与获取 | 注册、列表和展开 |
| Completion | `completion/complete` | 提交补全请求 | 执行补全回调 |
| MRTR / Elicitation | `resultType: input_required` 及输入响应 | 向调用方暴露输入需求并续传 | 构造输入请求并处理重试 |
| Progress | `notifications/progress` | 转发当前操作进度 | 在请求范围内发布进度 |
| Subscriptions | `subscriptions/listen` 及变更通知 | 管理长流与通知 | 过滤并发布订阅事件 |
| Cancellation | STDIO `notifications/cancelled` / HTTP 关闭 SSE | 取消当前 Operation | 尽快终止对应工作 |

Roots、Sampling 和 Logging 在 `2026-07-28` 中已弃用，本项目不新增对应 API。

## 7. 公共 C++ API 方向

### 7.1 公共类型

公共头文件保留 Qt/C++ 语义，不暴露 Wire JSON 内部类型、jsoncons 或 llhttp 类型。
主要类型包括：

- `McpClient`：发起请求、持有 Client 身份和能力、跟踪活动 Operation。
- `McpServer`：持有 Server 身份和能力、注册业务处理器、分发独立请求。
- `McpOperationBase`：非模板 `QObject`，承载状态、取消和过程事件信号。
- `McpOperation<T>`：保存类型化最终结果，不重复定义 QObject 信号。
- `McpError` / `McpResult<T>`：表达协议、Transport、校验和取消错误。
- `McpTool`、`McpResource`、`McpResourceTemplate`、`McpPrompt` 及对应结果类型。
- Discovery、Completion、Subscription、Progress 和 MRTR 所需的精简强类型模型。

### 7.2 Operation 所有权

- Client API 返回 `QSharedPointer<McpOperation<T>>`。
- Operation 不设置 QObject parent，只由 `QSharedPointer` 管理生命周期。
- `McpClient` 对活动 Operation 保留共享引用，完成后移除自身引用。
- 调用方仍持有引用时，可继续读取最终 status、result 和 error。
- Client 析构时终止未完成 Operation，设置最终状态后释放自身引用。
- 详细跨线程调用和最终销毁策略暂不提前抽象，实现时按 Qt 真实线程需求确定。

## 8. Transport 设计

### 8.1 Transport 通用边界

Transport 只传送完整 JSON-RPC 消息和与当前请求关联的流事件。它不解析
Tools、Resources、Prompts 或 MRTR 业务语义。

### 8.2 STDIO

- Client Transport 启动配置的子进程，分别管理 stdin、stdout 和 stderr。
- Server Transport 通过当前进程 stdin/stdout 运行，日志只写入 stderr。
- stdout 只允许协议消息，非协议输出视为 Transport 错误。
- 支持进程启动命令、参数、工作目录、显式环境变量和环境变量传递列表。
- STDIO 取消使用规范定义的 `notifications/cancelled`。

### 8.3 Streamable HTTP

- 每个 Client 消息使用独立 HTTP POST。
- Client 发送 `Accept: application/json, text/event-stream`，并支持单 JSON 响应和请求级 SSE 响应。
- 通知成功返回 HTTP 202 且无 body；请求返回 JSON 或在 SSE 事件后返回最终响应。
- Client 生成标准 `MCP-Protocol-Version`、`Mcp-Method`、`Mcp-Name` 和需要的
  `Mcp-Param-*` Headers。
- Server 以大小写不敏感方式处理 Header 名，校验 Header 与 body 一致，不一致时
  返回 HTTP 400 和 `HeaderMismatch` 标准错误。
- Server 验证 Origin，默认本地监听仅绑定 loopback。
- 关闭当前请求的 SSE 响应流即取消该请求，Server 不得再发送后续事件。
- `subscriptions/listen` 使用独立长寿命 SSE 响应流。
- GET 和 DELETE 返回 405；`Mcp-Session-Id` 和 `Last-Event-ID` 不创建任何旧版状态。

## 9. 内部 HTTP 模块

### 9.1 Facade 边界

- `StreamableHttpClientTransport` 只依赖 `HttpClient`，不直接操作
  `QNetworkAccessManager` 或 `QNetworkReply`。
- `StreamableHttpServerTransport` 只依赖 `HttpServer`，不直接操作 `QTcpServer`、
  `QTcpSocket` 或 llhttp。
- HTTP 模块对上只暴露请求、响应、Headers、连接生命周期和流式读写所需的最小
  内部类型。

### 9.2 Client 实现

- 底层使用 Qt Network。
- `QNetworkReply` 的增量数据、结束、错误和取消被转换为 HttpClient 内部事件。
- HttpClient 不识别 SSE 字段，仅提供响应字节流。

### 9.3 Server 实现

- 底层使用 `QTcpServer` / `QTcpSocket`。
- llhttp 只负责增量 HTTP framing 解析，不理解 MCP 或 SSE。
- 连接、请求解析、响应写入、keep-alive、chunked body、流式响应和容量限制分属
  内部独立职责。
- 断开事件能够精确关联到当前响应 writer，供 MCP Transport 执行取消。

## 10. TestApp 设计

TestApp 只使用 LibMcp 公共 API，不访问 Wire、HTTP 或 Transport 私有实现。

### 10.1 Client 页面

主页面使用表格，每行表示一个 Client 配置，包含：

- 名称。
- 类型（STDIO / Streamable HTTP）。
- 参数摘要或可下拉的 JSON 配置摘要。
- 最终连接状态和行内错误摘要。
- 连接开关、编辑按钮和删除按钮。

交互规则：

- 顶部“添加”按钮打开模态配置对话框。
- 编辑按钮复用同一对话框并预填当前配置。
- 连接开关打开即连接，关闭即断开，不另设“启用”概念。
- 界面不展示“连接中”中间文案；操作期间只防止重复点击。
- 连接失败时开关回到关闭，行内显示错误摘要，详细信息输出到统一日志。
- 已连接 Client 禁止编辑和删除，用户必须先关闭连接。
- 删除未连接 Client 前显示包含 Client 名称的确认对话框。
- 配置保存到 `TestApp/mcp-clients.json`，可包含明文凭据；重启后读取配置但不自动连接。

STDIO 配置对话框参考已确认界面，包含：

- 名称和类型切换。
- 启动命令。
- 可增删的参数列表。
- 可增删的环境变量键值列表。
- 可增删的环境变量传递列表。
- 工作目录。

Streamable HTTP 配置对话框包含：

- 名称和类型切换。
- URL。
- Bearer 令牌环境变量名。
- 可增删的固定 Header 键值列表。
- 可增删的“Header 名 -> 环境变量名”列表。

### 10.2 Server 页面

- 显示 Server 运行状态、Endpoint 和固定协议版本。
- 显示 Active Requests、总请求数、成功数、协议错误数和 Transport 错误数。
- 显示最近请求的 ClientInfo、method、开始时间、耗时和最终结果。
- 不显示 connected clients、Session ID 或 Session 生命周期。
- Server 启动后锁定监听 IP、端口和路径，停止后恢复编辑。
- 启动和停止通过明确按钮控制，操作结果同时更新页面状态和统一日志。

## 11. 配置与运行观测

- Client 持久化格式使用独立配置版本，与 MCP 协议版本无关。
- 加载时先完整验证到临时结构，全部成功后再替换当前配置集。
- 运行统计是可观测数据，不影响无状态协议行为。
- 最近 ClientInfo 列表是有界诊断记录，不表示 Client 当前连接。
- 容量上限和详细线程安全策略在实现中按真实负载设定，不建立通用监控框架。

## 12. 错误处理原则

- 协议错误、Transport 错误、HTTP 错误、Schema 错误、业务 Tool 错误和取消错误分类明确。
- 有官方标准错误码的场景必须使用官方错误码，不用通用错误替代。
- 公共错误不包含 jsoncons、llhttp、`QNetworkReply` 或 Socket 的实现类型。
- 错误文本给出可操作的上下文，日志可附加内部诊断，但不把凭据写入错误文本。

## 13. 实施阶段

本项目只区分两个阶段。

### 阶段一：开发

开发阶段按依赖方向进行，但不再将内部工作项命名为额外阶段：

1. 固定官方 Schema 和第三方源码，建立清晰 CMake 私有依赖。
2. 重建错误、公共模型、Operation 和 Wire codec。
3. 实现 JSON Schema 内部校验模块。
4. 实现内部 HTTP facade 和 STDIO/Streamable HTTP Transport。
5. 实现 Client 和 Server 的全部未弃用核心能力。
6. 完成 TestApp Client/Server 页面和配置持久化。
7. 同步更新 `docs/libmcp` 用法、架构和协议能力说明。

开发阶段允许运行快速、定向的编译和单元测试作为日常反馈，但不将它们宣称为最终验收。

### 阶段二：测试、返工修改与优化循环

本阶段反复执行“测试 -> 定位 -> 返工修改/优化 -> 再测试”，直到所有验收条件满足：

1. 运行模块级 Qt Test 和 LibMcp 全量测试。
2. 用官方 Schema 校验请求、结果、通知、错误和元数据样本。
3. 运行 HTTP framing、SSE、STDIO、取消、MRTR 和 Subscription 真实协议测试。
4. LibMcp Client 分别连接官方 TypeScript/Python SDK Server。
5. 官方 TypeScript/Python SDK Client 分别连接 LibMcp Server。
6. 运行整个 AiLib 的全量自动化测试，区分既有失败与本次回归。
7. 对失败和高复杂度代码返工，优化模块边界、可读性、正确性和必要性能。
8. 重复上述测试，直到无未解决协议偏差、回归或高风险警告。

## 14. 验收与 Token 约束

测试和验收只使用文本、代码和协议级证据：

- 禁止调用截屏、屏幕录制、实时视频分析或连续图像采样进行测试。
- 禁止使用图像识别、像素对比或视频回放作为 UI 验收方式。
- TestApp 使用 Qt Test 查找控件、触发交互并断言状态。
- 网络行为使用 FakeTransport、本地 HTTP Server、受控子进程和官方 SDK 测试。
- 报告只保留测试摘要、失败断言、必要日志和互操作结果，不采集冗余媒体。

最终验收要求：

1. LibMcp 和 AiLib 全量自动化测试通过。
2. 官方 Schema 样本与负向样本全部通过。
3. TypeScript 和 Python 官方 SDK 四个互操作方向全部通过。
4. STDIO 和 Streamable HTTP 均覆盖普通结果、错误、取消、流式事件和关闭。
5. 无旧握手、Session ID、旧 SSE GET 或协议降级路径残留。
6. 新增手写 C/C++ 代码符合项目的模块化、可读性和中文注释硬约束。

## 15. 不在本次范围内

- `2025-11-25` 及更早协议兼容。
- HTTP+SSE Transport。
- 协议级 Session 和 connected-client 管理。
- Roots、Sampling 和 Logging 等已弃用功能的新 API。
- Authorization/OAuth 实现。
- 对外公开的 HTTP Server、JSON Schema Validator 或第三方类型。
- 通用插件系统、多后端 Factory 或面向假设需求的扩展层。
- 截屏、录屏、实时视频分析或图像对比式测试。

## 16. 官方依据

- [MCP 2026-07-28 Specification](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/index.mdx)
- [MCP 2026-07-28 Schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.json)
- [MCP 2026-07-28 Changelog](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx)
- [Streamable HTTP](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/transports/streamable-http.mdx)
- [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12/json-schema-core)
