# 阶段 5：最小 Agent Loop

已打通同步主链：应用输入历史 → LLMClient → 完整 Assistant ToolCall → ToolExecutor → ToolResult → 再次请求模型 → 最终回答。普通和 Streaming 都使用同一个 Agent Loop。Agent 不创建线程、不管理 GUI、不维护长期会话，也不理解厂商协议字段。

## 文件和接口

- 新增 `include/AiLib/agent/Agent.h`、`src/agent/Agent.cpp`。
- 新增 `tests/unit/tst_Agent.cpp`，加入 CMake 和公共头文件独立编译检查。
- 补充已有 AgentRequest / AgentResult / AgentLimits 的类型说明。
- ToolExecutor 保留已有四参数 `execute()`，增加带 `bool& handlerExecuted` 输出的重载。它区分确认前取消与 Handler 实际执行后取消，使 Agent 能保留已产生的实际业务结果；不是新结果 DTO 或 Run 状态机制。

Agent 构造接收 `unique_ptr<LLMClient>`、`ToolRegistry&`、可空非拥有的 `IToolApprovalProvider*` 和可选 ID。Client 由 Agent 独占，Executor 是值成员，Registry 和确认策略由应用拥有。ID 未指定时自动生成，指定时由应用保证实例身份唯一。运行接口：

```cpp
bool run(
    const AgentRequest& request,  // 本轮完整输入和配置
    AgentResult& result,          // 本轮增量和停止原因
    SdkError& error);             // 正常停止 true，异常故障 false
```

## 已实现行为

1. 开始前检查 Client、轮数、时间配置和整个 enabledTools 名单；不存在的工具名称直接失败，不发送请求。
2. 清空本次 ChatRequest 副本中的外部 tools，仅从 Registry 获取白名单定义。空白名单表示零工具；重复白名单名称只发送一份定义。
3. 私有 ChatRequest 副本保存本次循环上下文，原始历史保持顺序和内容，不裁剪、不总结、不修改调用方输入。
4. 按顺序返回本次新产生的 Assistant、Tool 和最终 Assistant 消息。不同轮次可以复用相同 ToolCall ID；只检查当前响应内 ID 唯一。
5. 只有完整成功的 chat 才进入工具执行阶段，Streaming Callback 不执行工具。单轮多个工具依次执行并反馈给下一轮模型。
6. 工具参数失败、Handler false/异常、ApprovalDenied、ApprovalUnavailable 均产生失败 ToolResult，模型可以继续决策。不在本次白名单中的模型调用返回 ToolNotEnabled，不执行其 Handler。
7. 主动取消和 Approval Cancel 正常结束为 Cancelled；Handler 已执行并产生结果后才停止，保留实际 ToolResult。不制造前置取消或等待确认取消的虚假业务结果。
8. 网络/Provider/底层请求超时等返回 false + Failed + SdkError，并保留此前增量。无有效内容不创建空的部分助手消息；流失败保留有效部分内容并标记 Incomplete。
9. 完整响应 Length 正常结束，不执行该响应的任何工具，不自动续写。最后一轮仍请求工具，整批不执行，返回 MaxTurns。
10. 每轮使用局部 RequestOptions，以 Agent 的 llmTimeoutSeconds 和 llmRetryPolicy 覆盖。传递同一个 CancellationToken，总截止时间继续传给 Tool/Approval。
11. 同实例并发/重入返回 AgentBusy；运行标记用作用域守卫复位。不建立 runId、approvalId 或 Registry 冻结机制。
12. 返回每轮 Usage 并按字段累计；任一参与轮次字段未知则完整累计未知，不推算 totalTokens。未调用模型的总量为零；无法安全累计的溢出值保持未知。

## 当前阶段边界

maxToolCalls 的实际 Handler 次数统计留到阶段 6。当前只能使用默认 -1；显式 0 或正数在启动时返回 UnsupportedAgentLimit，不会静默忽略限制。小于 -1 仍然是配置错误。

阶段 5 已接入协作式总截止时间，但 RequestOptions/Transport 的请求超时仍为整秒。当前将剩余时间向上取整传入请求，返回后再次检查总截止时间，单次网络请求的总预算最多有不足一秒的取整误差；不可中断的 Tool 或应用确认操作仍需等返回后停止。阶段 6/7 需要进一步落实毫秒精度的网络截止时间和重试等待预算，不将现状视为已经完成全部 Agent 超时契约。

工具执行状态事件、实际调用次数限制和更完整的取消/超时组合验证留到阶段 6。RetryPolicy 已按覆盖关系传递，但 LLMClient 尚不执行重试，阶段 7 完成；不引入 backoff、自定义重试回调或其他未确定机制。CLI/GUI Demo 留到阶段 8。

## 验证

- macOS、Qt 6.11.1、C++17 编译通过。
- CTest 10/10 组通过。
- 新增 Agent 实际用例 26 个；Qt Test 显示 28，包含 init/cleanup。全项目实际用例累计 160 个。
- 覆盖普通回答、单/多工具、多轮工具、原始历史不变、白名单来源、重复名称、工具业务失败/异常/参数错误、越白名单调用、拒绝/取消/策略缺失、启动前配置错误、Length/MaxTurns、网络和底层超时、执行前取消、执行后取消保留结果、重入和重复运行、流式执行时机、部分流保留、Usage 未知传播、局部 timeout 覆盖。
- OpenAI Chat Compatible 和 Anthropic Messages 都使用真实 Adapter + FakeTransport 验证完整工具闭环；没有依赖 API Key 或真实网络。
- 38 个公共头文件分别独立编译通过。
- 安装到 `install/phase5` 后，独立程序仅使用安装头文件和动态库，配合应用侧 FakeTransport 完成两轮 LLM、工具结果 42、最终回答 42 的闭环，退出码 0。
- Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际验证。

按阶段约定停在阶段 5 验收点，阶段 6 待确认后开始。
