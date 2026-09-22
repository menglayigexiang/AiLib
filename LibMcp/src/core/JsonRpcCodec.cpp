#include "JsonRpcCodec_p.h"

namespace LibMcp::Internal {

QJsonObject makeRequest(const QString &id,
                        const QString &method,
                        const QJsonObject &params)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("method"), method},
            {QStringLiteral("params"), params}};
}

QJsonObject makeResult(const QJsonValue &id, const QJsonObject &result)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), result}};
}

QJsonObject makeError(const QJsonValue &id,
                      int code,
                      const QString &message,
                      const QJsonValue &data)
{
    QJsonObject error{{QStringLiteral("code"), code},
                      {QStringLiteral("message"), message}};
    if (!data.isUndefined()) {
        error.insert(QStringLiteral("data"), data);
    }
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"), error}};
}

QJsonObject makeNotification(const QString &method,
                             const QJsonObject &params)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("method"), method},
            {QStringLiteral("params"), params}};
}

McpResult<void> validateJsonRpcMessage(const QJsonObject &message)
{
    if (message.value(QStringLiteral("jsonrpc")).toString()
        != QStringLiteral("2.0")) {
        return McpResult<void>::failure(
            {McpErrorCode::InvalidMessage,
             QStringLiteral("jsonrpc 字段必须为 2.0")});
    }
    const bool request = message.contains(QStringLiteral("method"));
    const bool response = message.contains(QStringLiteral("result"))
                          || message.contains(QStringLiteral("error"));
    if (request == response) {
        return McpResult<void>::failure(
            {McpErrorCode::InvalidMessage,
             QStringLiteral("JSON-RPC 消息结构无效")});
    }
    return McpResult<void>::success();
}

} // namespace LibMcp::Internal
