#pragma once

#include <LibMcp/McpResult.h>

#include <QJsonObject>

namespace LibMcp::Internal {

McpResult<void> validateJsonSchemaDocument(
    const QJsonObject& schema);  // 验证 Schema 本身是否符合 JSON Schema 2020-12
McpResult<void> validateJsonValue(
    const QJsonObject& schema,  // 用于校验实例的 JSON Schema 2020-12 文档
    const QJsonValue& value);   // 验证 JSON 实例是否符合指定 Schema

}  // namespace LibMcp::Internal
