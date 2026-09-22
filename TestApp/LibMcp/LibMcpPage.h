#pragma once

#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>

#include <QWidget>

// 前向声明单行文本输入控件，避免在头文件中引入完整定义。
class QLineEdit;
// 前向声明整数输入控件，避免在头文件中引入完整定义。
class QSpinBox;

// 管理 LibMcp 客户端与服务端人工联调功能的测试页面。
class LibMcpPage final : public QWidget
{
    Q_OBJECT
public:
    explicit LibMcpPage(QWidget *parent = nullptr);  // 创建 MCP 测试页面，parent 为可选父控件

private:
    void addClientConfiguration();              // 保存界面填写的远端客户端配置
    void connectConfiguredClient();             // 连接选定客户端并显示远端工具列表
    void startLocalServer();                    // 启动界面配置的本地 MCP 服务端
    LibMcp::McpClientManager m_clientManager;       // 管理远端 MCP 配置与客户端实例
    std::unique_ptr<LibMcp::McpServer> m_server;    // 持有当前本地 MCP 服务端
    QLineEdit *m_clientId = nullptr;                // Client 配置 ID 输入框
    QLineEdit *m_clientUrl = nullptr;               // 远端 MCP URL 输入框
    QLineEdit *m_serverAddress = nullptr;           // 本地监听 IP 输入框
    QLineEdit *m_serverPath = nullptr;              // 本地 MCP HTTP 路径输入框
    QSpinBox *m_serverPort = nullptr;                // 本地监听端口输入框
};
