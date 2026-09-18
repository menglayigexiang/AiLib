# 阶段 2：最小普通 Chat 闭环

## 实际范围

同步非流式请求现已贯通：ChatRequest → LLMClient → IProtocolAdapter → TransportRequest → ITransport → TransportResponse → ChatResponse。Client 协调编码、网络和解析，不理解厂商 JSON；Adapter 不执行网络或工具。

- ITransport、TransportRequest、TransportResponse 为真实接口与值模型。send 返回 true 表示完成 HTTP 传输，包括 HTTP 4xx/5xx；false 表示网络、配置、取消或超时等传输故障。错误 HTTP 状态由 Adapter 映射。
- QtHttpTransport 在每个调用线程创建 QNetworkAccessManager/QNetworkReply/QEventLoop/QTimer，局部等待，不创建线程。需要调用方提前创建 QCoreApplication。支持单次尝试超时、协作取消、部分正文保留；不自动跟随 HTTP 重定向，不自动重试。
- IProtocolAdapter 本阶段只有普通请求编码与响应解码两个方法，没有提前定义 Streaming 接口。
- OpenAIChatCompatibleAdapter 编码单候选请求、文本历史、Function Tool 定义、助手调用和成功/失败工具结果；解析助手文字、兼容服务明确提供的 reasoning_content、ToolCall、Usage、FinishReason。
- 图片输入支持 URL、内存和本地文件；保留 Text/Image 原始顺序。Bytes/LocalFile 使用 MIME 数据库检测图片类型，生成 Data URL。本阶段不做完整像素解码、尺寸检查或可配置文件大小限制；不能将 MIME 识别等同完整图片有效性验证。FileReference 在当前 Chat 协议图片输入中明确不支持。
- LLMClient 独占 Adapter/Transport，ProviderConfig 及默认选项按值保存，无动态 setter。本次完整 RequestOptions 替换默认选项，不做逐字段合并。可并发 chat，请求及结果都是调用局部值。
- LLMClientFactory 只创建 OpenAIChatCompletions 协议对应的 OpenAIChatCompatibleAdapter；其他内置协议返回 UnsupportedProtocol。失败保持已有 output Client 不变。直接注入不依赖 Factory 或 ProtocolType 枚举选择；注入者负责选择正确的协议实现。
- FakeTransport 正式实现 ITransport，支持响应队列、多次响应、故障及部分数据预设、请求/超时记录；队列和记录由内部互斥锁保护。
- LocalHttpServer 仅为测试设施，使用随机环回端口。未引入外部 Provider、密钥或运行时服务器。

## 编码契约

API 根路径追加 /chat/completions，不补 /v1，保留网关前缀。带 query、fragment 或 user information 的根地址明确报配置错误，避免歧义。apiKey 为空时不生成默认认证。自定义认证覆盖默认认证，Header 名称不区分大小写。

Protected Headers 为 Content-Type、Accept、Host、Content-Length、Transfer-Encoding；大小写重复的自定义 Header 报错，不随机挑选。Header 格式错误发送前失败。

reserved fields 包含 model、messages、temperature、top_p、max_tokens、max_completion_tokens、tools、tool_choice、stream、stream_options、n、functions、function_call、parallel_tool_calls。无论对应字段是否设置，extraParameters 出现这些名称均拒绝。

maxOutputTokens 当前映射为 max_completion_tokens。各兼容服务对该字段的支持仍需人工验收；不通过 URL/模型名称选择另一字段，也不自动重发修改后的请求。

成功 ToolResult 直接编码 data 的 JSON 文本；失败使用 success/errorCode/errorMessage JSON fallback，callId 在 tool_call_id 协议字段中关联。

只有一个响应候选合法；缺失、零候选、多候选均报错。原生调用 ID 非空、响应内唯一，函数参数必须是完整 JSON 对象字符串。异常响应保留已解析的文本、完整调用、Usage、FinishReason 和原始诊断，completionState 为 Incomplete；调用方不得执行失败响应中的工具。

Length 是正常模型结束原因：chat 返回 true、CompletionState::Complete，但 MessageStatus::Incomplete。输入 Incomplete 历史消息明确拒绝，不自动裁剪、修复或静默删内容。

reasoning_content 属于兼容服务扩展响应字段，可读入 ReasoningContent；当前不支持将 ReasoningContent 回传为历史，请求时明确 UnsupportedFeature。字段结构无跨字段的原始时间顺序时，响应按 Reasoning → Text → ToolCalls 标准化，各字段内部顺序保持。

依据：[OpenAI 官方 Chat Completions API](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)。兼容服务的额外字段仍需对应人工验收。

## 关键接口

```cpp
bool ITransport::send(
    const TransportRequest&, const RequestOptions&,
    TransportResponse&, SdkError&);

bool IProtocolAdapter::encodeChatRequest(
    const ProviderConfig&, const ChatRequest&,
    TransportRequest&, SdkError&) const;

bool IProtocolAdapter::decodeChatResponse(
    const TransportResponse&, ChatResponse&, SdkError&) const;

bool LLMClient::chat(
    const ChatRequest&, ChatResponse&, SdkError&) const;

bool LLMClient::chat(
    const ChatRequest&, ChatResponse&, SdkError&,
    const RequestOptions&) const;

bool LLMClientFactory::create(
    const ProviderConfig&, std::unique_ptr<LLMClient>&,
    SdkError&, const RequestOptions& defaults = {});
```

## 阶段边界

stream=true 明确返回 UnsupportedFeature。本阶段无 StreamEvent、RequestDecoder、StreamSession 或 SSE 解析；阶段 3 再实现。

RetryPolicy 已存在且保留默认值，但阶段 2 尚不执行自动重试，只进行一次请求。Retry-After 已规范化保存为 error.retryAfterMs，重试等待和分类执行留到阶段 7。不能将配置存在误解为功能已经实现。

无 Agent、Registry、Executor、Approval 执行、MCP、Provider Builtin Tool 或 Context Management。测试的多轮工具消息由测试人工提供结果，不代表 Agent 已实现。

## 文件清单

新增公共头文件：

- LibAiCore/include/AiLib/network/TransportRequest.h
- LibAiCore/include/AiLib/network/TransportResponse.h
- LibAiCore/include/AiLib/network/ITransport.h
- LibAiCore/include/AiLib/network/QtHttpTransport.h
- LibAiCore/include/AiLib/protocol/IProtocolAdapter.h
- LibAiCore/include/AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h
- LibAiCore/include/AiLib/client/LLMClient.h
- LibAiCore/include/AiLib/client/LLMClientFactory.h

新增实现：

- LibAiCore/src/network/QtHttpTransport.cpp
- LibAiCore/src/protocol/openai/OpenAIChatCompatibleAdapter.cpp
- LibAiCore/src/client/LLMClient.cpp
- LibAiCore/src/client/LLMClientFactory.cpp

新增测试：

- tests/support/FakeTransport.h
- tests/support/LocalHttpServer.h
- tests/unit/tst_Chat.cpp
- tests/transport/tst_QtHttpTransport.cpp

修改：LibAiCore/CMakeLists.txt、tests/CMakeLists.txt、README.md。新增本文；阶段 1 验收记录保留为历史记录。

## 验证结果

验证环境：macOS、Qt 6.11.1、C++17，使用 Command Line Tools 工具链。

- SDK、测试程序和 28 个公共头文件的独立编译检查全部通过。
- CTest 六组测试全部通过；Qt Test 合计 74 个测试项通过（不计各组初始化和清理），无失败或跳过。
- FakeTransport 覆盖普通 Chat、参数冲突、协议错误、工具消息编解码、模型输出截断和同一 Client 并发调用。
- 本地 HTTP 服务覆盖真实 Qt 网络栈的状态码、拆包、超时、取消、断连数据保留及并发请求。
- SDK 安装后，独立消费程序通过已安装的公共头文件和动态库完成 Factory 创建及自定义 Transport 的普通 Chat 调用。
- 安装规则只收集公共头文件，排除 `.DS_Store` 等非 SDK 文件。

当前环境没有 Qt 5.15；Qt 5.15、最低 Qt 6.2 及 Windows/Linux 尚未实际编译验收。真实 Provider 调用也尚未进行，自动测试均离线完成。Streaming、自动重试与 Agent 运行时继续按后续阶段实现。
