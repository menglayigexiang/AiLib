#include "McpClientWorkbench.h"

#include <QClipboard>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace LibMcp;

namespace {

QString formattedJson(const QJsonValue& value)  // 将 JSON 值格式化为便于人工检查的文本
{
    if (value.isObject()) {
        return QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
    }
    if (value.isArray()) {
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    }
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QJsonObject toolCallResultJson(
    const McpToolCallResult& result)  // 将公开工具结果转换为调试界面 JSON
{
    QJsonObject value{  // 汇总不丢失结构化内容的工具结果字段
        {QStringLiteral("content"), result.content},
        {QStringLiteral("isError"), result.isError}};
    if (!result.structuredContent.isUndefined()) {
        value.insert(QStringLiteral("structuredContent"),
                     result.structuredContent);
    }
    if (!result.inputRequests.isEmpty()) {
        value.insert(QStringLiteral("inputRequests"), result.inputRequests);
        value.insert(QStringLiteral("requestState"), result.requestState);
    }
    return value;
}

QJsonObject operationErrorJson(
    const QString& action,  // 失败操作的人类可读名称
    const McpError& error)  // 需要呈现的 LibMcp 错误
{                          // 组织稳定且可复制的协议错误诊断
    return {{QStringLiteral("action"), action},
            {QStringLiteral("code"), static_cast<int>(error.code)},
            {QStringLiteral("remoteCode"), error.remoteCode},
            {QStringLiteral("message"), error.message},
            {QStringLiteral("data"), error.data}};
}

QJsonObject discoveryResultJson(
    const McpDiscoveryResult& result)  // 将能力发现快照转换为调试界面 JSON
{
    QJsonObject serverInfo{  // 转换公开 ServerInfo 便于显示
        {QStringLiteral("name"), result.serverInfo.name},
        {QStringLiteral("version"), result.serverInfo.version}};
    if (!result.serverInfo.title.isEmpty()) {
        serverInfo.insert(QStringLiteral("title"), result.serverInfo.title);
    }
    return {{QStringLiteral("supportedVersions"),
             QJsonArray::fromStringList(result.supportedVersions)},
            {QStringLiteral("serverInfo"), serverInfo},
            {QStringLiteral("capabilities"), result.capabilities},
            {QStringLiteral("instructions"), result.instructions}};
}

QString transportStateText(
    McpClientManager::TransportState state)  // 将独立 Transport 状态转换为调试文案
{
    switch (state) {
    case McpClientManager::TransportState::Stopped:
        return QStringLiteral("未启动");
    case McpClientManager::TransportState::Starting:
        return QStringLiteral("启动中");
    case McpClientManager::TransportState::Active:
        return QStringLiteral("已启动，等待响应");
    case McpClientManager::TransportState::Reachable:
        return QStringLiteral("Endpoint 可达");
    case McpClientManager::TransportState::Error:
        return QStringLiteral("通信失败");
    }
    return QStringLiteral("未知");
}

QString protocolStateText(
    McpClientManager::ProtocolState state)  // 将固定协议验证状态转换为调试文案
{
    switch (state) {
    case McpClientManager::ProtocolState::NotChecked:
        return QStringLiteral("未验证");
    case McpClientManager::ProtocolState::Checking:
        return QStringLiteral("验证中");
    case McpClientManager::ProtocolState::Compatible:
        return QStringLiteral("2026-07-28 可用");
    case McpClientManager::ProtocolState::Incompatible:
        return QStringLiteral("不兼容 2026-07-28");
    case McpClientManager::ProtocolState::Invalid:
        return QStringLiteral("响应无效");
    }
    return QStringLiteral("未知");
}

} // namespace

McpClientWorkbench::McpClientWorkbench(
    McpClientManager* manager,  // 提供配置和运行 Client 的共享管理器
    QWidget* parent)            // 创建 Client 协议调试工作台
    : QWidget(parent),
      m_manager(manager)
{
    auto* root = new QVBoxLayout(this);  // 按 Client、发现区和工具区组织页面
    auto* clientBar = new QHBoxLayout();  // 将当前 Client 与协议操作放在首行
    auto* clientLabel = new QLabel(QStringLiteral("当前 Client"), this);  // 标注配置列表当前选择
    m_selectedClientName = new QLabel(QStringLiteral("未选择"), this);
    m_selectedClientName->setObjectName(QStringLiteral("mcpWorkbenchSelectedClient"));
    m_connectionState = new QLabel(this);
    m_connectionState->setObjectName(QStringLiteral("mcpWorkbenchConnectionState"));
    m_discoverButton = new QPushButton(QStringLiteral("发现 Server"), this);
    m_discoverButton->setObjectName(QStringLiteral("mcpDiscoverButton"));
    m_loadToolsButton = new QPushButton(QStringLiteral("加载工具"), this);
    m_loadToolsButton->setObjectName(QStringLiteral("mcpLoadToolsButton"));
    clientBar->addWidget(clientLabel);
    clientBar->addWidget(m_selectedClientName);
    clientBar->addSpacing(12);
    clientBar->addWidget(m_connectionState);
    clientBar->addStretch();
    clientBar->addWidget(m_discoverButton);
    clientBar->addWidget(m_loadToolsButton);
    root->addLayout(clientBar);

    m_discoveryOutput = new QPlainTextEdit(this);
    m_discoveryOutput->setObjectName(QStringLiteral("mcpDiscoveryOutput"));
    m_discoveryOutput->setReadOnly(true);
    m_discoveryOutput->setPlaceholderText(
        QStringLiteral("点击“发现 Server”查看协议版本、ServerInfo、能力和使用说明。"));
    m_discoveryOutput->setMinimumHeight(100);

    auto* contentSplitter = new QSplitter(Qt::Horizontal, this);  // 允许调整工具列表与调用详情宽度
    contentSplitter->setObjectName(QStringLiteral("mcpWorkbenchContentSplitter"));
    auto* toolsPanel = new QWidget(contentSplitter);  // 承载可选择的工具列表
    toolsPanel->setMinimumWidth(240);
    auto* toolsLayout = new QVBoxLayout(toolsPanel);  // 搜索栏下方填满紧凑工具名称列表
    auto* toolsHeader = new QHBoxLayout();  // 并排显示工具数量和搜索入口
    m_toolCount = new QLabel(QStringLiteral("工具（0）"), toolsPanel);
    m_toolCount->setObjectName(QStringLiteral("mcpToolCount"));
    m_toolSearch = new QLineEdit(toolsPanel);
    m_toolSearch->setObjectName(QStringLiteral("mcpToolSearch"));
    m_toolSearch->setPlaceholderText(QStringLiteral("搜索工具名称或说明…"));
    m_toolSearch->setClearButtonEnabled(true);
    toolsHeader->addWidget(m_toolCount);
    toolsHeader->addWidget(m_toolSearch, 1);
    toolsLayout->addLayout(toolsHeader);
    m_toolTable = new QTableWidget(0, 1, toolsPanel);
    m_toolTable->setObjectName(QStringLiteral("mcpToolTable"));
    m_toolTable->setHorizontalHeaderLabels({QStringLiteral("名称")});
    m_toolTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_toolTable->verticalHeader()->setVisible(false);
    m_toolTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_toolTable->setWordWrap(true);
    m_toolTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_toolTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toolTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    toolsLayout->addWidget(m_toolTable, 1);

    auto* detailPanel = new QWidget(contentSplitter);  // 承载工具详情、输入和调用结果
    detailPanel->setMinimumWidth(420);
    auto* detailLayout = new QVBoxLayout(detailPanel);  // 按阅读与操作顺序排列工具调用流程
    m_toolDescription = new QPlainTextEdit(
        QStringLiteral("请先加载并选择一个工具。"), detailPanel);
    m_toolDescription->setObjectName(QStringLiteral("mcpToolDescription"));
    m_toolDescription->setReadOnly(true);
    m_toolDescription->setMaximumHeight(88);
    m_toolDescription->setPlaceholderText(QStringLiteral("选择工具后显示标题和完整说明。"));
    detailLayout->addWidget(m_toolDescription);

    auto* detailTabs = new QTabWidget(detailPanel);  // 分页提供充足的 Schema 与调用结果阅读空间
    detailTabs->setObjectName(QStringLiteral("mcpToolDetailTabs"));
    auto* schemaPage = new QWidget(detailTabs);  // 使用完整宽度承载 Schema 标签和复制操作
    auto* schemaLayout = new QVBoxLayout(schemaPage);  // 工具栏下方填满当前 Schema
    auto* schemaBar = new QHBoxLayout();  // 将说明与当前 Schema 复制操作分置两端
    auto* schemaHint = new QLabel(QStringLiteral("Schema 已格式化显示，不自动换行。"), schemaPage);  // 说明阅读方式
    auto* copySchemaButton = new QPushButton(QStringLiteral("复制当前 Schema"), schemaPage);  // 复制当前标签页 JSON
    copySchemaButton->setObjectName(QStringLiteral("mcpCopySchemaButton"));
    schemaBar->addWidget(schemaHint);
    schemaBar->addStretch();
    schemaBar->addWidget(copySchemaButton);
    schemaLayout->addLayout(schemaBar);
    m_schemaTabs = new QTabWidget(schemaPage);
    m_schemaTabs->setObjectName(QStringLiteral("mcpSchemaTabs"));
    m_inputSchema = new QPlainTextEdit(m_schemaTabs);
    m_inputSchema->setObjectName(QStringLiteral("mcpToolInputSchema"));
    m_inputSchema->setReadOnly(true);
    m_inputSchema->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_outputSchema = new QPlainTextEdit(m_schemaTabs);
    m_outputSchema->setObjectName(QStringLiteral("mcpToolOutputSchema"));
    m_outputSchema->setReadOnly(true);
    m_outputSchema->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_schemaTabs->addTab(m_inputSchema, QStringLiteral("Input Schema"));
    m_schemaTabs->addTab(m_outputSchema, QStringLiteral("Output Schema"));
    schemaLayout->addWidget(m_schemaTabs, 1);
    detailTabs->addTab(schemaPage, QStringLiteral("Schema"));

    auto* callPage = new QWidget(detailTabs);  // 承载参数、补充输入和大尺寸结果区域
    auto* callLayout = new QVBoxLayout(callPage);  // 按调用过程纵向排列可调整内容
    auto* argumentsGroup = new QGroupBox(QStringLiteral("调用参数（JSON 对象）"), callPage);  // 工具参数分组
    auto* argumentsLayout = new QVBoxLayout(argumentsGroup);  // 参数编辑器与操作按钮纵向排列
    m_arguments = new QPlainTextEdit(QStringLiteral("{}"), argumentsGroup);
    m_arguments->setObjectName(QStringLiteral("mcpToolArguments"));
    m_arguments->setMinimumHeight(100);
    argumentsLayout->addWidget(m_arguments);
    auto* callBar = new QHBoxLayout();  // 显示调用、取消、进度与状态
    m_callButton = new QPushButton(QStringLiteral("调用工具"), argumentsGroup);
    m_callButton->setObjectName(QStringLiteral("mcpCallToolButton"));
    m_cancelButton = new QPushButton(QStringLiteral("取消"), argumentsGroup);
    m_cancelButton->setObjectName(QStringLiteral("mcpCancelToolButton"));
    m_progress = new QProgressBar(argumentsGroup);
    m_progress->setObjectName(QStringLiteral("mcpToolProgress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_callState = new QLabel(QStringLiteral("等待调用"), argumentsGroup);
    m_callState->setObjectName(QStringLiteral("mcpToolCallState"));
    callBar->addWidget(m_callButton);
    callBar->addWidget(m_cancelButton);
    callBar->addWidget(m_progress, 1);
    callBar->addWidget(m_callState);
    argumentsLayout->addLayout(callBar);
    callLayout->addWidget(argumentsGroup);

    m_inputArea = new QGroupBox(QStringLiteral("需要补充输入"), callPage);
    m_inputArea->setObjectName(QStringLiteral("mcpToolInputArea"));
    auto* inputLayout = new QFormLayout(m_inputArea);  // 展示输入请求并收集响应映射
    m_inputRequests = new QPlainTextEdit(m_inputArea);
    m_inputRequests->setObjectName(QStringLiteral("mcpToolInputRequests"));
    m_inputRequests->setReadOnly(true);
    m_inputResponses = new QPlainTextEdit(QStringLiteral("{}"), m_inputArea);
    m_inputResponses->setObjectName(QStringLiteral("mcpToolInputResponses"));
    m_retryButton = new QPushButton(QStringLiteral("提交补充输入并重试"), m_inputArea);
    m_retryButton->setObjectName(QStringLiteral("mcpRetryToolButton"));
    inputLayout->addRow(QStringLiteral("Server 请求"), m_inputRequests);
    inputLayout->addRow(QStringLiteral("响应映射"), m_inputResponses);
    inputLayout->addRow(QString{}, m_retryButton);
    m_inputArea->setVisible(false);
    callLayout->addWidget(m_inputArea);

    auto* resultGroup = new QGroupBox(QStringLiteral("结果 / 错误"), callPage);  // 最终输出分组
    auto* resultLayout = new QVBoxLayout(resultGroup);  // 填满只读输出文本
    m_resultOutput = new QPlainTextEdit(resultGroup);
    m_resultOutput->setObjectName(QStringLiteral("mcpToolResultOutput"));
    m_resultOutput->setReadOnly(true);
    m_resultOutput->setMinimumHeight(180);
    resultLayout->addWidget(m_resultOutput);
    callLayout->addWidget(resultGroup, 1);
    detailTabs->addTab(callPage, QStringLiteral("调用与结果"));
    detailTabs->setCurrentIndex(1);
    detailLayout->addWidget(detailTabs, 1);

    contentSplitter->addWidget(toolsPanel);
    contentSplitter->addWidget(detailPanel);
    contentSplitter->setStretchFactor(0, 0);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes({300, 700});

    auto* pageSplitter = new QSplitter(Qt::Vertical, this);  // 允许压缩发现信息以扩大工具调试区域
    pageSplitter->setObjectName(QStringLiteral("mcpWorkbenchPageSplitter"));
    pageSplitter->addWidget(m_discoveryOutput);
    pageSplitter->addWidget(contentSplitter);
    pageSplitter->setStretchFactor(0, 1);
    pageSplitter->setStretchFactor(1, 4);
    pageSplitter->setSizes({120, 480});
    root->addWidget(pageSplitter, 1);

    connect(m_discoverButton,
            &QPushButton::clicked,
            this,
            [this] { discoverServer(); });
    connect(m_loadToolsButton,
            &QPushButton::clicked,
            this,
            [this] { loadTools(); });
    connect(m_toolSearch,
            &QLineEdit::textChanged,
            this,
            [this](const QString&) { filterTools(); });  // 输入时立即过滤名称和说明
    connect(m_toolTable,
            &QTableWidget::currentCellChanged,
            this,
            [this](int currentRow, int, int, int) {  // 鼠标、键盘和代码选择统一刷新详情
                selectTool(currentRow);
            });
    connect(copySchemaButton,
            &QPushButton::clicked,
            this,
            [this] {  // 将当前可见 Schema 原样复制到系统剪贴板
                const QPlainTextEdit* editor =  // 当前需要复制的 Schema 编辑器
                    m_schemaTabs->currentIndex() == 0
                    ? m_inputSchema
                    : m_outputSchema;
                QGuiApplication::clipboard()->setText(editor->toPlainText());
            });
    connect(m_callButton,
            &QPushButton::clicked,
            this,
            [this] { callSelectedTool(false); });
    connect(m_cancelButton,
            &QPushButton::clicked,
            this,
            [this] { cancelToolCall(); });
    connect(m_retryButton,
            &QPushButton::clicked,
            this,
            [this] { callSelectedTool(true); });
    refreshClients();
}

void McpClientWorkbench::setSelectedClientId(
    const QString& id)  // 使用配置列表选中的 Client 作为调试目标
{
    if (m_selectedClientId == id) {
        refreshClients();
        return;
    }
    m_selectedClientId = id;
    resetToolDetails();
    m_connectionState->clear();
    refreshClients();
}

void McpClientWorkbench::refreshClients()  // 按 Manager 当前状态刷新选中 Client 与操作可用性
{
    const QList<McpClientConfig> configs = m_manager->configs();  // 获取当前配置快照
    auto selectedConfig = configs.cend();  // 查找配置列表当前选择对应的配置
    for (auto config = configs.cbegin(); config != configs.cend(); ++config) {  // 保留仍然存在的选择
        if (config->id == m_selectedClientId) {
            selectedConfig = config;
            break;
        }
    }
    if (selectedConfig == configs.cend() && !configs.isEmpty()) {
        selectedConfig = configs.cbegin();
        m_selectedClientId = selectedConfig->id;
    } else if (selectedConfig == configs.cend()) {
        m_selectedClientId.clear();
    }
    m_selectedClientName->setText(
        selectedConfig == configs.cend()
            ? QStringLiteral("未选择")
            : selectedConfig->name);
    resetToolDetails();
    if (selectedClient()) {
        showTools(m_manager->clientTools(selectedClientId()));
    }
    const QString selectedId = selectedClientId();  // 读取当前 Client 的分层状态
    m_connectionState->setText(
        selectedId.isEmpty()
            ? QStringLiteral("请选择 Client")
            : QStringLiteral("Transport：%1 ｜ MCP：%2")
                  .arg(transportStateText(
                           m_manager->clientTransportState(selectedId)),
                       protocolStateText(
                           m_manager->clientProtocolState(selectedId))));
    const McpDiscoveryResult discovery =  // 读取启用流程已经取得的发现快照
        m_manager->clientDiscovery(selectedId);
    if (!discovery.supportedVersions.isEmpty()) {
        m_discoveryOutput->setPlainText(
            formattedJson(discoveryResultJson(discovery)));
    } else {
        m_discoveryOutput->clear();
    }
    updateControls();
}

McpClient* McpClientWorkbench::selectedClient() const  // 返回当前选择且已经运行的 Client
{
    const QString id = selectedClientId();  // 固定当前选择的配置 ID
    return m_manager->clientState(id) == McpClientManager::ClientState::Ready
               ? m_manager->client(id)
               : nullptr;
}

QString McpClientWorkbench::selectedClientId() const  // 返回当前选择的 Client 配置 ID
{
    return m_selectedClientId;
}

void McpClientWorkbench::discoverServer()  // 发送 server/discover 并显示能力结果
{
    McpClient* client = selectedClient();  // 固定本次发现使用的运行 Client
    if (!client || m_discoveryOperation) {
        return;
    }
    m_connectionState->setText(QStringLiteral("正在发现…"));
    m_discoveryOperation = client->discover();
    const auto operation = m_discoveryOperation;  // 保证回调执行前 Operation 仍然存活
    connect(operation.data(),
            &McpOperationBase::finished,
            this,
            [this, operation] {  // 显示发现结果并释放页面持有的引用
                if (operation->status() == McpOperationBase::Status::Succeeded
                    && operation->result()) {
                    const McpDiscoveryResult& result = *operation->result();  // 读取完整发现结果
                    m_discoveryOutput->setPlainText(
                        formattedJson(discoveryResultJson(result)));
                    m_connectionState->setText(QStringLiteral("协议发现成功"));
                } else {
                    m_discoveryOutput->setPlainText(
                        formattedJson(operationErrorJson(
                            QStringLiteral("发现 Server"),
                            operation->error())));
                    showError(QStringLiteral("发现 Server"), operation->error());
                }
                m_discoveryOperation.reset();
                updateControls();
            });
    updateControls();
}

void McpClientWorkbench::loadTools()  // 发送 tools/list 并重建工具列表
{
    McpClient* client = selectedClient();  // 固定本次列表请求使用的运行 Client
    if (!client || m_listToolsOperation) {
        return;
    }
    m_connectionState->setText(QStringLiteral("正在加载工具…"));
    m_listToolsOperation = client->listTools();
    const auto operation = m_listToolsOperation;  // 保证回调执行前 Operation 仍然存活
    connect(operation.data(),
            &McpOperationBase::finished,
            this,
            [this, operation] {  // 按最终列表重建表格并显示状态
                if (operation->status() == McpOperationBase::Status::Succeeded
                    && operation->result()) {
                    showTools(*operation->result());
                    m_connectionState->setText(
                        QStringLiteral("已加载 %1 个工具").arg(m_tools.size()));
                } else {
                    showError(QStringLiteral("加载工具"), operation->error());
                }
                m_listToolsOperation.reset();
                updateControls();
            });
    updateControls();
}

void McpClientWorkbench::showTools(
    const QList<McpTool>& tools)  // 使用工具快照重建列表与默认详情
{                                // 集中维护工具快照、表格行和默认选中项的一致性
    m_tools = tools;
    filterTools();
}

void McpClientWorkbench::filterTools()  // 按搜索文本重建紧凑工具名称列表
{
    const QString filter = m_toolSearch->text().trimmed();  // 当前名称或说明过滤文本
    m_visibleToolIndexes.clear();
    for (int index = 0; index < m_tools.size(); ++index) {  // 收集匹配工具在完整快照中的索引
        const McpTool& tool = m_tools.at(index);  // 当前参与过滤的工具描述
        const QString description = tool.description.value_or(QString{});  // 可搜索的工具说明
        const QString title = tool.title.value_or(QString{});  // 可搜索的人类可读标题
        if (filter.isEmpty()
            || tool.name.contains(filter, Qt::CaseInsensitive)
            || title.contains(filter, Qt::CaseInsensitive)
            || description.contains(filter, Qt::CaseInsensitive)) {
            m_visibleToolIndexes.append(index);
        }
    }

    m_toolTable->blockSignals(true);
    m_toolTable->setRowCount(m_visibleToolIndexes.size());
    for (int row = 0; row < m_visibleToolIndexes.size(); ++row) {  // 用单列名称重建紧凑列表
        const McpTool& tool = m_tools.at(m_visibleToolIndexes.at(row));  // 当前可见工具
        auto* item = new QTableWidgetItem(tool.name);  // 显示名称并通过提示保留完整说明
        item->setToolTip(
            tool.description.value_or(QStringLiteral("没有说明。")));
        m_toolTable->setItem(row, 0, item);
    }
    m_toolTable->blockSignals(false);
    m_toolCount->setText(
        filter.isEmpty()
            ? QStringLiteral("工具（%1）").arg(m_tools.size())
            : QStringLiteral("工具（%1/%2）")
                  .arg(m_visibleToolIndexes.size())
                  .arg(m_tools.size()));
    if (!m_visibleToolIndexes.isEmpty()) {
        m_toolTable->setCurrentCell(0, 0);
        selectTool(0);
    } else {
        m_toolDescription->setPlainText(
            m_tools.isEmpty()
                ? QStringLiteral("当前 Client 没有可用工具。")
                : QStringLiteral("没有匹配的工具。"));
        m_inputSchema->clear();
        m_outputSchema->clear();
        m_arguments->setPlainText(QStringLiteral("{}"));
        m_resultOutput->clear();
        updateControls();
    }
}

int McpClientWorkbench::selectedToolIndex() const  // 返回当前可见行对应的原始工具索引
{
    const int row = m_toolTable->currentRow();  // 当前工具列表选择行
    return row >= 0 && row < m_visibleToolIndexes.size()
        ? m_visibleToolIndexes.at(row)
        : -1;
}

void McpClientWorkbench::selectTool(int row)  // 展示指定可见行工具的描述和 Schema
{
    if (row < 0 || row >= m_visibleToolIndexes.size()) {
        updateControls();
        return;
    }
    const int toolIndex = m_visibleToolIndexes.at(row);  // 映射到完整工具快照索引
    const McpTool& tool = m_tools.at(toolIndex);  // 读取所选工具的稳定快照
    m_toolDescription->setPlainText(
        QStringLiteral("%1\n%2")
            .arg(tool.title.value_or(tool.name),
                 tool.description.value_or(QStringLiteral("没有说明。"))));
    m_inputSchema->setPlainText(formattedJson(tool.inputSchema));
    m_outputSchema->setPlainText(
        tool.outputSchema.isEmpty()
            ? QStringLiteral("未声明 outputSchema")
            : formattedJson(tool.outputSchema));
    m_arguments->setPlainText(QStringLiteral("{}"));
    m_inputArea->setVisible(false);
    m_requestState.clear();
    m_resultOutput->clear();
    m_callState->setText(QStringLiteral("等待调用"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    updateControls();
}

void McpClientWorkbench::callSelectedTool(bool retry)  // 调用工具或携带补充输入重试
{
    McpClient* client = selectedClient();  // 固定本次调用使用的运行 Client
    const int toolIndex = selectedToolIndex();  // 获取当前可见行对应的工具索引
    if (!client || toolIndex < 0 || m_toolCallOperation) {
        return;
    }
    QJsonObject arguments;  // 保存经过语法校验的工具参数
    if (!parseObject(m_arguments, QStringLiteral("调用参数"), &arguments)) {
        return;
    }
    QJsonObject inputResponses;  // 保存可选的 MRTR 补充输入响应
    if (retry
        && !parseObject(m_inputResponses,
                        QStringLiteral("补充输入响应"),
                        &inputResponses)) {
        return;
    }
    const QString toolName = m_tools.at(toolIndex).name;  // 固定操作完成前的工具名称
    m_resultOutput->clear();
    m_callState->setText(retry ? QStringLiteral("正在重试…")
                               : QStringLiteral("正在调用…"));
    m_progress->setRange(0, 0);
    m_toolCallOperation = client->callTool(
        toolName,
        arguments,
        retry ? m_requestState : QString{},
        inputResponses);
    const auto operation = m_toolCallOperation;  // 保证信号回调执行前 Operation 仍然存活
    connect(operation.data(),
            &McpOperationBase::progressChanged,
            this,
            [this](double progress,
                   double total,
                   const QString& message) {  // 更新确定或不确定进度及可读说明
                if (total > 0.0) {
                    m_progress->setRange(0, 1000);
                    m_progress->setValue(
                        qBound(0, qRound(progress / total * 1000.0), 1000));
                } else {
                    m_progress->setRange(0, 0);
                }
                m_callState->setText(
                    message.isEmpty() ? QStringLiteral("处理中…") : message);
            });
    connect(operation.data(),
            &McpOperationBase::inputRequired,
            this,
            [this](const QJsonObject& request) {  // 立即展示 Server 发出的补充输入请求
                m_inputArea->setVisible(true);
                m_inputRequests->setPlainText(formattedJson(request));
                m_callState->setText(QStringLiteral("需要补充输入"));
            });
    connect(operation.data(),
            &McpOperationBase::finished,
            this,
            [this, operation] {  // 展示最终业务结果、MRTR 状态或协议错误
                m_progress->setRange(0, 100);
                if (operation->status() == McpOperationBase::Status::Succeeded
                    && operation->result()) {
                    const McpToolCallResult& result = *operation->result();  // 读取完整工具结果
                    m_resultOutput->setPlainText(
                        formattedJson(toolCallResultJson(result)));
                    m_progress->setValue(100);
                    if (!result.inputRequests.isEmpty()) {
                        m_requestState = result.requestState;
                        m_inputArea->setVisible(true);
                        m_inputRequests->setPlainText(
                            formattedJson(result.inputRequests));
                        m_inputResponses->setPlainText(QStringLiteral("{}"));
                        m_callState->setText(QStringLiteral("需要补充输入"));
                    } else {
                        m_requestState.clear();
                        m_inputArea->setVisible(false);
                        m_callState->setText(
                            result.isError ? QStringLiteral("工具返回错误")
                                           : QStringLiteral("调用完成"));
                    }
                } else if (operation->status()
                           == McpOperationBase::Status::Cancelled) {
                    m_resultOutput->setPlainText(QStringLiteral("调用已取消。"));
                    m_progress->setValue(0);
                    m_callState->setText(QStringLiteral("已取消"));
                } else {
                    showError(QStringLiteral("调用工具"), operation->error());
                    m_progress->setValue(0);
                }
                m_toolCallOperation.reset();
                updateControls();
            });
    updateControls();
}

void McpClientWorkbench::cancelToolCall()  // 取消当前尚未完成的工具调用
{
    if (m_toolCallOperation && !m_toolCallOperation->isFinished()) {
        m_callState->setText(QStringLiteral("正在取消…"));
        m_toolCallOperation->cancel();
    }
}

void McpClientWorkbench::resetToolDetails()  // 清空与旧 Client 关联的工具详情
{
    m_tools.clear();
    m_visibleToolIndexes.clear();
    m_toolTable->setRowCount(0);
    m_toolCount->setText(QStringLiteral("工具（0）"));
    m_toolDescription->setPlainText(QStringLiteral("请先加载并选择一个工具。"));
    m_inputSchema->clear();
    m_outputSchema->clear();
    m_arguments->setPlainText(QStringLiteral("{}"));
    m_inputArea->setVisible(false);
    m_requestState.clear();
    m_resultOutput->clear();
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_callState->setText(QStringLiteral("等待调用"));
}

void McpClientWorkbench::updateControls()  // 根据连接和操作状态更新按钮可用性
{
    const bool ready = selectedClient() != nullptr;  // 当前选择是否已经通过完整协议验证
    const bool discoveryBusy = m_discoveryOperation
                               && !m_discoveryOperation->isFinished();  // 是否正在能力发现
    const bool toolsBusy = m_listToolsOperation
                           && !m_listToolsOperation->isFinished();  // 是否正在加载工具
    const bool callBusy = m_toolCallOperation
                          && !m_toolCallOperation->isFinished();  // 是否正在调用工具
    const bool hasTool = selectedToolIndex() >= 0;  // 是否选择有效的过滤后工具
    m_connectionState->setText(
        ready ? m_connectionState->text().isEmpty()
                    ? QStringLiteral("Transport：Endpoint 可达 ｜ MCP：2026-07-28 可用")
                    : m_connectionState->text()
              : m_connectionState->text().isEmpty()
                    ? QStringLiteral("请先在 Client 页启用并通过协议验证")
                    : m_connectionState->text());
    m_discoverButton->setEnabled(ready && !discoveryBusy);
    m_loadToolsButton->setEnabled(ready && !toolsBusy);
    m_toolSearch->setEnabled(!callBusy);
    m_callButton->setEnabled(ready && hasTool && !callBusy);
    m_cancelButton->setEnabled(callBusy);
    m_retryButton->setEnabled(
        ready && hasTool && !callBusy && !m_requestState.isEmpty());
}

bool McpClientWorkbench::parseObject(
    QPlainTextEdit* editor,      // 需要解析的 JSON 对象编辑器
    const QString& fieldName,    // 错误信息使用的字段名称
    QJsonObject* value)          // 输出成功解析的对象
{                               // 拒绝语法错误或非对象 JSON 并在界面内反馈
    QJsonParseError parseError;  // 保存 JSON 解析位置和错误说明
    const QJsonDocument document = QJsonDocument::fromJson(
        editor->toPlainText().toUtf8(), &parseError);  // 解析用户输入文本
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString detail = parseError.error == QJsonParseError::NoError
                                   ? QStringLiteral("必须是 JSON 对象")
                                   : parseError.errorString();  // 生成直接可修正的错误摘要
        m_callState->setText(QStringLiteral("%1无效").arg(fieldName));
        m_resultOutput->setPlainText(
            QStringLiteral("%1：%2").arg(fieldName, detail));
        editor->setFocus();
        return false;
    }
    *value = document.object();
    return true;
}

void McpClientWorkbench::showError(
    const QString& action,         // 失败操作的人类可读名称
    const McpError& error)         // 在状态和输出区显示协议错误
{                                 // 将错误代码、消息和数据集中展示便于诊断
    m_connectionState->setText(QStringLiteral("%1失败").arg(action));
    m_callState->setText(QStringLiteral("失败"));
    m_resultOutput->setPlainText(
        formattedJson(operationErrorJson(action, error)));
}
