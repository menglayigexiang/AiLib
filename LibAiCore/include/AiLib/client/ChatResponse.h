#pragma once

#include <AiLib/core/Message.h>
#include <AiLib/core/Usage.h>

namespace AiLib {
enum class FinishReason { Unknown, Stop, Length, ToolCalls, ContentFilter, Other };
enum class CompletionState { Incomplete, Complete };
struct ChatResponse {
    QString id;                                                     // 服务端响应标识，未提供时为空
    QString model;                                                  // 服务端报告的模型标识
    Message message;                                                // 单个助手响应的有序内容
    Usage usage;                                                    // 本次调用的已报告用量，未知字段保持未知
    FinishReason finishReason = FinishReason::Unknown;              // 模型停止原因，与协议完整性分离
    CompletionState completionState = CompletionState::Incomplete;  // 协议响应是否完整获得，默认未完成
};
}  // AiLib 命名空间结束
