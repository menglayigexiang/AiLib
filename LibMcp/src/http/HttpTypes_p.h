#pragma once

#include <QByteArray>
#include <QHash>
#include <QUrl>

#include <chrono>

namespace LibMcp::Internal {

using HttpHeaders = QHash<QByteArray, QByteArray>;  // 保存不解释业务语义的 HTTP Headers

// 描述一次通用 HTTP 请求，不包含 MCP 协议语义。
struct HttpRequest
{
    QUrl url;            // 请求目标地址
    QByteArray method;   // 大写 HTTP 方法
    QByteArray target;   // Server 收到的原始请求目标
    HttpHeaders headers; // 请求 Headers
    QByteArray body;     // 完整请求正文
    std::chrono::milliseconds transferTimeout{30000};  // 无数据传输超时，零表示长寿命流不限时
};

// 描述一次完整 HTTP 响应，不包含 MCP 协议语义。
struct HttpResponse
{
    int statusCode = 0;  // HTTP 状态码，未收到响应时为零
    HttpHeaders headers; // 响应 Headers，名称统一转为小写
    QByteArray body;     // 完整响应正文
};

// 描述 HttpServer 需要写回指定请求的完整响应。
struct HttpServerResponse
{
    int statusCode = 200;       // HTTP 状态码
    QByteArray reason = "OK";   // HTTP 原因短语
    HttpHeaders headers;        // 响应 Headers
    QByteArray body;            // 完整响应正文
};

} // namespace LibMcp::Internal
