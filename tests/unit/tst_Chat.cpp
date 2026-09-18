#include <AiLib/client/LLMClientFactory.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include "../support/FakeTransport.h"
#include "../support/TestOptions.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryFile>
#include <QtTest>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>

using namespace AiLib;
namespace {
ProviderConfig provider()  // 创建包含网关前缀的免密钥测试服务配置
{
    ProviderConfig config;  // 用于离线编码及 Factory 组装的服务配置
    config.baseUrl = QUrl(QStringLiteral("https://example.com/gateway/v1/"));
    return config;
}
ChatRequest request()  // 构造最小普通文字模型请求
{
    ChatRequest value;  // 带一个用户输入的 canonical 请求
    value.model = QStringLiteral("test-model");
    value.messages.append(Message::user(QStringLiteral("你好")));
    return value;
}
QJsonObject rootResponse()  // 构造单候选完整助手响应，服务端明确报告输入用量为零
{
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("response_1")},
        {QStringLiteral("model"), QStringLiteral("test-model")},
        {QStringLiteral("choices"),
         QJsonArray{
             QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("stop")},
                         {QStringLiteral("message"),
                          QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                      {QStringLiteral("content"), QStringLiteral("你好！")}}}}}},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 0}}}};
}
TransportResponse response(const QJsonObject& root,
                           int status = 200)  // 将预设厂商 JSON 包装为真实接口的 HTTP 响应
{
    return TransportResponse{status, {}, QJsonDocument(root).toJson(QJsonDocument::Compact)};
}
QJsonObject tool(const QString& id,
                 const QString& arguments = QStringLiteral(
                     "{\"device_id\":1001}"))  // 构造原生函数调用对象，便于覆盖非法 ID 和参数
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("type"), QStringLiteral("function")},
                       {QStringLiteral("function"),
                        QJsonObject{{QStringLiteral("name"), QStringLiteral("get_temperature")},
                                    {QStringLiteral("arguments"), arguments}}}};
}
QJsonObject toolResponse(const QJsonArray& calls)  // 构造包含完整工具调用集合的单候选响应
{
    QJsonObject root = rootResponse();  // 保留基本响应标识和用量的工具响应
    root[QStringLiteral("choices")] =
        QJsonArray{QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("tool_calls")},
                               {QStringLiteral("message"),
                                QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                            {QStringLiteral("content"), QStringLiteral("我查一下")},
                                            {QStringLiteral("tool_calls"), calls}}}}};
    return root;
}
}  // 测试数据辅助函数命名空间结束

// 覆盖 Client、Factory 和普通 Chat Adapter 的离线闭环
class ChatTest : public QObject {
    Q_OBJECT
private slots:
    void plainChatAndOptions()  // 验证中文普通回答、默认选项及单次覆盖
    {
        auto transport = std::make_unique<FakeTransport>();  // Client 即将独占的测试传输
        auto* fake = transport.get();                        // Client 生命周期内有效的测试观察指针
        fake->enqueue(response(rootResponse()));
        fake->enqueue(response(rootResponse()));
        RequestOptions defaults =  // 用于验证本次选项不修改默认配置的 Client 默认值
            singleAttemptOptions();
        defaults.timeoutSeconds = 7;
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), defaults);  // 独占注入依赖的普通 Client
        ChatResponse output;                               // 完整响应输出
        SdkError error;                                    // 流程故障输出
        QVERIFY(client.chat(request(), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("你好！"));
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(output.message.role, Role::Assistant);
        QCOMPARE(*output.usage.inputTokens, qint64(0));
        QVERIFY(!output.usage.outputTokens);
        QVERIFY(!output.usage.totalTokens);
        QCOMPARE(error.category, ErrorCategory::None);
        RequestOptions options = singleAttemptOptions();  // 覆盖单次调用的完整配置
        options.timeoutSeconds = 3;
        QVERIFY(client.chat(request(), output, error, options));
        QCOMPARE(fake->timeouts(), QList<int>({7, 3}));
        QCOMPARE(defaults.timeoutSeconds, 7);
        QCOMPARE(fake->requests().first().url.toString(),
                 QStringLiteral("https://example.com/gateway/v1/chat/completions"));
        const QJsonObject encoded =
            QJsonDocument::fromJson(
                fake->requests().first().body)  // 真实传输请求体，不依赖 Client 内部实现
                .object();
        QCOMPARE(encoded.value(QStringLiteral("n")).toInt(), 1);
        QVERIFY(!encoded.contains(QStringLiteral("temperature")));
        QVERIFY(!encoded.contains(QStringLiteral("max_completion_tokens")));
    }
    void reservedFields_data()  // 列出必须在发送前拒绝的标准字段及多候选入口
    {
        QTest::addColumn<QString>("field");
        for (const auto& field :
             QStringList{"model", "messages", "temperature", "tools", "tool_choice", "stream", "n",
                         "functions", "max_tokens"})  // 当前保留参数测试名称
            QTest::newRow(field.toLatin1().constData()) << field;
    }
    void reservedFields()  // 即使标准参数未设置也拒绝同名扩展字段，不调用 Transport
    {
        QFETCH(QString, field);                              // 当前待拒绝的标准字段
        auto transport = std::make_unique<FakeTransport>();  // 未提供响应，任何发送都将暴露检查过晚
        auto* fake = transport.get();                        // 检查实际发送次数的非拥有指针
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), singleAttemptOptions());  // 普通离线 Client
        auto input = request();                                          // 插入冲突扩展字段的本次请求
        input.extraParameters.insert(field, 1);
        ChatResponse output;  // 本次响应输出
        SdkError error;       // 配置错误输出
        QVERIFY(!client.chat(input, output, error));
        QCOMPARE(error.code, QStringLiteral("ReservedParameter"));
        QVERIFY(fake->requests().isEmpty());
    }
    void headersAndPaths()  // 验证根路径拼接、大小写认证覆盖和受保护 Header
    {
        OpenAIChatCompatibleAdapter adapter;  // 无共享请求状态的具体协议实现
        auto config = provider();             // 待切换地址和自定义 Header 的服务配置
        TransportRequest output;              // HTTP 编码输出
        SdkError error;                       // 发送前配置错误输出
        QVERIFY(adapter.encodeChatRequest(config, request(), output, error));
        QVERIFY(std::none_of(output.headers.begin(), output.headers.end(),
                             [](const auto& header) {  // 检查免密钥请求没有自动认证
                                 return header.first == "authorization";
                             }));
        config.baseUrl = QUrl(QStringLiteral("https://example.com/gateway/v1"));
        config.apiKey = QStringLiteral("default-secret");
        config.customHeaders.insert(QStringLiteral("Authorization"),
                                    QStringLiteral("Custom token"));
        config.customHeaders.insert(QStringLiteral("X-Gateway"), QStringLiteral("company"));
        QVERIFY(adapter.encodeChatRequest(config, request(), output, error));
        QCOMPARE(output.url.path(), QStringLiteral("/gateway/v1/chat/completions"));
        QVERIFY(output.headers.contains(
            qMakePair(QByteArray("authorization"), QByteArray("Custom token"))));
        config.customHeaders.insert(QStringLiteral("cOnTeNt-TyPe"), QStringLiteral("text/plain"));
        QVERIFY(!adapter.encodeChatRequest(config, request(), output, error));
        QCOMPARE(error.code, QStringLiteral("ProtectedHeader"));
        config.customHeaders.remove(QStringLiteral("cOnTeNt-TyPe"));
        config.customHeaders.insert(QStringLiteral("authorization"), QStringLiteral("duplicate"));
        QVERIFY(!adapter.encodeChatRequest(config, request(), output, error));
        QCOMPARE(error.code, QStringLiteral("DuplicateHeader"));
    }
    void invalidResponses_data()  // 准备零候选、多候选、非法消息及用量等响应
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("code");
        QTest::newRow("invalid-json") << QByteArray("{broken") << QStringLiteral("InvalidJson");
        auto root = rootResponse();  // 依次修改响应字段的样本对象
        root[QStringLiteral("choices")] = QJsonArray{};
        QTest::newRow("zero-choice")
            << QJsonDocument(root).toJson() << QStringLiteral("InvalidCandidateCount");
        root = rootResponse();
        const auto choices =  // 唯一候选列表的副本，用于构造重复候选
            root.value(QStringLiteral("choices")).toArray();
        root[QStringLiteral("choices")] = QJsonArray{choices.first(), choices.first()};
        QTest::newRow("two-choices")
            << QJsonDocument(root).toJson() << QStringLiteral("InvalidCandidateCount");
        root = rootResponse();
        root[QStringLiteral("usage")] = QJsonObject{{QStringLiteral("prompt_tokens"), -1}};
        QTest::newRow("negative-usage")
            << QJsonDocument(root).toJson() << QStringLiteral("InvalidUsage");
        root[QStringLiteral("usage")] = QJsonObject{{QStringLiteral("prompt_tokens"), 1.5}};
        QTest::newRow("fractional-usage")
            << QJsonDocument(root).toJson() << QStringLiteral("InvalidUsage");
        QTest::newRow("duplicate-call-id")
            << QJsonDocument(toolResponse(QJsonArray{tool("a"), tool("a")})).toJson()
            << QStringLiteral("InvalidToolCall");
        QTest::newRow("empty-call-id") << QJsonDocument(toolResponse(QJsonArray{tool("")})).toJson()
                                       << QStringLiteral("InvalidToolCall");
        QTest::newRow("partial-arguments")
            << QJsonDocument(toolResponse(QJsonArray{tool("a", "{\"device_id\":")})).toJson()
            << QStringLiteral("InvalidToolArguments");
        QTest::newRow("array-arguments")
            << QJsonDocument(toolResponse(QJsonArray{tool("a", "[]")})).toJson()
            << QStringLiteral("InvalidToolArguments");
    }
    void invalidResponses()  // 拒绝非法响应并保留诊断信息和响应不完整状态
    {
        QFETCH(QByteArray, body);             // 当前非法协议响应的原始字节
        QFETCH(QString, code);                // 期望的规范错误码
        OpenAIChatCompatibleAdapter adapter;  // 协议解析对象
        ChatResponse output;                  // 失败时也可保留有效响应数据
        SdkError error;                       // 协议错误详情
        QVERIFY(!adapter.decodeChatResponse(TransportResponse{200, {}, body}, output, error));
        QCOMPARE(error.code, code);
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QVERIFY(!error.providerError.isNull());
        QCOMPARE(*error.httpStatus, 200);
    }
    void
    protocolFailureKeepsParsedData()  // 工具 ID 非法时保留已解析文本、Usage 和 FinishReason，不报告成功
    {
        OpenAIChatCompatibleAdapter adapter;  // 响应协议解析器
        ChatResponse output;                  // 已经解析到的有效 canonical 数据
        SdkError error;                       // 工具关联错误的诊断
        QVERIFY(!adapter.decodeChatResponse(
            response(toolResponse(QJsonArray{tool("a"), tool("a")})), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("我查一下"));
        QCOMPARE(output.message.toolCalls().size(), 1);
        QCOMPARE(output.finishReason, FinishReason::ToolCalls);
        QVERIFY(output.usage.inputTokens.has_value());
        QCOMPARE(*output.usage.inputTokens, qint64(0));
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QCOMPARE(output.message.status, MessageStatus::Incomplete);
    }
    void networkFailureAndOutputReset()  // 传输故障不被伪装为协议错误，清理旧结果并返回原始诊断
    {
        auto transport = std::make_unique<FakeTransport>();  // 预设真实接口的网络连接故障
        auto* fake = transport.get();                        // 检查实际请求次数的观察指针
        SdkError fault;                                      // 要完整保留的传输错误
        fault.category = ErrorCategory::Network;
        fault.code = QStringLiteral("ConnectionReset");
        fault.message = QStringLiteral("connection reset");
        fake->enqueueError(fault);
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), singleAttemptOptions());  // 使用预设故障的 Client
        ChatResponse output;                                             // 故意预置旧成功数据以检测残留
        output.message = Message::user(QStringLiteral("old result"));
        output.completionState = CompletionState::Complete;
        SdkError error;  // 本次独立故障输出
        QVERIFY(!client.chat(request(), output, error));
        QCOMPARE(error.category, ErrorCategory::Network);
        QCOMPARE(error.code, fault.code);
        QVERIFY(output.message.contents.isEmpty());
        QCOMPARE(output.message.role, Role::Assistant);
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QCOMPARE(fake->requests().size(), 1);
        LLMClient missing(provider(), nullptr, nullptr);  // 检验无依赖的构造不会在调用时崩溃
        QVERIFY(!missing.chat(request(), output, error));
        QCOMPARE(error.code, QStringLiteral("MissingDependency"));
    }
    void providerErrors_data()  // 列出常见 HTTP 失败对应的 SDK 错误分类
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("category");
        QTest::newRow("400") << 400 << int(ErrorCategory::Provider);
        QTest::newRow("401") << 401 << int(ErrorCategory::Authentication);
        QTest::newRow("403") << 403 << int(ErrorCategory::Authentication);
        QTest::newRow("429") << 429 << int(ErrorCategory::RateLimited);
        QTest::newRow("503") << 503 << int(ErrorCategory::Provider);
    }
    void providerErrors()  // 服务端失败规范化，保留错误码与 Retry-After，不在阶段 2 重试
    {
        QFETCH(int, status);                                 // 当前服务端 HTTP 错误状态
        QFETCH(int, category);                               // 期望的 SDK 分类
        auto transport = std::make_unique<FakeTransport>();  // 本次离线 HTTP 失败模拟
        auto* fake = transport.get();                        // 用于确认没有隐式重复发送的观察指针
        auto http = response(                                // 原生服务端错误
            QJsonObject{{QStringLiteral("error"),
                         QJsonObject{{QStringLiteral("message"), QStringLiteral("服务暂不可用")},
                                     {QStringLiteral("code"), QStringLiteral("temporary")}}}},
            status);
        http.headers.append(qMakePair(QByteArray("rEtRy-AfTeR"), QByteArray("30")));
        fake->enqueue(http);
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), singleAttemptOptions());  // 完整 Client 错误链
        ChatResponse output;                                             // 失败结果输出
        SdkError error;                                                  // 服务端诊断输出
        QVERIFY(!client.chat(request(), output, error));
        QCOMPARE(int(error.category), category);
        QCOMPARE(*error.httpStatus, status);
        QCOMPARE(*error.retryAfterMs, qint64(30000));
        QCOMPARE(error.details.value(QStringLiteral("providerCode")).toString(),
                 QStringLiteral("temporary"));
        QCOMPARE(fake->requests().size(), 1);
    }
    void manualFunctionCallRoundTrip()  // 不依赖 Agent 的两次 Chat 验证函数调用和成功、失败结果编码
    {
        auto transport = std::make_unique<FakeTransport>();  // 预设两次模型响应的测试传输
        auto* fake = transport.get();                        // 请求编码的只读观察入口
        fake->enqueue(response(toolResponse(QJsonArray{tool("a"), tool("b")})));
        fake->enqueue(response(rootResponse()));
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), singleAttemptOptions());  // 不执行工具的模型 Client
        auto input = request();                                          // 首次请求的工具描述和用户消息
        FunctionToolDefinition definition;                               // 描述本地函数，不包含 Handler
        definition.name = QStringLiteral("get_temperature");
        definition.inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
        input.tools.append(definition);
        ChatResponse output;  // 每次模型响应输出
        SdkError error;       // 流程错误输出
        QVERIFY(client.chat(input, output, error));
        QCOMPARE(output.message.toolCalls().size(), 2);
        QCOMPARE(
            output.message.toolCalls().first().arguments.value(QStringLiteral("device_id")).toInt(),
            1001);
        input.messages.append(output.message);
        ToolResult success;  // 人工提供的成功工具业务结果
        success.callId = QStringLiteral("a");
        success.data = QJsonObject{{QStringLiteral("temperature"), 38.6}};
        ToolResult denied;  // 人工提供的工具拒绝结果，仍允许下一次模型请求
        denied.callId = QStringLiteral("b");
        denied.success = false;
        denied.errorCode = QStringLiteral("ApprovalDenied");
        denied.errorMessage = QStringLiteral("用户拒绝");
        input.messages.append(
            Message{Role::Tool, {ToolResultContent{success}}, MessageStatus::Complete});
        input.messages.append(
            Message{Role::Tool, {ToolResultContent{denied}}, MessageStatus::Complete});
        QVERIFY(client.chat(input, output, error));
        const auto messages =
            QJsonDocument::fromJson(fake->requests().last().body)  // 回传的完整消息序列
                .object()
                .value(QStringLiteral("messages"))
                .toArray();
        QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(2).toObject().value(QStringLiteral("tool_call_id")).toString(),
                 QStringLiteral("a"));
        QVERIFY(!messages.at(2)
                     .toObject()
                     .value(QStringLiteral("content"))
                     .toString()
                     .contains(QStringLiteral("success")));
        const auto failure =  // 无原生错误标记协议的失败 JSON fallback
            QJsonDocument::fromJson(
                messages.at(3).toObject().value(QStringLiteral("content")).toString().toUtf8())
                .object();
        QVERIFY(!failure.value(QStringLiteral("success")).toBool(true));
        QCOMPARE(failure.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("ApprovalDenied"));
        QVERIFY(!failure.contains(QStringLiteral("callId")));
    }
    void imageInputOrder()  // 验证 URL、内存、本地图片编码及 Text-Image-Text 原始顺序
    {
        const QByteArray png = QByteArray::fromBase64(  // 一像素 PNG 的测试字节
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6SAAAAABJRU5ErkJggg==");
        QTemporaryFile file;  // 仅由测试创建的临时本地图片
        QVERIFY(file.open());
        QCOMPARE(file.write(png), qint64(png.size()));
        file.flush();
        auto input = request();  // 顺序混合多种图片来源的输入消息
        input.messages[0].contents = {
            TextContent{"前"},
            ImageContent{MediaResource::fromUrl("https://example.com/image.png")},
            TextContent{"后"}, ImageContent{MediaResource::fromBytes(png, "image/png")},
            ImageContent{MediaResource::fromLocalFile(file.fileName())}};
        OpenAIChatCompatibleAdapter adapter;  // 普通 Chat 请求编码实现
        TransportRequest output;              // 编码后的原始 HTTP 请求
        SdkError error;                       // 编码错误输出
        QVERIFY(adapter.encodeChatRequest(provider(), input, output, error));
        const auto parts = QJsonDocument::fromJson(output.body)  // Provider 需要的有序内容数组
                               .object()
                               .value("messages")
                               .toArray()
                               .first()
                               .toObject()
                               .value("content")
                               .toArray();
        QCOMPARE(parts.size(), 5);
        QCOMPARE(parts.at(0).toObject().value("text").toString(), QStringLiteral("前"));
        QCOMPARE(parts.at(1).toObject().value("type").toString(), QStringLiteral("image_url"));
        QCOMPARE(parts.at(2).toObject().value("text").toString(), QStringLiteral("后"));
        QCOMPARE(parts.at(3).toObject().value("image_url").toObject().value("url").toString(),
                 parts.at(4).toObject().value("image_url").toObject().value("url").toString());
    }
    void invalidImagesAndFileReference()  // 拒绝空数据、伪造图片字节、来源冲突和未支持文件引用
    {
        OpenAIChatCompatibleAdapter adapter;  // 当前图片输入编码实现
        auto input = request();               // 依次替换非法图片来源的输入请求
        TransportRequest output;              // 编码失败时不得形成可发送请求
        SdkError error;                       // 参数或不支持错误输出
        input.messages[0].contents.append(
            ImageContent{MediaResource::fromBytes("not an image", "image/png")});
        QVERIFY(!adapter.encodeChatRequest(provider(), input, output, error));
        QCOMPARE(error.code, QStringLiteral("InvalidImage"));
        input.messages[0].contents.last() =
            ImageContent{MediaResource::fromFileReference("file_1")};
        QVERIFY(!adapter.encodeChatRequest(provider(), input, output, error));
        QCOMPARE(error.code, QStringLiteral("UnsupportedFeature"));
        auto resource =  // 含多个来源字段的非法资源
            MediaResource::fromUrl("https://example.com/a.png");
        resource.filePath = QStringLiteral("other.png");
        input.messages[0].contents.last() = ImageContent{resource};
        QVERIFY(!adapter.encodeChatRequest(provider(), input, output, error));
        QCOMPARE(error.code, QStringLiteral("InvalidMediaResource"));
    }
    void unsupportedAndCancellation()  // 验证未支持内容、配置错误及预取消均不发送 HTTP
    {
        auto transport =  // 用于检测错误请求是否进入网络的测试传输
            std::make_unique<FakeTransport>();
        auto* fake = transport.get();  // 已发送请求观察指针
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport), singleAttemptOptions());  // 离线 Client
        auto input = request();                                          // 依次设置未支持和非法参数的请求
        ChatResponse output;                                             // 调用输出，失败不得保留上一次假成功
        SdkError error;                                                  // 本次错误输出
        input.messages[0].contents.append(AudioContent{});
        QVERIFY(!client.chat(input, output, error));
        QCOMPARE(error.code, QStringLiteral("UnsupportedFeature"));
        CancellationSource source;  // 测试调用前主动取消的统一取消源
        source.cancel();
        RequestOptions options = singleAttemptOptions();  // 带已取消令牌的请求选项
        options.cancellation = source.token();
        QVERIFY(!client.chat(request(), output, error, options));
        QCOMPARE(error.category, ErrorCategory::Cancelled);
        options = {};
        options.timeoutSeconds = 0;
        QVERIFY(!client.chat(request(), output, error, options));
        QCOMPARE(error.category, ErrorCategory::Configuration);
        QVERIFY(fake->requests().isEmpty());
    }
    void lengthAndMissingUsage()  // 完整普通响应的 Length 不是 SdkError，缺失用量保持未知
    {
        auto root = rootResponse();                                        // 改为输出被模型长度限制截断的完整响应
        auto choice = root.value("choices").toArray().first().toObject();  // 唯一候选的可修改副本
        choice[QStringLiteral("finish_reason")] = QStringLiteral("length");
        root[QStringLiteral("choices")] = QJsonArray{choice};
        root.remove(QStringLiteral("usage"));
        OpenAIChatCompatibleAdapter adapter;  // 普通响应解析实现
        ChatResponse output;                  // 截断但协议完整的规范响应
        SdkError error;                       // 应保持为空的流程错误
        QVERIFY(adapter.decodeChatResponse(response(root), output, error));
        QCOMPARE(output.finishReason, FinishReason::Length);
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(output.message.status, MessageStatus::Incomplete);
        QVERIFY(!output.usage.inputTokens);
        QCOMPARE(error.category, ErrorCategory::None);
    }
    void factoryAndCustomInjection()  // Factory 只创建真实内置协议，自定义注入不依赖协议枚举
    {
        auto config = provider();           // 待切换协议的服务配置
        std::unique_ptr<LLMClient> client;  // 接收 Factory 转移的 Client 所有权
        SdkError error;                     // 工厂失败原因
        QVERIFY(LLMClientFactory::create(config, client, error));
        auto* previous = client.get();  // 失败前已有 Client，用于验证失败不覆盖输出
        config.protocol = ProtocolType::OpenAIResponses;
        QVERIFY(!LLMClientFactory::create(config, client, error));
        QCOMPARE(error.code, QStringLiteral("UnsupportedProtocol"));
        QCOMPARE(client.get(), previous);
        auto transport = std::make_unique<FakeTransport>();  // 注入真实接口实现，不经过内置 Factory
        transport->enqueue(response(rootResponse()));
        LLMClient injected(config, std::make_unique<OpenAIChatCompatibleAdapter>(),
                           std::move(transport),
                           singleAttemptOptions());  // 显式注入的 Adapter 优先，无需注册
        ChatResponse output;                         // 注入路径的正常响应
        QVERIFY(injected.chat(request(), output, error));
    }
    void concurrentClientCalls()  // 同一个 Client 的请求状态独立，测试线程由调用方创建
    {
        auto transport = std::make_unique<FakeTransport>();  // 线程安全的独立响应队列
        auto* fake = transport.get();                        // 多次调用记录的观察入口
        for (int i = 0; i < 8; ++i)                          // 预设响应的位置，每次调用消费一项
            fake->enqueue(response(rootResponse()));
        LLMClient client(provider(), std::make_unique<OpenAIChatCompatibleAdapter>(),
                         std::move(transport),
                         singleAttemptOptions());  // 多线程共享但配置只读的 Client
        std::atomic<int> completed{0};             // 已正确完成的独立模型调用数量
        std::vector<std::thread> threads;          // 仅由测试调用方管理的执行线程
        for (int i = 0; i < 8; ++i) {              // 当前启动的并发调用序号
            threads.emplace_back([&] {             // 并发同步调用，结果和错误均为线程局部值
                ChatResponse output;               // 本线程独立响应
                SdkError error;                    // 本线程独立错误
                if (client.chat(request(), output, error) &&
                    output.message.text() == QStringLiteral("你好！"))
                    ++completed;
            });
        }
        for (auto& thread : threads)
            thread.join();  // 当前需要等待结束的调用线程
        QCOMPARE(completed.load(), 8);
        QCOMPARE(fake->requests().size(), 8);
    }
};
QTEST_GUILESS_MAIN(ChatTest)
#include "tst_Chat.moc"
