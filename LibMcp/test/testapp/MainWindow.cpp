#include "MainWindow.h"

#include <LibMcp/StreamableHttpTransport.h>

#include <QFormLayout>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>

using namespace LibMcp;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LibMcp TestApp"));
    resize(800, 520);

    auto *tabs = new QTabWidget(this);
    auto *clientPage = new QWidget(tabs);
    auto *clientLayout = new QFormLayout(clientPage);
    m_clientId = new QLineEdit(QStringLiteral("remote"), clientPage);
    m_clientUrl = new QLineEdit(QStringLiteral("http://127.0.0.1:8080/mcp"), clientPage);
    auto *saveClient = new QPushButton(QStringLiteral("保存配置"), clientPage);
    auto *connectClient = new QPushButton(QStringLiteral("连接并列出工具"), clientPage);
    clientLayout->addRow(QStringLiteral("配置 ID"), m_clientId);
    clientLayout->addRow(QStringLiteral("MCP URL"), m_clientUrl);
    clientLayout->addRow(saveClient);
    clientLayout->addRow(connectClient);

    auto *serverPage = new QWidget(tabs);
    auto *serverLayout = new QFormLayout(serverPage);
    m_serverAddress = new QLineEdit(QStringLiteral("127.0.0.1"), serverPage);
    m_serverPort = new QSpinBox(serverPage);
    m_serverPort->setRange(1, 65535);
    m_serverPort->setValue(8080);
    m_serverPath = new QLineEdit(QStringLiteral("/mcp"), serverPage);
    auto *startServer = new QPushButton(QStringLiteral("启动 Server"), serverPage);
    serverLayout->addRow(QStringLiteral("监听 IP"), m_serverAddress);
    serverLayout->addRow(QStringLiteral("端口"), m_serverPort);
    serverLayout->addRow(QStringLiteral("路径"), m_serverPath);
    serverLayout->addRow(startServer);

    m_log = new QPlainTextEdit(tabs);
    m_log->setReadOnly(true);
    tabs->addTab(clientPage, QStringLiteral("Client"));
    tabs->addTab(serverPage, QStringLiteral("Server"));
    tabs->addTab(m_log, QStringLiteral("Log"));
    setCentralWidget(tabs);

    connect(saveClient, &QPushButton::clicked,
            this, &MainWindow::addClientConfiguration);
    connect(connectClient, &QPushButton::clicked,
            this, &MainWindow::connectConfiguredClient);
    connect(startServer, &QPushButton::clicked,
            this, &MainWindow::startLocalServer);
}

void MainWindow::connectConfiguredClient()
{
    const QString id = m_clientId->text().trimmed();
    if (!m_clientManager.client(id)) {
        addClientConfiguration();
    }

    auto *startWatcher = new QFutureWatcher<McpResult<void>>(this);
    connect(startWatcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, id, startWatcher] {
                const McpResult<void> startResult = startWatcher->result();
                startWatcher->deleteLater();
                if (startResult.isError()) {
                    appendLog(QStringLiteral("连接失败：%1")
                                  .arg(startResult.error().message));
                    return;
                }

                McpClient *client = m_clientManager.client(id);
                auto *toolsWatcher =
                    new QFutureWatcher<McpResult<QList<McpTool>>>(this);
                connect(toolsWatcher,
                        &QFutureWatcher<McpResult<QList<McpTool>>>::finished,
                        this,
                        [this, toolsWatcher] {
                            const auto result = toolsWatcher->result();
                            toolsWatcher->deleteLater();
                            if (result.isError()) {
                                appendLog(QStringLiteral("获取工具失败：%1")
                                              .arg(result.error().message));
                                return;
                            }
                            appendLog(QStringLiteral("连接成功，共发现 %1 个工具")
                                          .arg(result.value().size()));
                            for (const McpTool &tool : result.value()) {
                                appendLog(QStringLiteral("  - %1").arg(tool.name));
                            }
                        });
                toolsWatcher->setFuture(client->listTools());
            });
    startWatcher->setFuture(m_clientManager.startClient(id));
}

void MainWindow::appendLog(const QString &message)
{
    m_log->appendPlainText(message);
}

void MainWindow::addClientConfiguration()
{
    McpClientConfig config;
    config.id = m_clientId->text().trimmed();
    config.name = config.id;
    config.transportType = QStringLiteral("streamable-http");
    config.transportConfig = {{QStringLiteral("url"), m_clientUrl->text().trimmed()}};
    appendLog(m_clientManager.addConfig(config)
                  ? QStringLiteral("Client 配置已保存")
                  : QStringLiteral("Client 配置保存失败"));
}

void MainWindow::startLocalServer()
{
    if (m_server) {
        appendLog(QStringLiteral("Server 已经启动"));
        return;
    }

    auto transport = std::make_unique<StreamableHttpServerTransport>(
        QHostAddress(m_serverAddress->text().trimmed()),
        static_cast<quint16>(m_serverPort->value()),
        m_serverPath->text().trimmed());
    m_server = std::make_unique<McpServer>(
        std::move(transport),
        McpServer::ServerInfo{QStringLiteral("LibMcpTestServer"),
                              QStringLiteral("0.1.0"),
                              QStringLiteral("LibMcp Test Server")});

    McpTool echoTool;
    echoTool.name = QStringLiteral("echo");
    echoTool.description = QStringLiteral("返回调用方提交的参数。");
    echoTool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")}};
    m_server->addTool(echoTool,
                      [](const QJsonArray &input) { return input; });

    auto *watcher = new QFutureWatcher<McpResult<void>>(this);
    connect(watcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, watcher] {
                const McpResult<void> result = watcher->result();
                watcher->deleteLater();
                if (result.isError()) {
                    appendLog(QStringLiteral("Server 启动失败：%1")
                                  .arg(result.error().message));
                    m_server.reset();
                    return;
                }
                appendLog(QStringLiteral("Server 已启动：http://%1:%2%3")
                              .arg(m_serverAddress->text())
                              .arg(m_serverPort->value())
                              .arg(m_serverPath->text()));
            });
    watcher->setFuture(m_server->start());
}
