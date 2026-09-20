#include <AiLib/protocol/openai/OpenAIResponsesAdapter.h>
#include "../../stream/DecoderHelpers.h"
#include "../../stream/SseDecoder.h"
#include <QHash>
#include <QSet>

namespace AiLib {
namespace {

// 将 OpenAI Responses SSE 事件转换为标准流事件。
class OpenAIResponsesStreamDecoder final : public IStreamDecoder {
public:
    bool feed(const QByteArray& bytes, const StreamEventSink& sink,
              SdkError& error) override  // 解析任意网络分片中的完整 SSE 事件
    {
        return m_sse.feed(bytes,
            [&](const QString& name, const QByteArray& data, SdkError& failure) {  // 当前完整 SSE 事件
                return decode(name, data, sink, failure);
            }, error);
    }

    bool finish(const StreamEventSink& sink, SdkError& error) override  // 验证 EOF 前已收到完成事件
    {
        if (!m_sse.finish(
                [&](const QString& name, const QByteArray& data, SdkError& failure) {  // EOF 处完整 SSE 事件
                    return decode(name, data, sink, failure);
                }, error))
            return false;
        if (!m_completed)
            return streamError(error, QStringLiteral("Responses stream ended without a terminal response event"));
        return true;
    }

private:
    int ensurePart(int outputIndex, StreamPartType type, const StreamEventSink& sink,
                   SdkError& error)  // 为原生输出索引建立稳定 canonical Part
    {
        if (m_parts.contains(outputIndex))
            return m_parts.value(outputIndex);
        const int partIndex = m_parts.size();  // 按首次出现顺序分配的逻辑索引
        StreamEvent event = partEvent(StreamEventType::PartStarted, partIndex);  // Part 开始通知
        event.partType = type;
        if (!sink(event, error))
            return -1;
        m_parts.insert(outputIndex, partIndex);
        m_types.insert(outputIndex, type);
        return partIndex;
    }

    bool completePart(int outputIndex, const StreamEventSink& sink,
                      SdkError& error)  // 完成尚未结束的单个输出 Part
    {
        if (!m_parts.contains(outputIndex) || m_doneParts.contains(outputIndex))
            return true;
        StreamEvent event = partEvent(StreamEventType::PartCompleted, m_parts.value(outputIndex));  // Part 完成通知
        if (!sink(event, error))
            return false;
        m_doneParts.insert(outputIndex);
        return true;
    }

    bool decode(const QString& eventName, const QByteArray& data,
                const StreamEventSink& sink, SdkError& error)  // 转换一个 Responses 原生事件
    {
        if (m_completed)
            return streamError(error, QStringLiteral("Data arrived after Responses completion"));
        QJsonObject root;  // 当前 SSE JSON 对象
        if (!readStreamObject(data, root, error))
            return false;
        const QString type = root.value(QStringLiteral("type")).toString(eventName);  // 优先使用正文事件类型
        if (type == QStringLiteral("response.failed")) {
            QJsonObject failure = root;  // 统一为公共流错误辅助函数需要的形状
            failure.insert(QStringLiteral("error"), root.value(QStringLiteral("response")).toObject().value(QStringLiteral("error")));
            return emitStreamError(failure, sink, error);
        }
        if (type == QStringLiteral("error"))
            return emitStreamError(root, sink, error);
        if (type == QStringLiteral("response.created") || type == QStringLiteral("response.in_progress")) {
            const QJsonObject response = root.value(QStringLiteral("response")).toObject();  // 响应元数据
            StreamEvent event;  // 标准元数据通知
            event.type = StreamEventType::ResponseMetadataUpdated;
            event.responseId = response.value(QStringLiteral("id")).toString();
            event.model = response.value(QStringLiteral("model")).toString();
            return sink(event, error);
        }
        const int outputIndex = root.value(QStringLiteral("output_index")).toInt(-1);  // 原生输出项索引
        if (type == QStringLiteral("response.output_item.added")) {
            const QJsonObject item = root.value(QStringLiteral("item")).toObject();  // 新增输出项
            const QString itemType = item.value(QStringLiteral("type")).toString();  // 原生输出类型
            if (itemType == QStringLiteral("function_call")) {
                const int partIndex = ensurePart(outputIndex, StreamPartType::ToolCall, sink, error);  // 工具 Part
                if (partIndex < 0) return false;
                StreamEvent event = partEvent(StreamEventType::ToolCallDelta, partIndex);  // 工具身份增量
                const QString callId = item.value(QStringLiteral("call_id")).toString();  // 工具调用 ID
                if (!callId.isEmpty()) event.toolCallId = callId;
                event.toolNameDelta = item.value(QStringLiteral("name")).toString();
                return sink(event, error);
            }
            return true;
        }
        if (type == QStringLiteral("response.output_text.delta")) {
            const int partIndex = ensurePart(outputIndex, StreamPartType::Text, sink, error);  // 文本 Part
            if (partIndex < 0) return false;
            StreamEvent event = partEvent(StreamEventType::TextDelta, partIndex);  // 文本增量
            event.delta = root.value(QStringLiteral("delta")).toString();
            return event.delta.isEmpty() || sink(event, error);
        }
        if (type == QStringLiteral("response.function_call_arguments.delta")) {
            const int partIndex = ensurePart(outputIndex, StreamPartType::ToolCall, sink, error);  // 工具 Part
            if (partIndex < 0) return false;
            StreamEvent event = partEvent(StreamEventType::ToolCallDelta, partIndex);  // 参数增量
            event.delta = root.value(QStringLiteral("delta")).toString();
            return sink(event, error);
        }
        if (type == QStringLiteral("response.output_item.done"))
            return completePart(outputIndex, sink, error);
        if (type == QStringLiteral("response.completed") || type == QStringLiteral("response.incomplete")) {
            const QJsonObject response = root.value(QStringLiteral("response")).toObject();  // 最终响应
            for (auto it = m_parts.constBegin(); it != m_parts.constEnd(); ++it) {  // 兜底完成所有原生输出项
                if (!completePart(it.key(), sink, error)) return false;
            }
            const QJsonObject usageObject = response.value(QStringLiteral("usage")).toObject();  // 最终用量
            if (!usageObject.isEmpty()) {
                StreamEvent usageEvent;  // 标准用量通知
                usageEvent.type = StreamEventType::UsageUpdated;
                if (!streamUsage(usageObject, "input_tokens", "output_tokens", "total_tokens", usageEvent.usage, error)
                    || !sink(usageEvent, error))
                    return false;
            }
            StreamEvent finishEvent;  // 标准停止原因通知
            finishEvent.type = StreamEventType::FinishReasonReceived;
            bool hasTools = false;  // 是否出现工具输出 Part
            for (auto it = m_types.constBegin(); it != m_types.constEnd(); ++it) {  // 当前输出类型
                if (it.value() == StreamPartType::ToolCall) hasTools = true;
            }
            finishEvent.finishReason = type == QStringLiteral("response.incomplete")
                ? FinishReason::Length : hasTools ? FinishReason::ToolCalls : FinishReason::Stop;
            if (!sink(finishEvent, error)) return false;
            StreamEvent completedEvent;  // 协议完成通知
            completedEvent.type = StreamEventType::ResponseCompleted;
            if (!sink(completedEvent, error)) return false;
            m_completed = true;
            return true;
        }
        return true;  // 忽略不影响 canonical 结果的生命周期事件
    }

    SseDecoder m_sse;                    // SSE 行和事件边界解析器
    QHash<int, int> m_parts;             // 原生输出索引到逻辑 Part 索引
    QHash<int, StreamPartType> m_types;  // 原生输出索引对应的 Part 类型
    QSet<int> m_doneParts;               // 已完成的原生输出索引
    bool m_completed = false;            // 是否已交付 response.completed
};

}  // 内部 Responses 流解析实现命名空间结束

std::unique_ptr<IStreamDecoder>
OpenAIResponsesAdapter::createStreamDecoder(SdkError& error) const  // 创建无共享状态的请求级 Decoder
{
    error = {};
    return std::make_unique<OpenAIResponsesStreamDecoder>();
}

}  // AiLib 命名空间结束
