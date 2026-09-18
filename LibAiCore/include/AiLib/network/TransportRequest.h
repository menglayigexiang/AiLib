#pragma once

#include <QUrl>
#include <QByteArray>
#include <QList>
#include <QPair>

namespace AiLib {
using TransportHeaders = QList<QPair<QByteArray, QByteArray>>;  // 原始 HTTP 请求头列表，保留响应中重复字段

struct TransportRequest {        // 与模型协议无关的一次 HTTP 请求
    QUrl url;                    // 完整请求地址，由 Adapter 生成
    QByteArray method = "POST";  // HTTP 方法，默认 POST
    TransportHeaders headers;    // 原始请求头，不解释认证或厂商语义
    QByteArray body;             // 待发送的原始请求体
};
}  // AiLib 命名空间结束
