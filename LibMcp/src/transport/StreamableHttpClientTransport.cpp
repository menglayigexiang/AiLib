#include <LibMcp/StreamableHttpTransport.h>

#include "../core/FutureUtils_p.h"
#include "../http/HttpClient_p.h"

#include <LibMcp/LibMcpGlobal.h>

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonParseError>

#include <optional>

namespace LibMcp {
namespace {

constexpr qsizetype maximumSseEventBytes = 1024 * 1024;  // 单个未完成 SSE 事件的最大字节数

} // namespace

// 保存 Streamable HTTP Client Transport 的配置和通用 HTTP facade。
class StreamableHttpClientTransportPrivate
{
public:
    QUrl endpoint;                // MCP HTTP Endpoint
    QJsonObject headers;          // 使用者配置的额外 HTTP Headers
    Internal::HttpClient client;  // 不感知 MCP 语义的通用 HTTP Client
    bool running = false;         // Transport 当前是否允许发送消息
};

StreamableHttpClientTransport::StreamableHttpClientTransport(
    QUrl endpoint,        // MCP HTTP Endpoint
    QJsonObject headers,  // 使用者配置的额外 HTTP Headers
    QObject* parent)      // 可选 QObject 所有者
    : McpClientTransport(parent)
    , d(std::make_unique<StreamableHttpClientTransportPrivate>())
{  // 创建基于通用 HttpClient 的 MCP Transport
    d->endpoint = std::move(endpoint);
    d->headers = std::move(headers);
}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;  // 释放内部 HTTP Client

QFuture<McpResult<void>> StreamableHttpClientTransport::start()  // 校验 Endpoint 并允许发送消息
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

QFuture<McpResult<void>> StreamableHttpClientTransport::stop()  // 停止 Transport 并中止活动 HTTP 请求
{
    d->running = false;
    d->client.abortAll();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StreamableHttpClientTransport::sendMessage(
    const QJsonObject& message)  // 使用独立 HTTP POST 发送一个完整 MCP 消息
{
    if (!d->running) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("HTTP Transport 已停止")}));
    }

    Internal::HttpRequest request;  // 保存交给通用 HTTP facade 的请求数据
    request.url = d->endpoint;
    request.method = QByteArrayLiteral("POST");
    for (auto iterator = d->headers.constBegin();
         iterator != d->headers.constEnd();
         ++iterator) {
        request.headers.insert(iterator.key().toUtf8(),
                               iterator.value().toString().toUtf8());
    }
    request.headers.insert(QByteArrayLiteral("Content-Type"),
                           QByteArrayLiteral("application/json"));
    request.headers.insert(QByteArrayLiteral("Accept"),
                           QByteArrayLiteral("application/json, text/event-stream"));
    request.headers.insert(QByteArrayLiteral("MCP-Protocol-Version"),
                           QByteArrayLiteral(LIBMCP_PROTOCOL_VERSION));

    const QString method = message.value(QStringLiteral("method")).toString();  // 提取 MCP 方法供 HTTP 路由检查
    if (method == QStringLiteral("subscriptions/listen")) {
        request.transferTimeout = std::chrono::milliseconds::zero();  // 长寿命 SSE 订阅不使用普通请求空闲超时
    }
    if (!method.isEmpty()) {
        request.headers.insert(QByteArrayLiteral("Mcp-Method"),
                               method.toUtf8());
    }
    const QString name =  // 提取工具、资源或 Prompt 名称供 HTTP 路由检查
        message.value(QStringLiteral("params"))
            .toObject()
            .value(QStringLiteral("name"))
            .toString();
    if (!name.isEmpty()) {
        request.headers.insert(QByteArrayLiteral("Mcp-Name"),
                               name.toUtf8());
    }
    request.body = QJsonDocument(message).toJson(QJsonDocument::Compact);

    Internal::Promise<McpResult<void>> promise;  // 保存 Transport 异步结果写入端
    const QFuture<McpResult<void>> future = promise.future();  // 返回给调用方的结果读取端
    const auto sseBuffer = std::make_shared<QByteArray>();  // 保存尚未组成完整 SSE 事件的字节
    const auto receivedSse = std::make_shared<bool>(false);  // 标记当前 HTTP 响应是否为 SSE
    const auto sseError = std::make_shared<std::optional<McpError>>();  // 保存 SSE framing 超限等错误
    const auto validSseMessages = std::make_shared<qsizetype>(0);  // 记录已成功解码的 MCP SSE 消息数
    const auto handleData =
        [this, sseBuffer, receivedSse, sseError, validSseMessages](
            const Internal::HttpResponse& response,  // 当前响应头
            const QByteArray& chunk) {               // 本次收到的正文字节
            const QByteArray contentType =  // 读取不含参数的响应媒体类型
                response.headers.value(QByteArrayLiteral("content-type"))
                    .split(';').value(0).trimmed().toLower();
            if (contentType != QByteArrayLiteral("text/event-stream")) {
                return false;
            }
            *receivedSse = true;
            if (sseError->has_value()) {
                return true;
            }
            sseBuffer->append(chunk);
            if (sseBuffer->size() > maximumSseEventBytes
                && !sseBuffer->contains("\n\n")
                && !sseBuffer->contains("\r\n\r\n")) {
                *sseError = McpError{
                    McpErrorCode::InvalidMessage,
                    QStringLiteral("SSE 事件超过 1 MiB 安全上限")};
                sseBuffer->clear();
                return true;
            }
            sseBuffer->replace("\r\n", "\n");
            while (true) {
                const qsizetype eventEnd = sseBuffer->indexOf("\n\n");  // 定位一个完整 SSE 事件
                if (eventEnd < 0) {
                    break;
                }
                if (eventEnd > maximumSseEventBytes) {
                    *sseError = McpError{
                        McpErrorCode::InvalidMessage,
                        QStringLiteral("SSE 事件超过 1 MiB 安全上限")};
                    sseBuffer->clear();
                    return true;
                }
                const QByteArray event = sseBuffer->left(eventEnd);  // 取出当前完整事件
                sseBuffer->remove(0, eventEnd + 2);
                QByteArray data;  // 合并 SSE 事件中的所有 data 行
                bool hasDataField = false;  // 区分空 data 字段与纯注释或空事件
                for (const QByteArray& line : event.split('\n')) {
                    if (line.startsWith("data:")) {
                        hasDataField = true;
                        if (!data.isEmpty()) {
                            data.append('\n');
                        }
                        data.append(line.mid(5).trimmed());
                    }
                }
                if (!hasDataField || data.isEmpty()) {
                    *sseError = McpError{
                        McpErrorCode::InvalidMessage,
                        QStringLiteral("SSE 事件缺少非空 data 字段")};
                    sseBuffer->clear();
                    return true;
                }
                QJsonParseError parseError;  // 记录 SSE data 中 JSON 的解析结果
                const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);  // 解码当前 MCP 消息
                if (parseError.error != QJsonParseError::NoError
                    || !document.isObject()) {
                    *sseError = McpError{
                        McpErrorCode::InvalidMessage,
                        QStringLiteral("SSE data 不是合法 JSON 对象")};
                    sseBuffer->clear();
                    return true;
                }
                ++(*validSseMessages);
                emit messageReceived(document.object());
            }
            return true;
        };
    auto* watcher =  // 监视通用 HTTP facade 的完整响应
        new QFutureWatcher<McpResult<Internal::HttpResponse>>(this);
    connect(
        watcher,
        &QFutureWatcher<McpResult<Internal::HttpResponse>>::finished,
        this,
        [this, watcher, promise, sseBuffer, receivedSse, sseError, validSseMessages] {  // 将通用 HTTP 响应解释为 MCP 消息
            const McpResult<Internal::HttpResponse> result =  // 读取已完成的 HTTP 交换结果
                watcher->result();
            watcher->deleteLater();
            if (result.isError()) {
                promise.finish(McpResult<void>::failure(result.error()));
                return;
            }

            const Internal::HttpResponse& response = result.value();  // 读取状态码、Headers 和正文
            if (response.statusCode == 202 && response.body.trimmed().isEmpty()) {
                promise.finish(McpResult<void>::success());
                return;
            }
            if (response.statusCode < 200 || response.statusCode >= 300) {
                promise.finish(McpResult<void>::failure(
                    {McpErrorCode::TransportError,
                     QStringLiteral("MCP HTTP 响应状态码为 %1")
                         .arg(response.statusCode)}));
                return;
            }

            if (*receivedSse) {
                if (sseError->has_value()) {
                    promise.finish(McpResult<void>::failure(**sseError));
                    return;
                }
                if (!sseBuffer->trimmed().isEmpty()) {
                    promise.finish(McpResult<void>::failure(
                        {McpErrorCode::InvalidMessage,
                         QStringLiteral("SSE 连接结束时存在未完成事件")}));
                    return;
                }
                if (*validSseMessages == 0) {
                    promise.finish(McpResult<void>::failure(
                        {McpErrorCode::InvalidMessage,
                         QStringLiteral("SSE 响应未包含有效 MCP 消息")}));
                    return;
                }
                promise.finish(McpResult<void>::success());
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(response.body);  // 解析非流式 JSON 响应
            if (!document.isObject()) {
                promise.finish(McpResult<void>::failure(
                    {McpErrorCode::InvalidMessage,
                     QStringLiteral("HTTP 响应不是 JSON 对象")}));
                return;
            }
            emit messageReceived(document.object());
            promise.finish(McpResult<void>::success());
        });
    watcher->setFuture(d->client.send(request, handleData));
    return future;
}

} // namespace LibMcp
