#pragma once
#include <LibMcp/McpResult.h>
#include <QJsonObject>
namespace LibMcp::Internal {
McpResult<void> validateJsonSchema(const QJsonObject&,const QJsonValue&,const QString& = QStringLiteral("$"));
}
