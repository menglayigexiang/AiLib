#pragma once

#include <LibMcp/McpResult.h>

#include <QJsonObject>

namespace LibMcp::Internal {

// 保存从单次请求元数据中解析出的客户端上下文，不跨请求持久化。
struct RequestMetadata
{
    QJsonObject clientCapabilities;  // 本次请求声明的客户端能力
    QJsonObject clientInfo;          // 本次请求携带的可选客户端身份
    QJsonValue progressToken;        // 可选的请求内进度路由 Token
};

QJsonObject makeRequestMetadata(
    const QJsonObject& clientCapabilities,  // 本次请求声明的客户端能力
    const QJsonObject& clientInfo);         // 使用本次请求的能力和身份生成 2026-07-28 元数据

McpResult<RequestMetadata> decodeRequestMetadata(
    const QJsonObject& params);  // 校验并解析待处理 JSON-RPC 参数中的 2026-07-28 元数据

} // namespace LibMcp::Internal
