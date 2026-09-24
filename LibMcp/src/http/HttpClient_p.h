#pragma once

#include "HttpTypes_p.h"

#include <LibMcp/McpResult.h>

#include <QFuture>
#include <QObject>

#include <memory>
#include <functional>

namespace LibMcp::Internal {

class HttpClientPrivate;  // 隐藏 Qt Network 实现，避免其类型泄漏给 Transport

using HttpDataHandler =
    std::function<bool(const HttpResponse&, const QByteArray&)>;  // 消费增量正文，返回 true 表示无需保留该块

// 为内部 Transport 提供精炼的通用 HTTP 请求能力。
class HttpClient final : public QObject
{
public:
    explicit HttpClient(QObject* parent = nullptr);  // 创建使用当前线程事件循环的 HTTP Client
    ~HttpClient() override;                          // 终止未完成请求并释放网络资源

    QFuture<McpResult<HttpResponse>> send(
        const HttpRequest& request,       // 需要发送的完整 HTTP 请求
        HttpDataHandler dataHandler = {});// 可选的增量响应数据回调
    void abortAll();                  // 终止当前 Client 发出的全部未完成请求

private:
    std::unique_ptr<HttpClientPrivate> d;  // 保存 Qt Network 实现和活动 Reply
};

} // namespace LibMcp::Internal
