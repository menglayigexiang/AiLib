# 阶段 7：重试与网络可靠性

LLMClient 已实现可配置同步自动重试。普通 Chat 使用 RequestOptions.retryPolicy，Agent 每轮使用自己的 llmRetryPolicy 覆盖。Transport 保持单次网络尝试，不决定 LLM 重试，不执行工具，也不创建工作线程。

## 代码与接口

运行时实现集中在 `src/client/LLMClient.cpp`：临时错误分类、固定或服务端建议等待、单次尝试循环、流式重试边界和最终错误通知。QtHttpTransport 补充网络错误是否具有临时性的诊断信息。

新增私有 `src/protocol/RetryAfter.h`，统一两种 Adapter 的 Header 解析。没有新增公共重试 DTO、回调框架或配置字段，公共配置仍然是既定 RetryPolicy 与 RequestOptions.deadline。LLMClient.h 更新接口说明。

新增 `tests/unit/tst_Retry.cpp` 和测试专用 `tests/support/TestOptions.h`。既有单次错误用例显式设置 maxRetries=0，使它们继续验证单次故障而不是默认等待；公共默认值保持不变。

## 策略和错误分类

- maxRetries 不含首次请求，0 表示最多一次请求，2 表示最多三次，默认 5 表示最多六次。
- 默认固定间隔 30000ms，无指数退避、随机抖动或自定义 Retry Callback。
- 网络连接类错误由 retryOnNetworkError 控制；HTTP 429 由 retryOnRateLimit 控制；HTTP 500–599 由 retryOnServerError 控制。
- HTTP 400/401/403/404/408/422 等请求错误、配置/协议/解析错误、主动取消和单次请求预算 Timeout 不重试。
- Qt 网络细分中，连接拒绝、远端关闭、DNS 查找失败、连接层 Timeout、临时网络故障、会话失败、未知网络错误以及临时代理连接类故障可重试；TLS 握手、主动 abort 等不重试。Transport 将 `SdkError.details.retryable` 作为诊断提示，不改变公共错误分类。
- 自定义 Transport 只报告 Network、未提供细分提示时，按临时连接错误处理；可以显式设置 details.retryable=false 排除永久故障。HTTP 状态已提供时优先按状态和对应开关判断。
- 所有重试耗尽后保留最后一次 SdkError，包括 HTTP 状态、原始 Provider 错误及 Retry-After 建议。不自动重试 Handler，也不重复已经执行的工具。

## 等待、超时和取消

同步等待使用单调时钟计算实际累计等待，分段最多 20ms 检查唯一 CancellationToken 和精确 deadline；不新建线程，不处理 GUI，也不使用异步 Future。

Agent 总截止时间优先。服务端等待建议再长也受剩余总预算限制。每次新尝试开始前检查截止时间，Qt Transport 在该尝试中重新计算自己的单次请求截止时间，并取与总 deadline 较早者。因此重试等待和每次网络尝试均受同一总预算约束。

timeoutSeconds 是每次尝试的独立上限，不把多次尝试错误地合并成一次请求超时。普通 Chat 预算耗尽返回 false + Timeout 类 SdkError；Agent 自己的总预算耗尽正常返回 true + AgentFinishReason::Timeout。调用方主动取消仍为 Cancelled。

重试次数是当前逻辑 LLM 调用的内部尝试次数，不增加 Agent maxTurns，不增加 turnUsages 条目，不消耗工具调用额度，不修改 Client 或调用方原配置。

## Streaming 边界

每次尝试重新创建独立 RequestDecoder 和 StreamSession。尚无有效内容时，临时连接/429/5xx 可按策略重试。

非空 TextDelta、ReasoningDelta、ToolCallDelta，以及工具 Part 开始、已报告 Usage、FinishReason 或协议完成状态，均阻止后续自动重试。仅响应元数据、空文本或空文本 Part 开始不视为有效输出。对完成信息采用保守边界，避免已经得到有效模型状态后重复请求。

即使调用方没有设置 streamCallback，SDK 也跟踪上述边界。有效内容之后发生断线，返回失败并保留聚合的部分响应，绝不重放 token 或工具参数。

可重试失败不发送终止 Error 回调，最终失败才统一通知一次 Error。其他事件仍实时通知，因此无有效内容的失败尝试可能已经通知 Metadata 或空 PartStarted；最终响应只来自最后一次独立 Session，调用方不得借 callback 承担跨尝试的最终聚合职责。回调异常属于 Internal 故障，不自动重试。

## 修复的协议兼容性问题

测试发现先前使用 Qt RFC2822Date 解析不能识别标准 HTTP GMT 日期。已统一支持：非负整数字符串秒数、IMF-fixdate、RFC850 和 asctime 日期。RFC850 两位年份按当前世纪及五十年规则展开后解析，GMT 按 UTC 解释；过期日期保留为 0，非法或溢出建议保持未知并使用固定间隔。

Header 名称不区分大小写，正确建议保留在 SdkError.retryAfterMs，优先于 retryIntervalMs。解析依据 [RFC 9110 Retry-After](https://www.rfc-editor.org/rfc/rfc9110.html#name-retry-after) 与其 HTTP-date 规则；Qt 网络错误含义核对 [QNetworkReply 文档](https://doc.qt.io/qt-6/qnetworkreply.html#NetworkError-enum)。

## 验证结果

- macOS、Qt 6.11.1、C++17 编译通过。
- CTest 12/12 组通过。
- 新增 Retry 实际用例 45 个，Qt Test 显示 47（含 init/cleanup）；全项目累计 224 个实际用例。
- 覆盖永久/临时 HTTP 状态及开关、0/1/2/默认 5 次重试耗尽、连接细分、关闭网络重试、超时/协议错误不重试、普通和无回调 Streaming 的有效内容边界、Session 隔离、最终 Error 通知次数、Retry-After 秒数和三种日期、等待期间取消、截止时间覆盖长建议等待。
- 本地 HTTP + 真实 Qt 网络：普通及 Streaming 先两次 503 后成功；503 后挂起的新尝试按总 deadline 中止；两次各约 600ms 的请求在单次 timeout=1 秒时分别成功，不误用累计一秒上限。
- Anthropic Messages 普通及 Streaming 的 429 重试后成功，使用真实 Adapter + FakeTransport。
- Agent 每轮重试不增加逻辑轮次，业务工具只执行一次，成功/耗尽均保留正确增量；总预算覆盖重试等待，正常结束 Timeout。
- 39 个公共头文件分别独立编译通过。
- 安装至 `install/phase7`，独立程序仅使用安装 SDK，在一次 503 重试后完成工具结果 42、最终回答 42，工具流程事件仍只有开始/结束两个，退出码 0。
- Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际验证。
- 本阶段全部离线或本地 HTTP 测试，不调用真实 API Key。

按约定停在阶段 7 验收点。下一阶段为 CLI、最小 Qt Widgets Demo、真实 Provider 可选手动兼容性检查及整体验收。
