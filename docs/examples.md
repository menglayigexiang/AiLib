# CLI 与 Qt Widgets 示例

示例从统一 Provider 模型目录选择真实服务。首批包含 DeepSeek、Kimi 和 OpenAI；运行前需要提供对应 API Key。自动测试使用 FakeTransport 等测试替身，不把 Mock 建模为 Provider。

## 构建和运行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

CLI 单配置生成器路径为 `build/examples/ailib_cli`；多配置生成器根据配置进入 Debug/Release 子目录，Windows 文件名带 `.exe`。

```sh
build/examples/ailib_cli --provider deepseek --mode chat --prompt "你好"
build/examples/ailib_cli --provider kimi --mode stream --prompt "你好"
build/examples/ailib_cli --provider openai --model gpt-5.6-sol --mode agent
```

Agent 工具确认时：`1` 允许、`2` 拒绝、`3` 取消整个 Run；EOF 或其他输入默认拒绝。拒绝产生 ApprovalDenied ToolResult，模型可继续回答；取消正常结束为 Cancelled。CLI 所有流程在当前线程执行，阻塞式 stdin 无法被取消令牌强制打断；没有增加信号处理或输入线程。

Widgets 程序位于 `TestApp/bin/Debug` 或对应配置目录。macOS 启动 `TestApp.app`；Windows 启动 `TestApp.exe`；Linux 启动 `TestApp`。界面从统一目录提供 Provider 和可编辑模型选择，并默认启用 Streaming 与 add 工具。

1. 点击发送或回车，输入和配置被冻结，应用创建工作线程运行同步 Agent。
2. 模型完整生成 ToolCall 后显示确认弹窗，默认拒绝。Yes/No/Cancel 分别对应 Allow/Deny/Cancel。
3. 弹窗非模态，等待期间仍可操作 Stop；同一 CancellationToken 传播到 Agent、网络、工具和确认等待。
4. 输出区报告工具开始/结束和 Handler 是否实际执行；模型生成与工具执行事件保持独立。
5. 运行完成后展示 finishReason；部分流式文字在取消或故障后仍留在输出区。
6. 运行中关闭窗口会先取消，完成后再关闭；析构等待线程结束，避免 Registry/Approval 被提前销毁。

GUI 只使用队列投递更新 UI，不使用 BlockingQueuedConnection。GuiApproval 每次请求独立创建等待状态，可由多个 Agent 共用；前提是应用保证接收窗口活到全部等待结束。队列调用与 UI 线程职责参考 [QMetaObject 文档](https://doc.qt.io/qt-6/qmetaobject.html) 和 [QThread 文档](https://doc.qt.io/qt-6/qthread.html)。SDK 没有增加 Widgets 依赖或创建线程的行为。

## 历史由应用管理

Widgets 保存完整历史，给工作线程传值副本。Completed 后，应用把本轮用户输入和 newMessages 追加到历史；失败、取消或其他正常停止的本轮记录仍显示，但不进入长期历史，这是示例应用自己的策略。清空历史或切换 Provider 时由应用清空会话；Agent 不裁剪、不总结、不管理会话。

CLI 也演示 Completed 后追加 newMessages，但只执行一轮后退出，不保存到磁盘。普通 chat/stream 模式直接调用 Client，不经过 Agent。

## 真实 Provider

在调用方环境设置 `DEEPSEEK_API_KEY`、`KIMI_CODE_API_KEY` 或 `OPENAI_API_KEY`。不要把 Key 放到源码或运行命令参数中。CLI 根据 `--provider` 读取对应环境变量；Widgets 在窗口顶部输入密钥，密钥只保留在控件内存中。Key 为空时明确显示 MissingApiKey，不发送 HTTP 请求。

DeepSeek 使用 `deepseek-flash` 和 OpenAI Chat Compatible；Kimi Code 使用 `kimi-for-coding` 和 Anthropic Messages；OpenAI 使用 Responses 协议并收录 `gpt-5.6-sol`、`gpt-5.6-terra`、`gpt-5.6-luna`。模型框允许输入当前 Provider 下尚未收录的 model ID，目录不作为调用白名单。

```sh
build/examples/ailib_cli --provider deepseek --mode agent
build/examples/ailib_cli --provider kimi --mode agent
build/examples/ailib_cli --provider openai --model gpt-5.6-sol --mode agent
```

工具为无副作用加法；示例仍默认 Always 确认，用于展示应用策略。真实模型不保证按每个提示调用工具；若不调用工具，普通回答也能正常结束。

真实服务验收可以检查：普通回答、Streaming、add 工具闭环、拒绝后继续回答、等待确认时 Stop、流式 Stop。手动协议测试也可以启用 `AILIB_BUILD_MANUAL_TESTS=ON` 后运行 `manual_kimi_messages` / `manual_deepseek_chat`，加 `--stream` 验证流式工具往返。它们不进入 CTest。本阶段未使用真实凭据发起请求。

## 当前验证范围

macOS Qt 6.11.1/C++17 已编译并通过自动测试；Widgets 使用 offscreen Qt 平台插件进行自动交互测试，并操作实际 macOS 窗口验证 Allow 后得到 42/Completed。Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际验证。部署沿用既有 TestApp 规则，本阶段不声称已完成各平台安装包验收。

## 真实服务的代码验收

Widgets 支持 `--provider deepseek` / `--provider kimi` / `--provider openai` 启动配置。真实服务验收优先使用代码断言，不进行逐步 UI 截图。

启用 `AILIB_BUILD_MANUAL_TESTS=ON` 后新增 `manual_agent_acceptance deepseek|kimi` 和 `manual_gui_acceptance`。前者检查确认 Cancel 与有效文本取消后的 Incomplete；后者通过 Qt Test 调用实际 DemoWindow，检查 Allow/Deny、确认等待 Stop 和流式 Stop。两个目标不进入 CTest。macOS/Linux 可用 `QT_QPA_PLATFORM=offscreen build/tests/manual_gui_acceptance` 执行无截图测试。

本次真实结果及 Kimi 工具触发限制见 [真实服务验收报告](live-acceptance-report.md)；不要把首次通过视为全部兼容性已验收通过。

默认工具提示已明确为 C++ 加法函数测试，Kimi 的 Allow/Deny/Cancel 与 GUI 确认取消已复验通过。Auto 仍不保证必须调用工具；证据与可选协议探针入口见 [Kimi 工具复验报告](kimi-tool-followup.md)。
