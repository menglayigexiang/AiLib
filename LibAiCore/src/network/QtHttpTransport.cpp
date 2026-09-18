#include <AiLib/network/QtHttpTransport.h>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <chrono>
#include <limits>
#include <QTimer>
#include <memory>

namespace AiLib {
bool QtHttpTransport::send(const TransportRequest& request,  // 原始 HTTP 请求
                           const RequestOptions& options,    // 单次超时及取消
                           TransportResponse& response,      // 完整正文及元数据
                           SdkError& error)                  // 使用统一网络流程进行普通请求
{
    return sendImpl(request, options, response, {}, error);
}
bool QtHttpTransport::sendStream(const TransportRequest& request,      // 原始 HTTP 请求
                                 const RequestOptions& options,        // 单次超时及取消
                                 TransportResponse& response,          // HTTP 元数据及错误正文
                                 const TransportDataCallback& onData,  // 同步原始片段接收器
                                 SdkError& error)                      // 使用统一网络流程进行流式请求
{
    return sendImpl(request, options, response, onData, error);
}
bool QtHttpTransport::sendImpl(
    const TransportRequest& request,      // 原始请求，不包含模型业务逻辑
    const RequestOptions& options,        // 超时与取消由当前调用线程检查
    TransportResponse& response,          // 保留已收到的状态、头和部分响应体
    const TransportDataCallback& onData,  // 原始流式接收器，普通请求时为空
    SdkError& error)                      // 同步发送，不自动重试，不创建线程
{
    response = {};
    error = {};
    if (!QCoreApplication::instance()) {
        error.category = ErrorCategory::Configuration;
        error.code = QStringLiteral("MissingApplication");
        error.message = QStringLiteral("Qt transport requires QCoreApplication");
        return false;
    }
    if (!request.url.isValid() || request.url.host().isEmpty() ||
        (request.url.scheme() != QStringLiteral("http") &&
         request.url.scheme() != QStringLiteral("https")) ||
        request.method.isEmpty() || (options.timeoutSeconds != -1 && options.timeoutSeconds <= 0)) {
        error.category = ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("InvalidTransportRequest");
        error.message = QStringLiteral("Invalid HTTP URL, method or timeout");
        return false;
    }
    if (options.cancellation.isCancellationRequested()) {
        error.category = ErrorCategory::Cancelled;
        error.code = QStringLiteral("Cancelled");
        error.message = QStringLiteral("Request cancelled before sending");
        return false;
    }

    auto deadline = options.deadline;  // 调用链总预算或空值
    if (options.timeoutSeconds > 0) {
        const auto attemptDeadline =  // 单次尝试的独立上限
            std::chrono::steady_clock::now() + std::chrono::seconds(options.timeoutSeconds);
        if (!deadline || attemptDeadline < *deadline)
            deadline = attemptDeadline;
    }
    if (deadline && std::chrono::steady_clock::now() >= *deadline) {
        error.category = ErrorCategory::Timeout;
        error.code = QStringLiteral("Timeout");
        error.message = QStringLiteral("Request deadline expired before sending");
        return false;
    }
    QNetworkAccessManager manager;                // 仅在当前请求调用线程创建和使用的网络管理器
    QNetworkRequest networkRequest(request.url);  // 本次 Qt HTTP 请求
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::ManualRedirectPolicy);
    for (const auto& header : request.headers)  // 当前待写入的原始 HTTP 请求头
        networkRequest.setRawHeader(header.first, header.second);
    if (options.cancellation.isCancellationRequested() ||
        (deadline && std::chrono::steady_clock::now() >= *deadline)) {
        const bool cancelled =  // 发送边界再次确认主动取消优先
            options.cancellation.isCancellationRequested();
        error.category = cancelled ? ErrorCategory::Cancelled : ErrorCategory::Timeout;
        error.code = cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Timeout");
        error.message = QStringLiteral("Request stopped before sending");
        return false;
    }
    std::unique_ptr<QNetworkReply> reply(manager.sendCustomRequest(  // 请求级响应，先于 manager 销毁
        networkRequest, request.method, request.body));
    QEventLoop loop;              // 仅用于同步等待当前请求的局部事件循环
    QTimer timeoutTimer;          // 精确剩余毫秒预算的一次性超时通知
    QTimer poll;                  // 检查协作取消和截止时间的请求级定时器
    bool cancelled = false;       // 是否由调用方取消本次请求
    bool timedOut = false;        // 是否由当前尝试超时中止请求
    bool callbackFailed = false;  // 解码或事件接收器是否要求停止读取
    SdkError callbackError;       // 接收端错误，避免被网络 abort 错误覆盖
    const auto consume = [&] {    // 更新元数据并交付原始字节，不判断模型响应完整性
        const auto status =       // 当前已收到的 HTTP 状态
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (status.isValid())
            response.statusCode = status.toInt();
        response.headers = reply->rawHeaderPairs();
        const QByteArray bytes = reply->readAll();  // 本次可读取的原始网络片段
        if (bytes.isEmpty() || callbackFailed)
            return;
        if (!onData || !response.statusCode || *response.statusCode < 200 ||
            *response.statusCode >= 300) {
            response.body += bytes;
            return;
        }
        if (!onData(response, bytes, callbackError)) {
            callbackFailed = true;
            reply->abort();
            loop.quit();
        }
    };
    QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop,
                     [&] {  // 在请求线程检查取消和超时，避免跨线程直接操作 reply
                         if (options.cancellation.isCancellationRequested()) {
                             cancelled = true;
                             reply->abort();
                             loop.quit();
                         } else if (deadline && std::chrono::steady_clock::now() >= *deadline) {
                             timedOut = true;
                             reply->abort();
                             loop.quit();
                         }
                     });
    if (deadline) {
        const auto remaining =  // 创建网络对象后重新计算预算
            *deadline - std::chrono::steady_clock::now();
        const auto milliseconds =  // 当前剩余整毫秒
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        const qint64 delay = milliseconds.count() +  // 向上取整不足一毫秒，避免整秒误差
                             (remaining > milliseconds ? 1 : 0);
        if (delay <= std::numeric_limits<int>::max()) {
            timeoutTimer.setSingleShot(true);
            timeoutTimer.setTimerType(Qt::PreciseTimer);
            QObject::connect(&timeoutTimer, &QTimer::timeout, &loop,
                             [&] {  // 在网络所属线程中止到期请求
                                 if (deadline && std::chrono::steady_clock::now() < *deadline) {
                                     timeoutTimer.start(1);
                                     return;
                                 }
                                 cancelled = options.cancellation.isCancellationRequested();
                                 timedOut = !cancelled;
                                 reply->abort();
                                 loop.quit();
                             });
            timeoutTimer.start(static_cast<int>(delay > 0 ? delay : 0));
        }
    }
    poll.start(20);
    if (!reply->isFinished())
        loop.exec();
    poll.stop();
    timeoutTimer.stop();
    if (reply->isOpen())
        consume();
    const auto status = reply->attribute(  // 已收到的 HTTP 状态，未收到时 QVariant 无效
        QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid())
        response.statusCode = status.toInt();
    response.headers = reply->rawHeaderPairs();

    if (callbackFailed) {
        error = callbackError;
        error.httpStatus = response.statusCode;
        return false;
    }
    cancelled = cancelled || options.cancellation.isCancellationRequested();
    timedOut = timedOut || (deadline && std::chrono::steady_clock::now() >= *deadline);
    if (cancelled || timedOut) {
        error.category = cancelled ? ErrorCategory::Cancelled : ErrorCategory::Timeout;
        error.code = cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Timeout");
        error.message = cancelled ? QStringLiteral("Request cancelled")
                                  : QStringLiteral("HTTP request timed out");
    } else if (reply->error() != QNetworkReply::NoError &&
               !(response.statusCode && *response.statusCode >= 400 &&
                 int(reply->error()) >= 200)) {
        error.category = ErrorCategory::Network;
        error.code = QStringLiteral("NetworkError");
        error.message = reply->errorString();
        error.details.insert(QStringLiteral("networkError"), int(reply->error()));
        bool retryable = false;  // TLS、主动 abort 和其他永久性网络错误不重试
        switch (reply->error()) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyNotFoundError:
        case QNetworkReply::ProxyTimeoutError:
            retryable = true;
            break;
        default:
            break;
        }
        error.details.insert(QStringLiteral("retryable"), retryable);
    } else {
        return true;  // HTTP 4xx/5xx 返回给 Adapter，Transport 不解释服务端业务错误
    }
    error.httpStatus = response.statusCode;
    return false;
}
}  // AiLib 命名空间结束
