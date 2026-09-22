#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpTypes.h>

#include <QCoreApplication>
#include <QFuture>
#include <QObject>

#include <memory>

namespace LibMcp {

/// MCP Client 公共入口，负责初始化、能力检查和请求管理。
class LIBMCP_EXPORT McpClient final : public QObject
{
    Q_OBJECT
public:
    /// 初始化请求中发送给 Server 的宿主应用信息。
    struct ClientInfo
    {
        QString name;    ///< 宿主应用名称。
        QString version; ///< 宿主应用版本。
    };

    explicit McpClient(
        std::unique_ptr<McpClientTransport> transport,
        ClientInfo clientInfo = {QCoreApplication::applicationName(),
                                 QCoreApplication::applicationVersion()},
        QObject *parent = nullptr);
    ~McpClient() override;

    /// 启动 Transport 并完成 MCP 初始化与能力协商。
    QFuture<McpResult<void>> start();
    /// 停止 Transport，并终止尚未完成的请求。
    QFuture<McpResult<void>> stop();
    /// 返回 Client 是否已经完成初始化并处于可请求状态。
    bool isRunning() const;

    /// 返回构造时确定的宿主应用信息。
    ClientInfo clientInfo() const;
    /// 返回最近一次初始化获得的 Server 标识。
    McpServerInfo serverInfo() const;
    /// 返回 Server 公布的完整能力对象，未知字段会被保留。
    QJsonObject serverCapabilities() const;

    /// 列出 Server 公布的全部工具。
    QFuture<McpResult<QList<McpTool>>> listTools();
    /// 调用指定工具；输入数组会转换为 MCP arguments 对象。
    QFuture<McpResult<QJsonArray>> callTool(
        const QString &name, const QJsonArray &input);
    /// 列出 Server 公布的全部固定资源。
    QFuture<McpResult<QList<McpResource>>> listResources();
    /// 列出 Server 公布的全部资源模板。
    QFuture<McpResult<QList<McpResourceTemplate>>> listResourceTemplates();
    /// 读取一个 URI 对应的全部文本或二进制内容项。
    QFuture<McpResult<QList<McpResourceContent>>> readResource(const QString &uri);
    /// 列出 Server 公布的全部 Prompt。
    QFuture<McpResult<QList<McpPrompt>>> listPrompts();
    /// 使用参数展开指定 Prompt，并返回生成的消息列表。
    QFuture<McpResult<QList<McpPromptMessage>>> getPrompt(
        const QString &name, const QJsonObject &arguments = {});

signals:
    /// 上层尚未专门建模的 MCP 通知。
    void notificationReceived(const QString &method, const QJsonObject &params);

private:
    class Private;
    std::unique_ptr<Private> d; ///< Client 的可变状态和 Transport 所有权。
};

} // namespace LibMcp
