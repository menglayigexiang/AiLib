#include <AiLib/protocol/openai/OpenAIResponsesAdapter.h>
#include <AiLib/core/Content.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <cmath>

namespace AiLib {
namespace {
bool fail(SdkError& error, ErrorCategory category, const QString& code,
          const QString& message)  // 保存 Responses 协议错误并返回 false
{
    error = {};
    error.category = category;
    error.code = code;
    error.message = message;
    return false;
}

QByteArray jsonText(const QJsonValue& value)  // 将工具结果编码为紧凑 JSON 文本
{
    if (value.isObject())
        return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    if (value.isArray())
        return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

bool validBaseUrl(const QUrl& url)  // 验证可安全追加 Responses 路径的 API 根地址
{
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))
        && !url.hasQuery() && !url.hasFragment() && url.userInfo().isEmpty();
}

bool appendMessageInput(const Message& message, QJsonArray& input,
                        SdkError& error)  // 将单条 canonical 消息转换为 Responses 输入项
{
    if (message.role == Role::Tool) {
        for (const MessageContent& content : message.contents) {  // 当前工具结果内容
            const auto* result = std::get_if<ToolResultContent>(&content);  // 必须关联原工具调用
            if (!result || result->result.callId.trimmed().isEmpty())
                return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolResult"),
                            QStringLiteral("Tool messages must contain associated tool results"));
            QJsonValue value = result->result.success
                ? result->result.data
                : QJsonObject{{QStringLiteral("error_code"), result->result.errorCode},
                              {QStringLiteral("error_message"), result->result.errorMessage}};
            input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function_call_output")},
                                     {QStringLiteral("call_id"), result->result.callId},
                                     {QStringLiteral("output"), QString::fromUtf8(jsonText(value))}});
        }
        return true;
    }
    const QString role = message.role == Role::System ? QStringLiteral("system")
        : message.role == Role::Assistant ? QStringLiteral("assistant") : QStringLiteral("user"); // Responses 消息角色
    QJsonArray contentItems;  // 当前消息的有序文本内容
    const auto flushText = [&]() {  // 在工具项边界前保存已累计的消息文本
        if (contentItems.isEmpty())
            return;
        input.append(QJsonObject{{QStringLiteral("role"), role},
                                 {QStringLiteral("content"), contentItems}});
        contentItems = {};
    };
    for (const MessageContent& content : message.contents) {  // 当前 canonical 内容块
        if (const auto* text = std::get_if<TextContent>(&content)) {
            contentItems.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("input_text")},
                                             {QStringLiteral("text"), text->text}});
        } else if (const auto* call = std::get_if<ToolCallContent>(&content)) {
            if (message.role != Role::Assistant || call->call.id.trimmed().isEmpty()
                || call->call.name.trimmed().isEmpty())
                return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolCall"),
                            QStringLiteral("Tool calls require assistant role, ID and name"));
            flushText();
            input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function_call")},
                                     {QStringLiteral("call_id"), call->call.id},
                                     {QStringLiteral("name"), call->call.name},
                                     {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(call->call.arguments).toJson(QJsonDocument::Compact))}});
        } else {
            return fail(error, ErrorCategory::Unsupported, QStringLiteral("UnsupportedContent"),
                        QStringLiteral("Responses adapter currently supports text and function tools"));
        }
    }
    flushText();
    return true;
}

bool readUsage(const QJsonObject& object, Usage& usage,
               SdkError& error)  // 读取 Responses 非负整数 Token 用量
{
    const auto read = [&](const QString& key, std::optional<qint64>& target) {  // 读取单个可选计数
        const QJsonValue value = object.value(key);  // 原生 Token 计数
        if (value.isUndefined() || value.isNull())
            return true;
        const double number = value.toDouble(-1);  // 验证 JSON 数字的整数语义
        if (!value.isDouble() || !std::isfinite(number) || number < 0 || std::floor(number) != number
            || number >= 9223372036854775808.0)
            return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidUsage"),
                        QStringLiteral("Token counts must be nonnegative integers"));
        target = static_cast<qint64>(number);
        return true;
    };
    return read(QStringLiteral("input_tokens"), usage.inputTokens)
        && read(QStringLiteral("output_tokens"), usage.outputTokens)
        && read(QStringLiteral("total_tokens"), usage.totalTokens);
}
}  // 内部 Responses 编解码辅助函数命名空间结束

bool OpenAIResponsesAdapter::encodeChatRequest(
    const ProviderConfig& provider, const ChatRequest& request,
    TransportRequest& output, SdkError& error) const  // 编码 Responses 请求且失败不保留部分输出
{
    output = {};
    error = {};
    const QSet<QString> reserved{QStringLiteral("model"), QStringLiteral("input"), QStringLiteral("temperature"),
        QStringLiteral("top_p"), QStringLiteral("max_output_tokens"), QStringLiteral("tools"),
        QStringLiteral("tool_choice"), QStringLiteral("stream"), QStringLiteral("parallel_tool_calls")}; // Adapter 管理字段
    for (auto it = request.extraParameters.constBegin(); it != request.extraParameters.constEnd(); ++it) {  // 调用方扩展参数
        if (reserved.contains(it.key()))
            return fail(error, ErrorCategory::Configuration, QStringLiteral("ReservedParameter"), QStringLiteral("Reserved parameter: ") + it.key());
    }
    if (request.model.trimmed().isEmpty() || request.messages.isEmpty())
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidChatRequest"), QStringLiteral("Model and messages are required"));
    if ((request.temperature && (!std::isfinite(*request.temperature) || *request.temperature < 0 || *request.temperature > 2))
        || (request.topP && (!std::isfinite(*request.topP) || *request.topP < 0 || *request.topP > 1))
        || (request.maxOutputTokens && *request.maxOutputTokens <= 0))
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidModelParameter"), QStringLiteral("Invalid sampling or output-token limit"));
    if (!validBaseUrl(provider.baseUrl))
        return fail(error, ErrorCategory::Configuration, QStringLiteral("InvalidBaseUrl"), QStringLiteral("API root must be a credential-free HTTP(S) URL"));

    QJsonObject body = request.extraParameters;  // 成功前构造的局部请求体
    body.insert(QStringLiteral("model"), request.model);
    body.insert(QStringLiteral("stream"), request.stream);
    if (request.temperature) body.insert(QStringLiteral("temperature"), *request.temperature);
    if (request.topP) body.insert(QStringLiteral("top_p"), *request.topP);
    if (request.maxOutputTokens) body.insert(QStringLiteral("max_output_tokens"), *request.maxOutputTokens);
    QJsonArray input;  // Responses 有序输入项
    for (const Message& message : request.messages) {  // 当前 canonical 历史消息
        if (!appendMessageInput(message, input, error))
            return false;
    }
    if (input.isEmpty())
        return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidChatRequest"), QStringLiteral("At least one supported input item is required"));
    body.insert(QStringLiteral("input"), input);
    QJsonArray tools;       // Responses Function Tool 定义
    QSet<QString> names;    // 已见工具名
    for (const FunctionToolDefinition& tool : request.tools) {  // 当前 canonical 工具定义
        if (tool.name.trimmed().isEmpty() || names.contains(tool.name))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolDefinition"), QStringLiteral("Tool names must be nonempty and unique"));
        names.insert(tool.name);
        tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                 {QStringLiteral("name"), tool.name},
                                 {QStringLiteral("description"), tool.description},
                                 {QStringLiteral("parameters"), tool.inputSchema}});
    }
    if (!tools.isEmpty()) body.insert(QStringLiteral("tools"), tools);
    switch (request.toolChoice.mode) {
    case ToolChoiceMode::Auto: body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto")); break;
    case ToolChoiceMode::None: body.insert(QStringLiteral("tool_choice"), QStringLiteral("none")); break;
    case ToolChoiceMode::Required: body.insert(QStringLiteral("tool_choice"), QStringLiteral("required")); break;
    case ToolChoiceMode::Specific:
        if (request.toolChoice.toolName.trimmed().isEmpty() || !names.contains(request.toolChoice.toolName))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidToolChoice"), QStringLiteral("Specific tool choice must reference a declared tool"));
        body.insert(QStringLiteral("tool_choice"), QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                                                 {QStringLiteral("name"), request.toolChoice.toolName}});
        break;
    }
    TransportRequest encoded;  // 完整验证后的 HTTP 请求
    encoded.url = provider.baseUrl;
    QString path = encoded.url.path();  // 保留网关路径前缀
    while (path.endsWith('/')) path.chop(1);
    encoded.url.setPath(path + QStringLiteral("/responses"));
    QMap<QByteArray, QByteArray> headers{{"content-type", "application/json"},
                                         {"accept", request.stream ? "text/event-stream" : "application/json"}}; // 默认协议头
    if (!provider.apiKey.isEmpty()) headers.insert("authorization", "Bearer " + provider.apiKey.toUtf8());
    for (auto it = provider.customHeaders.constBegin(); it != provider.customHeaders.constEnd(); ++it) {  // 非秘密自定义头
        const QByteArray name = it.key().toLatin1().toLower();  // 大小写无关 Header 名称
        const QByteArray value = it.value().toUtf8();           // 实际 Header 值
        if (name.isEmpty() || value.contains('\r') || value.contains('\n') || value.contains('\0'))
            return fail(error, ErrorCategory::InvalidArgument, QStringLiteral("InvalidHeader"), QStringLiteral("Invalid custom Header"));
        if (name == "content-type" || name == "accept" || name == "host" || name == "content-length" || name == "transfer-encoding")
            return fail(error, ErrorCategory::Configuration, QStringLiteral("ProtectedHeader"), QStringLiteral("Protected Header: ") + it.key());
        headers.insert(name, value);
    }
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)  // 最终请求头
        encoded.headers.append(qMakePair(it.key(), it.value()));
    encoded.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    output = encoded;
    return true;
}

bool OpenAIResponsesAdapter::decodeChatResponse(
    const TransportResponse& input, ChatResponse& output,
    SdkError& error) const  // 解码完整 Responses JSON 响应
{
    output = {};
    error = {};
    QJsonParseError parseError;  // 响应 JSON 解析状态
    const QJsonDocument document = QJsonDocument::fromJson(input.body, &parseError);  // 原生响应树
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidJson"), QStringLiteral("Response body must be a JSON object"));
    const QJsonObject root = document.object();  // 原生 Responses 对象
    if (input.statusCode < 200 || input.statusCode >= 300 || root.contains(QStringLiteral("error"))) {
        const QJsonObject detail = root.value(QStringLiteral("error")).toObject();  // OpenAI 错误详情
        error.category = input.statusCode == 401 || input.statusCode == 403 ? ErrorCategory::Authentication
            : input.statusCode == 429 ? ErrorCategory::RateLimited : ErrorCategory::Provider;
        error.code = QStringLiteral("ProviderError");
        error.message = detail.value(QStringLiteral("message")).toString(QStringLiteral("OpenAI Responses request failed"));
        error.httpStatus = input.statusCode;
        error.providerError = root;
        return false;
    }
    ChatResponse decoded;  // 完整校验后提交的 canonical 响应
    decoded.id = root.value(QStringLiteral("id")).toString();
    decoded.model = root.value(QStringLiteral("model")).toString();
    decoded.message.role = Role::Assistant;
    if (!root.value(QStringLiteral("output")).isArray())
        return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidOutput"), QStringLiteral("Responses output must be an array"));
    QSet<QString> callIds;  // 当前响应内唯一工具调用 ID
    bool hasTools = false;  // 是否返回 Function Tool Call
    for (const QJsonValue& value : root.value(QStringLiteral("output")).toArray()) {  // 原生输出项
        if (!value.isObject())
            return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidOutput"), QStringLiteral("Responses output items must be objects"));
        const QJsonObject item = value.toObject();  // 当前输出项
        const QString type = item.value(QStringLiteral("type")).toString();  // 输出项类型
        if (type == QStringLiteral("message")) {
            const QJsonArray content = item.value(QStringLiteral("content")).toArray();  // 助手内容块
            for (const QJsonValue& partValue : content) {  // 当前助手内容块
                const QJsonObject part = partValue.toObject();  // 原生输出内容
                if (part.value(QStringLiteral("type")) == QJsonValue(QStringLiteral("output_text")))
                    decoded.message.contents.append(TextContent{part.value(QStringLiteral("text")).toString()});
            }
        } else if (type == QStringLiteral("function_call")) {
            const QString callId = item.value(QStringLiteral("call_id")).toString();  // 工具关联 ID
            const QString name = item.value(QStringLiteral("name")).toString();       // 工具名称
            QJsonParseError argumentError;  // 工具参数 JSON 状态
            const QJsonDocument arguments = QJsonDocument::fromJson(item.value(QStringLiteral("arguments")).toString().toUtf8(), &argumentError); // 参数对象
            if (callId.trimmed().isEmpty() || name.trimmed().isEmpty() || callIds.contains(callId)
                || argumentError.error != QJsonParseError::NoError || !arguments.isObject())
                return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidToolCall"), QStringLiteral("Responses function call is invalid"));
            callIds.insert(callId);
            decoded.message.contents.append(ToolCallContent{ToolCall{callId, name, arguments.object()}});
            hasTools = true;
        }
    }
    if (!readUsage(root.value(QStringLiteral("usage")).toObject(), decoded.usage, error))
        return false;
    const QString status = root.value(QStringLiteral("status")).toString();  // Responses 完成状态
    if (status == QStringLiteral("completed"))
        decoded.finishReason = hasTools ? FinishReason::ToolCalls : FinishReason::Stop;
    else if (status == QStringLiteral("incomplete"))
        decoded.finishReason = FinishReason::Length;
    else
        return fail(error, ErrorCategory::InvalidResponse, QStringLiteral("InvalidStatus"), QStringLiteral("Responses status is not complete"));
    decoded.completionState = CompletionState::Complete;
    decoded.message.status = decoded.finishReason == FinishReason::Length ? MessageStatus::Incomplete : MessageStatus::Complete;
    output = decoded;
    return true;
}

}  // AiLib 命名空间结束
