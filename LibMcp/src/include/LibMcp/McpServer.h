#pragma once

#include <LibMcp/McpServerTransport.h>
#include <LibMcp/McpTypes.h>

#include <functional>
#include <memory>

namespace LibMcp {

/// 可脱离 MCP 独立调用的同步 Tool 业务函数。
using McpToolFunction = std::function<QJsonArray(const QJsonArray &input)>;
/// 根据最终 URI 读取一个或多个 Resource 内容项。
using McpResourceFunction =
    std::function<QList<McpResourceContent>(const QString &uri)>;
/// 根据参数展开 Prompt 的同步业务函数。
using McpPromptFunction =
    std::function<QList<McpPromptMessage>(const QJsonObject &arguments)>;

/// MCP Server 公共入口，负责能力公布和业务回调分发。
class LIBMCP_EXPORT McpServer final : public QObject
{
    Q_OBJECT
public:
    struct ServerInfo
    {
        QString name;    ///< Server 程序名称。
        QString version; ///< Server 程序版本。
        QString title;   ///< 用户可读标题。
    };

    explicit McpServer(std::unique_ptr<McpServerTransport> transport,
                       ServerInfo serverInfo,
                       QObject *parent = nullptr);
    ~McpServer() override;

    /// 注册一个 Tool；名称重复、名称为空或函数为空时返回 false。
    bool addTool(const McpTool &tool, McpToolFunction function);
    /// 注册一个固定 URI Resource。
    bool addResource(const McpResource &resource, McpResourceFunction function);
    /// 注册一个 RFC 6570 Resource Template。
    bool addResourceTemplate(const McpResourceTemplate &resourceTemplate,
                             McpResourceFunction function);
    /// 注册一个 Prompt；名称重复、名称为空或函数为空时返回 false。
    bool addPrompt(const McpPrompt &prompt, McpPromptFunction function);

    /// 启动 Server Transport；Future 成功表示已经可以接收请求。
    QFuture<McpResult<void>> start();
    /// 停止 Server Transport 并清理连接状态。
    QFuture<McpResult<void>> stop();
    /// 返回 Server Transport 是否处于运行状态。
    bool isRunning() const;

signals:
    /// 一个 MCP 请求已经完成协议分发。
    void requestHandled(const QString &method);

private:
    class Private;
    std::unique_ptr<Private> d; ///< 注册表、生命周期和 Transport 所有权。
};

} // namespace LibMcp
