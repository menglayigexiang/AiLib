#pragma once

#include <AiLib/network/ITransport.h>

namespace AiLib {
// 在调用线程内创建请求级 Qt 网络对象，实现同步普通及流式 HTTP。
class AILIB_EXPORT QtHttpTransport final : public ITransport {
public:
    bool send(const TransportRequest& request,            // 原始 HTTP 请求
              const RequestOptions& options,              // 单次尝试超时及共享取消令牌
              TransportResponse& response,                // 输出状态、原始头及已收到的响应字节
              SdkError& error) override;                  // 无共享请求状态，可并发调用；要求 QCoreApplication 已存在
    bool sendStream(const TransportRequest& request,      // 原始 HTTP 请求
                    const RequestOptions& options,        // 超时及协作取消
                    TransportResponse& response,          // HTTP 元数据及错误正文
                    const TransportDataCallback& onData,  // 接收原始数据，失败时中止网络请求
                    SdkError& error) override;            // 同步增量传输，不创建工作线程

private:
    bool sendImpl(const TransportRequest& request,      // 原始 HTTP 请求
                  const RequestOptions& options,        // 单次尝试选项
                  TransportResponse& response,          // 元数据和普通响应正文
                  const TransportDataCallback& onData,  // 空回调表示普通请求
                  SdkError& error);                     // 共享超时、取消及网络错误处理
};
}  // AiLib 命名空间结束
