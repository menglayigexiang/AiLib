#pragma once

#include <AiLib/client/ChatResponse.h>
#include <AiLib/core/Error.h>
#include <functional>
#include <optional>

namespace AiLib {
enum class StreamEventType {
    ResponseMetadataUpdated,
    PartStarted,
    TextDelta,
    ReasoningDelta,
    ToolCallDelta,
    PartCompleted,
    UsageUpdated,
    FinishReasonReceived,
    ResponseCompleted,
    Error
};
enum class StreamPartType { Text, Reasoning, ToolCall };

// 表达一次标准化流通知；Part 事件使用稳定索引，响应级事件使用 -1。
struct StreamEvent {
    StreamEventType type = StreamEventType::ResponseMetadataUpdated;  // 当前事件的语义类别
    int partIndex = -1;                                               // 请求内逻辑 Part 标识，不是厂商原始索引
    StreamPartType partType = StreamPartType::Text;                   // PartStarted 声明的内容类型
    QString responseId;                                               // ResponseMetadataUpdated 提供的响应标识
    QString model;                                                    // ResponseMetadataUpdated 提供的服务端模型名称
    QString delta;                                                    // 文本、推理或工具参数的追加片段
    std::optional<QString> toolCallId;                                // 完整调用 ID 属性，可在后续分片才出现
    QString toolNameDelta;                                            // 工具名称的追加片段，不负责调用关联
    Usage usage;                                                      // UsageUpdated 的已报告字段快照，不是增量计数
    FinishReason finishReason = FinishReason::Unknown;                // 模型结束原因，不代表协议结束
    SdkError error;                                                   // Error 事件的具体流程故障
};
using StreamCallback =
    std::function<void(const StreamEvent&)>;  // 调用线程中的实时通知，不负责响应聚合
using StreamEventSink = std::function<bool(
    const StreamEvent&, SdkError&)>;  // Decoder 将事件交给 Session，失败时停止解析
}                                     // namespace AiLib
