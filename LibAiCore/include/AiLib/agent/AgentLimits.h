#pragma once

#include <AiLib/client/RequestOptions.h>

namespace AiLib {
// 声明本次运行的资源限制；各阶段逐步实现，未支持项不得静默忽略。
struct AgentLimits {
    int maxTurns = 20;              // 最多逻辑 LLM 调用轮数，默认 20，必须至少为 1
    int maxToolCalls = -1;          // 所有 Handler 实际执行次数总上限，-1 不限，0 禁止
    int totalTimeoutSeconds = 600;  // 整次 Run 的超时秒数，默认 600，-1 不限时
    int llmTimeoutSeconds = 120;    // 每次 LLM 请求尝试的超时秒数，默认 120，-1 不限时
    RetryPolicy llmRetryPolicy;     // Agent 每轮覆盖局部请求选项的重试策略
};
}  // AiLib 命名空间结束
