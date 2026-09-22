#include "McpProtocolCodec_p.h"

#include <LibMcp/LibMcpGlobal.h>

#include <QJsonArray>

#include <type_traits>

namespace LibMcp::Internal {
namespace {

void insertOptional(QJsonObject &object,
                    const QString &name,
                    const std::optional<QString> &value)
{
    if (value) {
        object.insert(name, *value);
    }
}

McpError invalidResponse(const QString &message)
{
    return {McpErrorCode::InvalidResponse, message};
}

} // namespace

QJsonObject requestMeta()
{
    return {
        {QStringLiteral("io.modelcontextprotocol/protocolVersion"),
         QStringLiteral(LIBMCP_PROTOCOL_VERSION)}};
}

QJsonObject encodeTool(const McpTool &tool)
{
    QJsonObject object{{QStringLiteral("name"), tool.name},
                       {QStringLiteral("inputSchema"), tool.inputSchema}};
    insertOptional(object, QStringLiteral("title"), tool.title);
    insertOptional(object, QStringLiteral("description"), tool.description);
    if (!tool.outputSchema.isEmpty()) {
        object.insert(QStringLiteral("outputSchema"), tool.outputSchema);
    }
    if (!tool.annotations.isEmpty()) {
        object.insert(QStringLiteral("annotations"), tool.annotations);
    }
    if (!tool.meta.isEmpty()) {
        object.insert(QStringLiteral("_meta"), tool.meta);
    }
    return object;
}

McpResult<McpTool> decodeTool(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("name")).isString()
        || !object.value(QStringLiteral("inputSchema")).isObject()) {
        return McpResult<McpTool>::failure(
            invalidResponse(QStringLiteral("工具描述无效")));
    }

    McpTool tool;
    tool.name = object.value(QStringLiteral("name")).toString();
    tool.inputSchema =
        object.value(QStringLiteral("inputSchema")).toObject();
    if (object.value(QStringLiteral("title")).isString()) {
        tool.title = object.value(QStringLiteral("title")).toString();
    }
    if (object.value(QStringLiteral("description")).isString()) {
        tool.description =
            object.value(QStringLiteral("description")).toString();
    }
    tool.outputSchema =
        object.value(QStringLiteral("outputSchema")).toObject();
    tool.annotations =
        object.value(QStringLiteral("annotations")).toObject();
    tool.meta = object.value(QStringLiteral("_meta")).toObject();
    return McpResult<McpTool>::success(std::move(tool));
}

QJsonObject encodeResource(const McpResource &resource)
{
    QJsonObject object{{QStringLiteral("name"), resource.name},
                       {QStringLiteral("uri"), resource.uri}};
    insertOptional(object, QStringLiteral("title"), resource.title);
    insertOptional(object,
                   QStringLiteral("description"),
                   resource.description);
    insertOptional(object, QStringLiteral("mimeType"), resource.mimeType);
    if (resource.size) {
        object.insert(QStringLiteral("size"),
                      static_cast<double>(*resource.size));
    }
    if (!resource.meta.isEmpty()) {
        object.insert(QStringLiteral("_meta"), resource.meta);
    }
    return object;
}

McpResult<McpResource> decodeResource(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("name")).isString()
        || !object.value(QStringLiteral("uri")).isString()) {
        return McpResult<McpResource>::failure(
            invalidResponse(QStringLiteral("资源描述无效")));
    }

    McpResource resource;
    resource.name = object.value(QStringLiteral("name")).toString();
    resource.uri = object.value(QStringLiteral("uri")).toString();
    if (object.value(QStringLiteral("title")).isString()) {
        resource.title = object.value(QStringLiteral("title")).toString();
    }
    if (object.value(QStringLiteral("description")).isString()) {
        resource.description =
            object.value(QStringLiteral("description")).toString();
    }
    if (object.value(QStringLiteral("mimeType")).isString()) {
        resource.mimeType =
            object.value(QStringLiteral("mimeType")).toString();
    }
    if (object.value(QStringLiteral("size")).isDouble()) {
        resource.size =
            static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
    }
    resource.meta = object.value(QStringLiteral("_meta")).toObject();
    return McpResult<McpResource>::success(std::move(resource));
}

QJsonObject encodeResourceTemplate(
    const McpResourceTemplate &resourceTemplate)
{
    QJsonObject object{
        {QStringLiteral("name"), resourceTemplate.name},
        {QStringLiteral("uriTemplate"), resourceTemplate.uriTemplate}};
    insertOptional(object, QStringLiteral("title"), resourceTemplate.title);
    insertOptional(object,
                   QStringLiteral("description"),
                   resourceTemplate.description);
    insertOptional(object,
                   QStringLiteral("mimeType"),
                   resourceTemplate.mimeType);
    if (!resourceTemplate.meta.isEmpty()) {
        object.insert(QStringLiteral("_meta"), resourceTemplate.meta);
    }
    return object;
}

McpResult<McpResourceTemplate> decodeResourceTemplate(
    const QJsonObject &object)
{
    if (!object.value(QStringLiteral("name")).isString()
        || !object.value(QStringLiteral("uriTemplate")).isString()) {
        return McpResult<McpResourceTemplate>::failure(
            invalidResponse(QStringLiteral("资源模板描述无效")));
    }

    McpResourceTemplate resourceTemplate;
    resourceTemplate.name =
        object.value(QStringLiteral("name")).toString();
    resourceTemplate.uriTemplate =
        object.value(QStringLiteral("uriTemplate")).toString();
    if (object.value(QStringLiteral("title")).isString()) {
        resourceTemplate.title =
            object.value(QStringLiteral("title")).toString();
    }
    if (object.value(QStringLiteral("description")).isString()) {
        resourceTemplate.description =
            object.value(QStringLiteral("description")).toString();
    }
    if (object.value(QStringLiteral("mimeType")).isString()) {
        resourceTemplate.mimeType =
            object.value(QStringLiteral("mimeType")).toString();
    }
    resourceTemplate.meta =
        object.value(QStringLiteral("_meta")).toObject();
    return McpResult<McpResourceTemplate>::success(
        std::move(resourceTemplate));
}

QJsonObject encodeResourceContent(const McpResourceContent &content)
{
    return std::visit(
        [](const auto &value) {
            QJsonObject object{{QStringLiteral("uri"), value.uri}};
            insertOptional(object,
                           QStringLiteral("mimeType"),
                           value.mimeType);
            if (!value.meta.isEmpty()) {
                object.insert(QStringLiteral("_meta"), value.meta);
            }

            using ContentType = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<ContentType,
                                         McpTextResourceContent>) {
                object.insert(QStringLiteral("text"), value.text);
            } else {
                object.insert(
                    QStringLiteral("blob"),
                    QString::fromLatin1(value.data.toBase64()));
            }
            return object;
        },
        content);
}

McpResult<McpResourceContent> decodeResourceContent(
    const QJsonObject &object)
{
    if (!object.value(QStringLiteral("uri")).isString()) {
        return McpResult<McpResourceContent>::failure(
            invalidResponse(QStringLiteral("资源内容缺少 URI")));
    }

    const QString uri = object.value(QStringLiteral("uri")).toString();
    const QJsonObject meta =
        object.value(QStringLiteral("_meta")).toObject();
    std::optional<QString> mimeType;
    if (object.value(QStringLiteral("mimeType")).isString()) {
        mimeType =
            object.value(QStringLiteral("mimeType")).toString();
    }

    if (object.value(QStringLiteral("text")).isString()) {
        McpTextResourceContent content{
            uri,
            mimeType,
            object.value(QStringLiteral("text")).toString(),
            meta};
        return McpResult<McpResourceContent>::success(
            McpResourceContent{std::move(content)});
    }
    if (object.value(QStringLiteral("blob")).isString()) {
        McpBlobResourceContent content{
            uri,
            mimeType,
            QByteArray::fromBase64(
                object.value(QStringLiteral("blob")).toString().toLatin1()),
            meta};
        return McpResult<McpResourceContent>::success(
            McpResourceContent{std::move(content)});
    }
    return McpResult<McpResourceContent>::failure(
        invalidResponse(QStringLiteral("资源内容缺少 text 或 blob")));
}

QJsonObject encodePrompt(const McpPrompt &prompt)
{
    QJsonObject object{{QStringLiteral("name"), prompt.name}};
    insertOptional(object, QStringLiteral("title"), prompt.title);
    insertOptional(object,
                   QStringLiteral("description"),
                   prompt.description);

    QJsonArray arguments;
    for (const McpPromptArgument &argument : prompt.arguments) {
        QJsonObject encoded{
            {QStringLiteral("name"), argument.name},
            {QStringLiteral("required"), argument.required}};
        insertOptional(encoded,
                       QStringLiteral("description"),
                       argument.description);
        arguments.append(encoded);
    }
    if (!arguments.isEmpty()) {
        object.insert(QStringLiteral("arguments"), arguments);
    }
    if (!prompt.meta.isEmpty()) {
        object.insert(QStringLiteral("_meta"), prompt.meta);
    }
    return object;
}

McpResult<McpPrompt> decodePrompt(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("name")).isString()) {
        return McpResult<McpPrompt>::failure(
            invalidResponse(QStringLiteral("Prompt 描述无效")));
    }

    McpPrompt prompt;
    prompt.name = object.value(QStringLiteral("name")).toString();
    if (object.value(QStringLiteral("title")).isString()) {
        prompt.title = object.value(QStringLiteral("title")).toString();
    }
    if (object.value(QStringLiteral("description")).isString()) {
        prompt.description =
            object.value(QStringLiteral("description")).toString();
    }
    for (const QJsonValue &value :
         object.value(QStringLiteral("arguments")).toArray()) {
        const QJsonObject encoded = value.toObject();
        McpPromptArgument argument;
        argument.name =
            encoded.value(QStringLiteral("name")).toString();
        argument.required =
            encoded.value(QStringLiteral("required")).toBool();
        if (encoded.value(QStringLiteral("description")).isString()) {
            argument.description =
                encoded.value(QStringLiteral("description")).toString();
        }
        prompt.arguments.append(std::move(argument));
    }
    prompt.meta = object.value(QStringLiteral("_meta")).toObject();
    return McpResult<McpPrompt>::success(std::move(prompt));
}

QJsonObject encodePromptMessage(const McpPromptMessage &message)
{
    const QString role = message.role == McpRole::User
                             ? QStringLiteral("user")
                             : QStringLiteral("assistant");
    return {{QStringLiteral("role"), role},
            {QStringLiteral("content"), message.content}};
}

McpResult<McpPromptMessage> decodePromptMessage(
    const QJsonObject &object)
{
    const QString role =
        object.value(QStringLiteral("role")).toString();
    if ((role != QStringLiteral("user")
         && role != QStringLiteral("assistant"))
        || !object.value(QStringLiteral("content")).isObject()) {
        return McpResult<McpPromptMessage>::failure(
            invalidResponse(QStringLiteral("Prompt 消息无效")));
    }

    return McpResult<McpPromptMessage>::success(
        {role == QStringLiteral("user") ? McpRole::User
                                        : McpRole::Assistant,
         object.value(QStringLiteral("content")).toObject()});
}

} // namespace LibMcp::Internal
