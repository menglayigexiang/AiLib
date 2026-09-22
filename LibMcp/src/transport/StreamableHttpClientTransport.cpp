#include <LibMcp/StreamableHttpTransport.h>

#include "../core/FutureUtils_p.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace LibMcp {

class StreamableHttpClientTransportPrivate
{
public:
    QUrl endpoint;
    QJsonObject headers;
    QNetworkAccessManager manager;
    QString sessionId;
    bool running = false;
};

StreamableHttpClientTransport::StreamableHttpClientTransport(
    QUrl endpoint,
    QJsonObject headers,
    QObject *parent)
    : McpClientTransport(parent)
    , d(std::make_unique<StreamableHttpClientTransportPrivate>())
{
    d->endpoint = std::move(endpoint);
    d->headers = std::move(headers);
}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;

QFuture<McpResult<void>> StreamableHttpClientTransport::start()
{
    if (!d->endpoint.isValid()
        || (d->endpoint.scheme() != QStringLiteral("http")
            && d->endpoint.scheme() != QStringLiteral("https"))) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("MCP HTTP URL 无效")}));
    }
    d->running = true;
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpClientTransport::stop()
{
    d->running = false;
    d->sessionId.clear();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpClientTransport::sendMessage(
    const QJsonObject &message)
{
    if (!d->running) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP Transport 已停止")}));
    }

    Internal::Promise<McpResult<void>> promise;
    const QFuture<McpResult<void>> future = promise.future();
    QNetworkRequest request(d->endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json, text/event-stream");
    if (!d->sessionId.isEmpty()) {
        request.setRawHeader("Mcp-Session-Id", d->sessionId.toUtf8());
    }
    for (auto iterator = d->headers.constBegin();
         iterator != d->headers.constEnd();
         ++iterator) {
        request.setRawHeader(iterator.key().toUtf8(),
                             iterator.value().toString().toUtf8());
    }

    QNetworkReply *reply = d->manager.post(
        request,
        QJsonDocument(message).toJson(QJsonDocument::Compact));
    connect(reply,
            &QNetworkReply::finished,
            this,
            [this, reply, promise] {
                const QByteArray body = reply->readAll();
                const QByteArray session =
                    reply->rawHeader("Mcp-Session-Id");
                if (!session.isEmpty()) {
                    d->sessionId = QString::fromUtf8(session);
                }

                if (reply->error() != QNetworkReply::NoError) {
                    promise.finish(McpResult<void>::failure(
                        {McpErrorCode::TransportError,
                         reply->errorString()}));
                } else if (body.trimmed().isEmpty()) {
                    promise.finish(McpResult<void>::success());
                } else {
                    const QJsonDocument document =
                        QJsonDocument::fromJson(body);
                    if (!document.isObject()) {
                        promise.finish(McpResult<void>::failure(
                            {McpErrorCode::InvalidMessage,
                             QStringLiteral("HTTP 响应不是 JSON 对象")}));
                    } else {
                        emit messageReceived(document.object());
                        promise.finish(McpResult<void>::success());
                    }
                }
                reply->deleteLater();
            });
    return future;
}

} // namespace LibMcp
