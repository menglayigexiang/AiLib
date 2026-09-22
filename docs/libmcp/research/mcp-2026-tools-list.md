# MCP 2026-07-28 `tools/list` 调研与 LibMcp 设计建议

本文仅依据 MCP 官方 `2026-07-28` 正式规范、该版本 `schema.ts`（其为协议消息结构的权威来源）以及官方 Tier 1 TypeScript SDK v2 的实现/迁移文档。调研日期：2026-09-21。

## 结论摘要

`tools/list` 不是“返回一个工具数组”这么简单。为了与外部 MCP 实现正确互操作，LibMcp 必须完整处理：每请求 `_meta`、`resultType`、游标分页、`ttlMs`/`cacheScope`、完整 Tool 字段、能力声明，以及基于 `subscriptions/listen` 的列表变化通知。

面向普通调用者，推荐只公开一个简单接口：

```cpp
QFuture<McpResult<QList<McpTool>>> listTools();
```

它自动遍历全部分页，并在内部正确处理缓存与通知失效。分页页对象、cursor 和缓存字段保留在内部协议模型中，不强迫普通使用者参与，也不丢失协议语义。

## 一、协议请求

方法名固定为：

```text
tools/list
```

Schema 定义为 `ListToolsRequest extends PaginatedRequest`，其 `params` 为 `PaginatedRequestParams`：

```json
{
  "jsonrpc": "2.0",
  "id": "1",
  "method": "tools/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {},
      "io.modelcontextprotocol/clientInfo": {
        "name": "ExampleClient",
        "version": "1.0.0"
      }
    },
    "cursor": "opaque-cursor"
  }
}
```

要求：

- `params` 必须存在，因为 `RequestParams._meta` 是必需字段。
- `_meta["io.modelcontextprotocol/protocolVersion"]` 必需，值为当前使用的协议版本。
- `_meta["io.modelcontextprotocol/clientCapabilities"]` 必需；没有可选能力时也应发送空对象。
- `_meta["io.modelcontextprotocol/clientInfo"]` 是 SHOULD，而非 MUST；LibMcp 已决定从构造时不可变的 `McpClient::ClientInfo` 填充。
- `cursor` 可选，是完全不透明的字符串。客户端必须原样回传，不能解析，也不能用 truthy 判断；空字符串同样可能是合法 cursor。
- 第一页不发送 `cursor`。后续页发送上一页的 `nextCursor`。
- HTTP Transport 还必须让 body 中的协议版本与 `MCP-Protocol-Version` Header 一致；这属于 Transport 的 HTTP 映射职责，而非 `listTools()` 职责。

## 二、协议响应

`ListToolsResult` 同时继承 `PaginatedResult` 和 `CacheableResult`：

```json
{
  "jsonrpc": "2.0",
  "id": "1",
  "result": {
    "resultType": "complete",
    "tools": [],
    "nextCursor": "opaque-next-cursor",
    "ttlMs": 300000,
    "cacheScope": "public",
    "_meta": {
      "io.modelcontextprotocol/serverInfo": {
        "name": "ExampleServer",
        "version": "1.0.0"
      }
    }
  }
}
```

字段要求：

- `resultType`：2026-07-28 Server 必须返回。正常列表结果必须是 `"complete"`。`ResultType` 在 Schema 中是可扩展字符串；Client 收到不能识别的结果类型时必须明确失败，不能误当完整列表。
- `tools`：必需数组，可以为空。
- `nextCursor`：可选。存在表示可能还有下一页；Client 应使用该值继续请求。必须按“字段是否存在”判断，不能用字符串是否为空判断。
- `ttlMs`：必需，非负整数，单位毫秒。0 表示结果立即陈旧；正数表示在该时长内可视为 fresh。
- `cacheScope`：必需，仅 `"public"` 或 `"private"`。
- `_meta`：可选；其中 Server SHOULD 在每个响应写入 `io.modelcontextprotocol/serverInfo`。未知扩展键必须保留。

`cacheScope` 语义：

- `public`：结果不含用户特定数据，可跨授权上下文缓存。
- `private`：只能在同一个授权上下文内复用，不得跨 token/身份共享。

LibMcp 第一版无需公开可插拔缓存策略，但必须正确解析并保存这些字段。内存缓存可以简单实现：按 Server 身份、授权上下文和请求参数分区；过期后重新请求。若暂不启用缓存，也不能把字段解析丢弃。

## 三、Tool 的完整字段

推荐公共强类型：

```cpp
struct McpIcon
{
    QUrl src;
    QString mimeType;
    QStringList sizes;
    QString theme; // 空、"light"、"dark"
};

struct McpToolAnnotations
{
    std::optional<QString> title;
    std::optional<bool> readOnlyHint;
    std::optional<bool> destructiveHint;
    std::optional<bool> idempotentHint;
    std::optional<bool> openWorldHint;
};

struct McpTool
{
    QString name;
    QString title;
    QString description;
    QList<McpIcon> icons;
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    std::optional<McpToolAnnotations> annotations;
    QJsonObject meta;
};
```

注意：可选 bool 不能直接建模成普通 `bool`，否则无法区分“字段缺失”和显式 `false`。若项目希望避免公开 `std::optional`，也可以使用一个极小的 `McpOptional<T>`，但不应为此引入复杂通用框架。

协议字段：

- `name`：必需；建议 1–128 字符，区分大小写，建议只含 ASCII 字母、数字、`_`、`-`、`.`；同一 Server 内应唯一。
- `title`：可选的人类可读显示名。
- `description`：可选的人类可读功能说明。
- `icons`：可选数组；单个图标包含必需 `src`，以及可选 `mimeType`、`sizes`、`theme`。
- `inputSchema`：必需，必须是 JSON Schema 对象，而且根 `type` 必须为 `"object"`。无 `$schema` 时按 JSON Schema 2020-12。
- `outputSchema`：可选，可以是任意合法 JSON Schema 2020-12，不要求根为 object。
- `annotations`：可选，包含上述四个行为 hint 以及旧的 `annotations.title`。所有 annotation 都是不可信提示，客户端不能据此作安全决策。
- `_meta`：可选开放对象，必须保留未知扩展键。

显示标题优先级为：`Tool.title`、`Tool.annotations.title`、`Tool.name`。

Streamable HTTP 还定义了 `inputSchema` 属性上的 `x-mcp-header`。HTTP Client 必须检查其合法性；若某 Tool 的 `x-mcp-header` 不合法，规范要求从 `tools/list` 对用户可用结果中排除该 Tool，并建议记录警告。stdio 可忽略此注解。该校验属于 Tool/HTTP 适配层，不属于 `RequestManager`。

## 四、分页语义与推荐实现

协议只定义单页请求/响应，不规定 SDK 的便利 API 必须自动聚合。官方 TypeScript SDK v2 选择：

- 无 cursor 的 `listTools()` 自动跟随 `nextCursor` 聚合全部页。
- 显式传 cursor 时仅获取一页。
- 自动聚合设置最大页数（默认 64），防止恶意/错误 Server 无限返回 cursor。
- 检查 cursor 循环，避免无限请求。

LibMcp 推荐采用同样的普通用户体验，但暂不公开第二套分页 API：

```cpp
QFuture<McpResult<QList<McpTool>>> McpClient::listTools();
```

内部私有方法：

```cpp
struct ListToolsPage
{
    QList<McpTool> tools;
    std::optional<QString> nextCursor;
    qint64 ttlMs = 0;
    McpCacheScope cacheScope = McpCacheScope::Private;
    QJsonObject meta;
    QJsonObject raw;
};

QFuture<McpResult<ListToolsPage>> listToolsPage(
    const std::optional<QString> &cursor);
```

自动聚合流程：

```text
检查 Client Running 且 Server 声明 tools capability
  → 请求第一页（不发 cursor）
  → 完整解析 ListToolsPage
  → 若 nextCursor 字段存在，原样请求下一页
  → 直到 nextCursor 不存在
  → 返回 QList<McpTool>
```

最低安全约束：

- 最大 64 页；超过后返回明确的 `InvalidResponse` 或专用 `PaginationLimitExceeded`。第一版可复用 `InvalidResponse`，避免扩张错误枚举。
- 记录已见 cursor；重复 cursor 立即失败。
- 每一页都独立经过 `RequestManager`，具有请求超时。
- 聚合顺序严格按页顺序和每页 Tool 顺序保留，不自行重新排序 Client 收到的列表。
- 任何一页失败，则整个 `listTools()` 失败，不返回不完整列表。
- Tool 解析失败应明确失败；唯一规范要求“剔除而非整页失败”的特殊情况，是 Streamable HTTP 下不合法的 `x-mcp-header` Tool。

## 五、缓存建议

`tools/list` 是协议规定的 Cacheable Result。普通 API 可以隐藏缓存细节，但内部不能忽略：

- fresh 缓存存在时，`listTools()` 可直接返回缓存。
- `ttlMs == 0` 时不将结果视为 fresh。
- 收到 `notifications/tools/list_changed` 时立即使对应缓存失效。
- `private` 缓存必须绑定授权上下文；`public` 才可跨授权上下文复用。
- Server 返回的 Tool 集合允许因“当前请求携带的授权”而不同，但不能因连接身份或同一连接上的其他请求副作用而变化。

对自动聚合结果采用保守合并：fresh 时长取所有页面剩余 TTL 的最小值；任一页为 `private`，聚合缓存即为 `private`。各页的原始缓存字段仍保存在内部页结果中。

第一版不建议暴露 `CacheMode`、缓存存储接口或刷新 Policy。先实现一个有界的 Client 内存缓存即可；如果尚未实现缓存，应每次重新请求，但仍正确解析字段。这仍兼容协议，因为缓存是 MAY。

## 六、能力与列表变化通知

Server 如果提供工具，必须在 `server/discover` 的 capabilities 中声明：

```json
{
  "tools": {
    "listChanged": true
  }
}
```

- `tools` capability 存在：Server 支持 `tools/list`/`tools/call`。
- `tools.listChanged == true`：Server 支持工具列表变化通知。
- 声明 `tools` 的 Server 必须响应 `tools/list`，集合可为空。
- Client 在 Server 没声明 `tools` 时不应发送 `tools/list`；LibMcp 推荐直接返回 `McpErrorCode::CapabilityNotSupported`（需要加入错误枚举），而不是等远端 `MethodNotFound`。

2026-07-28 不允许沿用旧版“连接上随时推送 list_changed”的假设。Client 必须打开：

```json
{
  "method": "subscriptions/listen",
  "params": {
    "_meta": { "...": "..." },
    "notifications": {
      "toolsListChanged": true
    }
  }
}
```

Server 规则：

- 只能向明确请求 `toolsListChanged: true` 的订阅流发送 `notifications/tools/list_changed`。
- `notifications/subscriptions/acknowledged` 必须是该 subscription ID 上的第一条通知；在确认前不能发送工具变化通知。
- 订阅流上的每条通知 `_meta` 都必须包含 `io.modelcontextprotocol/subscriptionId`，值是打开 listen 请求的 JSON-RPC ID。
- Server 声明 `listChanged` 且工具集合变化时 SHOULD 发送通知。
- 收到通知后 Client 应使工具缓存失效并重新调用 `tools/list`；通知本身不携带新列表。

LibMcp 第一版建议：

- `McpClient::start()` 完成 discover 后，如果 `tools.listChanged` 为 true，则由 Client 内部统一的 SubscriptionManager 打开 listen（可以与 prompts/resources 复用同一个 filter）。
- 确认订阅建立后，收到工具变化通知时清缓存并发出一个简单 Qt 信号：

```cpp
signals:
    void toolsChanged();
```

- 不把 subscription ID、acknowledged 和 listen 生命周期暴露给普通使用者。
- `subscriptions/listen` 是长生命周期请求，不能塞进目前假设“普通请求最终很快得到一个响应”的 `RequestManager` Pending Request 模型；应由后续独立的 SubscriptionManager 管理。`listTools()` 本身不因此变复杂。

## 七、Server 侧最低兼容实现

`McpServer` 对外仍可使用已经确定的简单注册接口：

```cpp
bool addTool(const McpTool &tool, McpToolFunction function);
```

Server 内部必须：

- 注册前检查 `name`、必需 `inputSchema`（根 type=object）和支持的 JSON Schema dialect。
- `tools/list` 返回当前授权上下文可见的工具。
- 返回稳定、确定的顺序；推荐按 `name` 的 UTF-8/代码点顺序排序，不依赖 `QHash` 迭代顺序。
- 每页带 `resultType: "complete"`、`ttlMs`、`cacheScope`，以及存在更多页时的 `nextCursor`。
- cursor 必须是不透明、可验证的 Server 令牌；非法或过期 cursor 返回 JSON-RPC `InvalidParams (-32602)`。
- 工具新增、移除或替换后，使 Server 内部列表版本变化，并向已确认且订阅了 `toolsListChanged` 的 listen 流发送通知。
- 工具列表不能因同一连接上发生过的请求而变化；若按权限过滤，必须以当前请求的授权输入为准。

第一版 Server 可以采用固定页大小和简单稳定 keyset cursor，不需要公开分页策略类。页大小是 Server 内部常量即可。

## 八、推荐的公共 API

最终推荐：

```cpp
class McpClient : public QObject
{
    Q_OBJECT

public:
    QFuture<McpResult<QList<McpTool>>> listTools();

signals:
    void toolsChanged();
};
```

理由：

- 返回强类型 `McpTool`，完整保留规范定义的 Tool 数据。
- 自动聚合分页，避免普通用户漏掉第二页后的工具。
- cursor、`resultType`、缓存和订阅都由 SDK 自动处理。
- 不暴露 Options、Policy 或多个重载。
- 协议页模型完整保留在内部，未来若确有高级分页需求，可增加高级接口而无需改动 Codec/RequestManager。

`listTools()` 只做薄编排：能力检查、调用私有分页方法、聚合、缓存。它不生成 Request ID、不匹配响应、不直接操作 Transport；这些仍由 `RequestManager` 负责。

## 九、官方依据

- [MCP 2026-07-28 Tools 规范](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/server/tools.mdx)
- [MCP 2026-07-28 TypeScript Schema（权威消息结构）](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)
- [MCP 2026-07-28 Basic / `_meta` / JSON Schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/index.mdx)
- [官方 TypeScript SDK v2 迁移文档：分页聚合、缓存与列表行为](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/docs/migration/upgrade-to-v2.md)
- [官方 TypeScript SDK 通知文档：2026-07-28 subscriptions/listen](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/docs/servers/notifications.md)

