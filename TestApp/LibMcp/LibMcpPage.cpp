#include "LibMcpPage.h"

#include <LibMcp/StreamableHttpTransport.h>

#include <QFormLayout>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

using namespace LibMcp;

Q_LOGGING_CATEGORY(lcLibMcpPage, "TestApp.LibMcp")  // 标识 LibMcp 测试页面产生的日志

LibMcpPage::LibMcpPage(QWidget *parent)  // 创建 MCP 测试页面，parent 为可选父控件
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);                // 填满统一 TestApp 的 LibMcp 页签
    auto *tabs = new QTabWidget(this);                   // 承载客户端和服务端页面
    auto *clientPage = new QWidget(tabs);                // 承载远端客户端配置控件
    auto *clientLayout = new QFormLayout(clientPage);    // 排列客户端配置字段与操作按钮
    m_clientId = new QLineEdit(QStringLiteral("remote"), clientPage);
    m_clientUrl = new QLineEdit(QStringLiteral("http://127.0.0.1:8080/mcp"), clientPage);
    auto *saveClient =                                      // 保存客户端配置的操作按钮
        new QPushButton(QStringLiteral("保存配置"), clientPage);
    auto *connectClient =                                   // 连接客户端并列出工具的操作按钮
        new QPushButton(QStringLiteral("连接并列出工具"), clientPage);
    clientLayout->addRow(QStringLiteral("配置 ID"), m_clientId);
    clientLayout->addRow(QStringLiteral("MCP URL"), m_clientUrl);
    clientLayout->addRow(saveClient);
    clientLayout->addRow(connectClient);

    auto *serverPage = new QWidget(tabs);              // 承载本地服务端配置控件
    auto *serverLayout = new QFormLayout(serverPage);  // 排列服务端配置字段与启动按钮
    m_serverAddress = new QLineEdit(QStringLiteral("127.0.0.1"), serverPage);
    m_serverPort = new QSpinBox(serverPage);
    m_serverPort->setRange(1, 65535);
    m_serverPort->setValue(8080);
    m_serverPath = new QLineEdit(QStringLiteral("/mcp"), serverPage);
    auto *startServer =  // 启动本地 MCP 服务端的操作按钮
        new QPushButton(QStringLiteral("启动 Server"), serverPage);
    serverLayout->addRow(QStringLiteral("监听 IP"), m_serverAddress);
    serverLayout->addRow(QStringLiteral("端口"), m_serverPort);
    serverLayout->addRow(QStringLiteral("路径"), m_serverPath);
    serverLayout->addRow(startServer);

    tabs->addTab(clientPage, QStringLiteral("Client"));
    tabs->addTab(serverPage, QStringLiteral("Server"));
    layout->addWidget(tabs);

    connect(saveClient, &QPushButton::clicked,
            this, &LibMcpPage::addClientConfiguration);
    connect(connectClient, &QPushButton::clicked,
            this, &LibMcpPage::connectConfiguredClient);
    connect(startServer, &QPushButton::clicked,
            this, &LibMcpPage::startLocalServer);
}

void LibMcpPage::connectConfiguredClient()  // 连接选定客户端并显示远端工具列表
{
    const QString id = m_clientId->text().trimmed();  // 当前需要连接的客户端配置 ID
    if (!m_clientManager.client(id)) {
        addClientConfiguration();
    }

    auto *startWatcher =  // 监视客户端异步启动结果
        new QFutureWatcher<McpResult<void>>(this);
    connect(startWatcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, id, startWatcher] {  // 启动完成后继续请求远端工具列表
                const McpResult<void> startResult =  // 保存客户端启动结果
                    startWatcher->result();
                startWatcher->deleteLater();
                if (startResult.isError()) {
                    qCWarning(lcLibMcpPage).noquote()
                        << QStringLiteral("连接失败：%1")
                               .arg(startResult.error().message);
                    return;
                }

                McpClient *client = m_clientManager.client(id);  // 获取已经启动的客户端实例
                auto *toolsWatcher =                             // 监视工具列表异步查询结果
                    new QFutureWatcher<McpResult<QList<McpTool>>>(this);
                connect(toolsWatcher,
                        &QFutureWatcher<McpResult<QList<McpTool>>>::finished,
                        this,
                        [this, toolsWatcher] {  // 查询完成后输出工具或错误信息
                            const auto result =  // 保存工具列表查询结果
                                toolsWatcher->result();
                            toolsWatcher->deleteLater();
                            if (result.isError()) {
                                qCWarning(lcLibMcpPage).noquote()
                                    << QStringLiteral("获取工具失败：%1")
                                           .arg(result.error().message);
                                return;
                            }
                            qCInfo(lcLibMcpPage).noquote()
                                << QStringLiteral("连接成功，共发现 %1 个工具")
                                       .arg(result.value().size());
                            for (const McpTool &tool : result.value()) {  // 逐项输出远端工具名称
                                qCInfo(lcLibMcpPage).noquote()
                                    << QStringLiteral("  - %1").arg(tool.name);
                            }
                        });
                toolsWatcher->setFuture(client->listTools());
            });
    startWatcher->setFuture(m_clientManager.startClient(id));
}

void LibMcpPage::addClientConfiguration()  // 保存界面填写的远端客户端配置
{
    McpClientConfig config;  // 汇总界面填写的客户端连接信息
    config.id = m_clientId->text().trimmed();
    config.name = config.id;
    config.transportType = QStringLiteral("streamable-http");
    config.transportConfig = {{QStringLiteral("url"), m_clientUrl->text().trimmed()}};
    if (m_clientManager.addConfig(config)) {
        qCInfo(lcLibMcpPage) << "Client 配置已保存";
    } else {
        qCWarning(lcLibMcpPage) << "Client 配置保存失败";
    }
}

void LibMcpPage::startLocalServer()  // 启动界面配置的本地 MCP 服务端
{
    if (m_server) {
        qCInfo(lcLibMcpPage) << "Server 已经启动";
        return;
    }

    auto transport =  // 创建按界面参数监听的 Streamable HTTP 服务端传输
        std::make_unique<StreamableHttpServerTransport>(
        QHostAddress(m_serverAddress->text().trimmed()),
        static_cast<quint16>(m_serverPort->value()),
        m_serverPath->text().trimmed());
    m_server = std::make_unique<McpServer>(
        std::move(transport),
        McpServer::ServerInfo{QStringLiteral("LibMcpTestServer"),
                              QStringLiteral("0.1.0"),
                              QStringLiteral("LibMcp Test Server")});

    McpTool echoTool;  // 提供用于人工验证调用链的回显工具
    echoTool.name = QStringLiteral("echo");
    echoTool.description = QStringLiteral("返回调用方提交的参数。");
    echoTool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")}};
    m_server->addTool(
        echoTool,
        [](const QJsonArray &input) { return input; });  // 原样返回输入参数以便人工核对

    auto *watcher =  // 监视本地服务端异步启动结果
        new QFutureWatcher<McpResult<void>>(this);
    connect(watcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, watcher] {  // 启动完成后记录服务地址或错误信息
                const McpResult<void> result = watcher->result();  // 保存服务端启动结果
                watcher->deleteLater();
                if (result.isError()) {
                    qCWarning(lcLibMcpPage).noquote()
                        << QStringLiteral("Server 启动失败：%1")
                               .arg(result.error().message);
                    m_server.reset();
                    return;
                }
                qCInfo(lcLibMcpPage).noquote()
                    << QStringLiteral("Server 已启动：http://%1:%2%3")
                           .arg(m_serverAddress->text())
                           .arg(m_serverPort->value())
                           .arg(m_serverPath->text());
            });
    watcher->setFuture(m_server->start());
}
