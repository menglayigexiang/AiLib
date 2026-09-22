# 阶段 1 验收记录

日期：2026-09-18。范围：公共 canonical model、公共配置、取消与媒体基础行为、Qt Test。尚未进入阶段 2。

## 实际实现

- 添加 20 个公共头文件。所有模型使用 AiLib 命名空间和普通值类型；共享生命周期只用于 Cancellation 状态。
- Content 使用有序 variant；ToolCallContent/ToolResultContent 只包装原有模型。text()/toolCalls() 是查询，不是另一份底层存储。
- Usage 使用 optional 区分未知与零；汇总语义写入架构文档，汇总运行时留到 Agent 阶段。
- 实现 Message::user/system/text/toolCalls。
- 实现 CancellationSource/Token 的原子共享取消状态，无线程创建。
- 实现媒体简单工厂、严格 Base64 和基本来源校验。解析失败不修改 output；合法空输入成功。校验不联网、不读取文件。
- 添加公共导出宏，Qt Core 和 C++17 依赖传播，SDK 头文件安装。移除空库占位源和 Windows 占位导出。
- 增加 Qt Test、公共头文件独立编译检查；TestApp 的 LibAiCore 页面使用一个真实导出 API 作为消费验证。
- tests/support 仅保留 .gitkeep，没有 FakeTransport、临时响应队列或 Transport 模型。

## 关键接口

```cpp
QString Message::text() const;
QList<ToolCall> Message::toolCalls() const;

bool MediaResource::fromBase64(
    const QByteArray&, const QString&, MediaResource&, SdkError&);
bool validateMediaResource(const MediaResource&, SdkError&);

CancellationToken CancellationSource::token() const;
void CancellationSource::cancel() noexcept;
bool CancellationToken::isCancellationRequested() const noexcept;
```

## 构建与测试结果

环境：macOS arm64，AppleClang 21.0.0，Homebrew Qt 6.11.1，Debug，C++17。

- SDK、TestApp、四个测试程序和全部 20 个头文件独立编译检查：通过。
- CTest：4/4 通过，0 失败。
- Qt Test 明细：PublicModels 4、Message 2、MediaResource 19（含数据驱动行）、Cancellation 3，共 28 个行为测试执行；加上各组 init/cleanup，Qt Test 报告合计 36 passed。
- TestApp 以 offscreen 模式启动并正常退出，返回 0。
- SDK component 的全新前缀安装：通过。外部消费程序仅使用安装头文件和动态库，编译、链接、运行返回 0；覆盖 Message、Cancellation、MediaResource 导出接口。
- git diff --check：通过。
- 核心 include/src 检查：没有 QObject/Q_OBJECT、QThread/std::thread、QtConcurrent、QFuture/QPromise，也没有 ITransport/IProtocolAdapter 占位接口。

测试仅在测试程序创建一个 std::thread 验证取消状态传播，SDK 本身不创建线程。

可重复命令（本机）：

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools /opt/homebrew/opt/qtbase/bin/qt-cmake \
  -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build -j 6
ctest --test-dir build -C Debug --output-on-failure
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --install build \
  --config Debug --prefix "$PWD/install/verified" --component SDK
QT_QPA_PLATFORM=offscreen TestApp/bin/Debug/TestApp.app/Contents/MacOS/TestApp
```

## 发现与限制

初次配置因系统选中的 Xcode 未接受许可证而失败；没有修改系统设置或接受许可证，改用本次命令的 DEVELOPER_DIR 指向已有 Command Line Tools 后通过。首次未指定该环境的安装曾提示相同问题，因此重新以该环境安装到全新前缀，并验证消费链接。

测试目录需要自行启用 CXX：现有根项目 LANGUAGES NONE，子项目的语言上下文不能当作测试目录的配置，因此 tests/CMakeLists.txt 使用 project(AiLibTests LANGUAGES CXX)。没有重构现有工程组织。

本机没有找到 Qt 5.15 SDK，因此 Qt 5.15 构建/测试尚未实机验证。Windows、Linux、最低 Qt 6.2 同样未实机验证。代码仅使用 Qt 5.15 可用 API，并针对 Capability 的 Qt 5/6 qHash 签名提供条件兼容；不能把静态兼容检查当作实机通过。

仅验证 SDK component 安装，未重新验收 TestApp 的完整 Qt 插件部署，也未提供 find_package(AiLib) 包配置。

## 文件变更

修改：CMakeLists.txt、LibAiCore/CMakeLists.txt、README.md、TestApp/main.cpp。

删除：LibAiCore/src/LibAiCore.cpp（原为空文件）。

新增公共头文件：

- `LibAiCore/include/AiLib/Export.h`
- `LibAiCore/include/AiLib/agent/AgentLimits.h`
- `LibAiCore/include/AiLib/agent/AgentRequest.h`
- `LibAiCore/include/AiLib/agent/AgentResult.h`
- `LibAiCore/include/AiLib/client/ChatRequest.h`
- `LibAiCore/include/AiLib/client/ChatResponse.h`
- `LibAiCore/include/AiLib/client/RequestOptions.h`
- `LibAiCore/include/AiLib/core/Cancellation.h`
- `LibAiCore/include/AiLib/core/Content.h`
- `LibAiCore/include/AiLib/core/Error.h`
- `LibAiCore/include/AiLib/core/MediaResource.h`
- `LibAiCore/include/AiLib/core/Message.h`
- `LibAiCore/include/AiLib/core/ModelInfo.h`
- `LibAiCore/include/AiLib/core/Usage.h`
- `LibAiCore/include/AiLib/provider/ProviderConfig.h`
- `LibAiCore/include/AiLib/tools/FunctionToolDefinition.h`
- `LibAiCore/include/AiLib/tools/ToolCall.h`
- `LibAiCore/include/AiLib/tools/ToolChoice.h`
- `LibAiCore/include/AiLib/tools/ToolErrorCodes.h`
- `LibAiCore/include/AiLib/tools/ToolResult.h`

新增实现：LibAiCore/src/core/Cancellation.cpp、MediaResource.cpp、Message.cpp。

新增测试：tests/CMakeLists.txt、tests/support/.gitkeep、tests/unit/tst_PublicModels.cpp、tst_Message.cpp、tst_MediaResource.cpp、tst_Cancellation.cpp。

新增文档：docs/architecture.md、docs/aicore/phase1-report.md。

## 阶段边界

未实现 Client/Agent/Registry/Executor/Approval 运行时、Transport、完整 ProtocolAdapter、Streaming Decoder/Session、FakeTransport、MCP、Context Management 或 Result<T>。默认值与契约已定义，但请求覆盖、Agent 限制、用量汇总和重试逻辑尚未执行。

等待用户阶段验收确认，再进入阶段 2 最小普通 Chat 闭环。
