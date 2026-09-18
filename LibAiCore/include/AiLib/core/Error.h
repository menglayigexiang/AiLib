#pragma once

#include <QString>
#include <QJsonValue>
#include <QVariantMap>
#include <optional>

namespace AiLib {
enum class ErrorCategory {
    None, InvalidArgument, Configuration, Network, Timeout, Cancelled,
    Authentication, RateLimited, Protocol, InvalidResponse, Unsupported,
    Provider, Internal
};

struct SdkError {
    ErrorCategory category = ErrorCategory::None;  // SDK 调用链的统一错误分类
    QString code;                                  // 稳定的机器可读错误码
    QString message;                               // 供用户、日志和界面阅读的错误说明
    std::optional<int> httpStatus;                 // HTTP 状态码，未收到响应时未知
    QJsonValue providerError;                      // 服务端原始错误数据，仅用于诊断
    std::optional<qint64> retryAfterMs;            // 服务端建议的重试等待毫秒数，未提供时未知
    QVariantMap details;                           // 字段路径、请求标识等补充诊断信息
};
}  // AiLib 命名空间结束
