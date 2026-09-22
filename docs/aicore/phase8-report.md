# 阶段 8：CLI、Qt Widgets Demo 与整体验收

已形成可运行应用闭环。新增 `examples/cli/main.cpp`、`examples/support/DemoSupport.h` 和 examples CMake 目标 ailib_cli；TestApp 的 LibAiCore 页面提供最小 Widgets 应用闭环。公共 SDK 接口和运行时未因 Demo 修改，线程、UI、历史与确认策略仍属于应用层。

## 本阶段实现

- CLI：普通 Chat、Streaming、普通/流式 Agent、同步 stdin 确认和工具执行状态；支持 Allow/Deny/Cancel，EOF 默认拒绝。
- Widgets：Provider 选择、输入、Streaming、严格工具白名单开关、流式输出、Stop、非模态确认、工具状态及最终 finishReason、清空历史。
- 应用使用 QThread::create 运行同步 Agent，值捕获本轮配置和历史，不跨线程读取控件。
- GuiApproval 通过 UI 队列创建弹窗，当前工作线程同步等待 condition_variable，每二十毫秒检查同一 cancellation 与截止时间；独立等待状态不把 SDK 改成异步框架。
- 取消/关闭窗口协作停止，不强杀线程；窗口析构先取消、等待线程，然后销毁其借用的 Registry 和 Approval。
- 应用只把 Completed 的本轮输入与 newMessages 纳入长期历史；异常或取消的实际文本继续展示。这是示例策略，不改变 Agent 增量消息规则。
- 默认离线 Transport 模拟原始 HTTP/SSE，但使用真实 Adapter、Session、Client、Agent 和工具链。真实 DeepSeek/Kimi 配置通过环境变量启用，未复制 Key 到源码、文档或输出。

## 验证

- macOS / Qt 6.11.1 / C++17 全项目编译通过，39 个公共头文件独立编译通过。
- CTest 15/15 组通过，包括此前 12 组核心测试、CLI Chat/Stream 两组以及 WidgetsDemo。
- WidgetsDemo 包含 14 个实际用例：Allow/Deny 工具闭环、流开始时取消、产生有效文字后取消、确认等待取消、确认时关闭窗口、运行中直接析构；其中 7 个用例通过 QProcess 验证真实 CLI 的普通/流式 Allow/Deny/Cancel 和 EOF 拒绝。
- 实际 macOS 窗口操作验收：确认界面包含 agentId/callId，允许 add，出现 Handler=已执行、42、Completed，控件恢复可用。
- 自动测试全部离线或本地 HTTP，不依赖 API Key 或外部服务；真实服务手动验收入口已提供，本阶段未调用真实 Provider。
- Qt 5.15、最低 Qt 6.2、Windows/Linux 尚未实际测试；各平台部署安装包未重新验收。

构建、运行、确认、历史策略和真实服务使用方式见 [示例使用说明](examples.md)。SDK 保持当前已确定的第一版范围：Local Function Tool、两种真实协议实现、同步 API、调用方管理线程和历史；MCP 与 Provider Builtin Tool 未实现。

按约定停在阶段 8 验收点。后续优先依据实际运行反馈修复问题、补齐目标平台验证，而非提前增加新抽象。
