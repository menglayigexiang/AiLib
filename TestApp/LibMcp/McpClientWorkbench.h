#pragma once

#include <LibMcp/McpClientManager.h>

#include <QList>
#include <QSharedPointer>
#include <QWidget>

// 提供 Client 选择的 Qt 组合框类型。
class QComboBox;
// 显示状态和工具说明的 Qt 文本类型。
class QLabel;
// 编辑或展示 JSON 文本的 Qt 编辑器类型。
class QPlainTextEdit;
// 显示工具调用进度的 Qt 进度条类型。
class QProgressBar;
// 触发协议操作的 Qt 按钮类型。
class QPushButton;
// 显示工具列表的 Qt 表格类型。
class QTableWidget;
// 承载按需显示区域的 Qt 基础控件类型。
class QWidget;

// 提供 MCP Client 能力发现、工具浏览和工具调用的交互式测试工作台。
class McpClientWorkbench final : public QWidget
{
    Q_OBJECT
public:
    explicit McpClientWorkbench(
        LibMcp::McpClientManager* manager,  // 提供配置和运行 Client 的共享管理器
        QWidget* parent = nullptr);         // 创建 Client 协议调试工作台

    void refreshClients();                  // 按 Manager 当前状态刷新 Client 选择和操作可用性

private:
    LibMcp::McpClient* selectedClient() const;  // 返回当前选择且已经运行的 Client
    QString selectedClientId() const;           // 返回当前选择的 Client 配置 ID
    void discoverServer();                      // 发送 server/discover 并显示能力结果
    void loadTools();                           // 发送 tools/list 并重建工具列表
    void showTools(const QList<LibMcp::McpTool>& tools);  // 使用工具快照重建列表与默认详情
    void selectTool(int row);                   // 展示指定行工具的描述和 Schema
    void callSelectedTool(bool retry);          // 调用工具或携带补充输入重试
    void cancelToolCall();                      // 取消当前尚未完成的工具调用
    void resetToolDetails();                    // 清空与旧 Client 关联的工具详情
    void updateControls();                      // 根据连接和操作状态更新按钮可用性
    bool parseObject(
        QPlainTextEdit* editor,                  // 需要解析的 JSON 对象编辑器
        const QString& fieldName,                // 错误信息使用的字段名称
        QJsonObject* value);                     // 输出成功解析的对象
    void showError(const QString& action,        // 失败操作的人类可读名称
                   const LibMcp::McpError& error);// 在状态和输出区显示协议错误

    LibMcp::McpClientManager* m_manager = nullptr;  // 页面共享且不由工作台拥有的 Client Manager
    QComboBox* m_clientSelector = nullptr;           // 当前用于协议操作的 Client
    QLabel* m_connectionState = nullptr;             // 当前 Client 最终连接状态
    QPushButton* m_discoverButton = nullptr;          // 发起能力发现
    QPushButton* m_loadToolsButton = nullptr;         // 加载完整工具列表
    QPlainTextEdit* m_discoveryOutput = nullptr;      // 显示 Server 版本、能力与说明
    QTableWidget* m_toolTable = nullptr;              // 显示 Server 工具名称和说明
    QLabel* m_toolDescription = nullptr;              // 显示当前工具的完整说明
    QPlainTextEdit* m_inputSchema = nullptr;          // 显示当前工具输入 Schema
    QPlainTextEdit* m_outputSchema = nullptr;         // 显示当前工具输出 Schema
    QPlainTextEdit* m_arguments = nullptr;            // 编辑 tools/call 参数对象
    QPushButton* m_callButton = nullptr;              // 发起工具调用
    QPushButton* m_cancelButton = nullptr;            // 取消活动工具调用
    QProgressBar* m_progress = nullptr;                // 显示工具调用进度或忙碌状态
    QLabel* m_callState = nullptr;                     // 显示工具调用当前阶段
    QWidget* m_inputArea = nullptr;                    // 按需显示 MRTR 补充输入区域
    QPlainTextEdit* m_inputRequests = nullptr;         // 显示 Server 请求的补充输入定义
    QPlainTextEdit* m_inputResponses = nullptr;        // 编辑补充输入响应映射
    QPushButton* m_retryButton = nullptr;              // 携带补充输入重试工具调用
    QPlainTextEdit* m_resultOutput = nullptr;           // 显示最终结果或错误 JSON
    QList<LibMcp::McpTool> m_tools;                    // 与工具表行号对应的当前工具快照
    QString m_requestState;                            // input_required 返回的不透明重试状态
    QSharedPointer<LibMcp::McpOperation<LibMcp::McpDiscoveryResult>> m_discoveryOperation;  // 活动发现操作
    QSharedPointer<LibMcp::McpOperation<QList<LibMcp::McpTool>>> m_listToolsOperation;       // 活动工具列表操作
    QSharedPointer<LibMcp::McpOperation<LibMcp::McpToolCallResult>> m_toolCallOperation;     // 活动工具调用操作
};
