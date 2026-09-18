#pragma once

#include <AiLib/core/Message.h>
#include <AiLib/core/Usage.h>
#include <optional>

namespace AiLib {
enum class AgentFinishReason {
    Completed,
    Length,
    Cancelled,
    MaxTurns,
    MaxToolCalls,
    Timeout,
    Failed
};
// 保存本轮实际产生的增量消息及统一结束原因。
struct AgentResult {
    AgentFinishReason finishReason = AgentFinishReason::Failed;  // 本次运行的结束原因，默认失败
    std::optional<Message> finalMessage;                         // 仅 Completed 时存在的完整最终回答
    QList<Message> newMessages;                                  // 本次实际产生的增量消息，不含输入历史
    Usage totalUsage;                                            // 累计用量，任何参与轮次字段未知则对应总量未知
    QList<Usage> turnUsages;                                     // 各轮已报告用量，重试不计作新增轮次
};
}  // AiLib 命名空间结束
