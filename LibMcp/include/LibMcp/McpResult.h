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
    McpErrorCode code = McpErrorCode::InternalError;  // SDK 内部归一化后的错误类别
    /// 可供日志或界面展示的错误说明。
    QString message;                                  // 可供日志或界面展示的错误说明
    /// 远端 JSON-RPC 错误码；非远端错误时为 0。
    int remoteCode = 0;                               // 远端 JSON-RPC 错误码，本地错误为零
    /// 远端提供的开放错误数据。
    QJsonValue data;                                  // 远端提供的开放错误数据
};

/// 显式保存成功值或错误，避免通过异常表达异步失败。
template<typename T>
class McpResult
{
public:
    static McpResult success(T value) { return McpResult(std::move(value)); }  // 构造包含成功值的结果
    static McpResult failure(McpError error) { return McpResult(std::move(error)); }  // 构造包含错误的结果

    bool isSuccess() const { return std::holds_alternative<T>(m_storage); }  // 查询是否包含成功值
    bool isError() const { return !isSuccess(); }                            // 查询是否包含错误
    const T &value() const { return std::get<T>(m_storage); }                // 读取不可变成功值
    T &value() { return std::get<T>(m_storage); }                            // 读取可变成功值
    const McpError &error() const { return std::get<McpError>(m_storage); }  // 读取错误值

private:
    explicit McpResult(T value) : m_storage(std::move(value)) {}             // 保存成功值
    explicit McpResult(McpError error) : m_storage(std::move(error)) {}      // 保存错误值

    /// 成功值和错误值共用的互斥存储。
    std::variant<T, McpError> m_storage;
};

/// 不携带成功值的结果特化。
template<>
class McpResult<void>
{
public:
    static McpResult success() { return McpResult(true, {}); }  // 构造无值成功结果
    static McpResult failure(McpError error) { return McpResult(false, std::move(error)); }  // 构造错误结果

    bool isSuccess() const { return m_success; }             // 查询操作是否成功
    bool isError() const { return !m_success; }              // 查询操作是否失败
    const McpError &error() const { return m_error; }        // 读取失败错误

private:
    McpResult(bool success, McpError error)  // 保存无值结果状态与错误
        : m_success(success), m_error(std::move(error))
    {
    }

    /// 操作是否成功完成。
    bool m_success = false;  // 操作是否成功完成
    /// 失败时保存的错误；成功时为空错误对象。
    McpError m_error;        // 失败时的错误，成功时为空对象
};

} // namespace LibMcp

Q_DECLARE_METATYPE(LibMcp::McpError)
