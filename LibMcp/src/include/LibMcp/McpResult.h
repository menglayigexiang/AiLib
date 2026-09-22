#pragma once

#include <LibMcp/LibMcpGlobal.h>

#include <QJsonValue>
#include <QString>

#include <utility>
#include <variant>

namespace LibMcp {

/// LibMcp 公共操作可能返回的错误类别。
enum class McpErrorCode
{
    TransportError,
    ConnectionClosed,
    RequestTimeout,
    InvalidMessage,
    InvalidResponse,
    ProtocolError,
    RemoteError,
    SchemaValidationFailed,
    InvalidState,
    UnsupportedProtocolVersion,
    CapabilityNotSupported,
    ToolError,
    BackendStopped,
    Cancelled,
    InternalError
};

/// 描述一次失败操作，并保留远端 JSON-RPC 错误信息。
struct LIBMCP_EXPORT McpError
{
    /// SDK 内部归一化后的错误类别。
    McpErrorCode code = McpErrorCode::InternalError;
    /// 可供日志或界面展示的错误说明。
    QString message;
    /// 远端 JSON-RPC 错误码；非远端错误时为 0。
    int remoteCode = 0;
    /// 远端提供的开放错误数据。
    QJsonValue data;
};

/// 显式保存成功值或错误，避免通过异常表达异步失败。
template<typename T>
class McpResult
{
public:
    static McpResult success(T value) { return McpResult(std::move(value)); }
    static McpResult failure(McpError error) { return McpResult(std::move(error)); }

    bool isSuccess() const { return std::holds_alternative<T>(m_storage); }
    bool isError() const { return !isSuccess(); }
    const T &value() const { return std::get<T>(m_storage); }
    T &value() { return std::get<T>(m_storage); }
    const McpError &error() const { return std::get<McpError>(m_storage); }

private:
    explicit McpResult(T value) : m_storage(std::move(value)) {}
    explicit McpResult(McpError error) : m_storage(std::move(error)) {}

    /// 成功值和错误值共用的互斥存储。
    std::variant<T, McpError> m_storage;
};

/// 不携带成功值的结果特化。
template<>
class McpResult<void>
{
public:
    static McpResult success() { return McpResult(true, {}); }
    static McpResult failure(McpError error) { return McpResult(false, std::move(error)); }

    bool isSuccess() const { return m_success; }
    bool isError() const { return !m_success; }
    const McpError &error() const { return m_error; }

private:
    McpResult(bool success, McpError error)
        : m_success(success), m_error(std::move(error))
    {
    }

    /// 操作是否成功完成。
    bool m_success = false;
    /// 失败时保存的错误；成功时为空错误对象。
    McpError m_error;
};

} // namespace LibMcp

Q_DECLARE_METATYPE(LibMcp::McpError)
