# Kimi 工具触发问题：代码对照与复验

日期：2026-09-18。macOS / Qt 6.11.1 / C++17。使用已授权的 Kimi Code 测试凭据，仅通过测试进程环境读取。全程采用代码分析与协议/Qt Test 验证，没有进行 UI 截图。

## 结论

没有发现工具定义丢失或 SDK 解码丢失。二十二次对照请求中，原始协议和 SDK 工具调用数量全部一致：原生共十五个 tool_use，SDK 共十五个 ToolCall。

工具触发与提示内容有关。带有明确 C++ 加法函数测试目的的提示更容易触发工具；简短算术提示在本轮六次 Specific 对照中均只返回文本/end_turn。Auto 也有一次完整提示返回纯文本，不能视为强制调用保证。

这些数据定位了问题发生在服务生成行为这一侧，但没有确定服务内部为什么如此处理提示或 Specific。不能据此宣称 Kimi 永远不能调用工具，也不能宣称修改提示就保证任意场景稳定。

## 对照方式

新增可选 `manual_kimi_tool_probe`：真实 AnthropicMessagesAdapter + QtHttpTransport。每次请求独立创建 Client 和捕获器，在内存观察编码正文及原始响应，同时交给真实 Decoder/Session 解码。只打印工具定义数量、工具选择字段、原始块数量/停止原因、SDK 调用数量和文字长度，不打印 Header、认证、请求正文或模型全文。

普通响应从 content 数组数 tool_use；流式响应从原始 content_block_start 数 tool_use。本次 Kimi SSE 的 data 为单行 JSON，该诊断计数器针对这次数据形态，不是新的通用 Streaming 解析实现。

所有请求编码均包含一个 add 工具，tool_choice 实际发送值与指定模式一致。单次 timeout=45 秒，不自动重试。范围只覆盖这一模型、端点、测试工具和提示样本。

## 实际矩阵

合并基础矩阵、重复基础样本和原 Demo 定义对照；每组含普通与流式请求。

| 工具说明 | 选择方式 | 提示 | 请求数 | 产生原生工具调用的响应数 |
| --- | --- | --- | ---: | ---: |
| 原手动测试说明 | Specific | 完整 C++ 测试提示 | 4 | 4 |
| 原手动测试说明 | Specific | 简短算术提示 | 4 | 0 |
| 原手动测试说明 | Required | 完整 C++ 测试提示 | 4 | 4 |
| 原手动测试说明 | Auto | 完整 C++ 测试提示 | 4 | 3 |
| 原 Demo 说明 | Specific | 完整 C++ 测试提示 | 2 | 2 |
| 原 Demo 说明 | Specific | 简短算术提示 | 2 | 0 |
| 原 Demo 说明 | Auto | 完整 C++ 测试提示 | 2 | 2 |

全部二十二个响应正常解码，原始/SDK 调用数量不一致为零。原 Demo 工具说明没有证明存在问题，因此未修改它。

## 最小调整与复验

CLI、Widgets 默认用户提示与手动 Agent 取消测试改为：

> 请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。

只增强示例的编程测试目的。不更改 Agent 工具选择规则，不增加隐式 LLM 请求，不自动把 Specific 改成 Required，不执行文本中口头提及的工具，不增加 Provider 名称推断或新的核心抽象。

Kimi 复验结果：

- 原有普通、Streaming 手动工具往返再次通过。
- CLI Auto 允许：实际 ToolCall，Handler 执行，最终回答包含 42，Completed/newMessages=3。
- CLI Auto 拒绝：实际 ToolCall，ApprovalDenied 反馈模型，模型继续回答，Completed/newMessages=3。
- CLI Auto 取消：实际确认触发，Handler 未执行，Cancelled/newMessages=1。
- 手动 Agent Specific 确认 Cancel：确认一次、Agent/Call 关联有效、Handler 执行零次，正常 Cancelled。
- 有效文字后取消流：Cancelled，保留有效文字和 Incomplete 增量消息。
- GUI Qt Test 代码断言 Allow/Deny：两项通过；得到正确工具执行状态及最终 Completed。
- GUI Qt Test 确认等待 Stop：Kimi 数据行通过，Handler 未执行，Cancelled，确认弹窗生命周期正常结束。
- 离线 CTest 回归 15/15 组通过，公共头文件编译检查通过。

上述是具体样本验收结果。此前未触发工具的失败记录仍保留在原报告，不把本轮成功覆盖为“历史始终正常”。

## 复验入口

启用 `AILIB_BUILD_MANUAL_TESTS=ON` 并构建后：

```sh
build/tests/manual_kimi_tool_probe                 # 默认八项基础对照
build/tests/manual_kimi_tool_probe --demo-variants # 原 Demo 定义的四项 Specific 对照
build/tests/manual_kimi_tool_probe --demo-auto     # 原 Demo 定义的两项 Auto 对照
build/tests/manual_agent_acceptance kimi
QT_QPA_PLATFORM=offscreen build/tests/manual_gui_acceptance approval:kimi-allow approval:kimi-deny
QT_QPA_PLATFORM=offscreen build/tests/manual_gui_acceptance approvalStop:kimi
```

探针退出码 0 只表示未发现正常响应中原始/SDK 工具数量不一致，不表示每项必定生成工具，也不表示全部请求故障场景都通过；逐项查看 ok/errorCode 摘要。真实手动测试均不进入 CTest。

本项至此完成代码诊断和示例复验。后续若要保证业务必须调用工具，需要明确讨论 Provider 当前的强制选择语义；不能靠示例提示或 Auto 作保证。现阶段保持核心接口不变。
