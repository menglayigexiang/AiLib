#pragma once
#include <AiLib/tools/ToolCall.h>
#include <AiLib/tools/ToolResult.h>
#include <QString>
#include <functional>
#include <optional>
namespace AiLib {
enum class AgentEventType { ToolExecutionStarted, ToolExecutionFinished };
// 报告完整模型响应之后的工具执行流程，与模型生成 ToolCall 的流事件分离。
struct AgentEvent {
    AgentEventType type = AgentEventType::ToolExecutionStarted;  // 工具流程开始或结束
    QString agentId;                                             // 来源 Agent 的稳定身份
    ToolCall call;                                               // 工具调用的关联及参数
    std::optional<ToolResult> result;                            // 结束事件的流程结果，开始事件为空
    bool handlerExecuted = false;                                // 是否实际进入 Handler，拒绝和前置失败为 false
};
using AgentCallback =
    std::function<void(const AgentEvent& event)>;  // 调用线程中的同步通知，不承担执行调度
}                                                  // AiLib 命名空间结束
