#include <LibMcp/StreamableHttpTransport.h>

#include "../core/FutureUtils_p.h"
#include "../core/JsonRpcCodec_p.h"
#include "../http/HttpServer_p.h"

#include <LibMcp/LibMcpGlobal.h>

#include <QJsonDocument>
#include <QSet>
#include <QUrl>

namespace LibMcp {
namespace {

Internal::HttpServerResponse emptyResponse(
    int statusCode,          // HTTP 状态码
    QByteArray reason)       // HTTP 原因短语
{                            // 构造没有正文的 HTTP 响应
    Internal::HttpServerResponse response;  // 保存待发送的空响应
    response.statusCode = statusCode;
    response.reason = std::move(reason);
    return response;
}

Internal::HttpServerResponse jsonErrorResponse(
    const QJsonValue& id,       // JSON-RPC 请求 ID
    int code,                   // JSON-RPC 或 MCP 标准错误码
    const QString& message)     // 错误说明
{                               // 构造 HTTP 400 JSON-RPC 错误响应
    Internal::HttpServerResponse response;  // 保存待发送的协议错误响应
    response.statusCode = 400;
    response.reason = QByteArrayLiteral("Bad Request");
    response.headers.insert(QByteArrayLiteral("Content-Type"),
                            QByteArrayLiteral("application/json"));
    response.body = QJsonDocument(
                        Internal::makeError(id, code, message))
                        .toJson(QJsonDocument::Compact);
    return response;
}

bool isAllowedOrigin(const QByteArray& originHeader)  // 判断 Origin 是否属于本机回环地址
{
    if (originHeader.isEmpty()) {
        return true;
    }
    const QUrl origin = QUrl::fromEncoded(originHeader);  // 解析浏览器提交的 Origin
    const QString host = origin.host().toLower();  // 提取大小写无关的主机名
    return host == QStringLiteral("localhost")
           || host == QStringLiteral("127.0.0.1")
           || host == QStringLiteral("::1");
}

} // namespace

// 保存 Streamable HTTP Server Transport 的配置和通用 HTTP facade。
class StreamableHttpServerTransportPrivate
{
public:
    StreamableHttpServerTransport* owner = nullptr;  // 接收 MCP 消息的 Transport
    QHostAddress requestedAddress;                   // 配置的监听地址
    quint16 requestedPort = 0;                       // 配置的监听端口
    QString path;                                    // 唯一 MCP POST Endpoint
    Internal::HttpServer server;                     // 不感知 MCP 语义的通用 HTTP Server
    QSet<Internal::HttpRequestId> streams;            // 已切换为请求级 SSE 的 HTTP 响应

    void receive(
        const Internal::HttpRequestId& requestId,  // 当前 HTTP 请求路由标识
        const Internal::HttpRequest& request)      // 当前完整 HTTP 请求
    {                                               // 校验 HTTP 层 MCP 约束并转交 JSON-RPC 消息
        if (request.method != QByteArrayLiteral("POST")) {
            server.sendResponse(
                requestId,
                emptyResponse(405, QByteArrayLiteral("Method Not Allowed")));
            return;
        }
        if (QString::fromUtf8(request.target) != path) {
            server.sendResponse(
                requestId,
                emptyResponse(404, QByteArrayLiteral("Not Found")));
            return;
        }
        if (!isAllowedOrigin(request.headers.value(QByteArrayLiteral("origin")))) {
            server.sendResponse(
                requestId,
                emptyResponse(403, QByteArrayLiteral("Forbidden")));
            return;
        }
        const QByteArray contentType =  // 读取不含参数的请求媒体类型
            request.headers.value(QByteArrayLiteral("content-type"))
                .split(';').value(0).trimmed().toLower();
        if (contentType != QByteArrayLiteral("application/json")) {
            server.sendResponse(
                requestId,
                emptyResponse(415, QByteArrayLiteral("Unsupported Media Type")));
            return;
        }

        QJsonParseError parseError;  // 保存 JSON 解析失败位置与原因
        const QJsonDocument document =  // 解析完整 JSON-RPC 请求正文
            QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError
            || !document.isObject()) {
            server.sendResponse(
                requestId,
                jsonErrorResponse({}, -32700, QStringLiteral("Parse error")));
            return;
        }

        const QJsonObject message = document.object();  // 保存待校验的 JSON-RPC 消息
        const QJsonValue id = message.value(QStringLiteral("id"));  // 保存错误响应需要回显的请求 ID
        const QString method = message.value(QStringLiteral("method")).toString();  // 读取正文中的 MCP 方法
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();  // 读取正文中的请求参数
        const QJsonObject meta = params.value(QStringLiteral("_meta")).toObject();  // 读取正文中的请求元数据
        const QString bodyVersion =  // 读取正文中的 MCP 协议版本
            meta.value(QStringLiteral("io.modelcontextprotocol/protocolVersion"))
                .toString();
        const QString headerVersion = QString::fromUtf8(  // 读取 HTTP 协议版本 Header
            request.headers.value(QByteArrayLiteral("mcp-protocol-version")));
        const QString headerMethod = QString::fromUtf8(  // 读取 HTTP 方法路由 Header
            request.headers.value(QByteArrayLiteral("mcp-method")));
        const QString bodyName = params.value(QStringLiteral("name")).toString();  // 读取正文中的可选能力名称
        const QString headerName = QString::fromUtf8(  // 读取 HTTP 能力名称 Header
            request.headers.value(QByteArrayLiteral("mcp-name")));

        if (headerVersion != bodyVersion
            || headerVersion != QStringLiteral(LIBMCP_PROTOCOL_VERSION)
            || headerMethod != method
            || (!headerName.isEmpty() && headerName != bodyName)) {
            server.sendResponse(
                requestId,
                jsonErrorResponse(id,
                                  -32020,
                                  QStringLiteral("MCP HTTP headers do not match request body")));
            return;
        }

        if (!message.contains(QStringLiteral("id"))) {
            emit owner->messageReceived(requestId, message);
            server.sendResponse(
                requestId,
                emptyResponse(202, QByteArrayLiteral("Accepted")));
            return;
        }
        emit owner->messageReceived(requestId, message);
    }
};

StreamableHttpServerTransport::StreamableHttpServerTransport(
    QHostAddress address,  // 配置的监听地址
    quint16 port,          // 配置的监听端口
    QString path,          // 唯一 MCP POST Endpoint
    QObject* parent)       // 可选 QObject 所有者
    : McpServerTransport(parent)
    , d(std::make_unique<StreamableHttpServerTransportPrivate>())
{  // 创建基于通用 HttpServer 的 MCP Transport
    d->owner = this;
    d->requestedAddress = std::move(address);
    d->requestedPort = port;
    d->path = path.startsWith(QLatin1Char('/'))
                  ? std::move(path)
                  : QStringLiteral("/") + path;
    d->server.setRequestHandler(
        [this](const Internal::HttpRequestId& requestId,  // 当前 HTTP 请求路由标识
               const Internal::HttpRequest& request) {   // 当前完整 HTTP 请求
            d->receive(requestId, request);
        });
    d->server.setRequestClosedHandler(
        [this](const Internal::HttpRequestId& requestId) {  // 已被对端关闭的 HTTP 请求
            d->streams.remove(requestId);
            emit requestClosed(requestId);
        });
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() = default;  // 释放内部 HTTP Server

QFuture<McpResult<void>> StreamableHttpServerTransport::start()  // 启动内部 HTTP Server
{
    return Internal::readyFuture(
        d->server.listen(d->requestedAddress, d->requestedPort));
}

QFuture<McpResult<void>> StreamableHttpServerTransport::stop()  // 停止内部 HTTP Server
{
    d->server.close();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpServerTransport::sendMessage(
    const McpTransportRequestId& requestId,  // 需要接收响应的 HTTP 请求标识
    const QJsonObject& message)              // 写入该 HTTP 请求的完整 JSON-RPC 响应
{
    const QByteArray event =  // 编码一个请求级 SSE MCP 消息
        "event: message\ndata: "
        + QJsonDocument(message).toJson(QJsonDocument::Compact)
        + "\n\n";
    if (message.contains(QStringLiteral("method"))) {
        if (!d->streams.contains(requestId)) {
            Internal::HttpServerResponse streamResponse;  // 配置 SSE 响应头
            streamResponse.headers.insert(
                QByteArrayLiteral("Content-Type"),
                QByteArrayLiteral("text/event-stream"));
            streamResponse.headers.insert(
                QByteArrayLiteral("Cache-Control"),
                QByteArrayLiteral("no-cache"));
            const McpResult<void> startResult =  // 保留当前 HTTP 连接以发送后续事件
                d->server.startStream(requestId, streamResponse);
            if (startResult.isError()) {
                return Internal::readyFuture(startResult);
            }
            d->streams.insert(requestId);
        }
        return Internal::readyFuture(d->server.writeStream(requestId, event));
    }
    if (d->streams.remove(requestId)) {
        const McpResult<void> writeResult =  // 在结束流前发送最终 JSON-RPC 响应
            d->server.writeStream(requestId, event);
        if (writeResult.isError()) {
            return Internal::readyFuture(writeResult);
        }
        return Internal::readyFuture(d->server.finishStream(requestId));
    }
    Internal::HttpServerResponse response;  // 保存通用 HTTP facade 需要的响应数据
    response.headers.insert(QByteArrayLiteral("Content-Type"),
                            QByteArrayLiteral("application/json"));
    response.body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    return Internal::readyFuture(
        d->server.sendResponse(requestId, response));
}

QHostAddress StreamableHttpServerTransport::address() const  // 返回实际或配置的监听地址
{
    return d->server.isListening() ? d->server.address()
                                   : d->requestedAddress;
}

quint16 StreamableHttpServerTransport::port() const  // 返回实际或配置的监听端口
{
    return d->server.isListening() ? d->server.port()
                                   : d->requestedPort;
}

QString StreamableHttpServerTransport::path() const  // 返回标准化 MCP Endpoint 路径
{
    return d->path;
}

} // namespace LibMcp
