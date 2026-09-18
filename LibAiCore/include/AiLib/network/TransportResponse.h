#pragma once

#include <AiLib/network/TransportRequest.h>
#include <optional>

namespace AiLib {
struct TransportResponse {          // HTTP 传输结果，HTTP 错误状态不等同网络传输失败
    std::optional<int> statusCode;  // 已收到的 HTTP 状态码，尚未收到时未知
    TransportHeaders headers;       // 服务端原始响应头
    QByteArray body;                // 已收到的响应体，传输失败时也可保留部分字节
};
}  // AiLib 命名空间结束
