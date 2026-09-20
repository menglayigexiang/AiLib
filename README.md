# AiLib

基于 Qt/C++ 的跨平台 LLM SDK，当前已完成公共模型、普通 Chat、Streaming、本地 Function Tool、Agent Loop、网络重试及 Provider 模型目录。支持 OpenAI Chat Compatible、OpenAI Responses、Anthropic Messages 三种协议，核心 API 全部同步，线程与历史由应用管理。CLI 和 Qt Widgets Demo 连接真实服务运行。

要求 CMake 3.21+、C++17、动态链接版本的 Qt 6.2+ 或 Qt 5.15。优先选择 Qt 6。库目标为 `AiLib::Core`，公共头文件通过 `<AiLib/...>` 使用；Qt Core 和 C++17 需求向消费目标传播。只有 TestApp 和 UI 测试依赖 Widgets，SDK 公共模型依赖 Core，网络实现私有依赖 Network。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Qt 5.15 验证可以添加 `-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON`，并指定 Qt 5 SDK 路径。切换 Qt 主版本、编译器或生成器前删除根 `build`、`LibAiCore/build`、`TestApp/build`。现有工程对子项目固定使用源码目录内的 build/bin，不要同时从多个根构建目录配置。

`BUILD_TESTING=OFF` 可关闭测试。自动测试不用 API Key 或外网；GUI 测试使用 offscreen 平台插件，不要求可见窗口；每个公共头文件还会单独编译检查包含完整性。tests/support 包含实现真实 ITransport 的 FakeTransport 和本地 HTTP 测试服务，不进入 SDK 运行时。

安装库和头文件（不运行应用部署）：

```sh
cmake --install build --config Debug --prefix "$PWD/install" --component SDK
```

完整安装省略 `--component SDK`，会执行现有 TestApp 的 Qt 部署规则。Windows 使用绝对安装前缀，Qt 与编译器/架构必须匹配。当前未提供 find_package(AiLib) 包配置，后续需要时再增加。

普通 Chat 示例（调用方创建 QCoreApplication，可在自己选择的线程同步执行）：

```cpp
#include <QCoreApplication>
#include <AiLib/client/LLMClientFactory.h>

int main(int argc, char* argv[]) // 应用自行创建 Qt 环境并同步调用 SDK
{
    QCoreApplication app(argc, argv); // 调用方持有的 Qt 事件环境
    AiLib::ProviderConfig provider; // 显式配置模型服务和协议
    provider.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:11434/v1"));
    provider.protocol = AiLib::ProtocolType::OpenAIChatCompletions;
    // 本地服务可免密钥，其他服务由调用方安全提供 apiKey。

    std::unique_ptr<AiLib::LLMClient> client; // 接收 Factory 创建的独占 Client
    AiLib::SdkError error; // 创建与请求的故障详情
    if (!AiLib::LLMClientFactory::create(provider, client, error))
        return 1;

    AiLib::ChatRequest request; // 本次单候选普通请求
    request.model = QStringLiteral("your-model-name");
    request.messages.append(AiLib::Message::user(QStringLiteral("你好")));
    AiLib::ChatResponse response; // 输出助手内容、用量及结束状态
    if (!client->chat(request, response, error))
        return 2;

    // response.message.text() 获取普通回答文字。
    return 0;
}
```

可选的本次执行选项：`client->chat(request, response, error, options)`。不传 options 时使用 Client 默认配置，显式传入时整体替换。RetryPolicy 已执行，默认最多重试 5 次、固定间隔 30 秒，0 表示不重试；`stream=true` 使用请求级 Decoder 和 Session，`options.streamCallback` 实时通知标准化事件。

图片放在 Message.contents，URL、Bytes、LocalFile 按协议编码。详细能力和边界见 [阶段 2 验收记录](docs/phase2-report.md)。

完整字段语义、所有权、超时/重试/取消规则和职责边界见 [架构契约](docs/architecture.md)。各阶段验证见 [阶段 1](docs/phase1-report.md)、[阶段 2](docs/phase2-report.md)、[阶段 3](docs/phase3-report.md)、[阶段 4](docs/phase4-report.md)、[阶段 5](docs/phase5-report.md)、[阶段 6](docs/phase6-report.md)、[阶段 7](docs/phase7-report.md)、[阶段 8](docs/phase8-report.md)。

Anthropic Messages / Kimi Code 配置：

```cpp
AiLib::ProviderConfig provider;  // Messages API 根地址及运行时凭据
provider.protocol = AiLib::ProtocolType::AnthropicMessages;
provider.baseUrl = QUrl(QStringLiteral("https://api.kimi.com/coding/"));
provider.apiKey = QString::fromUtf8(qgetenv("KIMI_CODE_API_KEY"));

AiLib::ChatRequest request;  // 在现有 Factory/Client 调用流程中使用
request.model = QStringLiteral("kimi-for-coding");
request.maxOutputTokens = 1024;
request.messages.append(AiLib::Message::user(QStringLiteral("解释 C++17 的 unique_ptr。")));
```

Adapter 在 API 根地址后追加 `/v1/messages`，这里不要把 baseUrl 写成完整端点或 `/coding/v1`。普通 Anthropic 服务可以使用 `https://api.anthropic.com`。未设置 maxOutputTokens 时使用 1024，因为 Messages 必须提供 max_tokens。默认认证为 x-api-key，协议版本头为 2023-06-01。

可选真实 Kimi 测试：配置时增加 `-DAILIB_BUILD_MANUAL_TESTS=ON`，构建后在设置 KIMI_CODE_API_KEY 的调用方环境运行 `build/tests/manual_kimi_messages`。它执行三次请求，验证普通 Chat 和一次加法工具往返，不进入 CTest。详细映射、限制和验证记录见 [Anthropic Messages 接入记录](docs/anthropic-messages-report.md)。

DeepSeek 的 OpenAI Compatible 手动测试使用同一个 `AILIB_BUILD_MANUAL_TESTS` 开关。设置 `DEEPSEEK_API_KEY` 后运行 `build/tests/manual_deepseek_chat`；当前配置为 `https://api.deepseek.com`、`deepseek-flash`，关闭 thinking，验证普通 Chat 和加法工具往返。真实验证记录见 [DeepSeek 接入测试](docs/deepseek-test-report.md)。

OpenAI Responses 使用 `ProtocolType::OpenAIResponses`，Factory 在 API 根地址后追加 `/responses`。示例目录收录 `gpt-5.6-sol`、`gpt-5.6-terra`、`gpt-5.6-luna`，凭据由应用通过 `OPENAI_API_KEY` 或 Widgets 输入框提供。Adapter 支持普通响应、SSE 流和 Function Tools。

流式调用示例（沿用已创建的 client）：

```cpp
AiLib::ChatRequest request;  // 本次流式问题及上下文
request.model = QStringLiteral("your-model-name");
request.stream = true;
request.messages.append(AiLib::Message::user(QStringLiteral("你好")));

AiLib::RequestOptions options;  // 单次超时、取消及实时通知
options.streamCallback = [](const AiLib::StreamEvent& event) {  // 回调只展示或通知，不执行工具
    if (event.type == AiLib::StreamEventType::TextDelta) {
        // 将 event.delta 交给应用的显示逻辑。
    }
};

AiLib::ChatResponse response;  // SDK Session 聚合的最终响应或有效部分
AiLib::SdkError error;        // 网络、取消、超时或协议故障
const bool ok = client->chat(request, response, error, options);  // 同步等待本次请求结束
```

回调在调用 chat 的线程运行；GUI 应用自行选择工作线程并把事件复制后交给 UI。Callback 内可通过 CancellationSource 请求协作取消。返回 false 时仍检查 response 中的有效内容，消息及协议状态标记为 Incomplete。FinishReason 通知后还可能收到 Usage，最终以 chat 返回值和 CompletionState 判断结果。

两种真实 Provider 手动测试都可添加 `--stream`：`build/tests/manual_kimi_messages --stream`、`build/tests/manual_deepseek_chat --stream`。不传参数保持原来的非流式测试。

本地工具可独立通过 `ToolRegistry::registerTool()` 注册，由 `ToolExecutor::execute()` 同步执行。应用拥有 Registry 和可选的 `IToolApprovalProvider`，执行器仅使用它们。工具失败返回 `true + 失败 ToolResult`；取消或总截止时间返回 `false + SdkError`，由后续 Agent 决定结束原因。Schema 校验范围、共享串行锁和确认边界见 [阶段 4 验收记录](docs/phase4-report.md)。

最小 Agent 已支持普通和 Streaming 的多轮本地工具调用，历史由应用传入，仅返回 `AgentResult.newMessages`。`maxToolCalls` 支持 -1 不限、0 禁止和正数总额度。`AgentRequest.callback` 接收工具流程开始/结束事件，`RequestOptions.deadline` 传递精确总预算。具体接口、测试和时间预算边界见 [阶段 6 验收记录](docs/phase6-report.md)。

重试只针对启用的临时连接错误、429 和 5xx，优先采用 Retry-After。等待和每次尝试受共享取消令牌及精确 deadline 约束；Streaming 有效输出后不再重试。实现边界与网络验证见 [阶段 7 验收记录](docs/phase7-report.md)。

CLI 与 Qt Widgets Demo 的运行、同步确认和历史维护方式见 [示例使用说明](docs/examples.md)。运行前设置对应 Provider 的 API Key；GUI 位于 `TestApp/bin/Debug` 下的 TestApp 应用。

第一版真实服务结果见 [真实服务验收报告](docs/live-acceptance-report.md)。DeepSeek 核心闭环和取消通过；Kimi 在明确 C++ 测试提示下的闭环、确认取消和 GUI Stop 已复验通过，但短提示及 Auto 仍可能不生成工具，详见 [Kimi 工具复验](docs/kimi-tool-followup.md)。可选手动目标 `manual_agent_acceptance` 和 `manual_gui_acceptance` 提供代码复验，不进入 CTest。
