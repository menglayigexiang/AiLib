#include "ProtocolSchemaValidator_p.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <exception>

namespace LibMcp::Internal {
namespace {

jsoncons::json toJsoncons(const QJsonValue& value)  // 将 Qt JSON 值转换为 jsoncons 内部值
{
    if (value.isObject()) {
        return jsoncons::json::parse(
            QJsonDocument(value.toObject())
                .toJson(QJsonDocument::Compact)
                .constData());
    }
    if (value.isArray()) {
        return jsoncons::json::parse(
            QJsonDocument(value.toArray())
                .toJson(QJsonDocument::Compact)
                .constData());
    }
    const QJsonArray wrapper{value};  // 借助数组序列化标量 JSON 值
    return jsoncons::json::parse(
               QJsonDocument(wrapper)
                   .toJson(QJsonDocument::Compact)
                   .constData())
        .at(0);
}

McpResult<QJsonObject> loadProtocolSchema()  // 从只读资源加载固定官方 Schema
{
    QFile file(QStringLiteral(":/libmcp/protocol/mcp-2026-07-28.schema.json"));  // 打开随动态库嵌入的 Schema
    if (!file.open(QIODevice::ReadOnly)) {
        return McpResult<QJsonObject>::failure(
            {McpErrorCode::InternalError,
             QStringLiteral("无法读取内置 MCP 2026-07-28 Schema")});
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());  // 解析官方 Schema 文档
    if (!document.isObject()) {
        return McpResult<QJsonObject>::failure(
            {McpErrorCode::InternalError,
             QStringLiteral("内置 MCP 2026-07-28 Schema 无效")});
    }
    return McpResult<QJsonObject>::success(document.object());
}

} // namespace

McpResult<void> validateProtocolDefinition(
    const QString& definitionName,  // 官方 Schema 中需要应用的 $defs 名称
    const QJsonValue& value)        // 按固定 2026-07-28 Schema 校验 Wire JSON 值
{
    const McpResult<QJsonObject> loaded = loadProtocolSchema();  // 加载唯一协议真相源
    if (loaded.isError()) {
        return McpResult<void>::failure(loaded.error());
    }
    QJsonObject schema = loaded.value();  // 复制 Schema 并选择指定定义作为根
    schema.insert(QStringLiteral("$ref"),
                  QStringLiteral("#/$defs/%1").arg(definitionName));
    try {
        const auto compiled = jsoncons::jsonschema::make_json_schema(  // 编译带本地引用的官方 Schema
            toJsoncons(schema));
        compiled.validate(toJsoncons(value));
        return McpResult<void>::success();
    } catch (const std::exception& exception) {
        return McpResult<void>::failure(
            {McpErrorCode::SchemaValidationFailed,
             QStringLiteral("MCP Wire 值不符合 %1：%2")
                 .arg(definitionName,
                      QString::fromUtf8(exception.what()))});
    }
}

} // namespace LibMcp::Internal
