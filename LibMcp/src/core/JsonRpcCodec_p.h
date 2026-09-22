#pragma once

#include <LibMcp/McpResult.h>

#include <QJsonObject>

namespace LibMcp::Internal {

QJsonObject makeRequest(const QString &id,
                        const QString &method,
                        const QJsonObject &params);
QJsonObject makeResult(const QJsonValue &id, const QJsonObject &result);
QJsonObject makeError(const QJsonValue &id,
                      int code,
                      const QString &message,
                      const QJsonValue &data = {});
QJsonObject makeNotification(const QString &method,
                             const QJsonObject &params = {});
McpResult<void> validateJsonRpcMessage(const QJsonObject &message);

} // namespace LibMcp::Internal
