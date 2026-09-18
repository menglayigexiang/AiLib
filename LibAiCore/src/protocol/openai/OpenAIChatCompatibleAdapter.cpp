#include "../RetryAfter.h"
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QSet>
#include <QMap>
#include <utility>
#include <QFile>
#include <QMimeDatabase>
#include <QDateTime>
#include <QLocale>
#include <cmath>
#include <limits>

namespace AiLib {
namespace {
bool fail(SdkError& error, ErrorCategory category, const QString& code, const QString& message)  // 按给定分类、错误码和说明生成流程失败
{
    error = {};
    error.category = category;
    error.code = code;
    error.message = message;
    return false;
}

QString jsonText(const QJsonValue& value)  // 将工具业务值编码为 JSON 文本，保留对象、数组和标量语义
{
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);  // 利用数组编码支持任意 JSON 标量
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

bool validHeaderName(const QByteArray& name)  // 校验 Header 名称是否由合法 HTTP token 字符组成
{
    if (name.isEmpty()) return false;
    for (const char ch : name) {  // 当前请求头名称字符
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
              || QByteArray("!#$%&'*+-.^_`|~").contains(ch))) return false;
    }
    return true;
}

bool imageUrl(const ImageContent& image, QString& output, SdkError& error)  // 根据图片资源来源生成协议 URL 或 Data URL，输出编码错误
{
    if (!validateMediaResource(image.resource, error)) return false;
    const auto& resource = image.resource;  // 图片的原始媒体资源，只读使用
    if (resource.storageType == StorageType::Url) {
        const QUrl url(resource.url);  // 已通过基本校验的图片 URL
        if (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            return fail(error, ErrorCategory::Unsupported, QStringLiteral("UnsupportedFeature"), QStringLiteral("Image URL must use HTTP or HTTPS"));
        output = resource.url;
        return true;
    }
    if (resource.storageType == StorageType::FileReference)
        return fail(error, ErrorCategory::Unsupported, QStringLiteral("UnsupportedFeature"), QStringLiteral("Chat image file references are not supported"));
    QByteArray bytes = resource.data;  // 直接字节或本地文件读取后的媒体数据
    if (resource.storageType == StorageType::LocalFile) {
        QFile file(resource.filePath);  // 当前线程读取的本地图片文件
        if (!file.open(QIODevice::ReadOnly))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("MediaReadFailed"), file.errorString());
        bytes = file.readAll();
        if (file.error() != QFileDevice::NoError)
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("MediaReadFailed"), file.errorString());
    }
    const QMimeDatabase database;                                                         // 使用 Qt Core 的 MIME 数据库识别常见媒体字节
    const QString detectedMime = database.mimeTypeForData(bytes).name();                  // 根据实际字节识别的媒体格式，不能仅相信声明的类型
    const QString mime = resource.mimeType.isEmpty() ? detectedMime : resource.mimeType;  // 优先使用明确声明的 MIME 类型
    if (bytes.isEmpty() || !detectedMime.startsWith(QStringLiteral("image/")) || !mime.startsWith(QStringLiteral("image/")) || mime.contains('\r') || mime.contains('\n') || mime.contains(';'))
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidImage"), QStringLiteral("Image bytes must be nonempty and have an image MIME type"));
    output = QStringLiteral("data:") + mime + QStringLiteral(";base64,") + QString::fromLatin1(bytes.toBase64());
    return true;
}

bool encodeMessage(const Message& message, QJsonObject& output, SdkError& error)  // 将有序 canonical 消息转换为 Chat 消息，拒绝无法表达的内容
{
    output = {};
    if (message.status != MessageStatus::Complete)
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("IncompleteMessage"), QStringLiteral("Incomplete messages must be handled by the caller before reuse"));
    switch (message.role) {
    case Role::System: output.insert(QStringLiteral("role"), QStringLiteral("system")); break;
    case Role::User: output.insert(QStringLiteral("role"), QStringLiteral("user")); break;
    case Role::Assistant: output.insert(QStringLiteral("role"), QStringLiteral("assistant")); break;
    case Role::Tool: output.insert(QStringLiteral("role"), QStringLiteral("tool")); break;
    default: return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidRole"), QStringLiteral("Unknown message role"));
    }
    if (message.role == Role::Tool) {
        if (message.contents.size() != 1 || !std::holds_alternative<ToolResultContent>(message.contents.first()))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolMessage"), QStringLiteral("Tool message must contain exactly one ToolResult"));
        const auto& result = std::get<ToolResultContent>(message.contents.first()).result;  // 已关联调用的工具业务结果
        if (result.callId.isEmpty())
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolCallId"), QStringLiteral("Tool result requires a call ID"));
        output.insert(QStringLiteral("tool_call_id"), result.callId);
        output.insert(QStringLiteral("content"), result.success ? jsonText(result.data) : jsonText(QJsonObject{
            {QStringLiteral("success"), false}, {QStringLiteral("errorCode"), result.errorCode}, {QStringLiteral("errorMessage"), result.errorMessage}}));
        return true;
    }
    QJsonArray content;                                            // 按原消息顺序编码的文本及图片内容块
    QJsonArray calls;                                              // 当前助手消息中的函数调用列表
    QSet<QString> ids;                                             // 当前消息中已出现的函数调用标识
    bool hasImage = false;                                         // 是否需要保留协议的数组内容形式
    for (const auto& part : message.contents) {                    // 当前有序 canonical 内容块
        if (const auto* text = std::get_if<TextContent>(&part)) {  // 当前普通文本内容视图
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text->text}});
        } else if (const auto* image = std::get_if<ImageContent>(&part)) {  // 当前图片输入内容视图
            if (message.role != Role::User)
                return fail(error, ErrorCategory::Unsupported, QStringLiteral("UnsupportedFeature"), QStringLiteral("Image input is supported only in user messages"));
            QString url;  // 图片资源编码后的 URL 或 Data URL
            if (!imageUrl(*image, url, error)) return false;
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}}});
            hasImage = true;
        } else if (const auto* tool = std::get_if<ToolCallContent>(&part)) {  // 当前完整工具调用内容视图
            if (message.role != Role::Assistant || tool->call.id.isEmpty() || tool->call.name.isEmpty() || ids.contains(tool->call.id))
                return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolCall"), QStringLiteral("Assistant ToolCall requires a name and a unique nonempty ID"));
            ids.insert(tool->call.id);
            calls.append(QJsonObject{{QStringLiteral("id"), tool->call.id}, {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), tool->call.name}, {QStringLiteral("arguments"), jsonText(tool->call.arguments)}}}});
        } else {
            return fail(error, ErrorCategory::Unsupported, QStringLiteral("UnsupportedFeature"), QStringLiteral("This content type cannot be encoded by the current Chat Adapter"));
        }
    }
    if (hasImage) output.insert(QStringLiteral("content"), content);
    else if (!content.isEmpty()) output.insert(QStringLiteral("content"), message.text());
    else output.insert(QStringLiteral("content"), calls.isEmpty() ? QJsonValue(QString()) : QJsonValue(QJsonValue::Null));
    if (!calls.isEmpty()) output.insert(QStringLiteral("tool_calls"), calls);
    return true;
}

std::optional<qint64> retryAfter(const TransportResponse& response)  // 通过公共 HTTP 辅助逻辑解析等待建议
{
    return ProtocolDetails::retryAfter(response);
}

bool parseUsage(const QJsonValue& input, Usage& output, SdkError& error)  // 严格读取服务端用量，缺失或 null 保持未知，不自行推算
{
    if (input.isUndefined() || input.isNull()) return true;
    if (!input.isObject())
        return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidUsage"), QStringLiteral("Usage must be an object"));
    const QJsonObject object = input.toObject();                                // 服务端原始 Token 用量对象
    const auto read = [&](const QString& key, std::optional<qint64>& target) {  // 根据字段名读取非负整数用量，不接受字符串或小数
        const QJsonValue value = object.value(key);                             // 当前待规范化的 Token 计数字段
        if (value.isUndefined() || value.isNull()) return true;
        const double number = value.toDouble(-1);  // 使用负值排除非数字和负计数
        if (!value.isDouble() || !std::isfinite(number) || number < 0 || std::floor(number) != number || number >= 9223372036854775808.0)
            return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidUsage"), QStringLiteral("Token counts must be nonnegative integers"));
        target = static_cast<qint64>(number);
        return true;
    };
    return read(QStringLiteral("prompt_tokens"), output.inputTokens)
        && read(QStringLiteral("completion_tokens"), output.outputTokens)
        && read(QStringLiteral("total_tokens"), output.totalTokens);
}
}  // 内部协议辅助函数命名空间结束

bool OpenAIChatCompatibleAdapter::encodeChatRequest(
    const ProviderConfig& provider,  // 当前 Client 的只读服务地址和认证配置
    const ChatRequest& request,      // 待转换的 canonical 请求
    TransportRequest& output,        // 全部校验成功后输出原始 HTTP 请求
    SdkError& error) const           // 发送前阻止协议不支持及保留字段冲突
{
    output = {};
    error = {};
    const QSet<QString> reserved{QStringLiteral("model"), QStringLiteral("messages"), QStringLiteral("temperature"), // 当前 Adapter 管理的标准字段
        QStringLiteral("top_p"), QStringLiteral("max_tokens"), QStringLiteral("max_completion_tokens"), QStringLiteral("tools"),
        QStringLiteral("tool_choice"), QStringLiteral("stream"), QStringLiteral("stream_options"), QStringLiteral("n"),
        QStringLiteral("functions"), QStringLiteral("function_call"), QStringLiteral("parallel_tool_calls")};
    for (auto it = request.extraParameters.constBegin(); it != request.extraParameters.constEnd(); ++it) { // 当前厂商扩展参数
        if (reserved.contains(it.key()))
            return fail(error, ErrorCategory::Configuration, QStringLiteral("ReservedParameter"), QStringLiteral("Reserved parameter: ") + it.key());
    }
    if (request.model.trimmed().isEmpty() || request.messages.isEmpty())
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidChatRequest"), QStringLiteral("Model and messages are required"));
    if ((request.temperature && (!std::isfinite(*request.temperature) || *request.temperature < 0 || *request.temperature > 2))
        || (request.topP && (!std::isfinite(*request.topP) || *request.topP < 0 || *request.topP > 1))
        || (request.maxOutputTokens && *request.maxOutputTokens <= 0))
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidModelParameter"), QStringLiteral("Invalid sampling or output-token limit"));

    TransportRequest encoded;  // 全部编码成功后才交给调用方的局部 HTTP 请求
    encoded.url = provider.baseUrl;
    if (!encoded.url.isValid() || encoded.url.host().isEmpty()
        || (encoded.url.scheme() != QStringLiteral("http") && encoded.url.scheme() != QStringLiteral("https"))
        || encoded.url.hasQuery() || encoded.url.hasFragment() || !encoded.url.userInfo().isEmpty())
        return fail(error, ErrorCategory::Configuration, QStringLiteral("InvalidBaseUrl"), QStringLiteral("API root must be an HTTP(S) URL without query, fragment or user information"));
    QString path = encoded.url.path();  // 保留网关和 API 根路径的端点拼接基础
    while (path.endsWith('/')) path.chop(1);
    encoded.url.setPath(path + QStringLiteral("/chat/completions"));
    QMap<QByteArray, QByteArray> headers;  // 不区分大小写的规范请求头，避免重复认证字段
    headers.insert("content-type", "application/json");
    headers.insert("accept", request.stream ? "text/event-stream" : "application/json");
    if (!provider.apiKey.isEmpty()) headers.insert("authorization", "Bearer " + provider.apiKey.toUtf8());
    for (auto it = provider.customHeaders.constBegin(); it != provider.customHeaders.constEnd(); ++it) {  // 当前应用自定义请求头
        const QByteArray name = it.key().toLatin1().toLower();                                            // 用小写 ASCII 标识进行冲突和覆盖判断
        const QByteArray value = it.value().toUtf8();                                                     // 自定义 Header 的实际传输值
        if (QString::fromLatin1(it.key().toLatin1()) != it.key() || !validHeaderName(name) || value.contains('\r') || value.contains('\n') || value.contains('\0'))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidHeader"), QStringLiteral("Invalid custom Header"));
        if (name == "content-type" || name == "accept" || name == "host" || name == "content-length" || name == "transfer-encoding")
            return fail(error, ErrorCategory::Configuration, QStringLiteral("ProtectedHeader"), QStringLiteral("Protected Header: ") + it.key());
        if (provider.customHeaders.size() > 1) {
            for (auto other = provider.customHeaders.constBegin(); other != it; ++other) {  // 已检查的自定义 Header，排除大小写不同的重复名称
                if (other.key().compare(it.key(), Qt::CaseInsensitive) == 0)
                    return fail(error, ErrorCategory::Configuration, QStringLiteral("DuplicateHeader"), QStringLiteral("Duplicate case-insensitive Header"));
            }
        }
        headers.insert(name, value);
    }
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)  // 当前最终 HTTP 请求头
        encoded.headers.append(qMakePair(it.key(), it.value()));
    if (headers.value("authorization").contains('\r') || headers.value("authorization").contains('\n') || headers.value("authorization").contains('\0'))
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidHeader"), QStringLiteral("Invalid authentication Header"));

    QJsonObject body = request.extraParameters;  // 扩展字段与 SDK 生成字段，保留字段已先验证
    body.insert(QStringLiteral("model"), request.model);
    body.insert(QStringLiteral("stream"), request.stream);
    if (request.stream) body.insert(QStringLiteral("stream_options"), QJsonObject{{QStringLiteral("include_usage"), true}});
    body.insert(QStringLiteral("n"), 1);
    if (request.temperature) body.insert(QStringLiteral("temperature"), *request.temperature);
    if (request.topP) body.insert(QStringLiteral("top_p"), *request.topP);
    if (request.maxOutputTokens) body.insert(QStringLiteral("max_completion_tokens"), *request.maxOutputTokens);
    QJsonArray messages;                            // 依输入顺序编码的消息列表
    for (const auto& message : request.messages) {  // 当前调用方历史消息，不裁剪或总结
        QJsonObject object;                         // 当前消息的厂商格式
        if (!encodeMessage(message, object, error)) return false;
        messages.append(object);
    }
    body.insert(QStringLiteral("messages"), messages);
    QJsonArray tools;                         // 本次发送的本地 Function Tool 描述，不含 Handler
    QSet<QString> names;                      // 检查当前工具定义名称是否重复
    for (const auto& tool : request.tools) {  // 当前函数工具定义
        if (tool.name.trimmed().isEmpty() || names.contains(tool.name))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolDefinition"), QStringLiteral("Tool names must be nonempty and unique"));
        names.insert(tool.name);
        tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("function"),
            QJsonObject{{QStringLiteral("name"), tool.name}, {QStringLiteral("description"), tool.description}, {QStringLiteral("parameters"), tool.inputSchema}}}});
    }
    if (!tools.isEmpty()) body.insert(QStringLiteral("tools"), tools);
    if (tools.isEmpty() && request.toolChoice.mode != ToolChoiceMode::Auto && request.toolChoice.mode != ToolChoiceMode::None)
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolChoice"), QStringLiteral("Required or specific tool choice needs tools"));
    if (!tools.isEmpty() || request.toolChoice.mode != ToolChoiceMode::Auto) {
        switch (request.toolChoice.mode) {
        case ToolChoiceMode::Auto: body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto")); break;
        case ToolChoiceMode::None: body.insert(QStringLiteral("tool_choice"), QStringLiteral("none")); break;
        case ToolChoiceMode::Required: body.insert(QStringLiteral("tool_choice"), QStringLiteral("required")); break;
        case ToolChoiceMode::Specific:
            if (!names.contains(request.toolChoice.toolName))
                return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolChoice"), QStringLiteral("Specific tool must be defined"));
            body.insert(QStringLiteral("tool_choice"), QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), request.toolChoice.toolName}}}});
            break;
        default: return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolChoice"), QStringLiteral("Unknown tool-choice mode"));
        }
    }
    encoded.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    output = std::move(encoded);
    return true;
}

bool OpenAIChatCompatibleAdapter::decodeChatResponse(
    const TransportResponse& input,  // 已收到的 HTTP 响应，包括服务端失败响应
    ChatResponse& output,            // canonical 结果，失败保留已解析的有效数据
    SdkError& error) const           // 严格验证单候选和 ToolCall 关联，保留原始诊断
{
    output = {};
    output.message.role = Role::Assistant;
    output.message.status = MessageStatus::Incomplete;
    error = {};
    QJsonParseError parseError;                                                       // 原始响应体的 JSON 解析错误及字节位置
    const QJsonDocument document = QJsonDocument::fromJson(input.body, &parseError);  // 请求级 JSON 响应树
    const QJsonObject root = document.object();                                       // 当前服务端响应的根对象
    const auto invalid = [&](const QString& code, const QString& message) {           // 统一保留响应原文和解析位置的协议失败边界
        fail(error, ErrorCategory::InvalidResponse, code, message);
        error.httpStatus = input.statusCode;
        error.providerError = document.isObject() ? QJsonValue(root) : QJsonValue(QString::fromUtf8(input.body));
        if (parseError.error != QJsonParseError::NoError) error.details.insert(QStringLiteral("jsonOffset"), parseError.offset);
        return false;
    };
    if (!input.statusCode) return invalid(QStringLiteral("MissingHttpStatus"), QStringLiteral("HTTP status is missing"));
    if (*input.statusCode < 200 || *input.statusCode >= 300 || (root.contains(QStringLiteral("error")) && !root.value(QStringLiteral("error")).isNull())) {
        error.category = *input.statusCode == 401 || *input.statusCode == 403 ? ErrorCategory::Authentication
            : *input.statusCode == 429 ? ErrorCategory::RateLimited : ErrorCategory::Provider;
        error.code = *input.statusCode == 429 ? QStringLiteral("RateLimited") : QStringLiteral("ProviderError");
        error.message = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("Provider rejected the request"));
        error.httpStatus = input.statusCode;
        error.providerError = root.contains(QStringLiteral("error")) ? root.value(QStringLiteral("error"))
            : document.isObject() ? QJsonValue(root) : QJsonValue(QString::fromUtf8(input.body));
        error.retryAfterMs = retryAfter(input);
        const QString code = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(); // 服务端原始错误码，只保存诊断不代替统一分类
        if (!code.isEmpty()) error.details.insert(QStringLiteral("providerCode"), code);
        return false;
    }
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalid(QStringLiteral("InvalidJson"), QStringLiteral("Provider response is not a JSON object"));
    if (!root.value(QStringLiteral("choices")).isArray() || root.value(QStringLiteral("choices")).toArray().size() != 1)
        return invalid(QStringLiteral("InvalidCandidateCount"), QStringLiteral("Exactly one candidate is required"));
    const QJsonValue choiceValue = root.value(QStringLiteral("choices")).toArray().first();  // 唯一候选的原始数据
    if (!choiceValue.isObject()) return invalid(QStringLiteral("InvalidChoice"), QStringLiteral("Candidate must be an object"));
    const QJsonObject choice = choiceValue.toObject();                        // 唯一候选的厂商对象
    const QJsonValue messageValue = choice.value(QStringLiteral("message"));  // 候选中的完整助手消息
    if (!messageValue.isObject() || messageValue.toObject().value(QStringLiteral("role")).toString() != QStringLiteral("assistant"))
        return invalid(QStringLiteral("InvalidAssistantMessage"), QStringLiteral("Candidate must contain an Assistant message"));
    output.id = root.value(QStringLiteral("id")).toString();
    output.model = root.value(QStringLiteral("model")).toString();
    if (!parseUsage(root.value(QStringLiteral("usage")), output.usage, error)) {
        error.httpStatus = input.statusCode;
        error.providerError = root;
        return false;
    }
    const QJsonValue reasonValue = choice.value(QStringLiteral("finish_reason"));  // 模型报告的停止原因，不代替协议完整性
    if (!reasonValue.isString() || reasonValue.toString().isEmpty())
        return invalid(QStringLiteral("MissingFinishReason"), QStringLiteral("Completed response requires a finish reason"));
    const QString reason = reasonValue.toString();  // 本协议的原始结束原因
    output.finishReason = reason == QStringLiteral("stop") ? FinishReason::Stop
        : reason == QStringLiteral("length") ? FinishReason::Length
        : reason == QStringLiteral("tool_calls") ? FinishReason::ToolCalls
        : reason == QStringLiteral("content_filter") ? FinishReason::ContentFilter : FinishReason::Other;
    const QJsonObject message = messageValue.toObject();  // 待规范化的完整助手响应
    if ((!message.value(QStringLiteral("audio")).isUndefined() && !message.value(QStringLiteral("audio")).isNull())
        || (!message.value(QStringLiteral("function_call")).isUndefined() && !message.value(QStringLiteral("function_call")).isNull())) {
        invalid(QStringLiteral("UnsupportedFeature"), QStringLiteral("Audio or legacy function output is not supported"));
        error.category = ErrorCategory::Unsupported;
        return false;
    }
    const QJsonValue reasoning = message.value(QStringLiteral("reasoning_content"));  // 兼容服务明确返回的推理文本，不自行推断或生成
    if (!reasoning.isUndefined() && !reasoning.isNull()) {
        if (!reasoning.isString()) return invalid(QStringLiteral("InvalidReasoning"), QStringLiteral("Reasoning must be a string"));
        if (!reasoning.toString().isEmpty()) output.message.contents.append(ReasoningContent{reasoning.toString()});
    }
    const QJsonValue content = message.value(QStringLiteral("content"));  // 普通文本内容，工具调用消息可为 null
    if (!content.isUndefined() && !content.isNull()) {
        if (!content.isString()) return invalid(QStringLiteral("UnsupportedResponseContent"), QStringLiteral("This phase supports text response content only"));
        if (!content.toString().isEmpty()) output.message.contents.append(TextContent{content.toString()});
    }
    const QJsonValue calls = message.value(QStringLiteral("tool_calls"));  // 原生函数调用列表，可不存在或为 null
    if (!calls.isUndefined() && !calls.isNull()) {
        if (!calls.isArray()) return invalid(QStringLiteral("InvalidToolCalls"), QStringLiteral("Tool calls must be an array"));
        QSet<QString> ids;                           // 当前助手响应已解析的调用 ID 集合
        for (const auto& value : calls.toArray()) {  // 当前待解析的原生 Function Call
            if (!value.isObject()) return invalid(QStringLiteral("InvalidToolCall"), QStringLiteral("Tool call must be an object"));
            const QJsonObject object = value.toObject();                                       // 当前工具调用原始对象
            const QJsonObject function = object.value(QStringLiteral("function")).toObject();  // 当前函数名称和参数字段
            ToolCall call;                                                                     // 完整解析后才加入 canonical 消息的调用
            call.id = object.value(QStringLiteral("id")).toString();
            call.name = function.value(QStringLiteral("name")).toString();
            if (object.value(QStringLiteral("type")).toString() != QStringLiteral("function")) {
                invalid(QStringLiteral("UnsupportedToolType"), QStringLiteral("Only client-executed Function Calls are supported"));
                error.category = ErrorCategory::Unsupported;
                return false;
            }
            if (call.id.trimmed().isEmpty() || call.name.trimmed().isEmpty() || ids.contains(call.id))
                return invalid(QStringLiteral("InvalidToolCall"), QStringLiteral("Function call requires a unique nonempty ID and a name"));
            const QJsonValue arguments = function.value(QStringLiteral("arguments"));  // 必须是完整 JSON 对象的字符串参数
            if (!arguments.isString()) return invalid(QStringLiteral("InvalidToolArguments"), QStringLiteral("Arguments must be a JSON string"));
            QJsonParseError argumentError;  // 当前工具参数解析错误
            const QJsonDocument argumentsDocument = QJsonDocument::fromJson(arguments.toString().toUtf8(), &argumentError); // 完整工具参数的 JSON 树
            if (argumentError.error != QJsonParseError::NoError || !argumentsDocument.isObject())
                return invalid(QStringLiteral("InvalidToolArguments"), QStringLiteral("Arguments must encode a complete JSON object"));
            call.arguments = argumentsDocument.object();
            ids.insert(call.id);
            output.message.contents.append(ToolCallContent{call});
        }
    }
    if (output.finishReason == FinishReason::ToolCalls && output.message.toolCalls().isEmpty())
        return invalid(QStringLiteral("MissingToolCalls"), QStringLiteral("Tool-call finish reason requires ToolCalls"));
    output.completionState = CompletionState::Complete;
    output.message.status = output.finishReason == FinishReason::Length ? MessageStatus::Incomplete : MessageStatus::Complete;
    return true;
}
}  // AiLib 命名空间结束
