#include <LibMcp/StreamableHttpTransport.h>

#include "../core/FutureUtils_p.h"

#include <QHash>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

namespace LibMcp {

class StreamableHttpServerTransportPrivate
{
public:
    StreamableHttpServerTransport *owner = nullptr;
    QHostAddress address;
    quint16 requestedPort = 0;
    QString path;
    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    QMultiHash<QString, QTcpSocket *> pendingResponses;

    void acceptConnections()
    {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            buffers.insert(socket, {});
            QObject::connect(
                socket,
                &QTcpSocket::readyRead,
                owner,
                [this, socket] { readRequest(socket); });
            QObject::connect(
                socket,
                &QTcpSocket::disconnected,
                owner,
                [this, socket] {
                    buffers.remove(socket);
                    for (auto iterator = pendingResponses.begin();
                         iterator != pendingResponses.end();) {
                        if (iterator.value() == socket) {
                            iterator = pendingResponses.erase(iterator);
                        } else {
                            ++iterator;
                        }
                    }
                    socket->deleteLater();
                });
        }
    }

    void sendHttpError(QTcpSocket *socket,
                       int status,
                       const QByteArray &reason)
    {
        const QByteArray response =
            "HTTP/1.1 " + QByteArray::number(status) + " " + reason
            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        socket->write(response);
        socket->disconnectFromHost();
    }

    void readRequest(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        if (requestLine.size() < 2 || requestLine.at(0) != "POST") {
            sendHttpError(socket, 405, "Method Not Allowed");
            return;
        }
        if (QString::fromUtf8(requestLine.at(1)) != path) {
            sendHttpError(socket, 404, "Not Found");
            return;
        }

        int contentLength = -1;
        QString sessionId;
        for (const QByteArray &rawLine : lines) {
            const QByteArray line = rawLine.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                contentLength =
                    line.mid(line.indexOf(':') + 1).trimmed().toInt();
            } else if (line.toLower().startsWith("mcp-session-id:")) {
                sessionId = QString::fromUtf8(
                    line.mid(line.indexOf(':') + 1).trimmed());
            }
        }

        const int bodyOffset = headerEnd + 4;
        if (contentLength < 0
            || buffer.size() - bodyOffset < contentLength) {
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(
            buffer.mid(bodyOffset, contentLength));
        if (!document.isObject()) {
            sendHttpError(socket, 400, "Bad Request");
            return;
        }

        if (sessionId.isEmpty()) {
            sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        const QJsonObject message = document.object();
        if (message.contains(QStringLiteral("id"))) {
            pendingResponses.insert(sessionId, socket);
        }
        emit owner->messageReceived(sessionId, message);

        if (!message.contains(QStringLiteral("id"))) {
            const QByteArray response =
                "HTTP/1.1 202 Accepted\r\nMcp-Session-Id: "
                + sessionId.toUtf8()
                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            socket->write(response);
            socket->disconnectFromHost();
        }
    }
};

StreamableHttpServerTransport::StreamableHttpServerTransport(
    QHostAddress address,
    quint16 port,
    QString path,
    QObject *parent)
    : McpServerTransport(parent)
    , d(std::make_unique<StreamableHttpServerTransportPrivate>())
{
    d->owner = this;
    d->address = std::move(address);
    d->requestedPort = port;
    d->path = path.startsWith(QLatin1Char('/'))
                  ? std::move(path)
                  : QStringLiteral("/") + path;
    connect(&d->server,
            &QTcpServer::newConnection,
            this,
            [this] { d->acceptConnections(); });
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() = default;

QFuture<McpResult<void>> StreamableHttpServerTransport::start()
{
    if (d->server.isListening()) {
        return Internal::readyFuture(McpResult<void>::success());
    }
    if (!d->server.listen(d->address, d->requestedPort)) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             d->server.errorString()}));
    }
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpServerTransport::stop()
{
    d->server.close();
    for (QTcpSocket *socket : d->buffers.keys()) {
        socket->abort();
    }
    d->buffers.clear();
    d->pendingResponses.clear();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpServerTransport::sendMessage(
    const McpSessionId &sessionId,
    const QJsonObject &message)
{
    auto iterator = d->pendingResponses.find(sessionId);
    if (iterator == d->pendingResponses.end()) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("没有等待响应的 HTTP 请求")}));
    }

    QTcpSocket *socket = iterator.value();
    d->pendingResponses.erase(iterator);
    const QByteArray body =
        QJsonDocument(message).toJson(QJsonDocument::Compact);
    const QByteArray response =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Mcp-Session-Id: "
        + sessionId.toUtf8() + "\r\nContent-Length: "
        + QByteArray::number(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
    socket->write(response);
    socket->disconnectFromHost();
    return Internal::readyFuture(McpResult<void>::success());
}

QHostAddress StreamableHttpServerTransport::address() const
{
    return d->server.isListening() ? d->server.serverAddress() : d->address;
}

quint16 StreamableHttpServerTransport::port() const
{
    return d->server.isListening() ? d->server.serverPort()
                                   : d->requestedPort;
}

QString StreamableHttpServerTransport::path() const
{
    return d->path;
}

} // namespace LibMcp
