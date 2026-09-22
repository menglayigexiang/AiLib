# 阶段 6：Agent 完整化

补齐实际工具调用次数上限、精确的调用链截止时间和客户端工具执行事件。沿用同步 API、唯一取消令牌、应用拥有线程和 Registry 的既定职责；没有增加 runId、approvalId、RunContext 或独立 Run 状态对象。

## 公共接口调整

新增 `agent/AgentEvent.h`：

- `AgentEventType`：ToolExecutionStarted / ToolExecutionFinished。
- `AgentEvent`：type、agentId、ToolCall、可选 ToolResult、handlerExecuted。
- `AgentCallback`：同步 `std::function<void(const AgentEvent&)>`。

`AgentRequest.callback` 在 Agent 运行线程接收上述事件。它和 `RequestOptions.streamCallback` 相互独立：前者报告完整模型输出后的客户端工具流程，后者报告模型生成过程。

`RequestOptions.deadline` 是可选的 `std::chrono::steady_clock::time_point`，传递精确的调用链截止时间，不是第二个取消来源。普通 Chat 也可以显式设置；空值不施加额外上限。新增 `isDeadlineExpired()` 便利查询。字段追加在已有字段后，现有较短聚合初始化保持原来的字段顺序。

其他实现调整：Agent.cpp、LLMClient.cpp、QtHttpTransport.cpp。新增 `tests/unit/tst_AgentControl.cpp`，更新阶段 5 配置用例和测试 CMake。

## 实际工具调用额度

maxToolCalls：-1 不限，0 不允许执行工具，正数是单次 Run 内所有工具实际进入 Handler 的次数总上限，小于 -1 在启动前失败。

拿到完整模型响应后，先检查当前整批 ToolCall 数量是否超过剩余额度；超过则整个批次不执行、不请求 Approval、不发送工具执行事件，正常结束为 MaxToolCalls。由于确认和业务执行尚未发生，批量预检使用潜在调用数，不提前猜测其中哪些调用可能被拒绝。

实际额度仅在 Executor 报告 `handlerExecuted=true` 后累计。不同工具和不同模型轮次共享总额度；Handler false 和异常也计数。参数错误、工具未找到/未启用、确认拒绝或不可用不计数。每个 Run 的计数独立，不存放在 FunctionToolDefinition 或 Registry 中。

maxTurns 仍使用既定字段，默认 20、最小 1；没有另建 maxLlmCalls 计数配置。最后一轮仍产生工具调用时，整批不执行并返回 MaxTurns。

## 精确截止时间和停止原因

Agent 在进入有效 Run 时记录单调时钟起点，总预算包含配置校验、模型请求、工具执行、确认等待以及事件回调处理。

每轮局部 RequestOptions 继续覆盖 llmTimeoutSeconds / llmRetryPolicy，并附加 Agent 总截止时间；若调用方已提供更早的单次请求 deadline，保留较早者。不会修改原 RequestOptions 或 Client 默认值。

timeoutSeconds 仍是用户配置的秒数；Qt Transport 实际中止时间使用单次请求截止时间与传入 deadline 的较早者。普通/流式请求共享实现：发送前重新检查，使用精确毫秒定时通知并以单调时钟复核到期，取消检查仍沿用原 Token。阶段 5 的“剩余亚秒向上取整后多等不足一秒”问题已消除。

Client 在请求开始、编码后、网络返回、流事件和数据处理边界检查取消/截止时间。Agent 到达自己的总截止时间正常返回 true + Timeout，清空 SdkError；底层单次请求先超时仍是 false + Failed + SdkError。主动取消或 Approval Cancel 正常返回 true + Cancelled。

精确预算不等于硬实时线程终止。操作系统和 Qt 事件调度可能有小量延迟；不可中途取消的 Handler、应用确认策略或回调仍需等待返回。SDK 不强杀线程，也不创建线程。已产生的真实工具结果和有效部分文本仍保留，部分助手消息标记 Incomplete。

## 事件语义

Started 在完整模型响应通过批量边界检查后、进入该次工具流程前通知；它描述包含验证/确认/锁等待的客户端工具流程开始，不承诺 Handler 已执行。result 为空、handlerExecuted=false。

Finished 在 Executor 返回并先保存实际增量消息后通知，携带流程结果及实际 Handler 标记。Deny/参数错误等也会有 Finished；前置取消或等待停止的结果可供 UI 展示，但不被添加为虚假的 Tool 消息反馈给模型。没有开始事件的超额/最后轮批次，不制造结束事件。

两种事件都携带 agentId 和原 ToolCall.id，不新增全局调用 ID。多个 Agent 可以共享应用回调，应用负责其跨线程安全。回调只通知，不拥有调度权；取消仍通过统一 CancellationToken 传播。

开始回调异常返回 AgentCallbackFailed，当前 Handler 不执行。结束回调异常同样返回 false + Failed，已经产生的 Assistant/Tool 消息保留。异常不逃出正常回调边界。

## 验证结果

- macOS、Qt 6.11.1、C++17 编译通过。
- CTest 11/11 组通过。
- 新增 AgentControl 实际用例 19 个，Qt Test 显示 21（包含 init/cleanup）；项目累计 179 个实际用例。
- 覆盖零额度、整批超额、恰好用满、跨轮/跨工具共享额度、参数失败和拒绝不计数、Handler false/异常计数、事件身份及顺序、开始/结束回调异常保留结果、确认等待取消/总超时、同实例并发拒绝、不同实例共享 Registry 并行运行、流文本后取消/超时及部分消息保留、过期 deadline 不发送请求。
- 本地 HTTP + 真实 Qt 网络验证：一秒总预算中，第一轮完成工具调用、工具消耗约 450ms，第二轮连接保持不返回；按剩余亚秒预算中止，总运行约一秒，而不是另等一整秒。此前 Assistant 和 Tool 消息保留，返回 true + Timeout。
- 不可中断同步工具在一秒总预算后返回时，真实业务结果仍保留，停止后续请求。
- 39 个公共头文件分别独立编译通过。
- 安装至 `install/phase6`；独立程序仅使用安装后的 SDK，设置 maxToolCalls=1，接收两个工具事件，完成工具结果 42 和最终回答 42 的闭环，退出码 0。
- Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际验证。
- 全部验证离线或本地 HTTP，不使用真实 API Key。

## 留到下一阶段

LLMClient 当前仍不执行自动重试。阶段 7 将实现既定固定间隔重试、临时错误分类、Retry-After、有效流内容后禁重试，以及重试等待/每次尝试遵守本阶段统一 deadline。不会借阶段划分新增指数退避、抖动或其他未确定策略。

阶段 6 完成后停在验收点；CLI 与 Qt Widgets Demo 留到阶段 8。
