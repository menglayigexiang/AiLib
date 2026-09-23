#pragma once

#include <LibMcp/McpServerTransport.h>
#include <LibMcp/McpTypes.h>

#include <functional>
#include <memory>

namespace LibMcp {

// 为当前无状态请求提供进度上报能力，不保存跨请求上下文。
class LIBMCP_EXPORT McpRequestContext final
{
public:
    using ProgressFunction =
        std::function<void(double, double, const QString&)>;  // 向当前请求传输发送进度

    explicit McpRequestContext(ProgressFunction progressFunction = {});  // 绑定当前请求的进度输出
    void reportProgress(
        double progress,               // 当前已完成的工作量
        double total = -1.0,           // 总工作量，未知时为负数
        const QString& message = {}) const;  // 可选的人类可读说明

private:
    ProgressFunction m_progressFunction;  // 当前请求的可选进度发送函数
};

/// 可脱离 MCP 独立调用的同步 Tool 业务函数。
using McpToolFunction =
    std::function<McpToolCallResult(
        const McpToolCallRequest& request,  // 工具参数与可选 MRTR 重试输入
        const McpRequestContext& context)>; // 当前请求的进度上下文
/// 根据最终 URI 读取一个或多个 Resource 内容项。
using McpResourceFunction =
    std::function<QList<McpResourceContent>(const QString &uri)>;
/// 根据参数展开 Prompt 的同步业务函数。
using McpPromptFunction =
    std::function<QList<McpPromptMessage>(const QJsonObject &arguments)>;
/// 根据引用、参数和上下文生成补全候选值。
using McpCompletionFunction =
    std::function<McpCompletionResult(const McpCompletionRequest& request)>;

/// MCP Server 公共入口，负责能力公布和业务回调分发。
class LIBMCP_EXPORT McpServer final : public QObject
{
    Q_OBJECT
public:
    // 保存 Server 在每个成功响应元数据中携带的身份。
    struct ServerInfo
    {
        QString name;    ///< Server 程序名称。
        QString version; ///< Server 程序版本。
        QString title;   ///< 用户可读标题。
    };

    explicit McpServer(
        std::unique_ptr<McpServerTransport> transport,  // Server 独占的请求级传输
        ServerInfo serverInfo,                          // 每个成功响应携带的 Server 身份
        QObject *parent = nullptr);                     // 创建指定身份的无状态 Server
    ~McpServer() override;                              // 释放注册表和 Transport

    /// 注册一个 Tool；名称重复、名称为空或函数为空时返回 false。
    bool addTool(const McpTool &tool, McpToolFunction function);  // 注册工具并校验输入输出 Schema
    /// 注册一个固定 URI Resource。
    bool addResource(const McpResource &resource, McpResourceFunction function);  // 注册固定 URI 资源
    /// 注册一个 RFC 6570 Resource Template。
    bool addResourceTemplate(
        const McpResourceTemplate &resourceTemplate,  // 需要注册的 URI Template
        McpResourceFunction function);                // 读取展开后 URI 的处理函数
    /// 注册一个 Prompt；名称重复、名称为空或函数为空时返回 false。
    bool addPrompt(const McpPrompt &prompt, McpPromptFunction function);  // 注册 Prompt 及其展开函数
    /// 设置唯一补全处理器；传入空函数可关闭补全能力。
    void setCompletionHandler(McpCompletionFunction function);  // 替换唯一补全处理函数
    void notifyResourceUpdated(
        const QString& uri);  // 向显式订阅该 URI 的活动流发布内容更新

    /// 启动 Server Transport；Future 成功表示已经可以接收请求。
    QFuture<McpResult<void>> start();                       // 启动 Server Transport
    /// 停止 Server Transport 并清理连接状态。
    QFuture<McpResult<void>> stop();                        // 正常结束订阅并停止 Transport
    /// 返回 Server Transport 是否处于运行状态。
    bool isRunning() const;                                 // 查询 Server Transport 是否已启动

signals:
    /// 一个已通过元数据校验的无状态请求开始分发。
    void requestStarted(
        const QString& method,                    // 请求方法
        const QJsonObject& clientInfo);           // 本次请求自带的 ClientInfo
    /// 一个 MCP 请求已经完成协议分发。
    void requestHandled(const QString &method);             // 通知一个 MCP 请求已完成分发
    /// 一个请求完成分发并报告耗时与最终分类。
    void requestFinished(
        const QString& method,                    // 请求方法
        qint64 elapsedMilliseconds,               // 分发耗时，单位毫秒
        bool success);                            // 是否命中并完成已知方法
    /// 请求在分发前或方法查找阶段发生协议错误。
    void protocolError(const LibMcp::McpError& error);  // 归一化后的协议错误

private:
    // 隐藏能力注册表、请求分发和订阅状态。
    class Private;
    std::unique_ptr<Private> d; ///< 注册表、生命周期和 Transport 所有权。
};

} // namespace LibMcp
