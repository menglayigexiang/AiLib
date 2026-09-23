#include <LibMcp/McpServer.h>

#include "../core/JsonRpcCodec_p.h"
#include "../core/JsonSchemaValidator_p.h"
#include "../core/McpProtocolCodec_p.h"
#include "../core/ProtocolMetadata_p.h"
#include "../core/ProtocolSchemaValidator_p.h"

#include <QHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>

namespace LibMcp {

McpRequestContext::McpRequestContext(
    ProgressFunction progressFunction)  // 绑定当前请求的进度输出
    : m_progressFunction(std::move(progressFunction))
{
}

void McpRequestContext::reportProgress(
    double progress,              // 当前已完成的工作量
    double total,                 // 总工作量，未知时为负数
    const QString& message) const // 可选的人类可读说明
{                                 // 在 Client 提供 Token 时发送当前进度
    if (m_progressFunction) {
        m_progressFunction(progress, total, message);
    }
}

namespace {

QRegularExpression templateExpression(const QString &uriTemplate)
{
    QString pattern;
    int position = 0;
    const QRegularExpression variable(QStringLiteral("\\{([^}]+)\\}"));
    auto iterator = variable.globalMatch(uriTemplate);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        pattern += QRegularExpression::escape(
            uriTemplate.mid(position, match.capturedStart() - position));
        pattern += QStringLiteral("[^/?#]+");
        position = match.capturedEnd();
    }
    pattern += QRegularExpression::escape(uriTemplate.mid(position));
    return QRegularExpression(QStringLiteral("^") + pattern
                              + QStringLiteral("$"));
}

QJsonObject completeResult(QJsonObject result)
{
    if (!result.contains(QStringLiteral("resultType"))) {
        result.insert(QStringLiteral("resultType"), QStringLiteral("complete"));
    }
    result.insert(QStringLiteral("ttlMs"), 0);
    result.insert(QStringLiteral("cacheScope"), QStringLiteral("private"));
    return result;
}

} // namespace

class McpServer::Private
{
public:
    struct ToolEntry
    {
        McpTool descriptor;
        McpToolFunction function;
    };

    struct ResourceEntry
    {
        McpResource descriptor;
        McpResourceFunction function;
    };

    struct ResourceTemplateEntry
    {
        McpResourceTemplate descriptor;
        McpResourceFunction function;
        QRegularExpression expression;
    };

    struct PromptEntry
    {
        McpPrompt descriptor;
        McpPromptFunction function;
    };

    // 保存一条长寿命订阅的传输路由和显式通知过滤器。
    struct Subscription
    {
        McpTransportRequestId transportRequestId;  // 承载订阅流的传输请求
        QJsonValue protocolRequestId;               // Client 分配的 JSON-RPC 请求 ID
        QJsonObject notifications;                  // Client 显式选择的通知类型
        qint64 startedAtMs = 0;                     // 订阅开始时的 UTC 毫秒时间戳
    };

    McpServer *owner = nullptr;
    std::unique_ptr<McpServerTransport> transport;
    ServerInfo serverInfo;
    QHash<QString, ToolEntry> tools;
    QHash<QString, ResourceEntry> resources;
    QHash<QString, ResourceTemplateEntry> resourceTemplates;
    QHash<QString, PromptEntry> prompts;
    McpCompletionFunction completionFunction;  // 当前 Server 的可选补全处理器
    QHash<QString, Subscription> subscriptions; // 按 JSON-RPC ID 路由的活动订阅
    bool running = false;

    QJsonObject capabilities() const  // 根据已注册能力生成当前 Server 能力声明
    {
        QJsonObject values;  // 保存返回给 Client 的 ServerCapabilities
        if (!tools.isEmpty()) {
            values.insert(
                QStringLiteral("tools"),
                QJsonObject{{QStringLiteral("listChanged"), true}});
        }
        if (!resources.isEmpty() || !resourceTemplates.isEmpty()) {
            values.insert(
                QStringLiteral("resources"),
                QJsonObject{{QStringLiteral("subscribe"), true},
                            {QStringLiteral("listChanged"), true}});
        }
        if (!prompts.isEmpty()) {
            values.insert(
                QStringLiteral("prompts"),
                QJsonObject{{QStringLiteral("listChanged"), true}});
        }
        if (completionFunction) {
            values.insert(QStringLiteral("completions"), QJsonObject{});
        }
        return values;
    }

    QJsonObject resultMeta() const  // 生成每个结果建议携带的 Server 身份元数据
    {
        QJsonObject info{  // 保存官方 Implementation 结构要求的 Server 身份
            {QStringLiteral("name"), serverInfo.name},
            {QStringLiteral("version"), serverInfo.version}};
        if (!serverInfo.title.isEmpty()) {
            info.insert(QStringLiteral("title"), serverInfo.title);
        }
        return {{QStringLiteral("io.modelcontextprotocol/serverInfo"), info}};
    }

    void sendResult(const McpTransportRequestId& requestId,
                    const QJsonValue &id,
                    QJsonObject result)
    {
        result.insert(QStringLiteral("_meta"), resultMeta());
        transport->sendMessage(
            requestId,
            Internal::makeResult(id, completeResult(std::move(result))));
    }

    void sendError(const McpTransportRequestId& requestId,
                   const QJsonValue &id,
                   int code,
                   const QString &message,
                   const QJsonValue &data = {})
    {
        transport->sendMessage(
            requestId,
            Internal::makeError(id, code, message, data));
    }

    void publishListChanged(
        const QString& filterName,  // 订阅过滤器中对应的选项名
        const QString& method)      // 需要发送的标准列表变更通知
    {                               // 只向显式选择该类型的订阅发布通知
        for (const Subscription& subscription : subscriptions) {
            if (!subscription.notifications.value(filterName).toBool()) {
                continue;
            }
            const QJsonObject meta{  // 标记通知所属的长寿命订阅
                {QStringLiteral("io.modelcontextprotocol/subscriptionId"),
                 subscription.protocolRequestId}};
            transport->sendMessage(
                subscription.transportRequestId,
                Internal::makeNotification(
                    method,
                    QJsonObject{{QStringLiteral("_meta"), meta}}));
        }
    }

    void publishResourceUpdated(const QString& uri)  // 向显式选择 URI 的订阅发布资源更新
    {
        for (const Subscription& subscription : subscriptions) {
            const QJsonArray uris =  // 读取当前订阅选择的资源 URI
                subscription.notifications
                    .value(QStringLiteral("resourceSubscriptions"))
                    .toArray();
            if (!uris.contains(uri)) {
                continue;
            }
            const QJsonObject meta{  // 标记通知所属的长寿命订阅
                {QStringLiteral("io.modelcontextprotocol/subscriptionId"),
                 subscription.protocolRequestId}};
            transport->sendMessage(
                subscription.transportRequestId,
                Internal::makeNotification(
                    QStringLiteral("notifications/resources/updated"),
                    QJsonObject{{QStringLiteral("_meta"), meta},
                                {QStringLiteral("uri"), uri}}));
        }
    }

    void closeSubscription(const QString& id)  // 正常结束指定订阅并发送最终结果
    {
        const auto iterator = subscriptions.find(id);  // 查找待关闭的长寿命订阅
        if (iterator == subscriptions.end()) {
            return;
        }
        const Subscription subscription = iterator.value();  // 在移除前保留响应路由
        subscriptions.erase(iterator);
        QJsonObject meta = resultMeta();  // 扩展最终结果元数据为订阅结束元数据
        meta.insert(QStringLiteral("io.modelcontextprotocol/subscriptionId"),
                    subscription.protocolRequestId);
        transport->sendMessage(
            subscription.transportRequestId,
            Internal::makeResult(
                subscription.protocolRequestId,
                completeResult(QJsonObject{{QStringLiteral("_meta"), meta}})));
        const qint64 elapsedMs =  // 计算长寿命请求从建立到关闭的总时长
            qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch()
                                - subscription.startedAtMs);
        emit owner->requestFinished(
            QStringLiteral("subscriptions/listen"), elapsedMs, true);
    }

    void receive(const McpTransportRequestId& requestId, const QJsonObject& message)
    {
        const QString wireDefinition = message.contains(QStringLiteral("id"))
                                           ? QStringLiteral("ClientRequest")
                                           : QStringLiteral("ClientNotification");  // 选择官方请求或通知定义
        const McpResult<void> wireValidation =  // 按官方 Schema 拒绝遗漏必填字段的 Wire 消息
            Internal::validateProtocolDefinition(wireDefinition, message);
        if (wireValidation.isError()) {
            const QJsonValue invalidId = message.value(QStringLiteral("id"));  // 尽可能回显无效请求 ID
            sendError(requestId,
                      invalidId.isUndefined() ? QJsonValue(QJsonValue::Null)
                                              : invalidId,
                      -32600,
                      QStringLiteral("Invalid Request"));
            emit owner->protocolError(wireValidation.error());
            return;
        }
        if (!message.contains(QStringLiteral("id"))) {
            if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("notifications/cancelled")) {
                closeSubscription(
                    message.value(QStringLiteral("params"))
                        .toObject()
                        .value(QStringLiteral("requestId"))
                        .toVariant()
                        .toString());
            }
            return;
        }

        const QJsonValue id = message.value(QStringLiteral("id"));
        const QString method =
            message.value(QStringLiteral("method")).toString();
        const QJsonObject params =
            message.value(QStringLiteral("params")).toObject();
        const McpResult<Internal::RequestMetadata> metadata =  // 校验本次请求自包含的协议上下文
            Internal::decodeRequestMetadata(params);
        if (metadata.isError()) {
            const McpError& error = metadata.error();  // 读取需要映射到 JSON-RPC 的校验错误
            emit owner->protocolError(error);
            sendError(requestId,
                      id,
                      error.remoteCode,
                      error.message,
                      error.data);
            return;
        }
        QElapsedTimer elapsed;  // 测量本次同步协议分发耗时
        elapsed.start();
        emit owner->requestStarted(method, metadata.value().clientInfo);
        bool recognized = true;  // 标记请求是否命中当前 Server 支持的方法

        if (method == QStringLiteral("server/discover")) {
            handleDiscover(requestId, id);
        } else if (method == QStringLiteral("tools/list")) {
            handleToolsList(requestId, id);
        } else if (method == QStringLiteral("tools/call")) {
            handleToolCall(requestId, id, params, metadata.value());
        } else if (method == QStringLiteral("resources/list")) {
            handleResourcesList(requestId, id);
        } else if (method == QStringLiteral("resources/templates/list")) {
            handleResourceTemplatesList(requestId, id);
        } else if (method == QStringLiteral("resources/read")) {
            handleResourceRead(requestId, id, params);
        } else if (method == QStringLiteral("prompts/list")) {
            handlePromptsList(requestId, id);
        } else if (method == QStringLiteral("prompts/get")) {
            handlePromptGet(requestId, id, params);
        } else if (method == QStringLiteral("completion/complete")) {
            handleCompletion(requestId, id, params);
        } else if (method == QStringLiteral("subscriptions/listen")) {
            const QJsonObject requested =  // 读取 Client 显式选择的通知类型
                params.value(QStringLiteral("notifications")).toObject();
            const QString subscriptionId = id.toVariant().toString();  // 规范化订阅的 JSON-RPC ID
            subscriptions.insert(
                subscriptionId,
                Subscription{requestId,
                             id,
                             requested,
                             QDateTime::currentMSecsSinceEpoch()});
            const QJsonObject notificationMeta{  // 为首条确认通知附加订阅 ID
                {QStringLiteral("io.modelcontextprotocol/subscriptionId"), id}};
            transport->sendMessage(
                requestId,
                Internal::makeNotification(
                    QStringLiteral("notifications/subscriptions/acknowledged"),
                    QJsonObject{{QStringLiteral("_meta"), notificationMeta},
                                {QStringLiteral("notifications"), requested}}));
        } else {
            recognized = false;
            sendError(requestId, id, -32601,
                      QStringLiteral("Method not found"));
            emit owner->protocolError(
                {McpErrorCode::ProtocolError,
                 QStringLiteral("Method not found"),
                 -32601});
        }
        emit owner->requestHandled(method);
        if (method != QStringLiteral("subscriptions/listen")) {
            emit owner->requestFinished(method, elapsed.elapsed(), recognized);
        }
    }

    void handleDiscover(
        const McpTransportRequestId& requestId,  // 标识需要接收响应的当前 Transport 请求
        const QJsonValue& id)           // 返回当前 Server 支持版本和能力
    {
        sendResult(
            requestId,
            id,
            {{QStringLiteral("supportedVersions"),
              QJsonArray{QStringLiteral(LIBMCP_PROTOCOL_VERSION)}},
             {QStringLiteral("capabilities"), capabilities()}});
    }

    void handleToolsList(const McpTransportRequestId& requestId, const QJsonValue& id)
    {
        QJsonArray values;
        QStringList names = tools.keys();
        names.sort();
        for (const QString &name : names) {
            values.append(Internal::encodeTool(tools.value(name).descriptor));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("tools"), values}});
    }

    void handleToolCall(const McpTransportRequestId& requestId,
                        const QJsonValue &id,
                        const QJsonObject& params,
                        const Internal::RequestMetadata& metadata)
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        const auto iterator = tools.constFind(name);
        if (iterator == tools.cend()) {
            sendError(requestId, id, -32602,
                      QStringLiteral("Unknown tool: %1").arg(name));
            return;
        }

        const QJsonObject arguments =
            params.value(QStringLiteral("arguments")).toObject();
        const McpResult<void> validation = Internal::validateJsonValue(
            iterator->descriptor.inputSchema,
            arguments);
        if (validation.isError()) {
            sendError(requestId, id, -32602,
                      validation.error().message);
            return;
        }

        try {
            McpToolCallRequest request;  // 保存工具参数和可选 MRTR 重试输入
            request.name = name;
            request.arguments = arguments;
            request.requestState = params.value(QStringLiteral("requestState")).toString();
            request.inputResponses = params.value(QStringLiteral("inputResponses")).toObject();
            const McpRequestContext context(  // 将当前请求 Token 封装为工具进度上下文
                [this, requestId, token = metadata.progressToken](
                    double progress,       // 当前已完成的工作量
                    double total,          // 总工作量，未知时为负数
                    const QString& message) {  // 可选的人类可读说明
                    if (token.isUndefined()) {
                        return;
                    }
                    QJsonObject progressParams{  // 编码标准进度通知参数
                        {QStringLiteral("progressToken"), token},
                        {QStringLiteral("progress"), progress}};
                    if (total >= 0.0) {
                        progressParams.insert(QStringLiteral("total"), total);
                    }
                    if (!message.isEmpty()) {
                        progressParams.insert(QStringLiteral("message"), message);
                    }
                    transport->sendMessage(
                        requestId,
                        Internal::makeNotification(
                            QStringLiteral("notifications/progress"),
                            progressParams));
                });
            const McpToolCallResult toolResult =  // 执行业务 Tool 并保留完整结果语义
                iterator->function(request, context);
            const bool inputRequired =  // 标记本轮只请求额外输入而尚未产生工具输出
                !toolResult.inputRequests.isEmpty()
                || !toolResult.requestState.isEmpty();
            for (auto input = toolResult.inputRequests.constBegin();
                 input != toolResult.inputRequests.constEnd();
                 ++input) {
                const QJsonObject inputRequest = input.value().toObject();  // 读取当前 MRTR 输入请求
                if (inputRequest.value(QStringLiteral("method")).toString()
                    != QStringLiteral("elicitation/create")) {
                    continue;
                }
                const QJsonValue elicitationValue =  // 读取当前请求声明的用户输入能力
                    metadata.clientCapabilities.value(QStringLiteral("elicitation"));
                const QString mode =  // 读取工具请求的表单或 URL 模式
                    inputRequest.value(QStringLiteral("params"))
                        .toObject()
                        .value(QStringLiteral("mode"))
                        .toString(QStringLiteral("form"));
                const bool supported = elicitationValue.isObject()
                    && (mode != QStringLiteral("url")
                        || elicitationValue.toObject().contains(
                            QStringLiteral("url")));  // 空 elicitation 能力按规范默认支持 form
                if (!supported) {
                    sendError(requestId,
                              id,
                              -32602,
                              QStringLiteral("Client 未声明所需的 elicitation 能力"));
                    return;
                }
            }
            if (!inputRequired && !iterator->descriptor.outputSchema.isEmpty()) {
                if (toolResult.structuredContent.isUndefined()) {
                    sendError(requestId,
                              id,
                              -32603,
                              QStringLiteral("Tool 声明了 outputSchema，但未返回 structuredContent"));
                    return;
                }
                const McpResult<void> outputValidation =  // 校验结构化 Tool 输出是否符合声明
                    Internal::validateJsonValue(
                        iterator->descriptor.outputSchema,
                        toolResult.structuredContent);
                if (outputValidation.isError()) {
                    sendError(requestId,
                              id,
                              -32603,
                              outputValidation.error().message);
                    return;
                }
            }

            QJsonObject result;  // 编码完成或需要输入的工具结果
            if (!inputRequired) {
                result.insert(QStringLiteral("content"), toolResult.content);
                result.insert(QStringLiteral("isError"), toolResult.isError);
            }
            if (!toolResult.structuredContent.isUndefined()) {
                result.insert(QStringLiteral("structuredContent"),
                              toolResult.structuredContent);
            }
            if (inputRequired) {
                result.insert(QStringLiteral("resultType"),
                              QStringLiteral("input_required"));
                if (!toolResult.inputRequests.isEmpty()) {
                    result.insert(QStringLiteral("inputRequests"),
                                  toolResult.inputRequests);
                }
                if (!toolResult.requestState.isEmpty()) {
                    result.insert(QStringLiteral("requestState"),
                                  toolResult.requestState);
                }
            }
            sendResult(requestId, id, result);
        } catch (const std::exception &exception) {
            sendResult(
                requestId,
                id,
                {{QStringLiteral("content"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("type"), QStringLiteral("text")},
                      {QStringLiteral("text"),
                       QString::fromUtf8(exception.what())}}}},
                 {QStringLiteral("isError"), true}});
        }
    }

    void handleResourcesList(const McpTransportRequestId& requestId,
                             const QJsonValue &id)
    {
        QJsonArray values;
        QStringList uris = resources.keys();
        uris.sort();
        for (const QString &uri : uris) {
            values.append(
                Internal::encodeResource(resources.value(uri).descriptor));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("resources"), values}});
    }

    void handleResourceTemplatesList(const McpTransportRequestId& requestId,
                                     const QJsonValue &id)
    {
        QJsonArray values;
        QStringList templates = resourceTemplates.keys();
        templates.sort();
        for (const QString &uriTemplate : templates) {
            values.append(Internal::encodeResourceTemplate(
                resourceTemplates.value(uriTemplate).descriptor));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("resourceTemplates"), values}});
    }

    void handleResourceRead(const McpTransportRequestId& requestId,
                            const QJsonValue &id,
                            const QJsonObject &params)
    {
        const QString uri = params.value(QStringLiteral("uri")).toString();
        McpResourceFunction function;
        const auto fixed = resources.constFind(uri);
        if (fixed != resources.cend()) {
            function = fixed->function;
        } else {
            for (auto iterator = resourceTemplates.cbegin();
                 iterator != resourceTemplates.cend();
                 ++iterator) {
                if (iterator->expression.match(uri).hasMatch()) {
                    function = iterator->function;
                    break;
                }
            }
        }
        if (!function) {
            sendError(requestId, id, -32002,
                      QStringLiteral("Resource not found: %1").arg(uri));
            return;
        }

        QJsonArray contents;
        for (const McpResourceContent &content : function(uri)) {
            contents.append(Internal::encodeResourceContent(content));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("contents"), contents}});
    }

    void handlePromptsList(const McpTransportRequestId& requestId,
                           const QJsonValue &id)
    {
        QJsonArray values;
        QStringList names = prompts.keys();
        names.sort();
        for (const QString &name : names) {
            values.append(
                Internal::encodePrompt(prompts.value(name).descriptor));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("prompts"), values}});
    }

    void handlePromptGet(const McpTransportRequestId& requestId,
                         const QJsonValue &id,
                         const QJsonObject &params)
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        const auto iterator = prompts.constFind(name);
        if (iterator == prompts.cend()) {
            sendError(requestId, id, -32602,
                      QStringLiteral("Unknown prompt: %1").arg(name));
            return;
        }

        const QJsonObject arguments =
            params.value(QStringLiteral("arguments")).toObject();
        for (const McpPromptArgument &argument :
             iterator->descriptor.arguments) {
            if (argument.required && !arguments.contains(argument.name)) {
                sendError(
                    requestId,
                    id,
                    -32602,
                    QStringLiteral("Missing prompt argument: %1")
                        .arg(argument.name));
                return;
            }
        }

        QJsonArray messages;
        for (const McpPromptMessage &message :
             iterator->function(arguments)) {
            messages.append(Internal::encodePromptMessage(message));
        }
        sendResult(requestId, id,
                   {{QStringLiteral("messages"), messages}});
    }

    void handleCompletion(
        const McpTransportRequestId& requestId,  // 标识需要接收响应的当前 Transport 请求
        const QJsonValue& id,                    // 当前 JSON-RPC 请求 ID
        const QJsonObject& params)               // 包含引用、参数和可选上下文的请求参数
    {                                            // 执行补全处理器并编码候选值
        if (!completionFunction) {
            sendError(requestId,
                      id,
                      -32601,
                      QStringLiteral("Completion is not supported"));
            return;
        }

        const QJsonObject reference = params.value(QStringLiteral("ref")).toObject();  // 读取 Prompt 或资源引用
        const QString referenceType = reference.value(QStringLiteral("type")).toString();  // 读取引用种类
        McpCompletionRequest request;  // 保存解码后的公共补全请求
        if (referenceType == QStringLiteral("ref/prompt")) {
            request.referenceType = McpCompletionReferenceType::Prompt;
            request.reference = reference.value(QStringLiteral("name")).toString();
        } else if (referenceType == QStringLiteral("ref/resource")) {
            request.referenceType = McpCompletionReferenceType::ResourceTemplate;
            request.reference = reference.value(QStringLiteral("uri")).toString();
        } else {
            sendError(requestId, id, -32602, QStringLiteral("Invalid completion reference"));
            return;
        }

        const QJsonObject argument = params.value(QStringLiteral("argument")).toObject();  // 读取当前待补全参数
        request.argumentName = argument.value(QStringLiteral("name")).toString();
        request.argumentValue = argument.value(QStringLiteral("value")).toString();
        request.context = params.value(QStringLiteral("context"))
                              .toObject()
                              .value(QStringLiteral("arguments"))
                              .toObject();
        if (request.reference.isEmpty() || request.argumentName.isEmpty()) {
            sendError(requestId, id, -32602, QStringLiteral("Invalid completion parameters"));
            return;
        }

        McpCompletionResult completion = completionFunction(request);  // 执行业务补全函数
        if (completion.values.size() > 100) {
            completion.values = completion.values.mid(0, 100);
        }
        QJsonArray values;  // 编码最多一百个补全候选值
        for (const QString& value : completion.values) {
            values.append(value);
        }
        QJsonObject encoded{{QStringLiteral("values"), values}};  // 保存 Completion 对象
        if (completion.total) {
            encoded.insert(QStringLiteral("total"), *completion.total);
        }
        if (completion.hasMore) {
            encoded.insert(QStringLiteral("hasMore"), *completion.hasMore);
        }
        sendResult(requestId,
                   id,
                   {{QStringLiteral("completion"), encoded}});
    }
};

McpServer::McpServer(std::unique_ptr<McpServerTransport> transport,
                     ServerInfo serverInfo,
                     QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->owner = this;
    d->transport = std::move(transport);
    d->serverInfo = std::move(serverInfo);
    connect(d->transport.get(),
            &McpServerTransport::messageReceived,
            this,
            [this](const McpTransportRequestId& requestId,
                   const QJsonObject &message) {
                d->receive(requestId, message);
            });
}

McpServer::~McpServer() = default;

bool McpServer::addTool(
    const McpTool& tool,          // 需要注册并对外公布的工具描述
    McpToolFunction function)     // 注册工具并校验输入输出 Schema
{
    if (tool.name.isEmpty() || !function || d->tools.contains(tool.name)) {
        return false;
    }

    const McpResult<void> inputSchemaResult =  // 保存输入 Schema 的 Draft 2020-12 校验结果
        Internal::validateJsonSchemaDocument(tool.inputSchema);
    if (inputSchemaResult.isError()) {
        return false;
    }
    if (!tool.outputSchema.isEmpty()) {
        const McpResult<void> outputSchemaResult =  // 保存可选输出 Schema 的校验结果
            Internal::validateJsonSchemaDocument(tool.outputSchema);
        if (outputSchemaResult.isError()) {
            return false;
        }
    }

    d->tools.insert(tool.name, {tool, std::move(function)});
    if (d->running) {
        d->publishListChanged(QStringLiteral("toolsListChanged"),
                              QStringLiteral("notifications/tools/list_changed"));
    }
    return true;
}

bool McpServer::addResource(const McpResource &resource,
                            McpResourceFunction function)
{
    if (resource.uri.isEmpty() || !function
        || d->resources.contains(resource.uri)) {
        return false;
    }
    d->resources.insert(resource.uri, {resource, std::move(function)});
    if (d->running) {
        d->publishListChanged(QStringLiteral("resourcesListChanged"),
                              QStringLiteral("notifications/resources/list_changed"));
    }
    return true;
}

bool McpServer::addResourceTemplate(
    const McpResourceTemplate &resourceTemplate,
    McpResourceFunction function)
{
    if (resourceTemplate.uriTemplate.isEmpty() || !function
        || d->resourceTemplates.contains(resourceTemplate.uriTemplate)) {
        return false;
    }
    const QRegularExpression expression =
        templateExpression(resourceTemplate.uriTemplate);
    if (!expression.isValid()) {
        return false;
    }
    d->resourceTemplates.insert(
        resourceTemplate.uriTemplate,
        {resourceTemplate, std::move(function), expression});
    if (d->running) {
        d->publishListChanged(QStringLiteral("resourcesListChanged"),
                              QStringLiteral("notifications/resources/list_changed"));
    }
    return true;
}

bool McpServer::addPrompt(const McpPrompt &prompt,
                          McpPromptFunction function)
{
    if (prompt.name.isEmpty() || !function
        || d->prompts.contains(prompt.name)) {
        return false;
    }
    d->prompts.insert(prompt.name, {prompt, std::move(function)});
    if (d->running) {
        d->publishListChanged(QStringLiteral("promptsListChanged"),
                              QStringLiteral("notifications/prompts/list_changed"));
    }
    return true;
}

void McpServer::setCompletionHandler(
    McpCompletionFunction function)  // 设置唯一补全处理器并同步能力声明
{
    d->completionFunction = std::move(function);
}

void McpServer::notifyResourceUpdated(
    const QString& uri)  // 向显式订阅该 URI 的活动流发布内容更新
{
    if (!uri.isEmpty() && d->running) {
        d->publishResourceUpdated(uri);
    }
}

QFuture<McpResult<void>> McpServer::start()
{
    d->running = true;
    return d->transport->start();
}

QFuture<McpResult<void>> McpServer::stop()
{
    d->running = false;
    const QStringList subscriptionIds = d->subscriptions.keys();  // 固定需要正常结束的活动订阅
    for (const QString& subscriptionId : subscriptionIds) {
        d->closeSubscription(subscriptionId);
    }
    return d->transport->stop();
}

bool McpServer::isRunning() const
{
    return d->running;
}

} // namespace LibMcp
