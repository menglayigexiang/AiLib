# 需求计划：Provider 模型目录与 OpenAI Responses 协议

> 状态：已实施  
> 范围：让应用查询厂商提供的模型，并让 AiLib 能调用 OpenAI 当前最新的 Responses API 模型。

## 1. 目标

本次只完成两项能力：

1. 建立简单的 `Provider -> QList<ModelInfo>` 模型目录，供 TestApp 的 LibAiCore 页面、CLI 和其他调用方列出厂商及其模型；
2. 实现 `OpenAIResponses` 协议，使 OpenAI 最新通用模型可用于现有 Client 和 Agent 流程。

首批目录包含：

| Provider | 协议 | 模型 |
|---|---|---|
| DeepSeek | `OpenAIChatCompletions` | 沿用项目当前模型 |
| Kimi | `AnthropicMessages` | 沿用项目当前模型 |
| OpenAI | `OpenAIResponses` | `gpt-5.6-sol`、`gpt-5.6-terra`、`gpt-5.6-luna` |

OpenAI 默认 Endpoint 为 `https://api.openai.com/v1`，API Key 继续由应用层从 `OPENAI_API_KEY` 或用户输入取得。

## 2. 非目标

- 不做动态模型参数面板；
- 不新增 `ParameterSpec`、`ParameterTarget`、`ParameterValues`、`fixedParameters` 或参数 Builder；
- 不从网络动态同步模型目录；
- 不把模型目录作为调用白名单；
- 不做动态 Adapter 插件；
- 不做配置持久化或多套配置档案；
- 不支持用户自定义 `baseUrl + protocol` 的 Runtime Provider；
- 不实现图像生成、音频、Realtime、Batch 等 OpenAI 专用 API；
- 不保留 TestApp 的 `offline/demo` 运行模式。

## 3. 模型目录设计

### 3.1 数据结构

复用现有 `ModelInfo`，不为模型参数另建数据模型。

```cpp
// 描述一个 Provider 的无凭据连接模板及已知模型。
struct ProviderEntry {
    ProviderConfig configTemplate;  // Provider 固有的无凭据连接配置
    QList<ModelInfo> models;        // 该 Provider 的已知模型
};
```

`configTemplate` 可以保存：

- `id`、`name`；
- `baseUrl`；
- `protocol`；
- 固有且非秘密的 `customHeaders`。

`configTemplate` 不得保存 API Key、Token、Cookie、Authorization 或运行策略。`registerProvider()` 强制要求 `apiKey` 为空。应用创建 Client 时复制模板并注入凭据：

```cpp
ProviderConfig config = entry.configTemplate;
config.apiKey = userApiKey;
```

### 3.2 ModelRegistry

`ModelRegistry` 是 LibAiCore 的公共模型目录服务：

- 支持独立构造，同时提供进程级 `instance()`；
- 使用 `QReadWriteLock` 支持并发注册和查询；
- 查询返回值副本，避免锁释放后的引用失效；
- 注册先完整校验，再原子写入；
- 错误接口沿用项目的 `bool + SdkError&`；
- Provider 唯一键为 `configTemplate.id`；
- 模型唯一键为 `providerId + modelId`，不同 Provider 可以有同名模型。

建议公共接口：

```cpp
bool registerProvider(const ProviderEntry& entry, SdkError& error);
QList<ProviderEntry> providers() const;
std::optional<ProviderEntry> findProvider(const QString& providerId) const;
std::optional<ModelInfo> findModel(
    const QString& providerId,
    const QString& modelId) const;
```

注册校验至少覆盖：Provider ID 和名称非空、ID 不重复、Endpoint 有效、API Key 为空、协议已实现、同一 Provider 内模型 ID 不重复。

模型目录描述 SDK 已知信息，但不限制调用。用户可以在已注册 Provider 下输入未收录的 model ID；这种模型不写入 Registry，也没有能力元数据，由服务端判断是否支持。

### 3.3 内置目录

Registry 机制放在 LibAiCore，厂商数据放在 `examples/support/BuiltinCatalog.{h,cpp}`。内置目录统一提供 DeepSeek、Kimi、OpenAI 条目，TestApp 的 LibAiCore 页面和 CLI 不再各自硬编码列表。

## 4. OpenAI Responses Adapter

`ProtocolType::OpenAIResponses` 已存在，但 `LLMClientFactory` 尚未实现对应分支。本次新增：

- `OpenAIResponsesAdapter`：普通请求编码与响应解码；
- `OpenAIResponsesStreamDecoder`：Responses SSE 事件解析；
- `LLMClientFactory` 的 `OpenAIResponses` 创建分支；
- `LLMClientFactory::supportsProtocol()`，作为协议实现状态的唯一查询入口；
- 对应 CMake 源文件和公开头文件配置。

Adapter 继续使用现有 canonical 类型，不改变 `ChatRequest`、`ChatResponse`、`StreamEvent`、Tool 或 Agent 公共接口。

### 4.1 请求映射

至少覆盖：

- 模型 ID；
- 有序消息历史与角色；
- 文本内容；
- `maxOutputTokens -> max_output_tokens`；
- Function Tool 定义；
- Tool Choice；
- 工具执行结果回传；
- 流式开关；
- 允许的 `extraParameters`，并拒绝 Adapter 保留字段冲突。

若 Responses API 无法无损表达某个现有 canonical 输入，应返回结构化 `SdkError`，不能静默丢弃。

### 4.2 响应映射

普通响应至少覆盖：

- 助手文本；
- Function Tool Call 的 ID、名称和参数；
- 输入、输出及总 Token 用量；
- 完成、长度限制、取消和错误等停止原因；
- HTTP 错误与 OpenAI 错误对象。

### 4.3 流式映射

流式 Decoder 应按现有 `IStreamDecoder` 契约处理 Responses SSE：

- 增量文本；
- Tool Call 名称与参数增量；
- 完成事件与最终用量；
- 服务端错误事件；
- EOF 前协议完整性检查；
- 分片输入、多个事件同片和跨片事件。

首期要求普通请求、流式响应和 Function Tools 都能进入现有 Agent 多轮流程。Adapter 只做协议映射和序列化，不承担模型目录或模型参数合并职责。

## 5. TestApp 与 CLI

- 删除 `OfflineTransport`、`offline/demo` 选项及特殊分支；
- 启动时注册内置目录；
- TestApp 的 LibAiCore 页面从 Registry 生成 Provider 和模型选择项；
- Provider 变化时刷新对应模型列表；
- 允许在当前 Provider 下手工输入未知 model ID，并提示该模型没有目录能力信息；
- API Key 输入值优先，空值时回退对应环境变量；
- CLI 的 Provider 和模型校验改用 Registry；
- Client 创建流程复制 `configTemplate`，注入 API Key 后交给 `LLMClientFactory`；
- 保持现有工作线程、确认流程和 Agent 多轮执行方式。

环境变量：

| Provider | 环境变量 |
|---|---|
| DeepSeek | `DEEPSEEK_API_KEY` |
| Kimi | `KIMI_CODE_API_KEY` |
| OpenAI | `OPENAI_API_KEY` |

## 6. 实施顺序

1. 盘点并移除 TestApp、CLI、手工测试和文档中的 offline/demo 依赖；
2. 实现 `OpenAIResponsesAdapter` 的普通请求与响应；
3. 实现 Responses SSE Decoder 和 Function Tools 映射；
4. 为 Factory 增加 `supportsProtocol()` 和 `OpenAIResponses` 分支；
5. 新增 `ProviderEntry`、`ModelRegistry` 及单元测试；
6. 新增 `BuiltinCatalog`，迁移 DeepSeek/Kimi 数据并加入 OpenAI 最新模型；
7. TestApp 的 LibAiCore 页面和 CLI 改为读取 Registry；
8. 更新架构、示例和协议支持文档；
9. 运行全部自动化测试与必要的真实协议验收。

## 7. 测试与验收

### 7.1 Responses 协议测试

- 普通文本请求编码及响应解码；
- 多轮消息及角色映射；
- Function Tool 定义、Tool Call 和工具结果回传；
- SSE 任意分片下的文本与工具参数聚合；
- 完成事件、用量和停止原因；
- HTTP 错误、协议错误、未知事件和不完整 EOF；
- Factory 对 `OpenAIResponses` 返回可用 Client；
- `supportsProtocol()` 与 Factory 实际创建能力一致。

### 7.2 Registry 测试

- Provider 注册、重复拒绝和查询；
- `providerId + modelId` 查询；
- 无凭据模板约束；
- 无效 Endpoint 与未实现协议拒绝；
- 同一 Provider 内重复模型拒绝；
- 并发查询与注册；
- 查询副本不受后续注册影响；
- 内置 DeepSeek、Kimi、OpenAI 数据正确。

### 7.3 最终验收

- TestApp 的 LibAiCore 页面可选择 DeepSeek、Kimi 和 OpenAI 的已知模型；
- OpenAI 显示 `gpt-5.6-sol`、`gpt-5.6-terra`、`gpt-5.6-luna`；
- OpenAI 普通、流式及工具调用能通过现有 Client/Agent 流程；
- 未收录 model ID 可在已注册 Provider 下发起请求；
- Registry 和示例代码均不保存 API Key；
- TestApp 与 CLI 不再包含 offline/demo 分支；
- 更新受 offline 删除影响的测试，其余现有测试继续通过。

## 8. 风险与控制

| 风险 | 控制方式 |
|---|---|
| Responses 事件种类多、流式顺序复杂 | 使用录制协议样本覆盖分片、工具调用、错误和 EOF |
| OpenAI 模型目录随时间变化 | 目录保持少量最新通用模型，未知 model ID 仍允许调用 |
| 目录展示了尚未实现的协议 | 注册时调用 `LLMClientFactory::supportsProtocol()` |
| Provider 模板误存凭据 | 强制 apiKey 为空，文档禁止秘密 Header，内置数据增加检查 |
| 删除 offline 影响现有验收 | 自动测试改用 Fake/Mock Client，不把 Mock 建模成 Provider |

## 9. 官方依据

- OpenAI 当前模型目录：https://platform.openai.com/docs/models
- Responses API：https://platform.openai.com/docs/api-reference/responses
- Streaming events：https://platform.openai.com/docs/api-reference/responses-streaming
