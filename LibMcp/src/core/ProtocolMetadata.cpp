#include "ProtocolMetadata_p.h"

#include <LibMcp/LibMcpGlobal.h>

#include <QJsonArray>

namespace LibMcp::Internal {
namespace {

const QString protocolVersionKey =  // RequestMeta 中的协议版本扩展键
    QStringLiteral("io.modelcontextprotocol/protocolVersion");
const QString clientCapabilitiesKey =  // RequestMeta 中的客户端能力扩展键
    QStringLiteral("io.modelcontextprotocol/clientCapabilities");
const QString clientInfoKey =  // RequestMeta 中的客户端身份扩展键
    QStringLiteral("io.modelcontextprotocol/clientInfo");

McpError invalidMetadata(
    const QString& message,  // 返回给调用方的错误说明
    int remoteCode,          // 对应的 JSON-RPC 标准或 MCP 错误码
    const QJsonValue& data = {})  // 可选的结构化错误数据
{                                // 构造协议元数据校验错误
    return {McpErrorCode::InvalidMessage,
            message,
            remoteCode,
            data};
}

} // namespace

QJsonObject makeRequestMetadata(
    const QJsonObject& clientCapabilities,  // 本次请求声明的客户端能力
    const QJsonObject& clientInfo)          // 本次请求携带的客户端身份
{                                           // 生成符合 2026-07-28 的自包含请求元数据
    QJsonObject metadata{  // 保存写入 params._meta 的标准扩展字段
        {protocolVersionKey, QStringLiteral(LIBMCP_PROTOCOL_VERSION)},
        {clientCapabilitiesKey, clientCapabilities}};
    if (!clientInfo.isEmpty()) {
        metadata.insert(clientInfoKey, clientInfo);
    }
    return metadata;
}

McpResult<RequestMetadata> decodeRequestMetadata(
    const QJsonObject& params)  // 待校验的 JSON-RPC 请求参数
{                               // 校验并解析 2026-07-28 请求元数据
    const QJsonValue metaValue =  // 读取请求级元数据容器
        params.value(QStringLiteral("_meta"));
    if (!metaValue.isObject()) {
        return McpResult<RequestMetadata>::failure(invalidMetadata(
            QStringLiteral("Missing request metadata"),
            -32602));
    }

    const QJsonObject metadata = metaValue.toObject();  // 保存待解析的请求级元数据
    const QJsonValue versionValue = metadata.value(protocolVersionKey);  // 读取协议版本值
    if (!versionValue.isString()
        || versionValue.toString() != QStringLiteral(LIBMCP_PROTOCOL_VERSION)) {
        const QString requestedVersion =  // 保存客户端实际提交的版本，缺失时为空字符串
            versionValue.isString() ? versionValue.toString() : QString{};
        return McpResult<RequestMetadata>::failure(
            {McpErrorCode::UnsupportedProtocolVersion,
             QStringLiteral("Unsupported protocol version"),
             -32022,
             QJsonObject{
                 {QStringLiteral("requested"), requestedVersion},
                 {QStringLiteral("supported"),
                  QJsonArray{QStringLiteral(LIBMCP_PROTOCOL_VERSION)}}}});
    }

    const QJsonValue capabilitiesValue = metadata.value(clientCapabilitiesKey);  // 读取客户端能力值
    if (!capabilitiesValue.isObject()) {
        return McpResult<RequestMetadata>::failure(invalidMetadata(
            QStringLiteral("Missing client capabilities"),
            -32602));
    }

    const QJsonValue clientInfoValue = metadata.value(clientInfoKey);  // 读取可选客户端身份值
    if (!clientInfoValue.isUndefined() && !clientInfoValue.isObject()) {
        return McpResult<RequestMetadata>::failure(invalidMetadata(
            QStringLiteral("Invalid client info"),
            -32602));
    }

    RequestMetadata result;  // 保存本次请求解析出的客户端上下文
    result.clientCapabilities = capabilitiesValue.toObject();
    result.clientInfo = clientInfoValue.toObject();
    result.progressToken = metadata.value(QStringLiteral("progressToken"));
    return McpResult<RequestMetadata>::success(std::move(result));
}

} // namespace LibMcp::Internal
