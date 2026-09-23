#pragma once

#include "HttpTypes_p.h"

#include <LibMcp/McpResult.h>

#include <QHostAddress>
#include <QObject>

#include <functional>
#include <memory>

namespace LibMcp::Internal {

using HttpRequestId = QString;  // 标识一个仍可响应的 HTTP 请求
using HttpRequestHandler =
    std::function<void(const HttpRequestId&, const HttpRequest&)>;  // 处理完整 HTTP 请求
using HttpRequestClosedHandler =
    std::function<void(const HttpRequestId&)>;  // 处理对端提前关闭请求

class HttpServerPrivate;  // 隐藏 Qt Socket 和 HTTP framing 实现

// 为内部 Transport 提供通用 HTTP 监听与响应能力。
class HttpServer final : public QObject
{
public:
    explicit HttpServer(QObject* parent = nullptr);  // 创建尚未监听的 HTTP Server
    ~HttpServer() override;                          // 关闭监听和全部活动连接

    void setRequestHandler(HttpRequestHandler handler);  // 设置完整请求处理函数
    void setRequestClosedHandler(HttpRequestClosedHandler handler);  // 设置请求关闭处理函数
    McpResult<void> listen(const QHostAddress& address, quint16 port);  // 开始监听指定地址和端口
    void close();                                            // 停止监听并关闭全部连接
    McpResult<void> sendResponse(
        const HttpRequestId& requestId,           // 需要接收响应的 HTTP 请求
        const HttpServerResponse& response);      // 写入完整 HTTP 响应并关闭连接
    McpResult<void> startStream(
        const HttpRequestId& requestId,            // 需要切换为流式响应的 HTTP 请求
        const HttpServerResponse& response);       // 只使用状态码、原因和 Headers
    McpResult<void> writeStream(
        const HttpRequestId& requestId,            // 目标流式 HTTP 请求
        const QByteArray& chunk);                   // 需要作为一个 HTTP chunk 写入的数据
    McpResult<void> finishStream(
        const HttpRequestId& requestId);            // 写入结束 chunk 并关闭连接
    bool isListening() const;                     // 查询当前是否正在监听
    QHostAddress address() const;                 // 返回实际监听地址
    quint16 port() const;                         // 返回实际监听端口

private:
    std::unique_ptr<HttpServerPrivate> d;  // 保存 Socket、parser 和活动请求状态
};

} // namespace LibMcp::Internal
