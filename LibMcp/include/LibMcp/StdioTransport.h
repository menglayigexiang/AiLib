#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpServerTransport.h>

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>

#include <memory>

namespace LibMcp {

// 描述启动 STDIO MCP Server 子进程所需的显式配置。
struct LIBMCP_EXPORT StdioClientConfig
{
    QString command;                         // Server 可执行文件或命令路径
    QStringList arguments;                   // 按顺序传给 Server 的命令行参数
    QString workingDirectory;                // 可选工作目录，空值使用当前目录
    QProcessEnvironment environment;         // 显式写入子进程的环境变量
    QStringList inheritedEnvironmentNames;   // 从当前进程按名称传递的环境变量
};

class StdioClientTransportPrivate;  // 隐藏 QProcess 和行 framing 实现

// 通过子进程 stdin/stdout 传送逐行 JSON-RPC 消息。
class LIBMCP_EXPORT StdioClientTransport final : public McpClientTransport
{
    Q_OBJECT
public:
    explicit StdioClientTransport(
        StdioClientConfig config,       // 子进程启动与环境配置
        QObject* parent = nullptr);     // 可选 QObject 所有者
    ~StdioClientTransport() override;   // 停止子进程并释放管道资源

    QFuture<McpResult<void>> start() override;  // 启动 Server 子进程
    QFuture<McpResult<void>> stop() override;   // 终止 Server 子进程
    QFuture<McpResult<void>> sendMessage(
        const QJsonObject& message) override;   // 向子进程 stdin 写入单行 JSON

signals:
    void standardErrorReceived(
        const QString& text);                   // 转发子进程 stderr 诊断文本

private:
    std::unique_ptr<StdioClientTransportPrivate> d;  // 保存子进程和输入缓冲
};

class StdioServerTransportPrivate;  // 隐藏标准流和事件通知实现

// 通过当前进程 stdin/stdout 提供逐行 JSON-RPC Server Transport。
class LIBMCP_EXPORT StdioServerTransport final : public McpServerTransport
{
    Q_OBJECT
public:
    explicit StdioServerTransport(QObject* parent = nullptr);  // 绑定当前进程标准流
    ~StdioServerTransport() override;                           // 停止监听但不关闭系统标准流

    QFuture<McpResult<void>> start() override;                  // 开始监听 stdin
    QFuture<McpResult<void>> stop() override;                   // 停止监听 stdin
    QFuture<McpResult<void>> sendMessage(
        const McpTransportRequestId& requestId,  // 当前 STDIO 请求路由标识
        const QJsonObject& message) override;    // 向 stdout 写入单行 JSON

private:
    std::unique_ptr<StdioServerTransportPrivate> d;  // 保存标准流与输入缓冲
};

} // namespace LibMcp
