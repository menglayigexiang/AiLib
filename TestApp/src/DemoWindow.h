#pragma once
#include <AiLib/tools/IToolApprovalProvider.h>
#include <AiLib/tools/ToolRegistry.h>
#include <AiLib/core/Message.h>
#include <QWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QThread>
#include <mutex>
#include <condition_variable>

// GUI 策略把确认交给 UI 线程；工作线程协作等待，不耦合进 SDK。
class GuiApproval final : public AiLib::IToolApprovalProvider {
public:
    explicit GuiApproval(QWidget& parent);                                 // 借用 UI 接收对象，窗口需活到运行结束
    bool requestApproval(const AiLib::ToolCall& call,                      // 待执行的规范化调用
                         const AiLib::FunctionToolDefinition& definition,  // 工具纯描述
                         const AiLib::ToolExecutionContext& context,       // 来源和停止上下文
                         AiLib::ToolApprovalResult& result,                // 输出用户确认决定
                         AiLib::SdkError& error) override;                 // 在工作线程同步等待独立确认结果
private:
    QWidget& m_parent;  // 应用拥有的 UI 接收窗口
};

// 最小应用演示：应用管理历史、工作线程、取消以及同步确认桥接。
class DemoWindow final : public QWidget {
public:
    explicit DemoWindow(
        QString initialProvider = QStringLiteral("offline"));  // 按初始 Provider 创建表单并注册示例工具
    ~DemoWindow() override;                              // 取消并等待工作线程后释放 Registry 和 Approval
protected:
    void closeEvent(QCloseEvent* event) override;  // 运行中先请求停止，完成后再关闭
private:
    void start();                          // 在应用创建的工作线程运行同步 Agent
    void stop();                           // 请求唯一取消来源停止，不强行终止线程
    void appendText(const QString& text);  // UI 线程追加模型文字
    void setBusy(bool busy);               // 运行时禁用配置修改，允许 Stop
    QComboBox* m_provider = nullptr;       // 离线或真实 Provider 选择
    QCheckBox* m_stream = nullptr;         // 是否开启 Streaming
    QCheckBox* m_tools = nullptr;          // 是否提供 add 工具白名单
    QLineEdit* m_input = nullptr;          // 下一轮用户输入
    QPushButton* m_send = nullptr;         // 启动按钮
    QPushButton* m_stop = nullptr;         // 协作取消按钮
    QPushButton* m_clear = nullptr;        // 清除应用拥有的历史
    QPlainTextEdit* m_output = nullptr;    // 模型文字及工具状态展示
    QLabel* m_status = nullptr;            // 本轮最终停止原因
    QThread* m_worker = nullptr;           // 应用创建且等待后销毁的工作线程
    bool m_busy = false;                   // 当前是否仍有运行和结果待交付
    bool m_closePending = false;           // 运行结束后是否关闭窗口
    AiLib::CancellationSource m_cancel;    // 当前 Run 的唯一取消源
    AiLib::ToolRegistry m_registry;        // 应用拥有且运行时只读的工具集合
    GuiApproval m_approval;                // 应用拥有的同步确认策略
    QList<AiLib::Message> m_history;       // 应用拥有的完整历史，仅成功完成后追加本轮增量
};
