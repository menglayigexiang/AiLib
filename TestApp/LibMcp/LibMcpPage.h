#pragma once

#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>

#include <QHash>
#include <QWidget>

class QLabel;       // 前向声明状态文本控件
class QLineEdit;    // 前向声明 Endpoint 输入控件
class QPushButton;  // 前向声明 Server 操作按钮
class QSpinBox;     // 前向声明端口输入控件
class QTableWidget; // 前向声明 Client 与请求记录表格

// 管理 LibMcp Client 配置和无状态 Server 观测的测试页面。
class LibMcpPage final : public QWidget
{
    Q_OBJECT
public:
    explicit LibMcpPage(QWidget* parent = nullptr);  // 创建 Client 与 Server 测试页

private:
    QWidget* createClientPage();                     // 创建 Client 列表与添加入口
    QWidget* createServerPage();                     // 创建 Server 状态、统计和请求列表
    void addClient();                                // 打开空白配置对话框并新增 Client
    void editClient(const QString& id);              // 编辑指定未连接 Client
    void removeClient(const QString& id);            // 确认并删除指定未连接 Client
    void setClientConnected(
        const QString& id,                            // 需要切换连接状态的 Client ID
        bool connected);                             // true 连接，false 断开
    void refreshClientTable();                       // 按 Manager 当前配置重建表格
    QString configurationSummary(
        const LibMcp::McpClientConfig& config) const;// 生成表格中的紧凑 JSON 参数摘要
    void loadConfigurations();                       // 从 mcp-clients.json 事务式加载配置
    void saveConfigurations() const;                 // 将当前配置原子写入 mcp-clients.json
    QString configurationPath() const;               // 返回 TestApp 配置文件路径
    void startServer();                              // 按当前 Endpoint 配置启动 Server
    void stopServer();                               // 停止当前 Server 并恢复 Endpoint 编辑
    void updateServerControls();                     // 同步 Server 状态文本和控件可用性

    LibMcp::McpClientManager m_clientManager;        // 保存配置与运行中的 Client 实例
    std::unique_ptr<LibMcp::McpServer> m_server;     // 当前无状态 MCP Server
    QHash<LibMcp::McpOperationBase*,
          QSharedPointer<LibMcp::McpOperationBase>> m_operations;  // 界面持有的活动操作
    QHash<QString, QString> m_clientErrors;             // 各 Client 最近一次连接错误摘要
    QTableWidget* m_clientTable = nullptr;           // Client 配置、状态和操作表格
    QLineEdit* m_serverAddress = nullptr;            // Server 监听地址
    QSpinBox* m_serverPort = nullptr;                // Server 监听端口
    QLineEdit* m_serverPath = nullptr;               // Server Endpoint 路径
    QLabel* m_serverStatus = nullptr;                // Server 最终运行状态
    QLabel* m_serverEndpoint = nullptr;              // Server 当前完整 Endpoint
    QLabel* m_protocolVersion = nullptr;             // 固定 MCP 协议版本
    QLabel* m_activeRequests = nullptr;               // 当前活动请求数量
    QLabel* m_totalRequests = nullptr;                // 累计请求数量
    QLabel* m_successRequests = nullptr;              // 累计成功分发数量
    QLabel* m_protocolErrors = nullptr;               // 累计协议错误数量
    QLabel* m_transportErrors = nullptr;              // 累计 Transport 错误数量
    QTableWidget* m_recentRequests = nullptr;         // 最近请求 ClientInfo 与结果列表
    QPushButton* m_startServer = nullptr;             // 启动 Server 操作按钮
    QPushButton* m_stopServer = nullptr;              // 停止 Server 操作按钮
    int m_totalRequestCount = 0;                      // 累计收到的合法请求数量
    int m_successRequestCount = 0;                    // 累计成功分发的请求数量
    int m_activeRequestCount = 0;                     // 当前仍在分发的请求数量
    int m_protocolErrorCount = 0;                     // 累计协议错误数量
    int m_transportErrorCount = 0;                    // 累计 Transport 错误数量
};
