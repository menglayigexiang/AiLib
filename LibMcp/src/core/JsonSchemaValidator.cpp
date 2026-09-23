#include "JsonSchemaValidator_p.h"

#include <QJsonDocument>
#include <QJsonArray>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <exception>
#include <string>

namespace LibMcp::Internal {
namespace {

jsoncons::json toJsoncons(const QJsonValue& value)  // 将 Qt JSON 值转换为校验器的内部 JSON 值
{
    QByteArray encoded;  // 保存 Qt 生成的紧凑 JSON 文本
    if (value.isObject()) {
        encoded = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    } else if (value.isArray()) {
        encoded = QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    } else {
        const QJsonArray wrapper{value};  // 借助单元数组序列化 Qt 不能单独写出的标量值
        const QByteArray wrapperText =  // 保存包含标量的临时 JSON 数组
            QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
        const jsoncons::json parsedWrapper =  // 解析临时数组以取出原始标量
            jsoncons::json::parse(wrapperText.constData());
        return parsedWrapper.at(0);
    }

    return jsoncons::json::parse(encoded.constData());
}

McpResult<void> validationError(
    const QString& message)  // 将第三方异常归一化为 LibMcp 内部校验错误
{
    return McpResult<void>::failure(
        {McpErrorCode::SchemaValidationFailed, message});
}

}  // namespace

McpResult<void> validateJsonSchemaDocument(
    const QJsonObject& schema)  // 验证 Schema 本身是否符合 JSON Schema 2020-12
{
    try {
        const jsoncons::json schemaValue =  // 转换待编译的 Schema 文档
            toJsoncons(schema);
        const jsoncons::json metaSchemaValue =  // 获取 jsoncons 内置的 Draft 2020-12 metaschema
            jsoncons::jsonschema::draft202012::
                schema_draft202012<jsoncons::json>::get_schema();
        const auto compiledMetaSchema =  // 编译 metaschema 以校验业务 Schema 文档
            jsoncons::jsonschema::make_json_schema(metaSchemaValue);
        compiledMetaSchema.validate(schemaValue);
        return McpResult<void>::success();
    } catch (const std::exception& exception) {
        return validationError(
            QStringLiteral("JSON Schema 无效：%1")
                .arg(QString::fromUtf8(exception.what())));
    }
}

McpResult<void> validateJsonValue(
    const QJsonObject& schema,  // 用于校验实例的 JSON Schema 2020-12 文档
    const QJsonValue& value)    // 验证 JSON 实例是否符合指定 Schema
{
    const McpResult<void> schemaResult =  // 先阻止非法 Schema 进入实例校验
        validateJsonSchemaDocument(schema);
    if (schemaResult.isError()) {
        return schemaResult;
    }

    try {
        const jsoncons::json schemaValue =  // 转换待编译的 Schema 文档
            toJsoncons(schema);
        const jsoncons::json instanceValue =  // 转换待校验的 JSON 实例
            toJsoncons(value);
        const auto compiledSchema =  // 以 Draft 2020-12 默认方言编译 Schema
            jsoncons::jsonschema::make_json_schema(schemaValue);
        compiledSchema.validate(instanceValue);
        return McpResult<void>::success();
    } catch (const std::exception& exception) {
        return validationError(
            QStringLiteral("JSON 实例不符合 Schema：%1")
                .arg(QString::fromUtf8(exception.what())));
    }
}

}  // namespace LibMcp::Internal
