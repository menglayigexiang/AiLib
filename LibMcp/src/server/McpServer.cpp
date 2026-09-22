#include <LibMcp/McpServer.h>

#include "../core/JsonRpcCodec_p.h"
#include "../core/JsonSchemaValidator_p.h"
#include "../core/McpProtocolCodec_p.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>

namespace LibMcp {
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
    result.insert(QStringLiteral("resultType"), QStringLiteral("complete"));
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

    McpServer *owner = nullptr;
    std::unique_ptr<McpServerTransport> transport;
    ServerInfo serverInfo;
    QHash<QString, ToolEntry> tools;
    QHash<QString, ResourceEntry> resources;
    QHash<QString, ResourceTemplateEntry> resourceTemplates;
    QHash<QString, PromptEntry> prompts;
    bool running = false;

    void sendResult(const McpSessionId &sessionId,
                    const QJsonValue &id,
                    QJsonObject result)
    {
        transport->sendMessage(
            sessionId,
            Internal::makeResult(id, completeResult(std::move(result))));
    }

    void sendError(const McpSessionId &sessionId,
                   const QJsonValue &id,
                   int code,
                   const QString &message,
                   const QJsonValue &data = {})
    {
        transport->sendMessage(
            sessionId,
            Internal::makeError(id, code, message, data));
    }

    void receive(const McpSessionId &sessionId, const QJsonObject &message)
    {
        if (!message.contains(QStringLiteral("id"))) {
            return;
        }

        const QJsonValue id = message.value(QStringLiteral("id"));
        const QString method =
            message.value(QStringLiteral("method")).toString();
        const QJsonObject params =
            message.value(QStringLiteral("params")).toObject();
        emit owner->requestHandled(method);

        if (method == QStringLiteral("initialize")) {
            handleInitialize(sessionId, id);
        } else if (method == QStringLiteral("tools/list")) {
            handleToolsList(sessionId, id);
        } else if (method == QStringLiteral("tools/call")) {
            handleToolCall(sessionId, id, params);
        } else if (method == QStringLiteral("resources/list")) {
            handleResourcesList(sessionId, id);
        } else if (method == QStringLiteral("resources/templates/list")) {
            handleResourceTemplatesList(sessionId, id);
        } else if (method == QStringLiteral("resources/read")) {
            handleResourceRead(sessionId, id, params);
        } else if (method == QStringLiteral("prompts/list")) {
            handlePromptsList(sessionId, id);
        } else if (method == QStringLiteral("prompts/get")) {
            handlePromptGet(sessionId, id, params);
        } else {
            sendError(sessionId, id, -32601,
                      QStringLiteral("Method not found"));
        }
    }

    void handleInitialize(const McpSessionId &sessionId, const QJsonValue &id)
    {
        QJsonObject capabilities;
        if (!tools.isEmpty()) {
            capabilities.insert(QStringLiteral("tools"),
                                QJsonObject{{QStringLiteral("listChanged"),
                                             false}});
        }
        if (!resources.isEmpty() || !resourceTemplates.isEmpty()) {
            capabilities.insert(
                QStringLiteral("resources"),
                QJsonObject{{QStringLiteral("subscribe"), false},
                            {QStringLiteral("listChanged"), false}});
        }
        if (!prompts.isEmpty()) {
            capabilities.insert(QStringLiteral("prompts"),
                                QJsonObject{{QStringLiteral("listChanged"),
                                             false}});
        }

        sendResult(
            sessionId,
            id,
            {{QStringLiteral("protocolVersion"),
              QStringLiteral(LIBMCP_PROTOCOL_VERSION)},
             {QStringLiteral("capabilities"), capabilities},
             {QStringLiteral("serverInfo"),
              QJsonObject{{QStringLiteral("name"), serverInfo.name},
                          {QStringLiteral("version"), serverInfo.version},
                          {QStringLiteral("title"), serverInfo.title}}}});
    }

    void handleToolsList(const McpSessionId &sessionId, const QJsonValue &id)
    {
        QJsonArray values;
        QStringList names = tools.keys();
        names.sort();
        for (const QString &name : names) {
            values.append(Internal::encodeTool(tools.value(name).descriptor));
        }
        sendResult(sessionId, id,
                   {{QStringLiteral("tools"), values}});
    }

    void handleToolCall(const McpSessionId &sessionId,
                        const QJsonValue &id,
                        const QJsonObject &params)
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        const auto iterator = tools.constFind(name);
        if (iterator == tools.cend()) {
            sendError(sessionId, id, -32602,
                      QStringLiteral("Unknown tool: %1").arg(name));
            return;
        }

        const QJsonObject arguments =
            params.value(QStringLiteral("arguments")).toObject();
        const McpResult<void> validation = Internal::validateJsonSchema(
            iterator->descriptor.inputSchema,
            arguments);
        if (validation.isError()) {
            sendError(sessionId, id, -32602,
                      validation.error().message);
            return;
        }

        try {
            const QJsonArray content =
                iterator->function(QJsonArray{arguments});
            sendResult(sessionId, id,
                       {{QStringLiteral("content"), content},
                        {QStringLiteral("isError"), false}});
        } catch (const std::exception &exception) {
            sendResult(
                sessionId,
                id,
                {{QStringLiteral("content"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("type"), QStringLiteral("text")},
                      {QStringLiteral("text"),
                       QString::fromUtf8(exception.what())}}}},
                 {QStringLiteral("isError"), true}});
        }
    }

    void handleResourcesList(const McpSessionId &sessionId,
                             const QJsonValue &id)
    {
        QJsonArray values;
        QStringList uris = resources.keys();
        uris.sort();
        for (const QString &uri : uris) {
            values.append(
                Internal::encodeResource(resources.value(uri).descriptor));
        }
        sendResult(sessionId, id,
                   {{QStringLiteral("resources"), values}});
    }

    void handleResourceTemplatesList(const McpSessionId &sessionId,
                                     const QJsonValue &id)
    {
        QJsonArray values;
        QStringList templates = resourceTemplates.keys();
        templates.sort();
        for (const QString &uriTemplate : templates) {
            values.append(Internal::encodeResourceTemplate(
                resourceTemplates.value(uriTemplate).descriptor));
        }
        sendResult(sessionId, id,
                   {{QStringLiteral("resourceTemplates"), values}});
    }

    void handleResourceRead(const McpSessionId &sessionId,
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
            sendError(sessionId, id, -32002,
                      QStringLiteral("Resource not found: %1").arg(uri));
            return;
        }

        QJsonArray contents;
        for (const McpResourceContent &content : function(uri)) {
            contents.append(Internal::encodeResourceContent(content));
        }
        sendResult(sessionId, id,
                   {{QStringLiteral("contents"), contents}});
    }

    void handlePromptsList(const McpSessionId &sessionId,
                           const QJsonValue &id)
    {
        QJsonArray values;
        QStringList names = prompts.keys();
        names.sort();
        for (const QString &name : names) {
            values.append(
                Internal::encodePrompt(prompts.value(name).descriptor));
        }
        sendResult(sessionId, id,
                   {{QStringLiteral("prompts"), values}});
    }

    void handlePromptGet(const McpSessionId &sessionId,
                         const QJsonValue &id,
                         const QJsonObject &params)
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        const auto iterator = prompts.constFind(name);
        if (iterator == prompts.cend()) {
            sendError(sessionId, id, -32602,
                      QStringLiteral("Unknown prompt: %1").arg(name));
            return;
        }

        const QJsonObject arguments =
            params.value(QStringLiteral("arguments")).toObject();
        for (const McpPromptArgument &argument :
             iterator->descriptor.arguments) {
            if (argument.required && !arguments.contains(argument.name)) {
                sendError(
                    sessionId,
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
        sendResult(sessionId, id,
                   {{QStringLiteral("messages"), messages}});
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
            [this](const McpSessionId &sessionId,
                   const QJsonObject &message) {
                d->receive(sessionId, message);
            });
}

McpServer::~McpServer() = default;

bool McpServer::addTool(const McpTool &tool, McpToolFunction function)
{
    if (tool.name.isEmpty() || !function || d->tools.contains(tool.name)) {
        return false;
    }
    d->tools.insert(tool.name, {tool, std::move(function)});
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
    return true;
}

QFuture<McpResult<void>> McpServer::start()
{
    d->running = true;
    return d->transport->start();
}

QFuture<McpResult<void>> McpServer::stop()
{
    d->running = false;
    return d->transport->stop();
}

bool McpServer::isRunning() const
{
    return d->running;
}

} // namespace LibMcp
