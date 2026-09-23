#pragma once

#include <LibMcp/McpResult.h>

#include <QJsonValue>
#include <QString>

namespace LibMcp::Internal {

McpResult<void> validateProtocolDefinition(
    const QString& definitionName,  // 官方 Schema 中需要应用的 $defs 名称
    const QJsonValue& value);       // 按固定 2026-07-28 Schema 校验 Wire JSON 值

} // namespace LibMcp::Internal
