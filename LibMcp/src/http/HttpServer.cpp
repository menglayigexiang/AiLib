#include "HttpServer_p.h"

#include <QHash>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

namespace LibMcp::Internal {
namespace {

constexpr qsizetype maximumRequestBytes = 16 * 1024 * 1024;  // 单个 HTTP 请求的安全上限

QByteArray normalizedHeaderName(const QByteArray& name)  // 统一 Header 名以便大小写不敏感查询
{
    return name.trimmed().toLower();
}

} // namespace

// 保存 HttpServer 的 Socket、HTTP framing 和活动请求路由状态。
class HttpServerPrivate
{
public:
    HttpServer* owner = nullptr;                      // 回调所属 facade
    QTcpServer server;                                // 接受 TCP 连接的底层监听器
    QHash<QTcpSocket*, QByteArray> buffers;           // 保存各连接尚未解析的字节
    QHash<HttpRequestId, QTcpSocket*> pending;        // 保存等待响应的请求
    QSet<HttpRequestId> streaming;                    // 保存已发送 chunked 响应头的请求
    HttpRequestHandler requestHandler;                // 接收完整 HTTP 请求的上层回调
    HttpRequestClosedHandler closedHandler;           // 接收提前关闭事件的上层回调

    void acceptConnections()  // 接受全部等待处理的 TCP 连接
    {
        while (QTcpSocket* socket = server.nextPendingConnection()) {
            buffers.insert(socket, {});
            QObject::connect(socket,
                             &QTcpSocket::readyRead,
                             owner,
                             [this, socket] { readRequest(socket); });
            QObject::connect(socket,
                             &QTcpSocket::disconnected,
                             owner,
                             [this, socket] { removeSocket(socket); });
        }
    }

    void removeSocket(QTcpSocket* socket)  // 清理断开连接及其未完成请求
    {
        buffers.remove(socket);
        for (auto iterator = pending.begin(); iterator != pending.end();) {
            if (iterator.value() != socket) {
                ++iterator;
                continue;
            }
            const HttpRequestId requestId = iterator.key();  // 保存即将移除的请求标识
            streaming.remove(requestId);
            iterator = pending.erase(iterator);
            if (closedHandler) {
                closedHandler(requestId);
            }
        }
        socket->deleteLater();
    }

    void reject(QTcpSocket* socket, int statusCode, const QByteArray& reason)  // 返回无正文 HTTP 错误
    {
        const QByteArray response =  // 构造最小且连接关闭的 HTTP 错误响应
            "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reason
            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        socket->write(response);
        socket->disconnectFromHost();
    }

    void readRequest(QTcpSocket* socket)  // 增量读取并组装一个完整 HTTP 请求
    {
        QByteArray& buffer = buffers[socket];  // 保存当前连接累计收到的请求字节
        buffer += socket->readAll();
        if (buffer.size() > maximumRequestBytes) {
            reject(socket, 413, "Payload Too Large");
            return;
        }

        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");  // 定位 Headers 结束位置
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');  // 拆分请求行与 Headers
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');  // 拆分方法、目标和版本
        if (requestLine.size() < 3) {
            reject(socket, 400, "Bad Request");
            return;
        }

        HttpRequest request;  // 保存解析完成的通用 HTTP 请求
        request.method = requestLine.at(0).trimmed().toUpper();
        request.target = requestLine.at(1).trimmed();
        int contentLength = 0;  // 请求正文长度，未声明时按空正文处理
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();  // 当前待解析 Header 行
            const qsizetype separator = line.indexOf(':');  // Header 名和值的分隔位置
            if (separator <= 0) {
                continue;
            }
            const QByteArray name = normalizedHeaderName(line.left(separator));  // 规范化 Header 名
            const QByteArray value = line.mid(separator + 1).trimmed();  // 保留 Header 值原始字节
            request.headers.insert(name, value);
            if (name == QByteArrayLiteral("content-length")) {
                bool validLength = false;  // 标记 Content-Length 是否为合法非负整数
                contentLength = value.toInt(&validLength);
                if (!validLength || contentLength < 0) {
                    reject(socket, 400, "Bad Request");
                    return;
                }
            }
        }

        const qsizetype bodyOffset = headerEnd + 4;  // 请求正文起始位置
        if (buffer.size() - bodyOffset < contentLength) {
            return;
        }
        request.body = buffer.mid(bodyOffset, contentLength);
        const HttpRequestId requestId =  // 为当前请求生成仅在 HTTP 层使用的路由标识
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        pending.insert(requestId, socket);
        if (requestHandler) {
            requestHandler(requestId, request);
        } else {
            reject(socket, 503, "Service Unavailable");
        }
    }
};

HttpServer::HttpServer(QObject* parent)  // 创建尚未监听的 HTTP Server
    : QObject(parent)
    , d(std::make_unique<HttpServerPrivate>())
{
    d->owner = this;
    connect(&d->server,
            &QTcpServer::newConnection,
            this,
            [this] { d->acceptConnections(); });
}

HttpServer::~HttpServer()  // 关闭监听和全部活动连接
{
    close();
}

void HttpServer::setRequestHandler(HttpRequestHandler handler)  // 设置完整请求处理函数
{
    d->requestHandler = std::move(handler);
}

void HttpServer::setRequestClosedHandler(HttpRequestClosedHandler handler)  // 设置请求关闭处理函数
{
    d->closedHandler = std::move(handler);
}

McpResult<void> HttpServer::listen(
    const QHostAddress& address,  // 需要绑定的监听地址
    quint16 port)                // 需要绑定的端口，零表示自动选择
{                                // 开始监听指定地址和端口
    if (d->server.isListening()) {
        return McpResult<void>::success();
    }
    if (!d->server.listen(address, port)) {
        return McpResult<void>::failure(
            {McpErrorCode::TransportError, d->server.errorString()});
    }
    return McpResult<void>::success();
}

void HttpServer::close()  // 停止监听并关闭全部连接
{
    d->server.close();
    const QList<QTcpSocket*> sockets = d->buffers.keys();  // 固定本轮需要关闭的连接集合
    for (QTcpSocket* socket : sockets) {
        socket->abort();
    }
    d->buffers.clear();
    d->pending.clear();
    d->streaming.clear();
}

McpResult<void> HttpServer::startStream(
    const HttpRequestId& requestId,          // 需要切换为流式响应的 HTTP 请求
    const HttpServerResponse& response)      // 只使用状态码、原因和 Headers
{                                            // 发送 chunked 响应头并保留连接
    QTcpSocket* socket = d->pending.value(requestId);  // 查找仍可写入的请求连接
    if (!socket || d->streaming.contains(requestId)) {
        return McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP 请求已关闭或已开始流式响应")});
    }
    QByteArray bytes =
        "HTTP/1.1 " + QByteArray::number(response.statusCode) + " "
        + response.reason + "\r\n";  // 构造流式 HTTP 状态行
    for (auto iterator = response.headers.constBegin();
         iterator != response.headers.constEnd();
         ++iterator) {
        bytes += iterator.key() + ": " + iterator.value() + "\r\n";
    }
    bytes += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    socket->write(bytes);
    d->streaming.insert(requestId);
    return McpResult<void>::success();
}

McpResult<void> HttpServer::writeStream(
    const HttpRequestId& requestId,  // 目标流式 HTTP 请求
    const QByteArray& chunk)         // 需要作为一个 HTTP chunk 写入的数据
{                                    // 写入一个符合 HTTP/1.1 的 chunk
    QTcpSocket* socket = d->pending.value(requestId);  // 查找活动流式连接
    if (!socket || !d->streaming.contains(requestId)) {
        return McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP 流已关闭或尚未开始")});
    }
    socket->write(QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n");
    return McpResult<void>::success();
}

McpResult<void> HttpServer::finishStream(
    const HttpRequestId& requestId)  // 写入结束 chunk 并关闭连接
{                                    // 正常结束已开始的 chunked 响应
    QTcpSocket* socket = d->pending.take(requestId);  // 取得并移除最终响应路由
    d->streaming.remove(requestId);
    if (!socket) {
        return McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP 流已关闭或不存在")});
    }
    socket->write("0\r\n\r\n");
    socket->disconnectFromHost();
    return McpResult<void>::success();
}

McpResult<void> HttpServer::sendResponse(
    const HttpRequestId& requestId,       // 需要接收响应的 HTTP 请求
    const HttpServerResponse& response)  // 写入完整 HTTP 响应并关闭连接
{
    QTcpSocket* socket = d->pending.take(requestId);  // 取得并移除一次性响应路由
    if (!socket) {
        return McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP 请求已关闭或不存在")});
    }

    QByteArray bytes =  // 构造 HTTP 状态行
        "HTTP/1.1 " + QByteArray::number(response.statusCode) + " "
        + response.reason + "\r\n";
    for (auto iterator = response.headers.constBegin();
         iterator != response.headers.constEnd();
         ++iterator) {
        bytes += iterator.key() + ": " + iterator.value() + "\r\n";
    }
    bytes += "Content-Length: " + QByteArray::number(response.body.size())
             + "\r\nConnection: close\r\n\r\n" + response.body;
    socket->write(bytes);
    socket->disconnectFromHost();
    return McpResult<void>::success();
}

bool HttpServer::isListening() const  // 查询当前是否正在监听
{
    return d->server.isListening();
}

QHostAddress HttpServer::address() const  // 返回实际监听地址
{
    return d->server.serverAddress();
}

quint16 HttpServer::port() const  // 返回实际监听端口
{
    return d->server.serverPort();
}

} // namespace LibMcp::Internal
