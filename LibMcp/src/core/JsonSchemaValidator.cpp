#include "JsonSchemaValidator_p.h"

#include <QJsonArray>

namespace LibMcp::Internal {
namespace {

bool matchesType(const QString &type, const QJsonValue &value)
{
    if (type == QStringLiteral("object")) {
        return value.isObject();
    }
    if (type == QStringLiteral("array")) {
        return value.isArray();
    }
    if (type == QStringLiteral("string")) {
        return value.isString();
    }
    if (type == QStringLiteral("number")) {
        return value.isDouble();
    }
    if (type == QStringLiteral("integer")) {
        return value.isDouble()
               && value.toDouble()
                      == static_cast<qint64>(value.toDouble());
    }
    if (type == QStringLiteral("boolean")) {
        return value.isBool();
    }
    if (type == QStringLiteral("null")) {
        return value.isNull();
    }
    return true;
}

McpResult<void> validationError(const QString &message)
{
    return McpResult<void>::failure(
        {McpErrorCode::SchemaValidationFailed, message});
}

} // namespace

McpResult<void> validateJsonSchema(const QJsonObject &schema,
                                   const QJsonValue &value,
                                   const QString &path)
{
    const QString type = schema.value(QStringLiteral("type")).toString();
    if (!type.isEmpty() && !matchesType(type, value)) {
        return validationError(
            QStringLiteral("%1 不符合类型 %2").arg(path, type));
    }

    const QJsonArray allowedValues =
        schema.value(QStringLiteral("enum")).toArray();
    if (!allowedValues.isEmpty() && !allowedValues.contains(value)) {
        return validationError(
            QStringLiteral("%1 的值不在允许范围内").arg(path));
    }

    if (!value.isObject()) {
        return McpResult<void>::success();
    }

    const QJsonObject object = value.toObject();
    for (const QJsonValue &required :
         schema.value(QStringLiteral("required")).toArray()) {
        if (required.isString() && !object.contains(required.toString())) {
            return validationError(
                QStringLiteral("%1.%2 是必填字段")
                    .arg(path, required.toString()));
        }
    }

    const QJsonObject properties =
        schema.value(QStringLiteral("properties")).toObject();
    for (auto iterator = properties.constBegin();
         iterator != properties.constEnd();
         ++iterator) {
        if (!object.contains(iterator.key()) || !iterator.value().isObject()) {
            continue;
        }
        const McpResult<void> result = validateJsonSchema(
            iterator.value().toObject(),
            object.value(iterator.key()),
            path + QLatin1Char('.') + iterator.key());
        if (result.isError()) {
            return result;
        }
    }
    return McpResult<void>::success();
}

} // namespace LibMcp::Internal
