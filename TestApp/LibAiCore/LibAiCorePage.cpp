#include "LibAiCorePage.h"
#include "../../examples/support/DemoSupport.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QTimer>
#include <QTextCursor>

namespace {
// 一次确认的等待状态，每个调用独立创建；不保存全局确认队列。
struct ApprovalWait {
    std::mutex mutex;                                                          // 保护当前决定
    std::condition_variable ready;                                             // 唤醒正在同步等待的线程
    bool decided = false;                                                      // UI 是否已给出结果
    AiLib::ToolApprovalDecision decision = AiLib::ToolApprovalDecision::Deny;  // 默认拒绝
};
}
GuiApproval::GuiApproval(QWidget& parent) : m_parent(parent)  // 保存非拥有的 UI 接收窗口
{
}
bool GuiApproval::requestApproval(const AiLib::ToolCall& call,                      // 待执行的规范化调用
                                  const AiLib::FunctionToolDefinition& definition,  // 工具纯描述
                                  const AiLib::ToolExecutionContext& context,       // 来源和停止上下文
                                  AiLib::ToolApprovalResult& result,                // 输出用户确认决定
                                  AiLib::SdkError& error)                           // 投递非阻塞弹窗，当前工作线程协作等待
{
    Q_UNUSED(definition)
    if (QThread::currentThread() == m_parent.thread()) {
        error.category = AiLib::ErrorCategory::Configuration;
        error.code = QStringLiteral("GuiApprovalNeedsWorkerThread");
        return false;
    }
    const auto state = std::make_shared<ApprovalWait>();  // 当前调用独立的共享等待状态
    const bool queued = QMetaObject::invokeMethod(        // 确认投递是否成功
        &m_parent,
        [this, state, call, context]() {  // UI 线程创建确认弹窗
            if (context.cancellation.isCancellationRequested() || context.isTimedOut())
                return;
            auto* box = new QMessageBox(  // 由 UI 窗口拥有的非模态弹窗
                QMessageBox::Question, QStringLiteral("工具确认"),
                QStringLiteral("Agent：%1\nCall：%2\nTool：%3\n参数：%4")
                    .arg(context.agentId, call.id, call.name,
                         QString::fromUtf8(
                             QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                &m_parent);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setDefaultButton(QMessageBox::No);
            QObject::connect(box, &QDialog::finished, box,
                             [state](int button) {                                // 写入用户决定并通知等待线程
                                 std::lock_guard<std::mutex> lock(state->mutex);  // 保护单次确认状态
                                 state->decision = button == QMessageBox::Yes
                                                       ? AiLib::ToolApprovalDecision::Allow
                                                   : button == QMessageBox::No
                                                       ? AiLib::ToolApprovalDecision::Deny
                                                       : AiLib::ToolApprovalDecision::Cancel;
                                 state->decided = true;
                                 state->ready.notify_one();
                             });
            auto* timer = new QTimer(box);  // UI 自己响应取消，关闭尚未完成的确认弹窗
            QObject::connect(
                timer, &QTimer::timeout, box, [box, context]() {  // UI 线程检查同一令牌和总截止时间
                    if (context.cancellation.isCancellationRequested() || context.isTimedOut())
                        box->reject();
                });
            timer->start(20);
            box->setWindowModality(Qt::NonModal);
            box->show();
        },
        Qt::QueuedConnection);
    if (!queued) {
        error.category = AiLib::ErrorCategory::Internal;
        error.code = QStringLiteral("ApprovalDispatchFailed");
        return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);  // 工作线程等待，不占用 UI 线程
    while (!state->decided) {
        if (context.cancellation.isCancellationRequested() || context.isTimedOut()) {
            result.decision = AiLib::ToolApprovalDecision::Cancel;
            return true;
        }
        state->ready.wait_for(lock, std::chrono::milliseconds(20));
    }
    result.decision = state->decision;
    return true;
}
LibAiCorePage::LibAiCorePage(
    QString initialProvider,  // 初始 Provider 标识
    QWidget* parent)          // 创建核心库测试页面并注册示例工具
    : QWidget(parent),
      m_approval(*this)
{
    auto* layout = new QVBoxLayout(this);  // 窗口主布局
    auto* settings = new QHBoxLayout;      // Provider 与运行配置行
    m_provider = new QComboBox(this);
    m_provider->setObjectName(QStringLiteral("provider"));
    AiLib::SdkError catalogError;  // 内置目录首次注册错误
    if (Demo::ensureBuiltinCatalog(catalogError)) {
        for (const AiLib::ProviderEntry& entry : AiLib::ModelRegistry::instance().providers())  // 当前 Provider 条目
            m_provider->addItem(entry.configTemplate.name, entry.configTemplate.id);
    }
    const int initialIndex = m_provider->findData(initialProvider);  // 请求的初始 Provider 位置
    if (initialIndex >= 0)
        m_provider->setCurrentIndex(initialIndex);
    m_model = new QComboBox(this);
    m_model->setObjectName(QStringLiteral("model"));
    m_model->setEditable(true);
    refreshModels();
    m_apiKey = new QLineEdit(this);  // 明文输入，不做持久化，关窗即失
    m_apiKey->setPlaceholderText(QStringLiteral("API Key"));
    m_apiKey->setObjectName("apiKey");
    m_apiKey->setClearButtonEnabled(true);
    m_stream = new QCheckBox(QStringLiteral("Streaming"), this);
    m_stream->setChecked(true);
    m_tools = new QCheckBox(QStringLiteral("启用 add 工具（需确认）"), this);
    m_tools->setChecked(true);
    m_tools->setObjectName("tools");
    settings->addWidget(m_provider);
    settings->addWidget(m_model);
    settings->addWidget(m_apiKey, 1);
    settings->addWidget(m_stream);
    settings->addWidget(m_tools);
    layout->addLayout(settings);
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setObjectName("output");
    layout->addWidget(m_output);
    m_input = new QLineEdit(
        QStringLiteral("请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。"),
        this);
    m_input->setObjectName("input");
    layout->addWidget(m_input);
    auto* buttons = new QHBoxLayout;  // 应用操作按钮行
    m_send = new QPushButton(QStringLiteral("发送"), this);
    m_send->setObjectName("send");
    m_stop = new QPushButton(QStringLiteral("Stop"), this);
    m_stop->setObjectName("stop");
    m_clear = new QPushButton(QStringLiteral("清空历史"), this);
    m_status = new QLabel(QStringLiteral("Ready · %1").arg(initialProvider), this);
    m_status->setObjectName("status");
    buttons->addWidget(m_send);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_clear);
    buttons->addWidget(m_status, 1);
    layout->addLayout(buttons);
    connect(m_send, &QPushButton::clicked, this,
            [this]() {  // 启动同步 Agent 的应用工作线程
                start();
            });
    connect(m_input, &QLineEdit::returnPressed, this, [this]() {  // 回车启动同一流程
        start();
    });
    connect(m_stop, &QPushButton::clicked, this, [this]() {  // 请求协作取消
        stop();
    });
    connect(m_clear, &QPushButton::clicked, this, [this]() {  // 清空应用历史和展示
        m_history.clear();
        m_output->clear();
    });
    connect(m_provider, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {  // 切换服务时由应用清除不兼容历史
                Q_UNUSED(index)
                m_history.clear();
                m_output->clear();
                refreshModels();
            });
    AiLib::SdkError error;  // 工具注册错误
    if (!Demo::registerTools(m_registry, error)) {
        m_status->setText(error.code);
        m_send->setEnabled(false);
    } else {
        setBusy(false);
    }
}
LibAiCorePage::~LibAiCorePage()  // 等待线程后才销毁其借用的工具和确认策略
{
    stop();
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
}
void LibAiCorePage::setBusy(bool busy)  // 控制应用运行期间可操作的 UI
{
    m_busy = busy;
    m_send->setEnabled(!busy);
    m_stop->setEnabled(busy);
    m_clear->setEnabled(!busy);
    m_provider->setEnabled(!busy);
    m_model->setEnabled(!busy);
    m_apiKey->setEnabled(!busy);
    m_tools->setEnabled(!busy);
    m_stream->setEnabled(!busy);
    m_input->setEnabled(!busy);
}
void LibAiCorePage::refreshModels()  // 使用当前 Provider 的目录值重建可编辑模型框
{
    const QString providerId = m_provider->currentData().toString();  // 当前 Provider 稳定 ID
    const auto entry = AiLib::ModelRegistry::instance().findProvider(providerId);  // Provider 目录副本
    m_model->clear();
    if (!entry)
        return;
    for (const AiLib::ModelInfo& model : entry->models)  // 当前已知模型
        m_model->addItem(model.displayName.isEmpty() ? model.id : model.displayName, model.id);
    if (m_model->count() > 0)
        m_model->setEditText(m_model->itemData(0).toString());
}
void LibAiCorePage::appendText(const QString& text)  // 将新片段追加到输出末尾
{
    m_output->moveCursor(QTextCursor::End);
    m_output->insertPlainText(text);
}
void LibAiCorePage::stop()  // UI 线程只设置共享取消状态
{
    m_cancel.cancel();
}
void LibAiCorePage::start()  // 捕获 UI 配置值后启动应用工作线程
{
    if (m_busy || m_input->text().trimmed().isEmpty())
        return;
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
    m_cancel = AiLib::CancellationSource{};
    const auto cancellation = m_cancel.token();          // 当前 Run 唯一取消令牌
    const QString provider = m_provider->currentData().toString();  // UI 线程读取的 Provider ID
    const QString model = m_model->currentData().isValid() && m_model->currentText() == m_model->itemText(m_model->currentIndex())
        ? m_model->currentData().toString() : m_model->currentText().trimmed();  // 已知或手工模型 ID
    const QString enteredApiKey = m_apiKey->text().trimmed();  // 用户为本轮 Run 输入的密钥
    const QString apiKey = enteredApiKey.isEmpty() ? Demo::demoApiKeyFromEnv(provider) : enteredApiKey;  // 输入优先，空时回退环境变量
    const bool stream = m_stream->isChecked();           // 当前流式展示选择
    const bool tools = m_tools->isChecked();             // 当前工具白名单选择
    QList<AiLib::Message> history = m_history;           // 工作线程消费的历史值副本
    history.append(AiLib::Message::user(m_input->text()));
    appendText(QStringLiteral("\nUser：%1\nAssistant：").arg(m_input->text()));
    setBusy(true);
    m_status->setText("Running");
    m_worker = QThread::create([this, cancellation, provider, model, apiKey, stream, tools,
                                history]() {                           // 应用线程创建 Client 和 Agent 并同步调用
        AiLib::SdkError error;                                         // 本轮 SDK 流程错误
        std::unique_ptr<AiLib::LLMClient> client;                      // 构造后移交 Agent 独占
        AiLib::AgentResult result;                                     // 本轮实际增量
        bool ok = Demo::createClient(provider, model, apiKey, client, error);  // Factory 配置结果
        if (ok) {
            AiLib::Agent agent(  // 当前 Run 的同步执行器
                std::move(client), m_registry, &m_approval, "widgets-agent");
            AiLib::AgentRequest request;  // 本轮配置，不修改 UI 或 Client 默认值
            request.chat = Demo::chatRequest(model, history, stream);
            request.requestOptions.cancellation = cancellation;
            if (tools)
                request.enabledTools = {QStringLiteral("add")};
            request.requestOptions.streamCallback =
                [this](const AiLib::StreamEvent& event) {  // 实时事件只投递文字到 UI
                    if (event.type == AiLib::StreamEventType::TextDelta) {
                        const QString text = event.delta;  // 交给 UI 的片段副本
                        QMetaObject::invokeMethod(
                            this, [this, text]() {  // UI 线程更新展示
                                appendText(text);
                            }, Qt::QueuedConnection);
                    }
                };
            request.callback = [this](const AiLib::AgentEvent& event) {  // 工具执行状态投递到 UI
                QString text;                                            // 已标准化的工具状态摘要
                if (event.type == AiLib::AgentEventType::ToolExecutionStarted) {
                    text = QStringLiteral("\n[工具开始 %1]\n").arg(event.call.id);
                } else {
                    QString outcome = QStringLiteral("无结果");  // 本次工具流程的结果
                    if (event.result)
                        outcome = event.result->success ? QStringLiteral("成功") : event.result->errorCode;
                    text = QStringLiteral("\n[工具结束 %1，%2，Handler=%3]\n").arg(
                        event.call.id, outcome,
                        event.handlerExecuted ? QStringLiteral("已执行") : QStringLiteral("未执行"));
                }
                QMetaObject::invokeMethod(
                    this, [this, text]() {  // UI 线程展示状态
                        appendText(text);
                    }, Qt::QueuedConnection);
            };
            ok = agent.run(request, result, error);
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, result, error, stream, history]() {  // UI 线程处理最终响应和应用历史
                if (!stream) {
                    for (const auto& message : result.newMessages) {  // 本轮实际产生的 Assistant 文本
                        if (message.role == AiLib::Role::Assistant)
                            appendText(message.text());
                    }
                }
                if (ok && result.finishReason == AiLib::AgentFinishReason::Completed) {
                    m_history = history;
                    m_history.append(result.newMessages);
                }
                m_status->setText(ok ? Demo::finishName(result.finishReason)
                                     : QStringLiteral("Failed · %1").arg(error.code));
                setBusy(false);
            },
            Qt::QueuedConnection);
    });
    m_worker->start();
}
