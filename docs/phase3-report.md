# 阶段 3：Streaming 闭环验收

## 实际范围与职责

两种现有协议均支持同步 Streaming，核心流程为：

`Transport 原始片段 → 请求级 Decoder → StreamEvent → 请求级 StreamSession → ChatResponse`

Session 聚合事件后再实时通知 Callback。客户端不解析具体厂商字段，不创建线程；同一个 Client 的并发请求拥有独立 Decoder、SSE 缓冲和 Session。工具执行、Approval、Agent Loop 仍未实现，本阶段不会产生工具副作用。

## 关键接口

- `ITransport::sendStream(request, options, response, onData, error)`：同步增量交付原始字节和 HTTP 元数据。普通 `send` 与流式复用真实 Qt 网络实现的超时、取消和错误逻辑。成功流不重复缓存全部原始 SSE；HTTP 故障保留正文用于 Adapter 错误解码。
- `IProtocolAdapter::createStreamDecoder(error)`：按请求创建解析器。
- `IStreamDecoder::feed(bytes, sink, error)` / `finish(sink, error)`：解析协议并在连接结束时确认协议完整性。Decoder 只保存协议顺序、映射和生命周期，不聚合最终工具参数。
- `StreamSession::apply(event, error)` / `response()`：统一聚合、契约验证与值快照；`fail(error)` 保存已有数据并标记 Incomplete。
- `RequestOptions::streamCallback`：标准化通知，调用线程执行，不承担最终响应聚合。外部异常转换 StreamCallbackFailed，不丢弃已经聚合的内容。
- `LLMClient::chat(request, response, error[, options])`：签名不变，模式由 request.stream 决定。不设置 callback 也能成功完成流式请求。

ITransport 和 Adapter 的新增方法带明确的不支持默认实现，原有自定义普通实现可以继续使用。公共接口新增虚方法及 RequestOptions 成员，使用旧动态库的程序应重新编译，不承诺此开发阶段的二进制兼容。

## 模型与事件

StreamEventType：ResponseMetadataUpdated、PartStarted、TextDelta、ReasoningDelta、ToolCallDelta、PartCompleted、UsageUpdated、FinishReasonReceived、ResponseCompleted、Error。

- `partIndex` 是请求内稳定 SDK 逻辑标识，响应级事件为 -1；由 Decoder 分配或映射，Session 不理解原生 index。
- `partType` 在 PartStarted 中声明 Text/Reasoning/ToolCall。
- `toolCallId` 是可后到的完整属性，与 partIndex 独立；toolNameDelta、delta 分别追加名称与参数。工具 ID 不允许中途改变，完整调用须非空且响应内唯一。
- PartCompleted 表示该逻辑块的生成阶段已结束，不表示工具执行。正式 ToolCall 只包含已关闭且 JSON 参数合法的对象。若参数截断且最终原因是 Length，则不放入 Message；正常完成却参数非法则失败。
- UsageUpdated 使用累计字段的最新快照，不按通知次数累加，缺失字段保持未知。Message.contents 按逻辑块首次出现顺序保存，不按内容类别拆散。
- FinishReasonReceived 后继续读取允许的 Usage 等事件；ResponseCompleted 才表示 Decoder 确认协议完成。后续网络异常仍会让最终结果失败并标记 Incomplete。

OpenAI 的 text/reasoning 字段分别对应持续聚合的逻辑块，工具原生索引分别映射为独立块。最终顺序描述协议逻辑块，不把同一字段的每个 token 再拆成一个 MessageContent。Anthropic 的 content block 逐块映射，保留块顺序。

## 协议边界

- SSE 支持 LF、CRLF、CR、UTF-8 跨包、BOM、注释及多行 data；仅交付完整事件，不把 EOF 处未闭合 data 当作完整事件。单行或单事件限制 8 MiB，防止异常帧无限增长。
- OpenAI Compatible：stream=true、include_usage=true、n=1；支持 content、reasoning_content、tool_calls。唯一候选完成后读取 Usage-only 分片，最终必须收到 `[DONE]`，没有结束标记不能成功。
- Anthropic Messages：支持 message_start、文本/推理/tool_use 块及对应增量、块结束、累计 Usage、message_delta、message_stop；允许 ping 和未来未知非内容通知。没有合法 message_stop 不能成功；已知块出现未知内容类型明确不支持。
- Native ToolCall ID、参数对象、候选数量及块生命周期严格校验，不自动修复。工具名称和参数由 Session 累积，Decoder 不输出提前执行指令。
- Anthropic reasoning signature 可解析但没有 canonical 存储字段；仅提供文本展示，包含 ReasoningContent 的输入继续明确不支持回传。Builtin Tool、流式图片/音视频及其他未建模块未实现。
- HTTP/网络故障、请求超时、协作取消、协议/JSON 错误均为 false + SdkError；Length 为 true + Complete 的协议结果，消息标记 Incomplete。有效文本、合法已完成工具、用量和 FinishReason 保留。
- 自动重试仍留到阶段 7，当前只进行一次请求。

## 文件清单

新增公共类型：

- LibAiCore/include/AiLib/stream/StreamEvent.h
- LibAiCore/include/AiLib/stream/IStreamDecoder.h
- LibAiCore/include/AiLib/stream/StreamSession.h

新增实现及内部辅助：

- LibAiCore/src/stream/StreamSession.cpp
- LibAiCore/src/stream/SseDecoder.h
- LibAiCore/src/stream/DecoderHelpers.h
- LibAiCore/src/protocol/openai/OpenAIStreamDecoder.cpp
- LibAiCore/src/protocol/anthropic/AnthropicStreamDecoder.cpp

新增 tests/unit/tst_Streaming.cpp。扩展 Client、Adapter、Transport 接口及实现，FakeTransport 支持预设分片和结束故障；更新 CMake、既有协议测试、两个手动测试、README 和架构记录。

类和结构体使用上方中文注释；接口、参数及变量使用中文行尾注释并按局部分组对齐。格式整理使用标准 formatter，检查非注释词法内容不变；不重排未涉及的历史文件。

## 自动验证

验证环境为 macOS、Qt 6.11.1、C++17，使用 Command Line Tools 工具链。Qt 5.15、最低 Qt 6.2 及其他平台未实际编译验收。

自动测试覆盖逐字节 SSE/UTF-8 拆包、BOM/多行 data/CRLF/CR、FinishReason 后 Usage、推理、晚到调用 ID、多个调用交错、截断参数、Length、取消、Callback 异常、多个/零候选、非法 JSON/ID/参数、原生乱序、网络与 Provider 错误、同 Client 并发请求。真实 Qt 网络测试验证在连接关闭前的回调、分次交付、协作取消、超时和异常断连时数据保留。

- SDK、现有应用及测试程序全部编译通过；32 个公共头文件独立编译检查通过。
- CTest 8/8 组通过，无失败或跳过；全项目 112 个测试项通过，其中 Streaming 新增 24 个测试项（均不计初始化和清理）。
- SDK 安装后，独立消费程序仅通过已安装头文件和动态库，注入离线 Transport，成功完成 SSE 拆包、实时事件和最终 ChatResponse 聚合。
- git diff --check 通过；源码及文档凭据扫描只在用户指定的 AGENTS.md 临时区发现 Key，没有新增复制位置。

## 真实 Provider 流式验证

使用规则中已授权的临时 Key；测试摘要不输出凭据、原始请求或原始响应正文。通过现有手动程序的 `--stream` 模式，分别执行普通问题、指定 add(19,23)、回传 sum=42 后的最终响应。工具由测试程序人工执行，不依赖尚未实现的 Agent。

| 服务 / 协议 | 三次请求 | 输入 / 输出 Token | 标准化事件总数 |
| --- | --- | --- | --- |
| Kimi Code / Anthropic Messages | 全部成功，最终包含 42 | 35/41、133/60、224/67 | 117 |
| DeepSeek / OpenAI Compatible | 全部成功，最终包含 42 | 21/42、311/47、374/101 | 335 |

两边工具阶段均生成且仅生成一个完整 ToolCall，最终阶段无后续工具调用。为了验证当前可回传的工具历史，手动测试显式关闭 thinking；推理分片另由离线测试覆盖。真实测试结果不作为 CI 必要条件。

## 协议依据

参考 [OpenAI Chat Completions 官方接口](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create) 和 [Anthropic Streaming 官方文档](https://platform.claude.com/docs/en/build-with-claude/streaming)。Session 和 Client 不依赖文档中任何具体结束标记。
