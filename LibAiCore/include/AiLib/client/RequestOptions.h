#pragma once

#include <AiLib/stream/StreamEvent.h>

#include <AiLib/core/Cancellation.h>
#include <chrono>
#include <optional>

namespace AiLib {
struct RetryPolicy {
    int maxRetries = 5;               // 最大重试次数，默认 5，不包含首次请求
    int retryIntervalMs = 30000;      // 固定重试间隔毫秒数，默认 30000
    bool retryOnNetworkError = true;  // 是否对临时网络连接错误重试
    bool retryOnRateLimit = true;     // 是否对 HTTP 429 限流重试
    bool retryOnServerError = true;   // 是否对临时 HTTP 5xx 错误重试
};
// 保存单次调用配置，流回调只通知调用方，不承担聚合职责。
struct RequestOptions {
    int timeoutSeconds = 120;        // 每次 HTTP 尝试的超时秒数，-1 不限时
    RetryPolicy retryPolicy;         // 本次普通 Chat 的自动重试配置
    CancellationToken cancellation;  // 调用方提供的协作取消令牌
    StreamCallback streamCallback;   // 标准化事件实时通知，在调用线程执行，不聚合最终响应
    std::optional<std::chrono::steady_clock::time_point>
        deadline;                   // 调用链传入的精确截止时间，空值不施加额外限制
    bool isDeadlineExpired() const  // 检查单调时钟截止时间，不创建线程
    {
        return deadline && std::chrono::steady_clock::now() >= *deadline;
    }
};
}  // AiLib 命名空间结束
