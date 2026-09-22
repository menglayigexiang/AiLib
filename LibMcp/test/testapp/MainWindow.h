#pragma once

#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>

#include <QMainWindow>

class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

/// LibMcp 的人工联调程序；自动测试不会操作此界面。
class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void addClientConfiguration();
    void connectConfiguredClient();
    void startLocalServer();
    void appendLog(const QString &message);

    LibMcp::McpClientManager m_clientManager; ///< 管理远端 MCP 配置。
    std::unique_ptr<LibMcp::McpServer> m_server; ///< 当前本地 Server。
    QLineEdit *m_clientId = nullptr;          ///< Client 配置 ID 输入框。
    QLineEdit *m_clientUrl = nullptr;         ///< 远端 MCP URL 输入框。
    QLineEdit *m_serverAddress = nullptr;     ///< 本地监听 IP 输入框。
    QLineEdit *m_serverPath = nullptr;        ///< 本地 MCP HTTP 路径输入框。
    QSpinBox *m_serverPort = nullptr;         ///< 本地监听端口输入框。
    QPlainTextEdit *m_log = nullptr;          ///< 只读运行日志。
};
