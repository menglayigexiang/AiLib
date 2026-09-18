#pragma once
#include <AiLib/core/Cancellation.h>
#include <QString>
#include <chrono>
#include <optional>
namespace AiLib {
// 携带调用来源、共享取消状态和可选的单调时钟截止时间，不拥有 Agent。
struct ToolExecutionContext {
    QString agentId;                                                // 发起调用的 Agent 实例身份
    CancellationToken cancellation;                                 // 本次运行共用的协作取消令牌
    std::optional<std::chrono::steady_clock::time_point> deadline;  // 总运行截止时间，空值不限时
    bool isTimedOut() const                                         // 查询总截止时间是否已到
    {
        return deadline && std::chrono::steady_clock::now() >= *deadline;
    }
};
}
