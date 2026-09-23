#pragma once

#include <LibMcp/McpTypes.h>

#include <QDialog>

class QComboBox;       // 前向声明 Transport 类型选择框
class QLineEdit;       // 前向声明单行配置输入框
class QPlainTextEdit;  // 前向声明 JSON 配置编辑框
class QStackedWidget;  // 前向声明按 Transport 切换的配置页

// 提供新增和编辑 MCP Client 配置共用的模态表单。
class McpClientConfigDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit McpClientConfigDialog(QWidget* parent = nullptr);  // 创建空白 Client 配置表单

    void setConfiguration(
        const LibMcp::McpClientConfig& configuration);  // 使用现有配置预填编辑表单
    LibMcp::McpClientConfig configuration() const;      // 返回经过表单解析的配置

private slots:
    void validateAndAccept();                           // 校验必填字段和 JSON 后接受对话框
    void updateTransportPage();                         // 根据类型切换 STDIO 或 HTTP 表单

private:
    QJsonValue parseJson(
        QPlainTextEdit* editor,                         // 需要解析的 JSON 编辑框
        const QString& fieldName,                       // 错误提示使用的字段名称
        QJsonValue::Type expectedType,                  // 期望的 JSON 顶层类型
        bool* valid) const;                             // 返回解析是否成功

    QLineEdit* m_name = nullptr;                        // 用户可读且同时作为稳定 ID 的名称
    QComboBox* m_type = nullptr;                        // STDIO 与 Streamable HTTP 类型选择
    QStackedWidget* m_pages = nullptr;                  // 当前 Transport 的参数表单
    QLineEdit* m_command = nullptr;                     // STDIO 启动命令
    QPlainTextEdit* m_arguments = nullptr;              // STDIO 参数 JSON 数组
    QPlainTextEdit* m_environment = nullptr;            // STDIO 环境变量 JSON 对象
    QPlainTextEdit* m_inheritEnvironment = nullptr;     // STDIO 继承环境变量 JSON 数组
    QLineEdit* m_workingDirectory = nullptr;            // STDIO 工作目录
    QLineEdit* m_url = nullptr;                         // Streamable HTTP Endpoint
    QLineEdit* m_bearerEnvironment = nullptr;           // Bearer Token 环境变量名
    QPlainTextEdit* m_headers = nullptr;                 // 固定 HTTP Headers JSON 对象
    QPlainTextEdit* m_environmentHeaders = nullptr;      // Header 到环境变量的 JSON 映射
};
