# AiLib 架构与阶段 1 契约

## 范围与依赖

技术栈为 C++17、Qt 5.15 / Qt 6.2+、CMake。阶段 1 仅提供 canonical model、公共配置、取消状态、媒体构造及基本校验、消息查询。没有 Agent/Client/Executor 运行时实现，也没有 Transport、完整 Adapter 接口或 FakeTransport。

层次为 Application → Agent → LLMClient → ProtocolAdapter → ITransport。实际请求由 Client 协调 Adapter 编解码和 Transport 传输，Adapter 不自行访问网络。Adapter 可以依赖 canonical model；模型不依赖 Adapter 或任何厂商类型。

Application 决定线程、GUI 更新、长期历史、Registry 生命周期与并发修改同步。SDK 对外采用同步调用和 bool + 输出参数 + SdkError。Callback 在调用线程执行。底层网络允许内部 Qt 事件机制，但 Agent 业务不使用信号槽，不创建线程。

公共模型依赖方向：ToolCall / ToolResult / MediaResource → Content → Message → ChatRequest / ChatResponse → AgentRequest / AgentResult。Usage 由响应和 AgentResult 使用；ProviderConfig、ModelInfo 是独立描述模型。

## 消息与内容

Message.contents 是唯一有序内容容器。MessageContent 使用 variant：Text、Reasoning、Image、Audio、Video、File、ToolCallContent、ToolResultContent。工具 Content 只包装 ToolCall / ToolResult，不复制字段。

text() 按顺序拼接 TextContent，不加入分隔符，不包含 Reasoning。toolCalls() 返回按内容顺序提取的值副本。查询不会修改底层内容。Message 不保存厂商索引、消息格式或原始 JSON 扩展袋。

MessageStatus 只表示 Complete / Incomplete。取消、超时、流异常时保留有效部分文本，无有效内容不创建空助手消息。未完成 ToolCall 只留在未来的请求级 Session，不进入正式消息。

第一版功能目标为文字和图片输入、文字输出；其他 Content 类型预留数据表达，不代表协议功能已经实现。

## 工具调用与结果

ToolCall.id 非空，在同一 Assistant 响应内唯一，不要求跨轮唯一。Adapter 负责协议规范化：原生 ID 非法不能修补；协议无原生 ID 且允许建立关联时可生成内部 ID。partIndex 只用于请求内分片聚合，不能替代 ToolCall.id。

ToolResult 描述 Function Call 的业务结果；SdkError 描述 SDK 调用链故障。成功使用 data，错误字段为空；失败使用开放字符串 errorCode 和可读 errorMessage。SDK 内置工具错误码集中在 ToolErrorCodes.h，自定义 Handler 可以扩展。data 不承载错误包装。

Executor 无条件根据输入调用填写 callId / toolName。Handler 只负责业务结果。Handler 返回 false 或抛出 C++ 异常默认转换为失败 ToolResult，由模型继续处理；不自动重试工具。Executor 的取消或 Agent 总截止时间停止信号由 Agent 处理，不转成继续执行的工具失败。

FunctionToolDefinition 仅保存名称、描述、Schema、Never/Always 确认策略、Serialized/Concurrent 并发策略。Handler、执行锁、统计不属于 Definition。Registry 工具条目保存共享执行锁，确认时不持有工具执行锁。Registry 支持多线程只读；涉及修改时调用方负责同步及在途对象生命周期。

第一版只实现本地 Function Tool。MCP 后续作为客户端执行后端，不属于 Provider Builtin Tool；当前不建立 MCP 类型或工具路由抽象。服务端 Builtin Tool 由 Client/Adapter/Provider 协议处理，第一版未支持时明确报 UnsupportedFeature/UnsupportedToolType。

## 媒体资源

MediaResource 普通值结构的 StorageType 为 Url、Bytes、LocalFile、FileReference。各类型仅允许对应来源字段，其他来源字段必须为空。FileReference 只保存 fileId，不加入 providerId 或跨服务迁移机制。

fromUrl/fromBytes/fromLocalFile/fromFileReference 直接构造，不执行网络或文件读取。简单构造后的字段仍需在 Adapter 编码前校验。mimeType 可以未知，由后续媒体处理决定要求。

fromBase64 使用 bool + output + error，严格校验 RFC 4648 标准字母表、四字符长度、末尾填充及零填充位，不接受空白、URL-safe 字母表、Data URL 或缺失填充。合法空输入成功得到空 Bytes；非法输入失败，output 不变。成功清空 error。

validateMediaResource 检查来源字段匹配、缺失、冲突及绝对 URL 基本合法性。HTTP(S) URL 必须有主机；其他 scheme 是否支持留给 Adapter。它不读取文件，不检查媒体内容或模型能力。空 Bytes 结构合法，不等同于有效图片。

## 用量与能力

Usage 的 inputTokens/outputTokens/totalTokens 都为 optional。nullopt 是未报告，0 是明确为零。不自行推算 totalTokens。

Agent 将返回每轮 Usage 和累计 Usage。某累计字段只要存在一轮未知，完整累计值就未知；不同字段独立处理。同轮 Streaming 更新使用最新累计值，不重复相加。没有 LLM 调用的 Run 总量可以为零。重试不是新的 Agent 轮次；耗尽尝试不保证掌握服务端实际计费用量。阶段 1 不实现汇总。

ModelInfo.capabilities 的缺失项/nullopt 为未知，true/false 是能力提示，不作为强制准入；contextWindow/maxOutputTokens 也允许未知。Adapter 校验协议是否能够编码，模型是否接受交由服务端；不根据模型名称推断。

## 配置与请求

ProviderConfig 显式选择 ProtocolType。baseUrl 是 API 根路径，Adapter 追加端点、保留前缀，不自动补 /v1，不猜协议。apiKey 可以为空，此时不生成默认认证 Header。

Header 名称比较不区分大小写；customHeaders 可覆盖默认认证。Adapter 控制的 Protected Headers 冲突报错。extraParameters 是 JSON 参数，不能包含 Adapter 的 reserved fields，即使对应标准字段未设置也不能使用。所有检查在 HTTP 发送前执行。

ProviderConfig 构造时由 Client 按值保存，之后不提供修改接口。LLMClient unique_ptr 独占 Adapter、Transport，Factory 转移所有权；直接注入同样适用。Factory 目前提供真实实现的 OpenAIChatCompatibleAdapter、OpenAIResponsesAdapter 和 AnthropicMessagesAdapter，`supportsProtocol()` 报告同一组实际能力，其他内置协议报 UnsupportedProtocol。

ModelRegistry 保存无凭据 ProviderConfig 模板及其已知 ModelInfo 列表。注册与查询由读写锁保护，查询返回值快照；模型使用 providerId + modelId 定位。目录提供已知信息而非调用白名单，未收录模型仍可由应用在已注册 Provider 下发送。

ChatResponse 只表达一个 Assistant 响应，正式结果候选数必须是一个。Streaming 的 Usage-only 事件不单独计算候选。多候选不得静默丢弃。

FinishReason 是模型停止原因；CompletionState 是响应是否协议完整获得。网络正常关闭不代表流完整。Length 可以对应完整协议响应和不完整消息。完成条件由 RequestDecoder 解释，Session/Client/Agent 不感知厂商结束标记。

后续配置优先级：Agent 本次覆盖 > 本次 RequestOptions > Client 默认 RequestOptions。RequestOptions 当前是完整值配置，不是逐字段 patch；阶段 1 不实现合并。

默认每次 HTTP 尝试超时 120 秒，-1 不限时，其余必须正数。RetryPolicy 默认重试 5 次（最多六次请求），间隔 30000ms，只启用网络连接类错误、429、临时 5xx；非临时请求错误不重试。服务端 Retry-After 可覆盖等待建议。收到有效流式内容后不重试，每次尝试创建独立 Decoder/Session。阶段 1 只定义配置，不实现等待和重试。

## Agent 契约

Agent unique_ptr 持有 Client，值成员持有 Executor；应用持有 Registry 和可选 IToolApprovalProvider，Executor 非拥有引用/指针使用它们。agentId 是实例稳定身份，Context 只携带，不绑定 Executor。未指定 ID 时生成，指定时应用确保唯一。Agent 同时只允许一个 run，重入返回 AgentBusy；不同 Agent 可并发。Client 允许并发 chat，请求可变状态独立。

AgentRequest.chat.messages 是调用方历史，Agent 不裁剪、不总结、不删除后重试。enabledTools 严格白名单，空为零工具；启动前检查所有名称已注册。Agent 不使用输入 chat.tools 作为工具来源。执行时再次遵守白名单。

maxTurns 默认 20，最小 1；一轮是一次逻辑 LLM 调用，不包含重试。maxToolCalls 默认 -1 不限，0 禁止，正数统计所有 Handler 实际执行总次数，失败也计数。整批 ToolCall 超出剩余额度则全部不执行。最后一轮仍输出工具调用则不执行，结束 MaxTurns。

总超时默认 600 秒，LLM 每次尝试默认 120 秒；-1 不限时，0 和小于 -1 无效。Agent 覆盖本轮 RequestOptions.timeoutSeconds/retryPolicy，不修改原配置。每次尝试受总剩余时间限制，重试等待计入总时间。没有统一工具超时，由 Handler 自行管理并协作遵守 Token/总截止时间。

单次 Run 的唯一取消来源是 request.requestOptions.cancellation，传给所有子流程。默认 Token 不取消，Source 取消不可重置；Source 的副本共享同一状态，销毁 Source 不使已有 Token 失效。无法中途取消的同步操作返回后检查，不强制终止线程。

Approval 同步接口使用 Allow/Deny/Cancel：Deny 或确认不可用转工具失败，Cancel 结束 Run。GUI 确认投递和等待由应用提供，不在 SDK 内实现 UI。

Streaming 不执行工具，只有完整成功响应后才处理整批调用。模型生成事件和实际执行事件名称分开。Length 不是 SdkError，不执行该响应工具，不自动续写。

AgentResult 只返回本次 newMessages，不拥有历史。finalMessage 只在 Completed 存在。bool true 表示可预期正常停止：Completed/Length/Cancelled/MaxTurns/MaxToolCalls/Timeout；false + Failed + SdkError 表示配置、Provider、网络、协议或内部故障。总时间耗尽是 Timeout；底层请求超时是 Failed。无论返回值都保留实际产生的新消息。

## 分阶段与测试

阶段 1 完成后停止，确认后才进入阶段 2。每阶段独立编译、Qt Test、汇报实际接口、结果和问题。阶段 2 才正式定义 ITransport/TransportRequest/TransportResponse/IProtocolAdapter 并实现 FakeTransport。

unit 上层测试使用 FakeTransport；transport 测试使用本地 HTTP 服务验证真实 Qt 网络栈；重试通过 Client + Transport 联合测试。manual 真实 Provider 测试可选，不是 CI 必要条件。测试基础设施不进入 SDK 运行时。阶段 1 只验证模型、查询、媒体和取消。

## 阶段 3 运行时接口

普通和 Streaming 共用同步 LLMClient::chat。ChatRequest.stream 决定模式；RequestOptions.streamCallback 在调用线程接收标准化事件，不负责聚合或工具执行。无 callback 的流式请求也能得到完整 ChatResponse。

ITransport.sendStream 提供原始片段及 HTTP 元数据，非成功状态正文继续交给 Adapter 的普通错误解析。IProtocolAdapter.createStreamDecoder 每次创建独立 IStreamDecoder；feed 将数据转换为事件，finish 验证 EOF 处协议状态。默认扩展实现明确返回不支持，既有自定义普通 Transport/Adapter 无需立即实现流式接口。

StreamSession 只接受 SDK 逻辑 partIndex，按首次出现顺序聚合文本、推理、工具名称、调用 ID 和参数。未完成或截断参数不进入正式消息；模型 Length 可正常结束。Usage 使用已报告字段的最新快照，不重复累计，不推导未知总量。Callback 抛异常转换为流程故障，同时保留已成功聚合内容。

实际协议终止标识只存在于具体 Decoder 内。FinishReason、CompletionState 与 SdkError 分离；连接异常、取消、请求超时或缺少完整结束标识均保留有效数据并返回 false + Incomplete + SdkError。自动重试仍未执行，等阶段 7 实现。


阶段 4 已实现独立 ToolRegistry / ToolExecutor / ToolHandler / IToolApprovalProvider。执行器采用 bool + ToolResult + SdkError：工具失败保持 true，取消、总截止时间或非法核心调用返回 false；Agent 后续按既定运行语义解释停止信号。ToolExecutionContext 以可选 steady_clock 截止时间传递总预算。参数 Schema 当前是严格校验的显式子集，未支持关键字在注册时拒绝；详见 phase4-report.md。Agent Loop 已在阶段 5 接入，见下节。


## 阶段 5 当前实现

最小 Agent 已通过独占 Client、值成员 Executor、应用拥有的 Registry/Approval 完成同步多轮工具闭环。严格白名单、增量消息、工具失败继续决策、完整流结束后执行、Cancelled/Length/MaxTurns/Failed、实例重入保护和 Usage 汇总已接入。实际 Handler 次数上限、工具状态事件及更完整的时间预算细化留到阶段 6；当前 maxToolCalls 非 -1 明确报不支持。秒级请求 timeout 剩余时间向上取整，严格毫秒级预算尚未完成。完整验收及限制见 phase5-report.md。上述阶段性实现不改变前文最终契约。


## 阶段 6 当前实现

Agent 已实现 maxToolCalls 的实际 Handler 次数统计和整批超额停止，所有工具共用 Run 内总额度；拒绝和前置校验失败不消耗额度。AgentRequest.callback 通知 ToolExecutionStarted/Finished，RequestOptions.deadline 传递精确单调时钟总预算，Qt Transport 按更早的尝试/总截止时间中止。阶段 5 的不支持额度和整秒取整限制已解除；前一节仅记录当时状态。单实例并发保护、跨 Agent 并行、停止保留实际结果和增量消息等已验证。未增加 runId 或第二套 CancellationToken。自动重试仍留到阶段 7，详见 phase6-report.md。


## 阶段 7 当前实现

LLMClient 已实现既定固定间隔重试，网络临时错误、429/5xx 分别按策略开关判断，maxRetries 不含首次请求。Retry-After 优先并保留，秒数及三种 HTTP 日期已统一解析。每次流式尝试创建独立 Decoder/Session，有效输出后不重试；只通知最终流 Error。等待和每次尝试遵守统一 Token/精确 deadline，不增加 Agent 轮数或重复执行工具。此前各节“不执行重试”仅记录当时阶段状态；完整验证见 phase7-report.md。CLI 与 Widgets Demo 尚待阶段 8。

## 阶段 8 当前实现

CLI 与 Widgets Demo 由应用自行管理线程、确认与历史，并从统一模型目录选择真实 Provider 和模型。GUI 工作线程同步 run，UI 队列更新文本与非模态确认，等待中检查同一取消令牌与截止时间；SDK 不增加 Widgets 或线程创建依赖。示例细节与验证范围见 [示例使用说明](examples.md) 和 [阶段 8 报告](phase8-report.md)。
