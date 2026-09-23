#include "HttpClient_p.h"

#include "../core/FutureUtils_p.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>

namespace LibMcp::Internal {

// 保存 HttpClient 的 Qt Network 实现，职责限定为通用 HTTP 交换。
class HttpClientPrivate
{
public:
    QNetworkAccessManager manager;       // 执行实际 HTTP 请求
    QSet<QNetworkReply*> activeReplies;  // 跟踪可取消的活动请求
};

HttpClient::HttpClient(QObject* parent)  // 创建使用当前线程事件循环的 HTTP Client
    : QObject(parent)
    , d(std::make_unique<HttpClientPrivate>())
{
}

HttpClient::~HttpClient()  // 终止未完成请求并释放网络资源
{
    abortAll();
}

QFuture<McpResult<HttpResponse>> HttpClient::send(
    const HttpRequest& request,       // 需要发送的完整 HTTP 请求
    HttpDataHandler dataHandler)     // 可选的增量响应数据回调
{
    if (!request.url.isValid() || request.method.isEmpty()) {
        return readyFuture(McpResult<HttpResponse>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("HTTP 请求地址或方法无效")}));
    }

    Promise<McpResult<HttpResponse>> promise;  // 保存异步 HTTP 结果写入端
    const QFuture<McpResult<HttpResponse>> future = promise.future();  // 返回给调用方的结果读取端
    QNetworkRequest networkRequest(request.url);  // 构造 Qt Network 请求对象
    for (auto iterator = request.headers.constBegin();
         iterator != request.headers.constEnd();
         ++iterator) {
        networkRequest.setRawHeader(iterator.key(), iterator.value());
    }

    QNetworkReply* reply =  // 执行任意方法的通用 HTTP 请求
        d->manager.sendCustomRequest(networkRequest,
                                     request.method,
                                     request.body);
    d->activeReplies.insert(reply);
    const auto responseHead = [reply] {  // 从 Reply 构造不包含正文的通用响应头
        HttpResponse response;  // 保存当前已可用的状态码和 Headers
        response.statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        for (const QNetworkReply::RawHeaderPair& header : reply->rawHeaderPairs()) {
            response.headers.insert(header.first.toLower(), header.second);
        }
        return response;
    };
    const auto body = std::make_shared<QByteArray>();  // 收集最终响应以兼容非流式调用方
    connect(reply,
            &QNetworkReply::readyRead,
            this,
            [reply, dataHandler, responseHead, body] {  // 立即上送流式字节并保留完整正文
                const QByteArray chunk = reply->readAll();  // 读取本次到达的增量字节
                body->append(chunk);
                if (dataHandler && !chunk.isEmpty()) {
                    dataHandler(responseHead(), chunk);
                }
            });
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, promise, responseHead, body] {  // 收集一次完整 HTTP 响应并结束 Future
            d->activeReplies.remove(reply);
            const int statusCode =  // HTTP 错误响应仍保留其状态码与正文
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            if (reply->error() != QNetworkReply::NoError
                && statusCode == 0) {
                const QString message = reply->errorString();  // 在销毁 Reply 前保存错误文本
                reply->deleteLater();
                promise.finish(McpResult<HttpResponse>::failure(
                    {McpErrorCode::TransportError, message}));
                return;
            }

            const QByteArray tail = reply->readAll();  // 读取 finished 前尚未触发 readyRead 的尾部字节
            body->append(tail);
            HttpResponse response = responseHead();  // 保存与业务协议无关的完整 HTTP 响应
            response.body = *body;
            reply->deleteLater();
            promise.finish(
                McpResult<HttpResponse>::success(std::move(response)));
        });
    return future;
}

void HttpClient::abortAll()  // 终止当前 Client 发出的全部未完成请求
{
    const QSet<QNetworkReply*> replies = d->activeReplies;  // 固定本轮需要终止的 Reply 集合
    for (QNetworkReply* reply : replies) {
        reply->abort();
    }
}

} // namespace LibMcp::Internal
