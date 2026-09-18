#include "SchemaValidator.h"
#include <QJsonArray>
#include <QSet>
#include <cmath>
namespace AiLib {
namespace {
bool checkSchema(const QJsonObject& schema,  // 当前参数约束
                 QString& reason,            // 验证失败说明输出
                 int depth)                  // 递归检查 Schema，限制深度避免栈耗尽
{
    if (depth > 64) {
        reason = QStringLiteral("Schema 嵌套超过 64 层");
        return false;
    }
    const QSet<QString> supported{
        // 当前支持的断言及注释关键字
        QStringLiteral("type"),        QStringLiteral("properties"),
        QStringLiteral("required"),    QStringLiteral("additionalProperties"),
        QStringLiteral("items"),       QStringLiteral("enum"),
        QStringLiteral("description"), QStringLiteral("title")};
    for (auto it = schema.begin(); it != schema.end(); ++it) {  // 逐项检查声明
        if (!supported.contains(it.key())) {
            reason = QStringLiteral("不支持的 Schema 关键字：") + it.key();
            return false;
        }
    }
    if (schema.contains("type")) {
        const QString type = schema.value("type").toString();  // 当前声明的类型
        const QStringList types{"object",  "array",   "string", "number",
                                "integer", "boolean", "null"};  // 支持的单一类型
        if (!types.contains(type)) {
            reason = QStringLiteral("type 必须是受支持的单一类型");
            return false;
        }
    }
    for (const auto& key :
         {QStringLiteral("description"), QStringLiteral("title")}) {  // 验证注释字段类型
        if (schema.contains(key) && !schema.value(key).isString()) {
            reason = key + QStringLiteral(" 必须是字符串");
            return false;
        }
    }
    if (schema.contains("additionalProperties") && !schema.value("additionalProperties").isBool()) {
        reason = QStringLiteral("additionalProperties 仅支持 bool");
        return false;
    }
    if (schema.contains("required")) {
        if (!schema.value("required").isArray()) {
            reason = QStringLiteral("required 必须是字符串数组");
            return false;
        }
        QSet<QString> names;                                           // 已声明的必填名称
        for (const auto& name : schema.value("required").toArray()) {  // 验证必填名称
            if (!name.isString() || names.contains(name.toString())) {
                reason = QStringLiteral("required 存在非法或重复名称");
                return false;
            }
            names.insert(name.toString());
        }
    }
    if (schema.contains("enum") &&
        (!schema.value("enum").isArray() || schema.value("enum").toArray().isEmpty())) {
        reason = QStringLiteral("enum 必须是非空数组");
        return false;
    }
    if (schema.contains("enum")) {
        const auto values = schema.value("enum").toArray();      // 枚举允许的值集合
        for (int index = 0; index < values.size(); ++index) {    // 验证枚举声明不能重复
            for (int earlier = 0; earlier < index; ++earlier) {  // 已检查的枚举项
                if (values.at(index) == values.at(earlier)) {
                    reason = QStringLiteral("enum 存在重复值");
                    return false;
                }
            }
        }
    }
    if (schema.contains("properties")) {
        if (!schema.value("properties").isObject()) {
            reason = QStringLiteral("properties 必须是对象");
            return false;
        }
        const auto properties = schema.value("properties").toObject();      // 子字段声明
        for (auto it = properties.begin(); it != properties.end(); ++it) {  // 验证各字段 Schema
            if (!it.value().isObject() || !checkSchema(it.value().toObject(), reason, depth + 1)) {
                if (reason.isEmpty())
                    reason = QStringLiteral("字段 Schema 必须是对象");
                return false;
            }
        }
    }
    if (schema.contains("items") &&
        (!schema.value("items").isObject() ||
         !checkSchema(schema.value("items").toObject(), reason, depth + 1))) {
        if (reason.isEmpty())
            reason = QStringLiteral("items 必须是 Schema 对象");
        return false;
    }
    return true;
}
bool checkValue(const QJsonObject& schema,  // 当前参数约束
                const QJsonValue& value,    // 当前 JSON 参数值
                const QString& path,        // 当前字段路径
                QString& reason,            // 验证失败说明输出
                int depth)                  // 按字段路径检查 JSON 值
{
    if (depth > 64) {
        reason = path + QStringLiteral("：参数嵌套超过 64 层");
        return false;
    }
    const QString type = schema.value("type").toString();  // 可选的类型约束
    bool matches =
        type.isEmpty() || (type == "object" && value.isObject()) ||  // 类型检查结果
        (type == "array" && value.isArray()) || (type == "string" && value.isString()) ||
        (type == "boolean" && value.isBool()) || (type == "null" && value.isNull()) ||
        (type == "number" && value.isDouble()) ||
        (type == "integer" && value.isDouble() && std::floor(value.toDouble()) == value.toDouble());
    if (!matches) {
        reason = path + QStringLiteral("：类型不符合 ") + type;
        return false;
    }
    if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) {
        reason = path + QStringLiteral("：值不在 enum 中");
        return false;
    }
    if (value.isObject()) {
        const auto object = value.toObject();                           // 当前对象参数
        const auto properties = schema.value("properties").toObject();  // 已知字段声明
        for (const auto& name : schema.value("required").toArray()) {   // 检查必填字段
            if (!object.contains(name.toString())) {
                reason = path + "." + name.toString() + QStringLiteral("：缺少必填字段");
                return false;
            }
        }
        for (auto it = object.begin(); it != object.end(); ++it) {  // 检查每个实际字段
            if (properties.contains(it.key())) {
                if (!checkValue(properties.value(it.key()).toObject(), it.value(),
                                path + "." + it.key(), reason, depth + 1))
                    return false;
            } else if (schema.contains("additionalProperties") &&
                       !schema.value("additionalProperties").toBool()) {
                reason = path + "." + it.key() + QStringLiteral("：不允许额外字段");
                return false;
            }
        }
    }
    if (value.isArray() && schema.contains("items")) {
        const auto array = value.toArray();                   // 当前数组参数
        for (int index = 0; index < array.size(); ++index) {  // 验证数组中各项
            if (!checkValue(schema.value("items").toObject(), array.at(index),
                            path + "[" + QString::number(index) + "]", reason, depth + 1))
                return false;
        }
    }
    return true;
}
}
bool validateToolSchema(const QJsonObject& schema,  // 当前参数约束
                        SdkError& error)            // 拒绝无法完整验证的 Schema 声明
{
    error = {};
    QString reason;  // Schema 检查失败说明
    if (checkSchema(schema, reason, 0))
        return true;
    error.category = ErrorCategory::InvalidArgument;
    error.code = QStringLiteral("InvalidToolSchema");
    error.message = reason;
    return false;
}
bool validateToolArguments(const QJsonObject& schema,     // 当前参数约束
                           const QJsonObject& arguments,  // 待验证的对象参数
                           QString& reason)               // 检查对象参数并返回字段路径
{
    reason.clear();
    return checkValue(schema, arguments, QStringLiteral("$"), reason, 0);
}
}
