# 第一版真实服务验收

日期：2026-09-18。环境：macOS、Qt 6.11.1、C++17。使用项目中已授权的临时凭据，测试进程通过环境变量读取；源码与报告不保存 Key。SDK 运行时接口未修改。

## 实际结果

| 验收项 | DeepSeek / OpenAI Chat Compatible | Kimi Code / Anthropic Messages |
| --- | --- | --- |
| 普通 Chat + 手动工具往返 | 通过 | 通过 |
| Streaming Chat + 手动工具往返 | 通过 | 通过 |
| Agent 允许工具后继续回答 | 通过，得到 42 | 首次通过；后续 GUI 测试未生成工具调用 |
| Agent 拒绝工具后继续回答 | 通过，ApprovalDenied 返回模型 | 首次通过；后续 GUI 测试未生成工具调用 |
| 确认决定 Cancel | 通过，Handler 未执行 | 未通过：模型没有实际生成 ToolCall，确认未触发 |
| 有效文字输出后取消网络流 | 通过 | 通过 |
| GUI 代码测试 Allow / Deny | 两项通过 | 两项未通过：未触发确认弹窗 |
| GUI 确认等待期间 Stop | 通过，Handler 未执行 | 尚未验收成功，当前工具触发问题仍存在 |
| GUI 有效文字输出后 Stop | 通过 | 通过 |

请求模型为已有示例配置：DeepSeek `deepseek-flash`、Kimi `kimi-for-coding`。两种配置均关闭 thinking，使用无副作用的 add 工具。

最初批次的十个测试场景中九个通过，一项 Kimi Auto 工具取消测试未触发调用。后续多次更明确提示以及指定 add 工具的取消测试仍没有触发 Kimi ToolCall；这些没有按成功取消计数。Kimi 流式取消独立执行并通过。

真实 GUI 代码测试首批六个实际用例，四个通过、两个 Kimi 确认用例未通过；另补测 DeepSeek 确认等待 Stop，通过。Qt Test 的 init/cleanup 不计入业务用例。项目离线回归 CTest 15/15 组通过。

## Kimi 工具触发问题的证据和边界

取消测试发送 Specific 工具选择，由 Anthropic Adapter 编码为 `tool_choice: {"type":"tool","name":"add"}`。代码检查显示 Adapter 会编码工具定义与工具选择，Agent 先清空外部工具列表，再从严格白名单追加已注册定义；没有发现本测试工具在这条链路中被移除。

为区分服务与 SDK，另外直接发送相同核心参数到 Kimi Messages 端点，解析原始 SSE 事件摘要：HTTP 200，内容块类型只有 `text`，停止原因为 `end_turn`，没有原生 `tool_use`。结束事件序列正常。

这次直接探测支持“服务端未生成工具调用”的判断，不支持“SDK 流解码丢失 ToolCall”的判断。确切原因尚未确定，不能据此宣称 Kimi 的所有工具功能不可用：同一轮验收的初始普通/流式工具往返和 Agent Allow/Deny 确实成功过。

SDK 当前正确保留了实际纯文本回复并正常 Completed；不把文字中的“将调用工具”伪造为 ToolCall，不根据工具名称执行 Handler，也不添加隐式重复 LLM 请求来强迫服务调用工具。Kimi 的工具确认/取消兼容性暂不能签为完整通过，应继续独立定位当前服务的工具选择行为。

## 本次代码与规则调整

- 新增 `tests/manual/agent_acceptance.cpp`：验证真实确认 Cancel、关联 ID、Handler 未执行，以及有效流式文本取消后的 Incomplete 增量消息。Specific 只用于 Cancel 场景，不进入下一轮，不改变普通 Agent 的 Auto 选择语义。
- 新增 `tests/manual/gui_acceptance.cpp`：Qt Test 直接验证实际 LibAiCorePage 的同步确认、工具结果、网络取消和线程生命周期；不截图，不依赖外部 UI 自动控制。
- 两个手动目标由 `AILIB_BUILD_MANUAL_TESTS` 开关控制，不注册到 CTest，自动回归不使用真实 Key。
- Widgets 支持 `--provider offline/deepseek/kimi` 启动选项，便于配置测试进程；不接受命令行凭据。Ready 状态显示实际初始 Provider，修复指定真实 Provider 时仍提示“默认离线”的展示问题。
- AGENTS.md 新增优先代码分析与测试、尽量不用 UI 截图的规则。用户提出该要求后，停止原生截图操作，剩余 GUI 验收改为 Qt Test 代码断言。

## 复验

配置 `AILIB_BUILD_MANUAL_TESTS=ON` 并构建，调用方环境提供相应 API Key，再运行：

```sh
build/tests/manual_deepseek_chat
build/tests/manual_deepseek_chat --stream
build/tests/manual_kimi_messages
build/tests/manual_kimi_messages --stream
build/tests/manual_agent_acceptance deepseek
build/tests/manual_agent_acceptance kimi
QT_QPA_PLATFORM=offscreen build/tests/manual_gui_acceptance
```

可指定 Qt Test 单个场景，例如 `manual_gui_acceptance approval:deepseek-allow` 或 `manual_gui_acceptance approvalStop`。工具未生成或 Provider 失败会明确返回测试失败，不用静默跳过伪装为通过。真实调用具有费用、服务状态及模型输出变化，不作为 CI 必需条件。

下一步应先处理 Kimi 当前工具触发不稳定这一具体兼容性问题，再补目标平台验收。当前不为此增加 MCP、模型名称推断、自动工具重试或新的核心抽象。

## 后续代码复验

增强示例的 C++ 测试提示后，Kimi CLI Allow/Deny/Cancel、确认取消、网络流取消及 GUI 确认等待 Stop 均在具体样本中通过。二十二次原始协议/SDK 对照未发现调用丢失，仍存在短提示及 Auto 不生成工具的服务行为，不能保证任意提示稳定。原批次失败记录保持不变；最新证据见 [Kimi 工具复验报告](kimi-tool-followup.md)。
