#pragma once

#include <AiLib/stream/StreamEvent.h>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>

namespace AiLib {
inline bool streamError(SdkError& error,         // 流程故障输出
                        const QString& message)  // 构造协议故障，不丢弃 Session 的已有结果
{
    error = {};
    error.category = ErrorCategory::Protocol;
    error.code = QStringLiteral("InvalidStreamResponse");
    error.message = message;
    return false;
}
inline bool readStreamObject(const QByteArray& data,  // 完整 SSE 事件数据
                             QJsonObject& output,     // 规范化输出
                             SdkError& error)         // 严格读取单个 SSE JSON 对象并保留原始诊断
{
    QJsonParseError parseError;     // 当前 SSE 数据的解析错误及偏移
    const QJsonDocument document =  // 当前完整事件的 JSON 树
        QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        streamError(error, QStringLiteral("SSE data must be a JSON object"));
        error.providerError = QString::fromUtf8(data);
        error.details.insert(QStringLiteral("jsonOffset"), parseError.offset);
        return false;
    }
    output = document.object();
    return true;
}
inline bool streamUsage(const QJsonObject& object,  // 原生统计对象
                        const char* input,          // 输入用量字段名称
                        const char* output,         // 输出用量字段名称
                        const char* total,          // 总用量字段名称
                        Usage& usage,               // 已报告用量输出
                        SdkError& error)            // 将厂商统计名称映射为已报告用量，保留未知语义
{
    const auto read = [&](const char* name,                                // 当前原生事件名称
                          std::optional<qint64>& field) {                  // 读取给定名称的非负整数用量
        const QJsonValue value = object.value(QString::fromLatin1(name));  // 当前用量原始字段
        if (value.isNull() || value.isUndefined())
            return true;
        const double number = value.toDouble(-1);  // 进行整数及范围校验的数值
        if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
            std::floor(number) != number || number >= 9223372036854775808.0)
            return streamError(error, QStringLiteral("Usage must be a nonnegative integer"));
        field = static_cast<qint64>(number);
        return true;
    };
    return read(input, usage.inputTokens) && read(output, usage.outputTokens) &&
           read(total, usage.totalTokens);
}
inline StreamEvent partEvent(StreamEventType type, int index)  // 构造给定逻辑索引的 Part 事件
{
    StreamEvent event;  // 待填充的标准化通知
    event.type = type;
    event.partIndex = index;
    return event;
}
inline bool emitStreamError(const QJsonObject& root,      // 当前原生事件对象
                            const StreamEventSink& sink,  // 标准化事件接收端
                            SdkError& error)              // 将流内 Provider 错误标准化并交给 Session
{
    const QJsonObject detail =  // Provider 原生错误属性
        root.value(QStringLiteral("error")).toObject();
    StreamEvent event;  // 标准化流程故障通知
    event.type = StreamEventType::Error;
    event.error.category = detail.value(QStringLiteral("type")) == QJsonValue("rate_limit_error")
                               ? ErrorCategory::RateLimited
                               : ErrorCategory::Provider;
    event.error.code = QStringLiteral("ProviderStreamError");
    event.error.message =
        detail.value(QStringLiteral("message")).toString(QStringLiteral("Provider stream failed"));
    event.error.providerError = root;
    event.error.details.insert(QStringLiteral("providerCode"),
                               detail.value(QStringLiteral("type")).toString());
    return sink(event, error);
}
}  // AiLib 命名空间结束
