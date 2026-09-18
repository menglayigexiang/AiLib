#include "../RetryAfter.h"
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QMap>
#include <QFile>
#include <QMimeDatabase>
#include <QDateTime>
#include <cmath>
#include <limits>
#include <utility>

namespace AiLib {
namespace {
bool fail(SdkError& error, ErrorCategory category, const char* code, const QString& message)  // 设置给定分类、稳定错误码及说明
{
    error = {};
    error.category = category;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return false;
}
QString jsonText(const QJsonValue& value)  // 将任意工具业务 JSON 值编码为文本
{
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);  // 数组包装使标量也能编码
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}
bool imageSource(const ImageContent& image, QJsonObject& source, SdkError& error)  // 将合法图片资源转换为 URL 或 Base64 来源
{
    if (!validateMediaResource(image.resource, error)) return false;
    const auto& resource = image.resource;  // 待编码的只读媒体资源
    if (resource.storageType == StorageType::Url) {
        const QUrl url(resource.url);  // 图片 URL 的协议校验视图
        if (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))
            return fail(error, ErrorCategory::InvalidArgument, "InvalidImage", QStringLiteral("Image URL must use HTTP or HTTPS"));
        source = {{"type", "url"}, {"url", resource.url}};
        return true;
    }
    if (resource.storageType == StorageType::FileReference)
        return fail(error, ErrorCategory::Unsupported, "UnsupportedFeature", QStringLiteral("Image file references are not supported"));
    QByteArray bytes = resource.data;  // 原始图片字节或本地文件读取结果
    if (resource.storageType == StorageType::LocalFile) {
        QFile file(resource.filePath);  // 同步读取的本地图片文件
        if (!file.open(QIODevice::ReadOnly)) return fail(error, ErrorCategory::InvalidArgument, "MediaReadFailed", file.errorString());
        bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) return fail(error, ErrorCategory::InvalidArgument, "MediaReadFailed", file.errorString());
    }
    const QString detected = QMimeDatabase().mimeTypeForData(bytes).name();               // 根据实际字节识别的图片格式
    const QString mime = resource.mimeType.isEmpty() ? detected : resource.mimeType;      // 声明或探测的媒体类型
    const QSet<QString> supported{"image/jpeg", "image/png", "image/gif", "image/webp"};  // Messages 支持的图片媒体类型
    if (bytes.isEmpty() || !supported.contains(mime) || detected != mime)
        return fail(error, ErrorCategory::InvalidArgument, "InvalidImage", QStringLiteral("Image bytes and supported MIME type must match"));
    source = {{"type", "base64"}, {"media_type", mime}, {"data", QString::fromLatin1(bytes.toBase64())}};
    return true;
}
bool blocks(const Message& message, QJsonArray& output, SdkError& error)  // 按 canonical 顺序编码合法角色的内容块
{
    if (message.status != MessageStatus::Complete)
        return fail(error, ErrorCategory::InvalidArgument, "IncompleteMessage", QStringLiteral("Caller must handle incomplete history before reuse"));
    QSet<QString> ids;                                             // 当前助手消息内的调用 ID 集合
    for (const auto& part : message.contents) {                    // 当前有序内容块
        if (const auto* text = std::get_if<TextContent>(&part)) {  // 当前普通文本块
            if (message.role == Role::Tool) return fail(error, ErrorCategory::InvalidArgument, "InvalidToolMessage", QStringLiteral("Tool messages require ToolResult content"));
            output.append(QJsonObject{{"type", "text"}, {"text", text->text}});
        } else if (const auto* image = std::get_if<ImageContent>(&part)) {  // 当前输入图片块
            if (message.role != Role::User) return fail(error, ErrorCategory::Unsupported, "UnsupportedFeature", QStringLiteral("Images are only supported in user messages"));
            QJsonObject source;  // 图片的协议来源描述
            if (!imageSource(*image, source, error)) return false;
            output.append(QJsonObject{{"type", "image"}, {"source", source}});
        } else if (const auto* call = std::get_if<ToolCallContent>(&part)) {  // 当前助手函数调用
            if (message.role != Role::Assistant || call->call.id.trimmed().isEmpty() || ids.contains(call->call.id) || call->call.name.trimmed().isEmpty())
                return fail(error, ErrorCategory::InvalidArgument, "InvalidToolCall", QStringLiteral("Assistant tool calls need nonempty unique IDs and names"));
            ids.insert(call->call.id);
            output.append(QJsonObject{{"type", "tool_use"}, {"id", call->call.id}, {"name", call->call.name}, {"input", call->call.arguments}});
        } else if (const auto* result = std::get_if<ToolResultContent>(&part)) {  // 当前工具业务结果
            if (message.role != Role::Tool || result->result.callId.trimmed().isEmpty())
                return fail(error, ErrorCategory::InvalidArgument, "InvalidToolMessage", QStringLiteral("Tool results need Tool role and call ID"));
            const auto& value = result->result;  // 不包含 SDK 流程错误的工具结果
            const QJsonValue data = value.success ? value.data : QJsonValue(QJsonObject{{"success", false}, {"errorCode", value.errorCode}, {"errorMessage", value.errorMessage}});  // 成功业务值或失败 fallback 语义
            output.append(QJsonObject{{"type", "tool_result"}, {"tool_use_id", value.callId}, {"is_error", !value.success}, {"content", jsonText(data)}});
        } else {
            return fail(error, ErrorCategory::Unsupported, "UnsupportedFeature", QStringLiteral("Content cannot be encoded by this Messages adapter; reasoning signatures are not modeled"));
        }
    }
    if (output.isEmpty()) return fail(error, ErrorCategory::InvalidArgument, "EmptyMessage", QStringLiteral("Message must contain content"));
    return true;
}
}  // 内部工具函数命名空间结束

bool AnthropicMessagesAdapter::encodeChatRequest(
    const ProviderConfig& provider,  // 当前服务配置
    const ChatRequest& request,      // canonical 输入消息及生成选项
    TransportRequest& output,        // 成功时替换的 HTTP 请求
    SdkError& error) const           // 同步校验并编码非流式请求
{
    error = {};
    const QSet<QString> reserved{"model", "messages", "system", "max_tokens", "temperature", "top_p", "tools", "tool_choice", "stream", "n", "choices"};  // SDK 控制字段及多候选绕过字段
    for (auto it = request.extraParameters.constBegin(); it != request.extraParameters.constEnd(); ++it) {  // 当前扩展参数
        if (reserved.contains(it.key())) return fail(error, ErrorCategory::Configuration, "ReservedParameter", QStringLiteral("Reserved parameter: ") + it.key());
    }
    if (request.model.trimmed().isEmpty() || request.messages.isEmpty()) return fail(error, ErrorCategory::InvalidArgument, "InvalidRequest", QStringLiteral("Model and messages are required"));
    if ((request.maxOutputTokens && *request.maxOutputTokens < 1)
        || (request.temperature && (!std::isfinite(*request.temperature) || *request.temperature < 0 || *request.temperature > 1))
        || (request.topP && (!std::isfinite(*request.topP) || *request.topP < 0 || *request.topP > 1)))
        return fail(error, ErrorCategory::InvalidArgument, "InvalidRequest", QStringLiteral("Invalid token limit or sampling parameter"));
    TransportRequest encoded;  // 请求局部值，失败不替换输出
    encoded.url = provider.baseUrl;
    if (!encoded.url.isValid() || encoded.url.host().isEmpty() || (encoded.url.scheme() != "https" && encoded.url.scheme() != "http")
        || encoded.url.hasQuery() || encoded.url.hasFragment() || !encoded.url.userInfo().isEmpty())
        return fail(error, ErrorCategory::Configuration, "InvalidBaseUrl", QStringLiteral("API root must be an HTTP(S) URL without query, fragment or credentials"));
    QString path = encoded.url.path();  // 保留网关前缀的 API 根路径
    while (path.endsWith('/')) path.chop(1);
    encoded.url.setPath(path + QStringLiteral("/v1/messages"));
    QMap<QByteArray, QByteArray> headers{{"content-type", "application/json"}, {"accept", "application/json"}, {"anthropic-version", "2023-06-01"}};  // 协议保护头及默认认证
    if (request.stream) headers.insert("accept", "text/event-stream");
    if (!provider.apiKey.isEmpty()) headers.insert("x-api-key", provider.apiKey.toUtf8());
    QSet<QByteArray> seen;                                                                                // 忽略大小写后已经设置的自定义 Header 名称
    for (auto it = provider.customHeaders.constBegin(); it != provider.customHeaders.constEnd(); ++it) {  // 当前自定义 Header
        const QByteArray name = it.key().toLatin1().toLower();                                            // 规范化的 HTTP token 名称
        const QByteArray value = it.value().toUtf8();                                                     // 实际传输的自定义值
        if (name.isEmpty() || QString::fromLatin1(it.key().toLatin1()) != it.key()) return fail(error, ErrorCategory::InvalidArgument, "InvalidHeader", QStringLiteral("Invalid header name"));
        for (const char ch : name) {  // 当前 Header 名称字符
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || QByteArray("!#$%&'*+-.^_`|~").contains(ch))) return fail(error, ErrorCategory::InvalidArgument, "InvalidHeader", QStringLiteral("Invalid header name"));
        }
        if (seen.contains(name)) return fail(error, ErrorCategory::Configuration, "DuplicateHeader", QStringLiteral("Duplicate case-insensitive header"));
        seen.insert(name);
        if (name == "content-type" || name == "accept" || name == "anthropic-version" || name == "host" || name == "content-length" || name == "transfer-encoding")
            return fail(error, ErrorCategory::Configuration, "ProtectedHeader", QStringLiteral("Protected header: ") + it.key());
        headers.insert(name, value);
    }
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {  // 最终协议及认证 Header
        if (it.value().contains('\r') || it.value().contains('\n') || it.value().contains('\0')) return fail(error, ErrorCategory::InvalidArgument, "InvalidHeader", QStringLiteral("Invalid header value"));
        encoded.headers.append(qMakePair(it.key(), it.value()));
    }
    QJsonObject body = request.extraParameters;  // 经保留字段校验后的请求体
    body.insert("model", request.model);
    body.insert("max_tokens", request.maxOutputTokens.value_or(1024));
    body.insert("stream", request.stream);
    if (request.temperature) body.insert("temperature", *request.temperature);
    if (request.topP) body.insert("top_p", *request.topP);
    QJsonArray messages;                            // 普通消息及转换为 user 的工具结果
    QJsonArray system;                              // 顶层 system 文本块
    bool sawConversation = false;                   // 防止静默移动对话中途的 system 指令
    for (const auto& message : request.messages) {  // 当前历史消息
        QJsonArray content;                         // 当前消息的有序协议内容
        if (!blocks(message, content, error)) return false;
        if (message.role == Role::System) {
            if (sawConversation) return fail(error, ErrorCategory::Unsupported, "UnsupportedFeature", QStringLiteral("Messages only supports leading system messages"));
            for (const auto& block : content) system.append(block);  // 当前顶层系统文本块
            continue;
        }
        if (message.role != Role::User && message.role != Role::Assistant && message.role != Role::Tool)
            return fail(error, ErrorCategory::InvalidArgument, "InvalidRole", QStringLiteral("Unknown message role"));
        sawConversation = true;
        const QString role = message.role == Role::Assistant ? QStringLiteral("assistant") : QStringLiteral("user");  // 工具结果是 user 内容块
        if (!messages.isEmpty() && messages.last().toObject().value("role").toString() == role) {
            QJsonObject previous = messages.last().toObject();          // 相邻同角色消息合并以保留整批工具结果
            QJsonArray combined = previous.value("content").toArray();  // 已有的有序内容
            for (const auto& block : content) combined.append(block);   // 当前追加的内容块
            previous.insert("content", combined);
            messages.replace(messages.size() - 1, previous);
        } else messages.append(QJsonObject{{"role", role}, {"content", content}});
    }
    if (messages.isEmpty()) return fail(error, ErrorCategory::InvalidArgument, "InvalidRequest", QStringLiteral("At least one conversation message is required"));
    body.insert("messages", messages);
    if (!system.isEmpty()) body.insert("system", system);
    QJsonArray tools;                         // Function Tool 的纯描述列表
    QSet<QString> names;                      // 当前工具名称集合
    for (const auto& tool : request.tools) {  // 当前工具定义
        if (tool.name.trimmed().isEmpty() || names.contains(tool.name)) return fail(error, ErrorCategory::InvalidArgument, "InvalidToolDefinition", QStringLiteral("Tool names must be nonempty and unique"));
        names.insert(tool.name);
        tools.append(QJsonObject{{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.inputSchema}});
    }
    if (!tools.isEmpty()) body.insert("tools", tools);
    QJsonObject choice;  // Messages 的工具选择对象
    switch (request.toolChoice.mode) {
    case ToolChoiceMode::Auto: choice.insert("type", "auto"); break;
    case ToolChoiceMode::None: choice.insert("type", "none"); break;
    case ToolChoiceMode::Required: choice.insert("type", "any"); break;
    case ToolChoiceMode::Specific:
        if (!names.contains(request.toolChoice.toolName)) return fail(error, ErrorCategory::InvalidArgument, "InvalidToolChoice", QStringLiteral("Specific tool must be defined"));
        choice = {{"type", "tool"}, {"name", request.toolChoice.toolName}};
        break;
    default: return fail(error, ErrorCategory::InvalidArgument, "InvalidToolChoice", QStringLiteral("Unknown tool choice"));
    }
    if (tools.isEmpty() && request.toolChoice.mode != ToolChoiceMode::Auto && request.toolChoice.mode != ToolChoiceMode::None)
        return fail(error, ErrorCategory::InvalidArgument, "InvalidToolChoice", QStringLiteral("Required choice needs tools"));
    if (!tools.isEmpty() || request.toolChoice.mode == ToolChoiceMode::None) body.insert("tool_choice", choice);
    encoded.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    output = std::move(encoded);
    return true;
}

bool AnthropicMessagesAdapter::decodeChatResponse(
    const TransportResponse& input,  // 服务端 HTTP 响应或错误
    ChatResponse& output,            // 部分成功解析的数据也保留
    SdkError& error) const           // 严格解析单个 Message 的原生内容块
{
    output = {};
    error = {};
    output.message.role = Role::Assistant;
    output.message.status = MessageStatus::Incomplete;
    QJsonParseError parseError;                                                       // JSON 解析故障及偏移
    const QJsonDocument document = QJsonDocument::fromJson(input.body, &parseError);  // 当前请求独立的 JSON 树
    const QJsonObject root = document.object();                                       // 响应根对象或空对象
    const auto invalid = [&](const QString& message) {                                // 失败保留原始响应及已解析的数据
        fail(error, ErrorCategory::InvalidResponse, "InvalidResponse", message);
        error.httpStatus = input.statusCode;
        error.providerError = document.isObject() ? QJsonValue(root) : QJsonValue(QString::fromUtf8(input.body));
        if (parseError.error != QJsonParseError::NoError) error.details.insert("jsonOffset", parseError.offset);
        return false;
    };
    if (!input.statusCode) return invalid(QStringLiteral("HTTP status is missing"));
    if (*input.statusCode < 200 || *input.statusCode >= 300 || root.value("type") == QJsonValue("error")) {
        const int status = *input.statusCode;                       // HTTP 层错误分类依据
        const QJsonObject detail = root.value("error").toObject();  // Provider 原生错误内容
        fail(error, status == 401 || status == 403 ? ErrorCategory::Authentication : status == 429 ? ErrorCategory::RateLimited : ErrorCategory::Provider,
             "ProviderError", detail.value("message").toString(QStringLiteral("Messages request failed")));
        error.httpStatus = status;
        error.providerError = document.isObject() ? QJsonValue(root) : QJsonValue(QString::fromUtf8(input.body));
        error.details.insert("providerCode", detail.value("type").toString());
        error.retryAfterMs = ProtocolDetails::retryAfter(input);
        return false;
    }
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return invalid(QStringLiteral("Response is not a JSON object"));
    if (root.value("type") != QJsonValue("message") || root.value("role") != QJsonValue("assistant") || !root.value("content").isArray())
        return invalid(QStringLiteral("Expected one assistant Message with a content array"));
    output.id = root.value("id").toString();
    output.model = root.value("model").toString();
    const QString stop = root.value("stop_reason").toString();  // 模型停止原因，不等同于协议完整性
    if (stop.isEmpty()) return invalid(QStringLiteral("Completed response requires stop_reason"));
    output.finishReason = stop == "max_tokens" ? FinishReason::Length : stop == "tool_use" ? FinishReason::ToolCalls
        : stop == "end_turn" || stop == "stop_sequence" ? FinishReason::Stop : stop == "refusal" ? FinishReason::ContentFilter : FinishReason::Other;
    if (root.contains("usage") && !root.value("usage").isObject()) return invalid(QStringLiteral("Invalid usage"));
    const QJsonObject usage = root.value("usage").toObject();                     // 服务端已报告的用量，未报告字段未知
    const auto readUsage = [&](const char* name, std::optional<qint64>& field) {  // 校验并读取非负整数 Token 数
        const QJsonValue value = usage.value(QString::fromLatin1(name));          // 当前统计字段
        if (value.isUndefined() || value.isNull()) return true;
        const double number = value.toDouble(-1);  // 数值校验使用的临时表示
        if (!value.isDouble() || !std::isfinite(number) || number < 0 || std::floor(number) != number || number >= 9223372036854775808.0) return false;
        field = static_cast<qint64>(number);
        return true;
    };
    if (!readUsage("input_tokens", output.usage.inputTokens) || !readUsage("output_tokens", output.usage.outputTokens) || !readUsage("total_tokens", output.usage.totalTokens)) return invalid(QStringLiteral("Usage must contain nonnegative integers"));
    QSet<QString> ids;                                          // 当前响应范围内的 ToolCall ID
    for (const auto& item : root.value("content").toArray()) {  // 当前原生有序内容块
        if (!item.isObject()) return invalid(QStringLiteral("Content block must be an object"));
        const QJsonObject block = item.toObject();            // 当前原生内容属性
        const QString type = block.value("type").toString();  // 只在 Adapter 内解释的协议类型
        if (type == "text" || type == "thinking") {
            const QString key = type == "text" ? QStringLiteral("text") : QStringLiteral("thinking");  // 当前文本字段名称
            if (!block.value(key).isString()) return invalid(QStringLiteral("Text content must be a string"));
            if (type == "text") output.message.contents.append(TextContent{block.value(key).toString()});
            else output.message.contents.append(ReasoningContent{block.value(key).toString()});
        } else if (type == "tool_use") {
            const QString id = block.value("id").toString();      // 原生调用关联标识，不自动修补
            const QString name = block.value("name").toString();  // 原生函数名称
            if (id.trimmed().isEmpty() || ids.contains(id) || name.trimmed().isEmpty() || !block.value("input").isObject()) return invalid(QStringLiteral("Tool calls need nonempty unique IDs, names and object input"));
            ids.insert(id);
            output.message.contents.append(ToolCallContent{ToolCall{id, name, block.value("input").toObject()}});
        } else {
            invalid(QStringLiteral("Unmodeled Messages content block: ") + type);
            error.category = ErrorCategory::Unsupported;
            error.code = QStringLiteral("UnsupportedFeature");
            return false;
        }
    }
    if (output.finishReason == FinishReason::ToolCalls && ids.isEmpty()) return invalid(QStringLiteral("tool_use stop reason requires calls"));
    output.completionState = CompletionState::Complete;
    output.message.status = output.finishReason == FinishReason::Length ? MessageStatus::Incomplete : MessageStatus::Complete;
    return true;
}
}  // AiLib 命名空间结束
