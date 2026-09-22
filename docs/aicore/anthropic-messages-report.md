# Anthropic Messages 协议接入

## 实现范围

新增 `AnthropicMessagesAdapter`，实现现有 `IProtocolAdapter` 的普通请求编码及普通响应解码。Factory 根据已有 `ProtocolType::AnthropicMessages` 创建它；Client、Transport 与 canonical model 无需增加协议专属字段。

- 根地址追加 `/v1/messages`，保留网关路径前缀。Kimi 使用 `https://api.kimi.com/coding/`；Anthropic 使用 `https://api.anthropic.com`。
- 默认 `x-api-key` 认证允许 customHeaders 忽略大小写后覆盖。保护 Content-Type、Accept、anthropic-version 以及 Host/Content-Length/Transfer-Encoding；重复名称、非法名称和值均报错。
- 固定 `anthropic-version: 2023-06-01`，非流式 `stream:false`。model、messages、system、max_tokens、temperature、top_p、tools、tool_choice、stream 以及多候选绕过字段不能出现在 extraParameters。
- maxOutputTokens 映射为必需的 max_tokens，未指定时使用 1024。temperature 合法范围 0–1，topP 为 0–1，Token 上限必须为正数。
- 前置 System 文本进入顶层 system；中途 System 消息无法保持原位置，明确报不支持，不静默移动。
- Text/Image/ToolCall/ToolResult 按块顺序编码；相邻同角色消息合并内容。Tool 角色转换为 user 的 tool_result，使一批工具结果能够一起回传。
- 工具定义使用 input_schema；Auto/None/Required/Specific 对应 auto/none/any/tool。调用使用 tool_use，参数为 JSON 对象；ToolResult 使用 tool_use_id 关联及原生 is_error。成功业务值直接编码，失败同时携带 success/errorCode/errorMessage。
- 图片支持 HTTP(S) URL、Bytes 和 LocalFile；二进制来源编码为 Base64，支持 JPEG/PNG/GIF/WebP，检查 MIME 与实际字节探测结果一致。本地读取错误明确返回；不实现远程文件引用或像素级图像校验。
- 响应 content 按原生顺序生成 TextContent、ReasoningContent、ToolCallContent。原生调用 ID 必须非空且当前响应内唯一，不修补。错误时保留此前有效内容及用量，整体仍为 Incomplete。
- end_turn/stop_sequence、max_tokens、tool_use、refusal 分别映射 Stop、Length、ToolCalls、ContentFilter；其他非空原因映射 Other。Length 是成功响应，消息标记 Incomplete。缺失 stop_reason 是响应错误。
- input_tokens/output_tokens/total_tokens 仅保存服务端报告的字段，未知保持 optional 空，不推导总量。缓存细分用量尚未建模。
- HTTP 和原生错误映射 SdkError，保留状态、原始诊断、Provider 错误类型及 Retry-After。取消和网络超时继续由现有 Client/Transport 处理。

## 明确未实现

Streaming、Agent 调度、ToolExecutor、自动重试仍按原阶段计划推进。Builtin Tool、Audio/Video/File 输入和未建模的响应块明确返回 UnsupportedFeature。

推理文本可以接收并显示；canonical ReasoningContent 尚未保存 Anthropic 签名，不能保证原样重放。因此请求包含 ReasoningContent 时明确报不支持，不静默删除、不伪造签名。本次 Kimi 手动工具测试通过 extraParameters 显式设置 thinking.type=disabled，未绕过上述校验。

## 文件

- `LibAiCore/include/AiLib/protocol/anthropic/AnthropicMessagesAdapter.h`
- `LibAiCore/src/protocol/anthropic/AnthropicMessagesAdapter.cpp`
- `tests/unit/tst_AnthropicMessages.cpp`
- `tests/manual/kimi_messages.cpp`

修改 Factory、库及测试 CMake、README 与架构说明；既有 Factory 不支持协议测试改用仍未实现的 OpenAIResponses。

## 验证

验证环境为 macOS、Qt 6.11.1、C++17；Qt 5.15 与其他平台未实际编译。

自动测试包括普通 Chat、Factory、保留字段、认证覆盖及保护头、工具块顺序与失败语义、图片编码、重复/空 ID、错误参数、用量、Length、原始 Provider 错误与 Retry-After。完整构建通过，29 个公共头文件独立编译通过；CTest 7/7 组通过，无失败或跳过。其中 Anthropic 新增 14 个测试项，全项目合计 88 个测试项（均不计初始化和清理）。git diff --check 通过，项目源码与文档扫描未发现 Kimi Key。

使用调用方提供的 Key，SDK 经真实 QtHttpTransport 完成三次 Kimi Code 请求：

| 请求 | 结果 | 输入 / 输出 Token |
| --- | --- | --- |
| C++17 unique_ptr 普通文本问题 | 成功，有效文字 73 字符 | 35 / 49 |
| 指定 add(a=19,b=23) | 成功，生成 1 个完整 ToolCall | 133 / 68 |
| 人工执行加法并回传 sum=42 | 成功，最终文字包含 42，无后续调用 | 232 / 88 |

工具执行由手动测试程序完成，这不表示 Agent 或 ToolExecutor 已实现。手动测试使用真实 User-Agent `AiLib/0.1.0 (SDK integration test)`，没有冒用其他客户端身份。Key 只在测试进程运行环境中传递，不写入项目文件，日志不输出凭据或原始 HTTP 请求。手动测试不注册到 CTest，不要求 CI 提供外网或 Key。

## 协议依据

请求和原生工具块映射依据 [Anthropic Messages 官方接口](https://platform.claude.com/docs/en/api/messages/create)。Kimi 根地址及模型标识依据 [Kimi Code 官方接入文档](https://www.kimi.com/code/docs/)，关闭思考的配置依据 [Kimi 模型配置](https://www.kimi.com/code/docs/kimi-code/models.html)。

## 后续凭据保存调整

后续用户明确要求把 Kimi Code 和 DeepSeek 两个临时 Key 保存到项目规则中，因此它们现在位于 `AGENTS.md` 顶部的临时测试区，带有删除提醒和起止标记。上面的“不写入项目文件”及扫描结论描述的是首次接入测试时的状态；当前只有该规则区保存明文凭据，源码、手动测试和报告继续不保存 Key。

## 阶段 3 更新

Streaming 已实现；上面的非流式范围及未实现说明保留为首次接入的历史记录。当前流式请求通过独立 AnthropicStreamDecoder 输出统一事件，由 StreamSession 聚合。推理签名回传限制继续保留。新增 `--stream` 的真实测试结果见 [阶段 3 验收记录](phase3-report.md)。
