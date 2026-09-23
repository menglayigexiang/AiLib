#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpOperation.h>
#include <LibMcp/McpTypes.h>

#include <QCoreApplication>
#include <QFuture>
#include <QObject>

#include <memory>

namespace LibMcp {

/// MCP Client 公共入口，负责无状态请求、能力发现和操作管理。
class LIBMCP_EXPORT McpClient final : public QObject
{
    Q_OBJECT
public:
    /// 每个请求中发送给 Server 的宿主应用信息。
    // 保存每个请求携带的 Client 程序身份。
    struct ClientInfo
    {
        QString name;    ///< 宿主应用名称。
        QString version; ///< 宿主应用版本。
    };

    explicit McpClient(
        std::unique_ptr<McpClientTransport> transport,  // Client 独占的消息传输
        ClientInfo clientInfo = {QCoreApplication::applicationName(),  // 每个请求携带的 Client 身份
                                 QCoreApplication::applicationVersion()},
        QJsonObject capabilities = {},              // 每个请求携带的 Client 能力
        QObject *parent = nullptr);                  // 创建使用指定传输、身份和能力的 Client
    ~McpClient() override;                          // 取消活动操作并释放 Transport

    /// 启动 Transport，不执行握手或建立协议 Session。
    QSharedPointer<McpOperation<void>> start();     // 启动 Transport，不执行握手
    /// 停止 Transport，并终止尚未完成的请求。
    QSharedPointer<McpOperation<void>> stop();      // 停止 Transport 并结束待处理请求
    /// 返回 Transport 是否已经启动并处于可请求状态。
    bool isRunning() const;                         // 查询 Transport 是否可用

    /// 返回构造时确定的宿主应用信息。
    ClientInfo clientInfo() const;                  // 返回构造时确定的 Client 身份
    /// 返回最近一次成功响应携带的 Server 标识。
    McpServerInfo serverInfo() const;                // 返回最近响应携带的 Server 身份
    /// 返回 Server 公布的完整能力对象，未知字段会被保留。
    QJsonObject serverCapabilities() const;          // 返回最近发现的 Server 能力

    /// 显式发现 Server 支持版本和能力，不建立隐式 Session 状态。
    QSharedPointer<McpOperation<McpDiscoveryResult>> discover();  // 显式查询 Server 版本与能力

    /// 列出 Server 公布的全部工具。
    QSharedPointer<McpOperation<QList<McpTool>>> listTools();      // 分页读取全部工具
    /// 调用指定工具并保留结构化内容与工具错误状态。
    QSharedPointer<McpOperation<McpToolCallResult>> callTool(
        const QString& name,                  // 需要调用的工具名称
        const QJsonObject& arguments = {},    // 传给工具 inputSchema 校验的参数对象
        const QString& requestState = {},     // 上一轮 input_required 返回的不透明状态
        const QJsonObject& inputResponses = {}); // 对上一轮输入请求的响应映射
    /// 列出 Server 公布的全部固定资源。
    QSharedPointer<McpOperation<QList<McpResource>>> listResources();  // 分页读取全部固定资源
    /// 列出 Server 公布的全部资源模板。
    QSharedPointer<McpOperation<QList<McpResourceTemplate>>> listResourceTemplates();  // 分页读取全部资源模板
    /// 读取一个 URI 对应的全部文本或二进制内容项。
    QSharedPointer<McpOperation<QList<McpResourceContent>>> readResource(const QString &uri);  // 读取指定 URI 内容
    /// 列出 Server 公布的全部 Prompt。
    QSharedPointer<McpOperation<QList<McpPrompt>>> listPrompts();  // 分页读取全部 Prompt
    /// 使用参数展开指定 Prompt，并返回生成的消息列表。
    QSharedPointer<McpOperation<QList<McpPromptMessage>>> getPrompt(
        const QString &name,                         // 需要展开的 Prompt 名称
        const QJsonObject &arguments = {});          // Prompt 参数对象
    /// 请求 Prompt 参数或资源模板变量补全。
    QSharedPointer<McpOperation<McpCompletionResult>> complete(
        const McpCompletionRequest& request);  // 描述引用、当前参数和已知上下文
    QSharedPointer<McpOperation<void>> listen(
        const McpSubscriptionFilter& filter);  // 打开长寿命通知订阅，取消 Operation 即关闭

signals:
    /// 上层尚未专门建模的 MCP 通知。
    void notificationReceived(const QString &method, const QJsonObject &params);  // 转发服务端通知

private:
    // 隐藏请求调度、分页和活动 Operation 状态。
    class Private;
    std::unique_ptr<Private> d; ///< Client 的可变状态和 Transport 所有权。
};

} // namespace LibMcp
