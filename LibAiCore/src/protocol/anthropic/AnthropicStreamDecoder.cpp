#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include "../../stream/SseDecoder.h"
#include "../../stream/DecoderHelpers.h"
#include <QJsonArray>
#include <QMap>

namespace AiLib {
namespace {
// 解释一次 Messages SSE 请求的块生命周期，将原生索引映射为逻辑 Part。
class AnthropicStreamDecoder final : public IStreamDecoder {
public:
    bool feed(const QByteArray& bytes,      // 原始网络片段
              const StreamEventSink& sink,  // 标准化事件接收端
              SdkError& error) override     // 将完整原生事件转换为统一流通知
    {
        return m_sse.feed(
            bytes,
            [&](const QString& name, const QByteArray& data,  // 完整事件的原始数据
                SdkError& failure) {                          // 解析当前 SSE 名称及 JSON 数据
                const bool success =                          // 当前原生事件是否被正常解析和聚合
                    decode(name, data, sink, failure);
                if (!success &&
                    (failure.providerError.isNull() || failure.providerError.isUndefined()))
                    failure.providerError = QString::fromUtf8(data);
                return success;
            },
            error);
    }
    bool finish(const StreamEventSink& sink,  // 标准化事件接收端
                SdkError& error) override     // 连接结束不能代替 Messages 的 message_stop
    {
        if (!m_sse.finish(
                [&](const QString& name, const QByteArray& data,  // 完整事件的原始数据
                    SdkError& failure) {                          // 解释 EOF 处最后一个完整 SSE 事件
                    const bool success =                          // 当前原生事件是否被正常解析和聚合
                        decode(name, data, sink, failure);
                    if (!success &&
                        (failure.providerError.isNull() || failure.providerError.isUndefined()))
                        failure.providerError = QString::fromUtf8(data);
                    return success;
                },
                error))
            return false;
        if (!m_done)
            return streamError(error,
                               QStringLiteral("Messages stream ended before protocol completion"));
        return true;
    }

private:
    // 记录协议块的定位和生命周期，不保存累计文本或工具参数。
    struct Block {
        int partIndex = -1;                          // 请求内稳定的 SDK 逻辑标识
        StreamPartType type = StreamPartType::Text;  // 已识别的内容类别
        bool stopped = false;                        // 是否已收到原生块结束事件
        bool hasInput = false;                       // 是否已经交付工具参数数据
        bool initialInput = false;                   // 起始块是否已携带非空完整参数
    };
    bool usageEvent(const QJsonValue& usage,      // 本请求的 SSE 原始字节缓冲
                    const StreamEventSink& sink,  // 标准化事件接收端
                    SdkError& error)              // 标准化累计统计的已报告字段
    {
        if (usage.isNull() || usage.isUndefined())
            return true;
        if (!usage.isObject())
            return streamError(error, QStringLiteral("Usage must be an object"));
        StreamEvent event;  // 响应级统计更新通知
        event.type = StreamEventType::UsageUpdated;
        if (!streamUsage(usage.toObject(), "input_tokens", "output_tokens", "total_tokens",
                         event.usage, error))
            return false;
        return sink(event, error);
    }
    bool blockEvent(const QString& type,          // 标准化事件或内容类别
                    const QJsonObject& root,      // 当前原生事件对象
                    const StreamEventSink& sink,  // 标准化事件接收端
                    SdkError& error)              // 处理原生内容块开始、分片及结束事件
    {
        const double rawIndex = root.value("index").toDouble(-1);  // 厂商块索引，仅在 Decoder 内解释
        if (!root.value("index").isDouble() || rawIndex < 0 || rawIndex > 2147483647 ||
            std::floor(rawIndex) != rawIndex)
            return streamError(error,
                               QStringLiteral("Content block needs a nonnegative integer index"));
        const int index = static_cast<int>(rawIndex);  // 原生索引映射键
        if (type == "content_block_start") {
            if (m_blocks.contains(index) || m_finished)
                return streamError(error, QStringLiteral("Duplicate or late content block"));
            if (!root.value("content_block").isObject())
                return streamError(error, QStringLiteral("Missing content_block object"));
            const QJsonObject content = root.value("content_block").toObject();  // 起始块的原生属性
            const QString kind = content.value("type").toString();               // 厂商内容类别
            Block block;                                                         // 当前块的生命周期记录
            block.partIndex = m_blocks.size();
            if (kind == "text")
                block.type = StreamPartType::Text;
            else if (kind == "thinking")
                block.type = StreamPartType::Reasoning;
            else if (kind == "tool_use")
                block.type = StreamPartType::ToolCall;
            else {
                streamError(error, QStringLiteral("Unmodeled Messages content block: ") + kind);
                error.category = ErrorCategory::Unsupported;
                error.code = QStringLiteral("UnsupportedFeature");
                error.providerError = root;
                return false;
            }
            StreamEvent start =  // 标准化块开始通知
                partEvent(StreamEventType::PartStarted, block.partIndex);
            start.partType = block.type;
            if (!sink(start, error))
                return false;
            StreamEvent delta =  // 起始块可能携带的初始内容
                partEvent(block.type == StreamPartType::ToolCall ? StreamEventType::ToolCallDelta
                          : block.type == StreamPartType::Text   ? StreamEventType::TextDelta
                                                                 : StreamEventType::ReasoningDelta,
                          block.partIndex);
            if (block.type == StreamPartType::ToolCall) {
                if (!content.value("id").isString() ||
                    content.value("id").toString().trimmed().isEmpty() ||
                    !content.value("name").isString() ||
                    content.value("name").toString().trimmed().isEmpty() ||
                    !content.value("input").isObject())
                    return streamError(
                        error,
                        QStringLiteral("Tool block requires native ID, name and object input"));
                delta.toolCallId = content.value("id").toString();
                delta.toolNameDelta = content.value("name").toString();
                if (!content.value("input").toObject().isEmpty()) {
                    delta.delta = QString::fromUtf8(QJsonDocument(content.value("input").toObject())
                                                        .toJson(QJsonDocument::Compact));
                    block.hasInput = true;
                    block.initialInput = true;
                }
            } else {
                const QString key = block.type == StreamPartType::Text  // 初始文本的原生字段名称
                                        ? QStringLiteral("text")
                                        : QStringLiteral("thinking");
                if (!content.value(key).isString())
                    return streamError(error, QStringLiteral("Initial text must be a string"));
                delta.delta = content.value(key).toString();
            }
            if (!sink(delta, error))
                return false;
            m_blocks.insert(index, block);
            return true;
        }
        auto it = m_blocks.find(index);  // 当前厂商块对应的生命周期状态
        if (it == m_blocks.end() || it->stopped || m_finished)
            return streamError(error, QStringLiteral("Missing, stopped or late content block"));
        Block& block = it.value();  // 当前需要更新的块状态
        if (type == "content_block_stop") {
            if (block.type == StreamPartType::ToolCall && !block.hasInput) {
                StreamEvent empty =
                    partEvent(StreamEventType::ToolCallDelta,  // 无参数工具的合法空 JSON 对象
                              block.partIndex);
                empty.delta = QStringLiteral("{}");
                if (!sink(empty, error))
                    return false;
            }
            if (!sink(partEvent(StreamEventType::PartCompleted, block.partIndex), error))
                return false;
            block.stopped = true;
            return true;
        }
        if (!root.value("delta").isObject())
            return streamError(error, QStringLiteral("Missing content delta"));
        const QJsonObject delta = root.value("delta").toObject();  // 当前原生增量属性
        const QString kind = delta.value("type").toString();       // 文本、推理、工具 JSON 或签名分片
        if (kind == "signature_delta" && block.type == StreamPartType::Reasoning) {
            if (!delta.value("signature").isString())
                return streamError(error, QStringLiteral("Invalid reasoning signature"));
            return true;  // 当前 canonical model 仅展示推理文本，回传仍明确不支持
        }
        const QString key = kind == "text_delta"       ? QStringLiteral("text")  // 原生追加文本字段
                            : kind == "thinking_delta" ? QStringLiteral("thinking")
                                                       : QStringLiteral("partial_json");
        if ((kind != "text_delta" || block.type != StreamPartType::Text) &&
            (kind != "thinking_delta" || block.type != StreamPartType::Reasoning) &&
            (kind != "input_json_delta" || block.type != StreamPartType::ToolCall))
            return streamError(error, QStringLiteral("Delta type does not match content block"));
        if (!delta.value(key).isString() ||
            (block.type == StreamPartType::ToolCall && block.initialInput))
            return streamError(error, QStringLiteral("Invalid or conflicting block delta"));
        StreamEvent event =  // 标准化追加通知
            partEvent(block.type == StreamPartType::Text        ? StreamEventType::TextDelta
                      : block.type == StreamPartType::Reasoning ? StreamEventType::ReasoningDelta
                                                                : StreamEventType::ToolCallDelta,
                      block.partIndex);
        event.delta = delta.value(key).toString();
        if (block.type == StreamPartType::ToolCall && !event.delta.isEmpty())
            block.hasInput = true;
        return sink(event, error);
    }
    bool decode(const QString& name,          // 原生事件名称
                const QByteArray& data,       // 完整 SSE 事件数据
                const StreamEventSink& sink,  // 标准化事件接收端
                SdkError& error)              // 检查协议顺序并输出响应级通知
    {
        QJsonObject root;  // 当前完整原生事件
        if (!readStreamObject(data, root, error))
            return false;
        const QString type = root.value("type").toString();  // JSON 声明的事件名称
        if (type.isEmpty() || (!name.isEmpty() && name != type))
            return streamError(error, QStringLiteral("SSE event name and JSON type disagree"));
        if (m_done)
            return streamError(error, QStringLiteral("Event arrived after Messages completion"));
        if (type == "ping")
            return true;
        if (type == "error")
            return emitStreamError(root, sink, error);
        if (type == "message_start") {
            if (m_started || !root.value("message").isObject())
                return streamError(error, QStringLiteral("Duplicate or invalid message_start"));
            const QJsonObject message =  // 单个原生 Assistant 响应头
                root.value("message").toObject();
            if (message.value("type") != QJsonValue("message") ||
                message.value("role") != QJsonValue("assistant") ||
                !message.value("content").isArray() ||
                !message.value("content").toArray().isEmpty())
                return streamError(
                    error,
                    QStringLiteral("message_start must contain one empty assistant Message"));
            StreamEvent event;  // 当前请求的响应身份通知
            event.type = StreamEventType::ResponseMetadataUpdated;
            event.responseId = message.value("id").toString();
            event.model = message.value("model").toString();
            if (!sink(event, error) || !usageEvent(message.value("usage"), sink, error))
                return false;
            m_started = true;
            return true;
        }
        if (!m_started)
            return streamError(error,
                               QStringLiteral("Messages event arrived before message_start"));
        if (type == "content_block_start" || type == "content_block_delta" ||
            type == "content_block_stop")
            return blockEvent(type, root, sink, error);
        if (type == "message_delta") {
            if (!root.value("delta").isObject())
                return streamError(error, QStringLiteral("message_delta requires an object"));
            const QJsonValue reason =  // 可选的原生模型停止原因
                root.value("delta").toObject().value("stop_reason");
            if (!reason.isNull() && !reason.isUndefined()) {
                if (!reason.isString() || reason.toString().isEmpty())
                    return streamError(error, QStringLiteral("Invalid stop_reason"));
                for (auto it = m_blocks.constBegin(); it != m_blocks.constEnd();
                     ++it) {  // 模型结束前应已关闭的原生块
                    if (!it->stopped)
                        return streamError(
                            error,
                            QStringLiteral("stop_reason arrived with an open content block"));
                }
                const QString stop = reason.toString();  // 待标准化的原因名称
                StreamEvent event;                       // 模型停止通知，与协议完成通知分开
                event.type = StreamEventType::FinishReasonReceived;
                event.finishReason = stop == "max_tokens" ? FinishReason::Length
                                     : stop == "tool_use" ? FinishReason::ToolCalls
                                     : stop == "end_turn" || stop == "stop_sequence"
                                         ? FinishReason::Stop
                                     : stop == "refusal" ? FinishReason::ContentFilter
                                                         : FinishReason::Other;
                if (!sink(event, error))
                    return false;
                m_finished = true;
            }
            return usageEvent(root.value("usage"), sink, error);
        }
        if (type == "message_stop") {
            if (!m_finished)
                return streamError(error,
                                   QStringLiteral("message_stop arrived without stop_reason"));
            StreamEvent event;  // 厂商协议确认完整结束的标准化通知
            event.type = StreamEventType::ResponseCompleted;
            if (!sink(event, error))
                return false;
            m_done = true;
            return true;
        }
        return true;  // 协议允许新增非内容事件，未知通知不改变已知聚合状态
    }
    SseDecoder m_sse;           // 本请求独立的原始 SSE 行及事件缓冲
    QMap<int, Block> m_blocks;  // 原生索引映射及生命周期，不保存累计内容
    bool m_started = false;     // 是否获得唯一 Assistant 响应头
    bool m_finished = false;    // 是否收到 stop_reason，后续仍可读取 Usage
    bool m_done = false;        // 是否获得原生完整结束通知
};
}  // 内部命名空间结束

std::unique_ptr<IStreamDecoder> AnthropicMessagesAdapter::createStreamDecoder(
    SdkError& error) const  // 创建请求独享的 Messages SSE 解析器
{
    error = {};
    return std::make_unique<AnthropicStreamDecoder>();
}
}  // AiLib 命名空间结束
