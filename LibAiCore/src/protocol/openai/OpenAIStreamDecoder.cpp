#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include "../../stream/SseDecoder.h"
#include "../../stream/DecoderHelpers.h"
#include <QJsonArray>
#include <QMap>

namespace AiLib {
namespace {
// 解析一次 Chat Completions SSE 请求，维护索引映射，不聚合最终工具参数。
class OpenAIStreamDecoder final : public IStreamDecoder {
public:
    bool feed(const QByteArray& bytes,      // 原始网络片段
              const StreamEventSink& sink,  // 标准化事件接收端
              SdkError& error) override     // 解析完整 SSE 事件并输出标准化通知
    {
        return m_sse.feed(
            bytes,
            [&](const QString&, const QByteArray& data,  // 完整事件的原始数据
                SdkError& failure) {                     // 解释当前完整事件的数据，忽略无语义的 SSE 名称
                const bool success =                     // 当前原生事件是否被正常解析和聚合
                    decode(data, sink, failure);
                if (!success &&
                    (failure.providerError.isNull() || failure.providerError.isUndefined()))
                    failure.providerError = QString::fromUtf8(data);
                return success;
            },
            error);
    }
    bool finish(const StreamEventSink& sink,  // 标准化事件接收端
                SdkError& error) override     // 网络关闭后必须已经收到合法协议结束标记
    {
        if (!m_sse.finish(
                [&](const QString&, const QByteArray& data,  // 完整事件的原始数据
                    SdkError& failure) {                     // 解释 EOF 处最后一个完整 SSE 事件
                    const bool success =                     // 当前原生事件是否被正常解析和聚合
                        decode(data, sink, failure);
                    if (!success &&
                        (failure.providerError.isNull() || failure.providerError.isUndefined()))
                        failure.providerError = QString::fromUtf8(data);
                    return success;
                },
                error))
            return false;
        if (!m_done)
            return streamError(
                error, QStringLiteral("Chat stream ended without its protocol completion marker"));
        return true;
    }

private:
    int ensurePart(const QString& key,           // 原生字段或逻辑定位键
                   StreamPartType type,          // 标准化事件或内容类别
                   const StreamEventSink& sink,  // 标准化事件接收端
                   SdkError& error)              // 将厂商定位方式映射为请求内逻辑索引
    {
        if (m_parts.contains(key))
            return m_parts.value(key);
        const int index = m_parts.size();  // 按首次出现次序分配的稳定逻辑标识
        StreamEvent event =                // 声明逻辑块的标准化事件
            partEvent(StreamEventType::PartStarted, index);
        event.partType = type;
        if (!sink(event, error))
            return -1;
        m_parts.insert(key, index);
        m_types.insert(index, type);
        return index;
    }
    bool textDelta(const QJsonObject& delta,     // 当前原生增量对象
                   const QString& key,           // 原生字段或逻辑定位键
                   StreamPartType type,          // 标准化事件或内容类别
                   const StreamEventSink& sink,  // 标准化事件接收端
                   SdkError& error)              // 将文本或推理片段转换为对应逻辑块的增量
    {
        const QJsonValue value = delta.value(key);  // 当前厂商文本字段
        if (value.isNull() || value.isUndefined())
            return true;
        if (!value.isString())
            return streamError(error, QStringLiteral("Stream text delta must be a string"));
        if (value.toString().isEmpty())
            return true;
        const int index = ensurePart(key, type, sink, error);  // 当前字段对应的稳定逻辑内容块
        if (index < 0)
            return false;
        StreamEvent event =  // 追加文本通知
            partEvent(type == StreamPartType::Text ? StreamEventType::TextDelta
                                                   : StreamEventType::ReasoningDelta,
                      index);
        event.delta = value.toString();
        return sink(event, error);
    }
    bool toolsDelta(const QJsonValue& value,      // 当前原生字段值
                    const StreamEventSink& sink,  // 标准化事件接收端
                    SdkError& error)              // 将工具原始索引及属性规范化，不保存累计参数
    {
        if (value.isNull() || value.isUndefined())
            return true;
        if (!value.isArray())
            return streamError(error, QStringLiteral("tool_calls delta must be an array"));
        for (const auto& item : value.toArray()) {  // 当前原生工具分片
            if (!item.isObject())
                return streamError(error, QStringLiteral("Tool delta must be an object"));
            const QJsonObject tool = item.toObject();  // 当前工具分片的原生属性
            const double rawIndex =                    // 厂商原始索引，仅用于 Decoder 映射
                tool.value("index").toDouble(-1);
            if (!tool.value("index").isDouble() || rawIndex < 0 || rawIndex > 2147483647 ||
                std::floor(rawIndex) != rawIndex)
                return streamError(
                    error, QStringLiteral("Tool delta requires a nonnegative integer index"));
            if (tool.contains("type") && tool.value("type") != QJsonValue("function"))
                return streamError(error, QStringLiteral("Only function tools are supported"));
            if (tool.contains("function") && !tool.value("function").isObject())
                return streamError(error, QStringLiteral("Invalid function delta"));
            const QJsonObject function =  // 原生函数名称和参数片段
                tool.value("function").toObject();
            const int index =  // 与调用 ID 无关的逻辑索引
                ensurePart(QStringLiteral("tool:") + QString::number(static_cast<int>(rawIndex)),
                           StreamPartType::ToolCall, sink, error);
            if (index < 0)
                return false;
            StreamEvent event =  // 工具属性及参数通知
                partEvent(StreamEventType::ToolCallDelta, index);
            if (tool.contains("id")) {
                if (!tool.value("id").isString() || tool.value("id").toString().trimmed().isEmpty())
                    return streamError(error, QStringLiteral("Native ToolCall ID is invalid"));
                event.toolCallId = tool.value("id").toString();
            }
            if (function.contains("name")) {
                if (!function.value("name").isString())
                    return streamError(error, QStringLiteral("Tool name delta must be a string"));
                event.toolNameDelta = function.value("name").toString();
            }
            if (function.contains("arguments")) {
                if (!function.value("arguments").isString())
                    return streamError(error,
                                       QStringLiteral("Tool arguments delta must be a string"));
                event.delta = function.value("arguments").toString();
            }
            if (!sink(event, error))
                return false;
        }
        return true;
    }
    bool decode(const QByteArray& data,       // 完整 SSE 事件数据
                const StreamEventSink& sink,  // 标准化事件接收端
                SdkError& error)              // 解释单候选 chunk、Usage 和协议结束
    {
        if (m_done)
            return streamError(error, QStringLiteral("Data arrived after Chat stream completion"));
        if (data.trimmed() == "[DONE]") {
            if (!m_sawChoice || !m_finished)
                return streamError(
                    error, QStringLiteral("Completion marker arrived before a candidate finished"));
            StreamEvent event;  // 独立于 FinishReason 的协议结束通知
            event.type = StreamEventType::ResponseCompleted;
            if (!sink(event, error))
                return false;
            m_done = true;
            return true;
        }
        QJsonObject root;  // 当前完整 chunk 的 JSON 内容
        if (!readStreamObject(data, root, error))
            return false;
        if (root.contains("error"))
            return emitStreamError(root, sink, error);
        if (!root.value("choices").isArray())
            return streamError(error, QStringLiteral("Chat chunk requires choices"));
        const QJsonArray choices = root.value("choices").toArray();  // 当前 chunk 的候选集合
        if (choices.size() > 1)
            return streamError(error,
                               QStringLiteral("Multiple stream candidates are not supported"));
        if (choices.isEmpty() && (!m_sawChoice || !root.value("usage").isObject()))
            return streamError(
                error, QStringLiteral("Zero candidates are only allowed in trailing usage chunks"));
        StreamEvent metadata;  // 响应级标识更新，不感知厂商索引
        metadata.type = StreamEventType::ResponseMetadataUpdated;
        metadata.responseId = root.value("id").toString();
        metadata.model = root.value("model").toString();
        if (!sink(metadata, error))
            return false;
        if (!choices.isEmpty()) {
            if (m_finished || !choices.first().isObject())
                return streamError(
                    error, QStringLiteral(
                               "Candidate delta arrived after FinishReason or has invalid shape"));
            const QJsonObject choice = choices.first().toObject();  // 唯一候选的原生分片
            if (choice.contains("index") &&
                (!choice.value("index").isDouble() || choice.value("index").toDouble() != 0))
                return streamError(error, QStringLiteral("Only candidate index zero is supported"));
            if (!choice.value("delta").isObject())
                return streamError(error, QStringLiteral("Candidate requires a delta object"));
            const QJsonObject delta = choice.value("delta").toObject();  // 当前候选新增的字段
            if (delta.contains("role") && delta.value("role") != QJsonValue("assistant"))
                return streamError(error, QStringLiteral("Candidate role must be assistant"));
            if ((!delta.value("audio").isNull() && !delta.value("audio").isUndefined()) ||
                delta.contains("function_call"))
                return streamError(error,
                                   QStringLiteral("Unmodeled stream content is not supported"));
            m_sawChoice = true;
            if (!textDelta(delta, QStringLiteral("reasoning_content"), StreamPartType::Reasoning,
                           sink, error) ||
                !textDelta(delta, QStringLiteral("content"), StreamPartType::Text, sink, error) ||
                !toolsDelta(delta.value("tool_calls"), sink, error))
                return false;
            const QJsonValue reason = choice.value("finish_reason");  // 原生模型停止原因
            if (!reason.isNull() && !reason.isUndefined()) {
                if (!reason.isString() || reason.toString().isEmpty())
                    return streamError(error,
                                       QStringLiteral("FinishReason must be a nonempty string"));
                const QString name = reason.toString();  // 待转换的厂商原因名称
                StreamEvent event;                       // 标准化模型停止通知，后续仍需读取 Usage
                event.type = StreamEventType::FinishReasonReceived;
                event.finishReason = name == "stop"             ? FinishReason::Stop
                                     : name == "length"         ? FinishReason::Length
                                     : name == "tool_calls"     ? FinishReason::ToolCalls
                                     : name == "content_filter" ? FinishReason::ContentFilter
                                                                : FinishReason::Other;
                if (!sink(event, error))
                    return false;
                for (auto it = m_types.constBegin(); it != m_types.constEnd();
                     ++it) {  // 本候选已声明的逻辑内容块
                    if (it.value() == StreamPartType::ToolCall &&
                        event.finishReason == FinishReason::Length)
                        continue;
                    if (!sink(partEvent(StreamEventType::PartCompleted, it.key()), error))
                        return false;
                }
                m_finished = true;
            }
        }
        const QJsonValue usage = root.value("usage");  // 可出现在模型结束后的响应级统计
        if (!usage.isNull() && !usage.isUndefined()) {
            if (!usage.isObject())
                return streamError(error, QStringLiteral("Usage must be an object"));
            StreamEvent event;  // 已报告用量快照，不将累计字段重复相加
            event.type = StreamEventType::UsageUpdated;
            if (!streamUsage(usage.toObject(), "prompt_tokens", "completion_tokens", "total_tokens",
                             event.usage, error))
                return false;
            if (!sink(event, error))
                return false;
        }
        return true;
    }
    SseDecoder m_sse;                   // 本请求独立的原始 SSE 行及事件缓冲
    QMap<QString, int> m_parts;         // 厂商定位方式到逻辑 Part 的映射
    QMap<int, StreamPartType> m_types;  // 当前逻辑块类别，供协议结束时输出完成通知
    bool m_sawChoice = false;           // 是否实际收到唯一候选
    bool m_finished = false;            // 是否收到候选 FinishReason
    bool m_done = false;                // 是否确认协议完整结束
};
}  // 内部命名空间结束

std::unique_ptr<IStreamDecoder>
OpenAIChatCompatibleAdapter::createStreamDecoder(SdkError& error) const  // 创建独立状态的流解析器
{
    error = {};
    return std::make_unique<OpenAIStreamDecoder>();
}
}  // AiLib 命名空间结束
