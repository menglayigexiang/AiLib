#include "LibMcpPage.h"

#include "McpClientConfigDialog.h"
#include "McpClientWorkbench.h"
#include "TestServerTools.h"

#include <LibMcp/StreamableHttpTransport.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace LibMcp;

Q_LOGGING_CATEGORY(lcLibMcpPage, "TestApp.LibMcp")  // 标识 LibMcp 测试页面产生的日志

LibMcpPage::LibMcpPage(QWidget* parent)  // 创建 Client 与 Server 测试页
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);  // 填满统一 TestApp 的 LibMcp 页签
    auto* tabs = new QTabWidget(this);  // 承载 Client 配置和 Server 观测页面
    tabs->setObjectName(QStringLiteral("mcpTabs"));
    tabs->addTab(createClientPage(), QStringLiteral("Client"));
    tabs->addTab(createServerPage(), QStringLiteral("Server"));
    layout->addWidget(tabs);
    connect(&m_clientManager,
            &McpClientManager::configsChanged,
            this,
            [this] {  // 配置变更后同步表格和持久化文件
                refreshClientTable();
                m_clientWorkbench->refreshClients();
                saveConfigurations();
            });
    connect(&m_clientManager,
            &McpClientManager::clientStateChanged,
            this,
            [this](const QString&, McpClientManager::ClientState) {  // 立即展示完整启用流程阶段
                refreshClientTable();
                m_clientWorkbench->refreshClients();
            });
    connect(&m_clientManager,
            &McpClientManager::clientTransportStateChanged,
            this,
            [this](const QString&, McpClientManager::TransportState) {  // 同步独立 Transport 状态列
                refreshClientTable();
                m_clientWorkbench->refreshClients();
            });
    connect(&m_clientManager,
            &McpClientManager::clientProtocolStateChanged,
            this,
            [this](const QString&, McpClientManager::ProtocolState) {  // 同步独立协议状态列
                refreshClientTable();
                m_clientWorkbench->refreshClients();
            });
    loadConfigurations();
    refreshClientTable();
    m_clientWorkbench->refreshClients();
    updateServerControls();
}

QWidget* LibMcpPage::createClientPage()  // 创建 Client 列表与添加入口
{
    auto* page = new QWidget(this);  // 承载融合后的 Client 管理与调试页面
    auto* layout = new QVBoxLayout(page);  // 让滚动区域填满 Client 页面
    auto* scrollArea = new QScrollArea(page);  // 小窗口中滚动整个 Client 内容，避免调试控件被压缩
    scrollArea->setObjectName(QStringLiteral("mcpClientScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* scrollContent = new QWidget(scrollArea);  // 承载配置列表和完整调试工作台
    auto* contentLayout = new QVBoxLayout(scrollContent);  // 按使用顺序排列配置与调试区域
    auto* configurationPanel = new QWidget(scrollContent);  // 承载 Client 配置与状态表
    auto* configurationLayout = new QVBoxLayout(configurationPanel);  // 排列配置工具栏和表格
    auto* toolbar = new QHBoxLayout();  // 将说明文本与主要操作分置两端
    auto* title = new QLabel(QStringLiteral("MCP Client 配置与状态"), configurationPanel);  // Client 页面标题
    auto* addButton = new QPushButton(QStringLiteral("添加"), configurationPanel);  // 打开新增配置对话框
    addButton->setObjectName(QStringLiteral("addMcpClientButton"));
    toolbar->addWidget(title);
    toolbar->addStretch();
    toolbar->addWidget(addButton);
    configurationLayout->addLayout(toolbar);

    m_clientTable = new QTableWidget(0, 7, configurationPanel);
    m_clientTable->setObjectName(QStringLiteral("mcpClientTable"));
    m_clientTable->setHorizontalHeaderLabels(
        {QStringLiteral("名称"),
         QStringLiteral("类型"),
         QStringLiteral("参数"),
         QStringLiteral("启用"),
         QStringLiteral("Transport"),
         QStringLiteral("MCP 协议"),
         QStringLiteral("操作")});
    m_clientTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_clientTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_clientTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_clientTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_clientTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_clientTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_clientTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_clientTable->verticalHeader()->setVisible(false);
    m_clientTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_clientTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clientTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clientTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const int visibleClientRows = 3;  // 配置列表固定展示的 Client 行数
    const int clientTableHeight =  // 包含表头、三行内容和边框的固定高度
        m_clientTable->horizontalHeader()->sizeHint().height()
        + m_clientTable->verticalHeader()->defaultSectionSize() * visibleClientRows
        + m_clientTable->frameWidth() * 2;
    m_clientTable->setFixedHeight(clientTableHeight);
    configurationLayout->addWidget(m_clientTable);
    m_clientWorkbench = new McpClientWorkbench(&m_clientManager, scrollContent);
    m_clientWorkbench->setMinimumHeight(620);
    contentLayout->addWidget(configurationPanel);
    contentLayout->addWidget(m_clientWorkbench, 1);
    scrollArea->setWidget(scrollContent);
    layout->addWidget(scrollArea, 1);
    connect(addButton, &QPushButton::clicked, this, [this] { addClient(); });
    connect(m_clientTable,
            &QTableWidget::currentCellChanged,
            this,
            [this](int currentRow, int, int, int) {  // 将配置表当前行同步为唯一调试目标
                const QTableWidgetItem* item = m_clientTable->item(currentRow, 0);  // 读取当前行保存的 Client ID
                m_clientWorkbench->setSelectedClientId(
                    item ? item->data(Qt::UserRole).toString() : QString{});
            });
    return page;
}

QWidget* LibMcpPage::createServerPage()  // 创建 Server 状态、统计和请求列表
{
    auto* page = new QWidget(this);  // 承载无状态 Server 控件
    auto* layout = new QVBoxLayout(page);  // 按配置、状态、统计和最近请求排列内容
    auto* endpointForm = new QFormLayout();  // 排列 Server Endpoint 配置字段
    m_serverAddress = new QLineEdit(QStringLiteral("127.0.0.1"), page);
    m_serverAddress->setObjectName(QStringLiteral("mcpServerAddressEdit"));
    m_serverPort = new QSpinBox(page);
    m_serverPort->setObjectName(QStringLiteral("mcpServerPortSpin"));
    m_serverPort->setRange(0, 65535);
    m_serverPort->setValue(8080);
    m_serverPath = new QLineEdit(QStringLiteral("/mcp"), page);
    m_serverPath->setObjectName(QStringLiteral("mcpServerPathEdit"));
    endpointForm->addRow(QStringLiteral("监听 IP"), m_serverAddress);
    endpointForm->addRow(QStringLiteral("端口"), m_serverPort);
    endpointForm->addRow(QStringLiteral("路径"), m_serverPath);
    layout->addLayout(endpointForm);

    auto* actions = new QHBoxLayout();  // 排列启动和停止操作
    m_startServer = new QPushButton(QStringLiteral("启动 Server"), page);
    m_startServer->setObjectName(QStringLiteral("startMcpServerButton"));
    m_stopServer = new QPushButton(QStringLiteral("停止 Server"), page);
    m_stopServer->setObjectName(QStringLiteral("stopMcpServerButton"));
    actions->addWidget(m_startServer);
    actions->addWidget(m_stopServer);
    actions->addStretch();
    layout->addLayout(actions);

    auto* detailsTabs = new QTabWidget(page);  // 分离服务描述与请求监控以保证阅读空间
    detailsTabs->setObjectName(QStringLiteral("mcpServerDetailsTabs"));
    auto* overviewPage = new QWidget(detailsTabs);  // 承载 Server 身份、能力和工具
    auto* overviewLayout = new QVBoxLayout(overviewPage);  // 纵向排列概览与工具表
    auto* statusForm = new QFormLayout();  // 显示 Server 最终状态、身份和协议信息
    m_serverStatus = new QLabel(overviewPage);
    m_serverStatus->setObjectName(QStringLiteral("mcpServerStatusLabel"));
    m_serverEndpoint = new QLabel(overviewPage);
    m_protocolVersion = new QLabel(QStringLiteral(LIBMCP_PROTOCOL_VERSION), overviewPage);
    m_serverInfo = new QLabel(overviewPage);
    m_serverInfo->setObjectName(QStringLiteral("mcpServerInfoLabel"));
    m_serverSupportedVersions = new QLabel(overviewPage);
    m_serverSupportedVersions->setObjectName(QStringLiteral("mcpServerSupportedVersionsLabel"));
    statusForm->addRow(QStringLiteral("状态"), m_serverStatus);
    statusForm->addRow(QStringLiteral("Endpoint"), m_serverEndpoint);
    statusForm->addRow(QStringLiteral("固定协议版本"), m_protocolVersion);
    statusForm->addRow(QStringLiteral("ServerInfo"), m_serverInfo);
    statusForm->addRow(QStringLiteral("支持版本"), m_serverSupportedVersions);
    overviewLayout->addLayout(statusForm);

    m_serverCapabilities = new QPlainTextEdit(overviewPage);
    m_serverCapabilities->setObjectName(QStringLiteral("mcpServerCapabilitiesOutput"));
    m_serverCapabilities->setReadOnly(true);
    m_serverCapabilities->setMaximumHeight(120);
    m_serverCapabilities->setPlaceholderText(QStringLiteral("启动 Server 后显示当前能力声明。"));
    overviewLayout->addWidget(new QLabel(QStringLiteral("Capabilities"), overviewPage));
    overviewLayout->addWidget(m_serverCapabilities);

    m_serverTools = new QTableWidget(0, 6, overviewPage);
    m_serverTools->setObjectName(QStringLiteral("mcpServerToolsTable"));
    m_serverTools->setHorizontalHeaderLabels(
        {QStringLiteral("名称"),
         QStringLiteral("标题"),
         QStringLiteral("说明"),
         QStringLiteral("Input Schema"),
         QStringLiteral("Output Schema"),
         QStringLiteral("Annotations")});
    m_serverTools->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_serverTools->verticalHeader()->setVisible(false);
    m_serverTools->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serverTools->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overviewLayout->addWidget(new QLabel(QStringLiteral("已注册工具"), overviewPage));
    overviewLayout->addWidget(m_serverTools, 1);
    detailsTabs->addTab(overviewPage, QStringLiteral("服务概览与工具"));

    auto* requestsPage = new QWidget(detailsTabs);  // 承载请求统计和最近请求
    auto* requestsLayout = new QVBoxLayout(requestsPage);  // 纵向排列指标与请求表

    auto* metrics = new QHBoxLayout();  // 横向显示核心无状态请求指标
    m_activeRequests = new QLabel(QStringLiteral("Active Requests: 0"), requestsPage);
    m_totalRequests = new QLabel(QStringLiteral("总请求: 0"), requestsPage);
    m_successRequests = new QLabel(QStringLiteral("成功: 0"), requestsPage);
    m_protocolErrors = new QLabel(QStringLiteral("协议错误: 0"), requestsPage);
    m_transportErrors = new QLabel(QStringLiteral("Transport 错误: 0"), requestsPage);
    metrics->addWidget(m_activeRequests);
    metrics->addWidget(m_totalRequests);
    metrics->addWidget(m_successRequests);
    metrics->addWidget(m_protocolErrors);
    metrics->addWidget(m_transportErrors);
    metrics->addStretch();
    requestsLayout->addLayout(metrics);

    m_recentRequests = new QTableWidget(0, 5, requestsPage);
    m_recentRequests->setObjectName(QStringLiteral("mcpRecentRequestsTable"));
    m_recentRequests->setHorizontalHeaderLabels(
        {QStringLiteral("ClientInfo"),
         QStringLiteral("Method"),
         QStringLiteral("开始时间"),
         QStringLiteral("耗时"),
         QStringLiteral("结果")});
    m_recentRequests->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_recentRequests->verticalHeader()->setVisible(false);
    m_recentRequests->setEditTriggers(QAbstractItemView::NoEditTriggers);
    requestsLayout->addWidget(m_recentRequests, 1);
    detailsTabs->addTab(requestsPage, QStringLiteral("请求监控"));
    layout->addWidget(detailsTabs, 1);
    connect(m_startServer, &QPushButton::clicked, this, [this] { startServer(); });
    connect(m_stopServer, &QPushButton::clicked, this, [this] { stopServer(); });
    return page;
}

void LibMcpPage::addClient()  // 打开空白配置对话框并新增 Client
{
    McpClientConfigDialog dialog(this);  // 收集新的 Client 配置
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!m_clientManager.addConfig(dialog.configuration())) {
        QMessageBox::warning(this,
                             QStringLiteral("无法添加"),
                             QStringLiteral("Client 名称为空或已经存在。"));
    }
}

void LibMcpPage::editClient(const QString& id)  // 编辑指定未连接 Client
{
    const QList<McpClientConfig> configs = m_clientManager.configs();  // 获取当前配置快照
    auto iterator = std::find_if(
        configs.cbegin(),
        configs.cend(),
        [&id](const McpClientConfig& config) { return config.id == id; });  // 查找指定配置
    if (iterator == configs.cend()) {
        return;
    }
    McpClientConfigDialog dialog(this);  // 复用新增表单编辑现有配置
    dialog.setConfiguration(*iterator);
    if (dialog.exec() == QDialog::Accepted
        && !m_clientManager.updateConfig(id, dialog.configuration())) {
        QMessageBox::warning(this,
                             QStringLiteral("无法保存"),
                             QStringLiteral("请先断开 Client，并确保名称不重复。"));
    }
}

void LibMcpPage::removeClient(const QString& id)  // 确认并删除指定未连接 Client
{
    if (QMessageBox::question(
            this,
            QStringLiteral("删除 MCP Client"),
            QStringLiteral("确定删除“%1”吗？").arg(id))
        != QMessageBox::Yes) {
        return;
    }
    if (!m_clientManager.removeConfig(id)) {
        QMessageBox::warning(this,
                             QStringLiteral("无法删除"),
                             QStringLiteral("请先断开该 Client。"));
    }
}

void LibMcpPage::setClientEnabled(
    const QString& id,  // 需要切换启用状态的 Client ID
    bool enabled)      // true 启用并验证，false 禁用
{                      // 发起完整启用或禁用流程并在结束后刷新表格
    const QSharedPointer<McpOperation<void>> operation = enabled
        ? m_clientManager.startClient(id)
        : m_clientManager.stopClient(id);  // 保存本次启用或禁用操作
    m_operations.insert(operation.data(), operation);
    const auto finishOperation = [this, id, enabled, operation] {  // 统一收敛同步与异步完成的状态操作
        QObject::disconnect(operation.data(), nullptr, this, nullptr);
        m_operations.remove(operation.data());
        if (operation->status() != McpOperationBase::Status::Succeeded) {
            qCWarning(lcLibMcpPage).noquote()
                << QStringLiteral("%1失败：%2")
                       .arg(enabled ? QStringLiteral("启用")
                                    : QStringLiteral("禁用"),
                            operation->error().message);
        } else {
            qCInfo(lcLibMcpPage).noquote()
                << QStringLiteral("Client %1 已%2")
                       .arg(id,
                            enabled ? QStringLiteral("可用")
                                    : QStringLiteral("禁用"));
        }
        refreshClientTable();
        m_clientWorkbench->refreshClients();
    };
    connect(operation.data(),
            &McpOperationBase::finished,
            this,
            finishOperation);
    if (operation->isFinished()) {
        finishOperation();
    }
}

void LibMcpPage::refreshClientTable()  // 按 Manager 当前配置重建表格
{
    const QTableWidgetItem* currentItem = m_clientTable->item(
        m_clientTable->currentRow(), 0);  // 刷新前当前选择的 Client 行
    const QString previousId = currentItem
        ? currentItem->data(Qt::UserRole).toString()
        : QString{};  // 尽量跨状态刷新保留同一个 Client
    const QList<McpClientConfig> configs = m_clientManager.configs();  // 获取排序后的配置快照
    m_clientTable->blockSignals(true);
    m_clientTable->setRowCount(configs.size());
    int selectedRow = configs.isEmpty() ? -1 : 0;  // 原选择消失时回退到首个 Client
    for (int row = 0; row < configs.size(); ++row) {
        const McpClientConfig& config = configs.at(row);  // 当前需要显示的 Client 配置
        const McpClientManager::ClientState state =  // 当前完整启用流程阶段
            m_clientManager.clientState(config.id);
        const McpClientManager::TransportState transportState =  // 当前独立 Transport 状态
            m_clientManager.clientTransportState(config.id);
        const McpClientManager::ProtocolState protocolState =  // 当前固定协议验证状态
            m_clientManager.clientProtocolState(config.id);
        const bool enabled = m_clientManager.clientEnabled(config.id);  // 用户是否希望启用该配置
        auto* nameItem = new QTableWidgetItem(config.name);  // 同时保存工作台选择所需的稳定 Client ID
        nameItem->setData(Qt::UserRole, config.id);
        m_clientTable->setItem(row, 0, nameItem);
        if (config.id == previousId) {
            selectedRow = row;
        }
        m_clientTable->setItem(
            row,
            1,
            new QTableWidgetItem(
                config.transportType == QStringLiteral("stdio")
                    ? QStringLiteral("STDIO")
                    : QStringLiteral("流式 HTTP")));
        auto* summaryItem = new QTableWidgetItem(configurationSummary(config));  // 可展开查看的 JSON 参数摘要
        summaryItem->setToolTip(summaryItem->text());
        m_clientTable->setItem(row, 2, summaryItem);

        auto* switchBox = new QCheckBox(QStringLiteral("启用"),
                                        m_clientTable);  // 只表达用户对 Client 的启用意图
        switchBox->setObjectName(QStringLiteral("mcpClientConnectionSwitch_%1").arg(config.id));
        switchBox->setChecked(enabled);
        switchBox->setEnabled(m_operations.isEmpty());
        connect(switchBox,
                &QCheckBox::toggled,
                this,
                [this, id = config.id, enabled](bool checked) {  // 忽略表格初始化并处理用户切换
                    if (checked != enabled) {
                        setClientEnabled(id, checked);
                    }
                });
        m_clientTable->setCellWidget(row, 3, switchBox);

        QString transportText;  // 映射底层 Transport 状态为不夸大连通性的文案
        switch (transportState) {
        case McpClientManager::TransportState::Stopped:
            transportText = QStringLiteral("未启动");
            break;
        case McpClientManager::TransportState::Starting:
            transportText = QStringLiteral("正在启动…");
            break;
        case McpClientManager::TransportState::Active:
            transportText = QStringLiteral("已启动，等待响应");
            break;
        case McpClientManager::TransportState::Reachable:
            transportText = QStringLiteral("Endpoint 可达");
            break;
        case McpClientManager::TransportState::Error:
            transportText = QStringLiteral("通信失败");
            break;
        }
        auto* transportItem = new QTableWidgetItem(transportText);  // 独立显示 Transport 是否真正可达
        if (transportState == McpClientManager::TransportState::Error) {
            transportItem->setToolTip(m_clientManager.clientError(config.id).message);
        }
        m_clientTable->setItem(row, 4, transportItem);

        QString protocolText;  // 映射固定 MCP 版本的独立验证结果
        switch (protocolState) {
        case McpClientManager::ProtocolState::NotChecked:
            protocolText = QStringLiteral("未验证");
            break;
        case McpClientManager::ProtocolState::Checking:
            protocolText = QStringLiteral("正在验证 2026-07-28…");
            break;
        case McpClientManager::ProtocolState::Compatible:
            if (state == McpClientManager::ClientState::Ready) {
                protocolText = QStringLiteral("可用 · 工具%1 / 资源%2 / 模板%3 / Prompt%4")
                                   .arg(m_clientManager.clientTools(config.id).size())
                                   .arg(m_clientManager.clientResources(config.id).size())
                                   .arg(m_clientManager.clientResourceTemplates(config.id).size())
                                   .arg(m_clientManager.clientPrompts(config.id).size());
            } else if (state == McpClientManager::ClientState::LoadingCapabilities) {
                protocolText = QStringLiteral("兼容 · 正在加载能力…");
            } else {
                protocolText = QStringLiteral("版本兼容 · 能力不可用");
            }
            break;
        case McpClientManager::ProtocolState::Incompatible:
            protocolText = QStringLiteral("不兼容 2026-07-28");
            break;
        case McpClientManager::ProtocolState::Invalid:
            protocolText = QStringLiteral("响应不是有效 MCP 2026-07-28");
            break;
        }
        auto* protocolItem = new QTableWidgetItem(protocolText);  // 显示协议兼容性与最终可用性
        if (state == McpClientManager::ClientState::Error) {
            protocolItem->setToolTip(m_clientManager.clientError(config.id).message);
        }
        m_clientTable->setItem(row, 5, protocolItem);

        auto* actions = new QWidget(m_clientTable);  // 承载当前行编辑和删除按钮
        auto* actionLayout = new QHBoxLayout(actions);  // 紧凑排列次要行内操作
        actionLayout->setContentsMargins(0, 0, 0, 0);
        auto* editButton = new QPushButton(QStringLiteral("编辑"), actions);  // 编辑未连接配置
        auto* deleteButton = new QPushButton(QStringLiteral("删除"), actions);  // 删除未连接配置
        editButton->setEnabled(!enabled);
        deleteButton->setEnabled(!enabled);
        connect(editButton,
                &QPushButton::clicked,
                this,
                [this, id = config.id] { editClient(id); });
        connect(deleteButton,
                &QPushButton::clicked,
                this,
                [this, id = config.id] { removeClient(id); });
        actionLayout->addWidget(editButton);
        actionLayout->addWidget(deleteButton);
        m_clientTable->setCellWidget(row, 6, actions);
    }
    if (selectedRow >= 0) {
        m_clientTable->setCurrentCell(selectedRow, 0);
    } else {
        m_clientTable->clearSelection();
    }
    m_clientTable->blockSignals(false);
    const QTableWidgetItem* selectedItem = m_clientTable->item(selectedRow, 0);  // 读取刷新后的唯一调试目标
    m_clientWorkbench->setSelectedClientId(
        selectedItem ? selectedItem->data(Qt::UserRole).toString() : QString{});
}

QString LibMcpPage::configurationSummary(
    const McpClientConfig& config) const  // 生成表格中的紧凑 JSON 参数摘要
{
    return QString::fromUtf8(
        QJsonDocument(config.transportConfig).toJson(QJsonDocument::Compact));
}

void LibMcpPage::loadConfigurations()  // 从 mcp-clients.json 事务式加载配置
{
    QFile file(configurationPath());  // 打开持久化 Client 配置文件
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcLibMcpPage) << "无法读取 MCP Client 配置";
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());  // 解析完整配置文档
    if (!document.isObject()) {
        qCWarning(lcLibMcpPage) << "MCP Client 配置不是 JSON 对象";
        return;
    }
    const McpResult<void> result = m_clientManager.deserialize(document.object());  // 事务式替换配置集合
    if (result.isError()) {
        qCWarning(lcLibMcpPage).noquote() << result.error().message;
    }
}

void LibMcpPage::saveConfigurations() const  // 将当前配置原子写入 mcp-clients.json
{
    QSaveFile file(configurationPath());  // 防止异常退出留下部分配置文件
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcLibMcpPage) << "无法写入 MCP Client 配置";
        return;
    }
    file.write(QJsonDocument(m_clientManager.serialize()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        qCWarning(lcLibMcpPage) << "提交 MCP Client 配置失败";
    }
}

QString LibMcpPage::configurationPath() const  // 返回 TestApp 配置文件路径
{
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/mcp-clients.json");
}

void LibMcpPage::startServer()  // 按当前 Endpoint 配置启动 Server
{
    if (m_server) {
        return;
    }
    auto transport = std::make_unique<StreamableHttpServerTransport>(  // 创建无状态 HTTP Server Transport
        QHostAddress(m_serverAddress->text().trimmed()),
        static_cast<quint16>(m_serverPort->value()),
        m_serverPath->text().trimmed());
    m_server = std::make_unique<McpServer>(
        std::move(transport),
        McpServer::ServerInfo{QStringLiteral("LibMcpTestServer"),
                              QStringLiteral("0.1.0"),
                              QStringLiteral("LibMcp Test Server")});
    connect(m_server.get(),
            &McpServer::capabilitiesChanged,
            this,
            [this] { refreshServerDescription(); });  // 注册表变化后立即刷新能力和工具
    connect(m_server.get(),
            &McpServer::requestStarted,
            this,
            [this](const QString& method, const QJsonObject& clientInfo) {  // 记录请求开始和自包含 ClientInfo
                ++m_totalRequestCount;
                ++m_activeRequestCount;
                m_activeRequests->setText(
                    QStringLiteral("Active Requests: %1").arg(m_activeRequestCount));
                m_totalRequests->setText(QStringLiteral("总请求: %1").arg(m_totalRequestCount));
                m_recentRequests->insertRow(0);
                m_recentRequests->setItem(
                    0,
                    0,
                    new QTableWidgetItem(
                        QStringLiteral("%1 %2")
                            .arg(clientInfo.value(QStringLiteral("name")).toString(),
                                 clientInfo.value(QStringLiteral("version")).toString())
                            .trimmed()));
                m_recentRequests->setItem(0, 1, new QTableWidgetItem(method));
                m_recentRequests->setItem(0, 2, new QTableWidgetItem(
                    QDateTime::currentDateTime().toString(Qt::ISODate)));
                m_recentRequests->setItem(0, 3, new QTableWidgetItem(QStringLiteral("—")));
                m_recentRequests->setItem(0, 4, new QTableWidgetItem(QStringLiteral("处理中")));
                while (m_recentRequests->rowCount() > 50) {
                    m_recentRequests->removeRow(m_recentRequests->rowCount() - 1);
                }
            });
    connect(m_server.get(),
            &McpServer::requestFinished,
            this,
            [this](const QString& method, qint64 elapsedMs, bool success) {  // 完成请求统计并更新最近记录
                m_activeRequestCount = qMax(0, m_activeRequestCount - 1);
                if (success) {
                    ++m_successRequestCount;
                }
                m_activeRequests->setText(
                    QStringLiteral("Active Requests: %1").arg(m_activeRequestCount));
                m_successRequests->setText(
                    QStringLiteral("成功: %1").arg(m_successRequestCount));
                for (int row = 0; row < m_recentRequests->rowCount(); ++row) {
                    if (m_recentRequests->item(row, 1)->text() == method
                        && m_recentRequests->item(row, 4)->text() == QStringLiteral("处理中")) {
                        m_recentRequests->item(row, 3)->setText(
                            QStringLiteral("%1 ms").arg(elapsedMs));
                        m_recentRequests->item(row, 4)->setText(
                            success ? QStringLiteral("成功") : QStringLiteral("协议错误"));
                        break;
                    }
                }
            });
    connect(m_server.get(),
            &McpServer::protocolError,
            this,
            [this](const McpError&) {  // 累计并显示协议错误数量
                ++m_protocolErrorCount;
                m_protocolErrors->setText(
                    QStringLiteral("协议错误: %1").arg(m_protocolErrorCount));
            });

    if (!registerTestServerTools(*m_server)) {
        qCWarning(lcLibMcpPage) << "未能注册全部 MCP Server 测试工具";
    }

    auto* watcher = new QFutureWatcher<McpResult<void>>(this);  // 监视 Server 启动结果
    connect(watcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, watcher] {  // 根据最终启动结果更新状态和日志
                const McpResult<void> result = watcher->result();  // 保存 Server 启动结果
                watcher->deleteLater();
                if (result.isError()) {
                    ++m_transportErrorCount;
                    m_transportErrors->setText(
                        QStringLiteral("Transport 错误: %1")
                            .arg(m_transportErrorCount));
                    qCWarning(lcLibMcpPage).noquote() << result.error().message;
                    m_server.reset();
                }
                updateServerControls();
            });
    watcher->setFuture(m_server->start());
    updateServerControls();
}

void LibMcpPage::stopServer()  // 停止当前 Server 并恢复 Endpoint 编辑
{
    if (!m_server) {
        return;
    }
    auto* watcher = new QFutureWatcher<McpResult<void>>(this);  // 监视 Server 停止结果
    connect(watcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, watcher] {  // 停止完成后释放 Server 并恢复配置输入
                const McpResult<void> result = watcher->result();  // 保存 Server 停止结果
                watcher->deleteLater();
                if (result.isError()) {
                    qCWarning(lcLibMcpPage).noquote() << result.error().message;
                }
                m_server.reset();
                updateServerControls();
            });
    watcher->setFuture(m_server->stop());
}

void LibMcpPage::updateServerControls()  // 同步 Server 状态文本和控件可用性
{
    const bool running = m_server && m_server->isRunning();  // 当前 Server 最终运行状态
    m_serverStatus->setText(running ? QStringLiteral("运行中")
                                    : QStringLiteral("已停止"));
    m_serverEndpoint->setText(
        QStringLiteral("http://%1:%2%3")
            .arg(m_serverAddress->text())
            .arg(m_serverPort->value())
            .arg(m_serverPath->text()));
    m_serverAddress->setEnabled(!m_server);
    m_serverPort->setEnabled(!m_server);
    m_serverPath->setEnabled(!m_server);
    m_startServer->setEnabled(!m_server);
    m_stopServer->setEnabled(m_server != nullptr);
    refreshServerDescription();
}

void LibMcpPage::refreshServerDescription()  // 刷新 Server 身份、版本、能力和工具快照
{
    const QStringList supportedVersions = m_server
        ? m_server->supportedProtocolVersions()
        : QStringList{QStringLiteral(LIBMCP_PROTOCOL_VERSION)};  // 当前实现对外支持的协议版本
    m_serverSupportedVersions->setText(supportedVersions.join(QStringLiteral(", ")));

    if (!m_server) {
        m_serverInfo->setText(QStringLiteral("尚未创建"));
        m_serverCapabilities->setPlainText(QStringLiteral("{}"));
        m_serverTools->setRowCount(0);
        return;
    }

    const McpServer::ServerInfo info = m_server->serverInfo();  // 读取 Server 对外身份
    m_serverInfo->setText(
        QStringLiteral("%1 %2 · %3")
            .arg(info.name, info.version, info.title));
    m_serverCapabilities->setPlainText(
        QString::fromUtf8(
            QJsonDocument(m_server->capabilities())
                .toJson(QJsonDocument::Indented)));

    const QList<McpTool> tools = m_server->tools();  // 获取按名称排序的工具快照
    m_serverTools->setRowCount(tools.size());
    for (int row = 0; row < tools.size(); ++row) {  // 将每个工具描述映射到表格一行
        const McpTool& tool = tools.at(row);  // 当前需要展示的工具描述
        const QString inputSchema =  // 紧凑显示输入 Schema，完整内容保留在提示中
            QString::fromUtf8(QJsonDocument(tool.inputSchema)
                                  .toJson(QJsonDocument::Compact));
        const QString outputSchema =  // 紧凑显示可选输出 Schema
            QString::fromUtf8(QJsonDocument(tool.outputSchema)
                                  .toJson(QJsonDocument::Compact));
        const QString annotations =  // 紧凑显示工具安全与行为注解
            QString::fromUtf8(QJsonDocument(tool.annotations)
                                  .toJson(QJsonDocument::Compact));
        const QStringList values{tool.name,
                                 tool.title.value_or(QString{}),
                                 tool.description.value_or(QString{}),
                                 inputSchema,
                                 outputSchema,
                                 annotations};  // 固定各列对应的展示文本
        for (int column = 0; column < values.size(); ++column) {  // 创建可复制且带完整提示的单元格
            auto* item = new QTableWidgetItem(values.at(column));  // 当前工具字段单元格
            item->setToolTip(values.at(column));
            m_serverTools->setItem(row, column, item);
        }
    }
}
