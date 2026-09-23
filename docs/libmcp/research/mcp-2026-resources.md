# MCP 2026-07-28 Resources 调研与 LibMcp 设计建议

## 结论先行

LibMcp 的 Resources 第一版应完整支持三项基础 RPC：

- `resources/list`
- `resources/templates/list`
- `resources/read`

推荐的 Client 公共接口保持为三个方法：

```cpp
QFuture<McpResult<QList<McpResource>>> listResources();

QFuture<McpResult<QList<McpResourceTemplate>>> listResourceTemplates();

QFuture<McpResult<McpReadResourceResult>> readResource(
    const QString &uri);
```

两个列表接口在内部自动处理全部分页。`readResource()` 返回完整强类型结果，不能只返回 `QString` 或 `QByteArray`，否则会丢失多内容项、每项 URI、MIME 类型、二进制内容和 `_meta`。

资源变化订阅属于协议的可选能力，2026-07-28 使用统一的 `subscriptions/listen`，不再使用旧的 `resources/subscribe` / `resources/unsubscribe`。当前实现已经提供长寿命订阅、资源过滤、取消和最终结果，不保留旧式订阅入口。

## 依据与范围

本结论只依据以下官方材料：

- [MCP 2026-07-28 TypeScript Schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)
- [MCP 2026-07-28 JSON Schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.json)
- [2026-07-28 规范变更说明](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx)
- [官方 Server Concepts：Resources](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/docs/2026-07-28/learn/server-concepts.mdx)
- [官方 TypeScript SDK Resources 文档](https://github.com/modelcontextprotocol/typescript-sdk/blob/main/docs/servers/resources.md)
- [官方 Python SDK Client 文档](https://github.com/modelcontextprotocol/python-sdk/blob/main/docs/client/index.md)
- [官方 Python SDK 缓存文档](https://github.com/modelcontextprotocol/python-sdk/blob/main/docs/client/caching.md)

## 1. 能力声明

Server 只要提供资源能力，就应在 `server/discover` 的 `ServerCapabilities` 中出现 `resources`：

```json
{
  "resources": {
    "subscribe": true,
    "listChanged": true
  }
}
```

语义如下：

- `resources` 存在：Server 提供资源读取能力。
- `subscribe` 可选：Server 支持订阅单个资源 URI 的更新。
- `listChanged` 可选：Server 支持资源列表变化通知。
- 字段缺失不能当成 `true`。

Client 在调用资源 RPC 前可检查 `resources` 能力；如果 Server 未声明该能力，SDK 应返回 `CapabilityNotSupported`，而不是仍然发送请求。

建议的强类型：

```cpp
struct McpResourceCapabilities
{
    std::optional<bool> subscribe;
    std::optional<bool> listChanged;
};
```

使用 `std::optional<bool>` 可以区分“未声明”和“明确为 false”。

## 2. resources/list

### 2.1 请求

方法名固定为：

```text
resources/list
```

请求继承通用分页请求，`params` 必须存在并带每请求 `_meta`；后续页可带不透明 `cursor`：

```json
{
  "jsonrpc": "2.0",
  "id": "1",
  "method": "resources/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    },
    "cursor": "opaque-cursor"
  }
}
```

第一页省略 `cursor`。Cursor 是不透明字符串，Client 不得解释、改写或自行生成。

### 2.2 响应

`ListResourcesResult` 包含：

- `resultType`：2026-07-28 的所有 Result 必需；正常最终结果为 `"complete"`。
- `resources`：必需的 `Resource[]`。
- `ttlMs`：必需，非负毫秒数。
- `cacheScope`：必需，只能是 `"public"` 或 `"private"`。
- `nextCursor`：可选；存在表示可能还有下一页。
- `_meta`：可选，必须保留未知字段。

安全且始终合规的 Server 默认值是：

```json
{
  "resultType": "complete",
  "resources": [],
  "ttlMs": 0,
  "cacheScope": "private"
}
```

### 2.3 分页实现

公共 `listResources()` 自动聚合所有页：

```text
请求第一页
  -> 保存 resources 顺序
  -> 有 nextCursor 时原样请求下一页
  -> 无 nextCursor 时完成
```

内部应防止恶意或错误 Server 导致无限循环：记录已见 cursor，并设置合理最大页数。任何一页失败时整体失败，不返回悄悄截断的部分列表。

Server 应以稳定顺序返回资源，便于缓存和可重复测试；LibMcp 可按 URI 排序。Client 不应擅自重排外部 Server 返回的顺序。

## 3. resources/templates/list

### 3.1 请求与响应

方法名固定为：

```text
resources/templates/list
```

它与 `resources/list` 使用相同的分页模型。结果字段为：

- `resultType`：必需。
- `resourceTemplates`：必需的 `ResourceTemplate[]`。
- `ttlMs`：必需。
- `cacheScope`：必需。
- `nextCursor`：可选。
- `_meta`：可选。

注意：`resources/templates/list` 同样继承 `CacheableResult`。官方 Tier 1 SDK 也把它列为缓存提示适用的方法。

### 3.2 模板 URI 规则

`ResourceTemplate.uriTemplate` 是字符串，并必须符合 RFC 6570 URI Template。它不是一个可直接读取的 URI。例如：

```text
weather://forecast/{city}/{date}
```

Client 必须先填充模板参数，生成实际 URI，再调用 `resources/read`。不建议用 `QUrl` 保存 `uriTemplate`，因为 `QUrl` 不是 RFC 6570 模板类型，花括号及模板表达式不应由它规范化。公共模型应保留原始 `QString`。

模板参数补全属于 `completion/complete`，不是 Resources 列表本身的职责，可在 Completion 模块中后续实现。

## 4. resources/read

### 4.1 请求

方法名固定为：

```text
resources/read
```

请求 `params.uri` 必需，可使用任意协议；如何解释 URI 由 Server 决定：

```json
{
  "jsonrpc": "2.0",
  "id": "3",
  "method": "resources/read",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    },
    "uri": "file:///workspace/readme.md"
  }
}
```

建议公共 API 使用 `QString`，并保留调用者传入的原始 URI 文本。内部可以用 `QUrl` 严格模式做基本语法检查，但不得因标准化、解码或重新编码改变 wire 值。

未找到资源时，2026-07-28 使用标准 JSON-RPC `Invalid params`（`-32602`）；旧版 `-32002` 已保留但不再用于本版本。

### 4.2 最终响应

`ReadResourceResult` 包含：

- `resultType`：必需，最终结果为 `"complete"`。
- `contents`：必需，可包含多个文本或二进制内容项。
- `ttlMs`：必需。
- `cacheScope`：必需。
- `_meta`：可选。

一个读取请求可能返回多个内容项，且每个内容项均带自己的 URI。

### 4.3 MRTR

2026-07-28 的 `resources/read` 成功响应是以下二者之一：

```text
ReadResourceResult | InputRequiredResult
```

也就是说，资源处理器可以请求额外的 elicitation、sampling 或 roots 输入，Client 完成这些输入后，以新 JSON-RPC Request ID 重试原请求，并原样回传不透明 `requestState`。

第一版 Resources Codec 必须能够识别 `resultType: "input_required"`，不能把它误报为无效资源结果。MRTR 的循环驱动应由 Client 公共的 MRTR 模块统一处理，不放进 `readResource()` 或 Resources Codec 内。最终普通用户仍得到 `McpReadResourceResult`。

## 5. 数据类型

### 5.1 Resource

Schema 中：

- `name`：必需，程序用途名称，也是无 `title` 时的显示回退。
- `uri`：必需，URI 字符串。
- `title`：可选。
- `description`：可选。
- `mimeType`：可选。
- `icons`：可选。
- `annotations`：可选。
- `size`：可选，原始内容字节数，发生在 base64 与 tokenization 之前。
- `_meta`：可选。

建议：

```cpp
struct McpResource
{
    QString name;
    QString uri;

    std::optional<QString> title;
    std::optional<QString> description;
    std::optional<QString> mimeType;
    QList<McpIcon> icons;
    std::optional<McpAnnotations> annotations;
    std::optional<qint64> size;
    QJsonObject meta;
};
```

JSON Schema 的 `size` 是 number；SDK 应拒绝负数、非整数或超出 `qint64` 范围的值，避免静默截断。

### 5.2 ResourceTemplate

必需字段：

- `name`
- `uriTemplate`

可选字段：

- `title`
- `description`
- `mimeType`
- `icons`
- `annotations`
- `_meta`

建议：

```cpp
struct McpResourceTemplate
{
    QString name;
    QString uriTemplate;

    std::optional<QString> title;
    std::optional<QString> description;
    std::optional<QString> mimeType;
    QList<McpIcon> icons;
    std::optional<McpAnnotations> annotations;
    QJsonObject meta;
};
```

### 5.3 Annotations 与 Icon

Resources 和 ResourceTemplates 共用协议定义：

```cpp
enum class McpRole { User, Assistant };

struct McpAnnotations
{
    QList<McpRole> audience;
    std::optional<double> priority;       // 0..1
    std::optional<QString> lastModified;  // 原始 ISO 8601 文本
};

struct McpIcon
{
    QString src;
    std::optional<QString> mimeType;
    QStringList sizes;
    std::optional<McpIconTheme> theme;    // light / dark
};
```

`lastModified` 建议保留原始字符串，同时在 Codec 中校验 ISO 8601；不要用 `QDateTime` 往返序列化改变对端文本。

### 5.4 ResourceContents

两种 wire 类型没有显式 `type` 判别字段，而是由 `text` 或 `blob` 区分：

```cpp
struct McpTextResourceContents
{
    QString uri;
    std::optional<QString> mimeType;
    QString text;
    QJsonObject meta;
};

struct McpBlobResourceContents
{
    QString uri;
    std::optional<QString> mimeType;
    QByteArray data; // wire 上是 base64 字符串
    QJsonObject meta;
};

using McpResourceContents = std::variant<
    McpTextResourceContents,
    McpBlobResourceContents>;

struct McpReadResourceResult
{
    QList<McpResourceContents> contents;
    qint64 ttlMs = 0;
    McpCacheScope cacheScope = McpCacheScope::Private;
    QJsonObject meta;
};
```

二进制内容在 C++ 公共 API 中使用 `QByteArray` 最自然；Codec 负责严格 base64 解码与编码。原始 `_meta` 要保留。

## 6. 缓存字段

`resources/list`、`resources/templates/list` 和 `resources/read` 的最终结果都必须包含：

```text
ttlMs >= 0
cacheScope == public | private
```

第一版建议：

- Client 正确解析、校验并保留字段。
- Server 默认发 `ttlMs: 0`、`cacheScope: "private"`。
- 暂时不实现 Client 本地缓存。
- 暂时不引入 CachePolicy、共享 CacheStore 等抽象。

这能保持协议兼容，同时避免过早增加缓存失效复杂度。

## 7. 变化订阅

2026-07-28 已删除旧式：

```text
resources/subscribe
resources/unsubscribe
```

统一使用长生命周期请求：

```text
subscriptions/listen
```

Resources 相关过滤条件：

```json
{
  "notifications": {
    "resourcesListChanged": true,
    "resourceSubscriptions": [
      "file:///workspace/readme.md"
    ]
  }
}
```

Server 只能在该监听流上发送 Client 明确订阅的类型：

- `notifications/resources/list_changed`
- `notifications/resources/updated`，参数含更新后的 `uri`

每条通过订阅流发送的通知必须在 `_meta` 中带 `io.modelcontextprotocol/subscriptionId`，以关联打开该流的 JSON-RPC Request ID。资源更新 URI 可能是订阅 URI 的子资源。

能力映射：

- `resources.listChanged == true` 才能提供资源列表变化。
- `resources.subscribe == true` 才能提供特定资源更新。

订阅功能应由独立 `SubscriptionManager` 统一承载 Tools、Prompts 和 Resources，不应分别塞进 `readResource()`。它可以在基础 Resources 第一阶段后实现。

## 8. 推荐 Client 公共 API

保持三个接口即可：

```cpp
class McpClient : public QObject
{
    Q_OBJECT

public:
    QFuture<McpResult<QList<McpResource>>> listResources();

    QFuture<McpResult<QList<McpResourceTemplate>>>
    listResourceTemplates();

    QFuture<McpResult<McpReadResourceResult>> readResource(
        const QString &uri);
};
```

不公开 cursor、分页页对象、缓存策略或 Request ID。完整协议字段仍由内部页类型和 Codec 保留：

```cpp
struct ListResourcesPage
{
    QList<McpResource> resources;
    std::optional<QString> nextCursor;
    qint64 ttlMs;
    McpCacheScope cacheScope;
    QJsonObject meta;
};
```

`listResourceTemplates` 使用对应页类型。二者通过已有 `sendMcpRequest()` 和 `RequestManager` 发送，不直接接触 Transport。

## 9. 推荐 Server 内部实现

基础职责：

- 独立 `ResourceRegistry` 保存固定资源和模板。
- `resources/list` / `resources/templates/list` 做稳定排序与 cursor 分页。
- `resources/read` 先精确匹配固定 URI，再匹配 RFC 6570 模板。
- 找不到资源返回 JSON-RPC `-32602`。
- 最终响应总是带 `resultType`、`ttlMs`、`cacheScope`。
- Handler 结果完整转换为文本或二进制 `contents`。
- MRTR 交给 Server 公共请求调度层，不由 Registry 处理。

为保持业务函数可复用，后续设计 Server 注册接口时，可让普通 Resource Handler 只接收已解析的实际 URI，并返回 `QList<McpResourceContents>`；不要把 Transport、JSON-RPC ID 或请求上下文强塞给普通 Handler。支持 MRTR 的高级 Handler 可以以后作为单独能力讨论，不能破坏基础 wire 兼容。

## 10. 第一阶段必须实现与可后置项

### 必须实现

- 三项 RPC 的准确 method、请求和结果 Codec。
- 每请求 `_meta`。
- `resultType`。
- 两个列表的 cursor 分页与自动聚合。
- 三项结果的 `ttlMs` / `cacheScope`。
- `Resource`、`ResourceTemplate` 完整字段。
- 文本与 blob 两种 ResourceContents。
- URI 原值保留；模板按 RFC 6570 建模。
- `resources/read` 对 `InputRequiredResult` 的识别与公共 MRTR 层衔接。
- `resources` 能力检查。
- 未找到资源使用 `-32602`。

### 可以后置

- Client 实际缓存响应。
- `subscriptions/listen` 的长流管理与通知信号。
- URI Template 参数自动补全。
- 面向 UI 的资源树、搜索和预览。
- 可插拔缓存后端。
- 高级 MRTR Resource Handler 便利接口。

## 11. 最小测试清单

- 列出零个、一个和多页固定资源。
- 列出多页模板并原样回传 cursor。
- 检测重复 cursor，防止无限分页。
- 解码所有 Resource / ResourceTemplate 可选字段。
- 读取单个与多个文本内容项。
- 读取 blob，并严格校验 base64。
- 保留每个内容项的 URI、MIME type 和 `_meta`。
- 缺失 `resultType`、`ttlMs`、`cacheScope` 时拒绝 2026-07-28 响应。
- 未知 URI 返回 `-32602`。
- `input_required` 结果交给 MRTR 层而非报 InvalidResponse。
- 未声明 resources 能力时 Client 不发资源请求。
- 外部 Server 返回顺序在 Client 侧保持不变。
