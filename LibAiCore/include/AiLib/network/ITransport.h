#pragma once

#include <AiLib/Export.h>
#include <AiLib/network/TransportResponse.h>
#include <AiLib/client/RequestOptions.h>
#include <AiLib/core/Error.h>
#include <functional>

namespace AiLib {
using TransportDataCallback =
    std::function<bool(const TransportResponse&,
                       const QByteArray&,
                       SdkError&)>;  // 交付原始片段及 HTTP 元数据，失败时停止传输

// 同步网络传输边界，不解释模型协议，也不自动重试。
class AILIB_EXPORT ITransport {
public:
    virtual ~ITransport() = default;                              // 支持通过抽象接口销毁具体传输实现
    virtual bool send(const TransportRequest& request,            // 完整的原始 HTTP 请求
                      const RequestOptions& options,              // 本次超时及取消，Transport 不消费重试策略
                      TransportResponse& response,                // 输出 HTTP 结果，包括非成功状态码
                      SdkError& error) = 0;                       // 同步传输，false 表示网络、取消或超时等故障
    virtual bool sendStream(const TransportRequest& request,      // 完整的原始 HTTP 请求
                            const RequestOptions& options,        // 当前请求超时及取消
                            TransportResponse& response,          // HTTP 元数据，错误状态时保留响应正文
                            const TransportDataCallback& onData,  // 调用线程中的原始字节接收器
                            SdkError& error)                      // 同步流传输，旧实现默认明确不支持
    {
        Q_UNUSED(request)
        Q_UNUSED(options)
        Q_UNUSED(response)
        Q_UNUSED(onData)
        error = {};
        error.category = ErrorCategory::Unsupported;
        error.code = QStringLiteral("UnsupportedStreamingTransport");
        error.message = QStringLiteral("Transport does not implement streaming");
        return false;
    }
};
}  // AiLib 命名空间结束
