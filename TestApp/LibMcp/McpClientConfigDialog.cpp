#include "McpClientConfigDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QPlainTextEdit* createJsonEditor(
    const QString& initialText,  // 编辑框初始 JSON 文本
    QWidget* parent)             // 编辑框 QObject 所有者
{                                // 创建高度适中的等宽 JSON 编辑框
    auto* editor = new QPlainTextEdit(initialText, parent);  // 承载结构化 Transport 参数
    editor->setTabChangesFocus(true);
    editor->setMaximumHeight(92);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    return editor;
}

QString compactJson(const QJsonValue& value)  // 将 JSON 值格式化为紧凑可编辑文本
{
    if (value.isArray()) {
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

} // namespace

McpClientConfigDialog::McpClientConfigDialog(QWidget* parent)  // 创建空白 Client 配置表单
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("添加 MCP Client"));
    setModal(true);
    resize(700, 650);
    auto* rootLayout = new QVBoxLayout(this);  // 按标题、公共字段、参数页和按钮排列内容
    auto* heading = new QLabel(QStringLiteral("连接至自定义 MCP"), this);  // 对话框主标题
    QFont headingFont = heading->font();  // 调整标题层级但沿用系统字体
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.25);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    rootLayout->addWidget(heading);

    auto* commonForm = new QFormLayout();  // 排列名称和 Transport 类型
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("mcpClientNameEdit"));
    m_name->setPlaceholderText(QStringLiteral("MCP server name"));
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("mcpClientTypeCombo"));
    m_type->addItem(QStringLiteral("STDIO"), QStringLiteral("stdio"));
    m_type->addItem(QStringLiteral("流式 HTTP"), QStringLiteral("streamable-http"));
    commonForm->addRow(QStringLiteral("名称"), m_name);
    commonForm->addRow(QStringLiteral("类型"), m_type);
    rootLayout->addLayout(commonForm);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("mcpClientTransportPages"));
    auto* stdioPage = new QWidget(m_pages);  // 承载 STDIO 专属配置项
    auto* stdioForm = new QFormLayout(stdioPage);  // 排列 STDIO 命令及 JSON 参数
    m_command = new QLineEdit(stdioPage);
    m_command->setObjectName(QStringLiteral("mcpStdioCommandEdit"));
    m_command->setPlaceholderText(QStringLiteral("openai-dev-mcp serve-sqlite"));
    m_arguments = createJsonEditor(QStringLiteral("[]"), stdioPage);
    m_environment = createJsonEditor(QStringLiteral("{}"), stdioPage);
    m_inheritEnvironment = createJsonEditor(QStringLiteral("[]"), stdioPage);
    m_workingDirectory = new QLineEdit(stdioPage);
    m_workingDirectory->setPlaceholderText(QStringLiteral("~/code"));
    stdioForm->addRow(QStringLiteral("启动命令"), m_command);
    stdioForm->addRow(QStringLiteral("参数（JSON 数组）"), m_arguments);
    stdioForm->addRow(QStringLiteral("环境变量（JSON 对象）"), m_environment);
    stdioForm->addRow(QStringLiteral("环境变量传递（JSON 数组）"), m_inheritEnvironment);
    stdioForm->addRow(QStringLiteral("工作目录"), m_workingDirectory);

    auto* httpPage = new QWidget(m_pages);  // 承载 Streamable HTTP 专属配置项
    auto* httpForm = new QFormLayout(httpPage);  // 排列 URL 与 Header 配置
    m_url = new QLineEdit(httpPage);
    m_url->setObjectName(QStringLiteral("mcpHttpUrlEdit"));
    m_url->setPlaceholderText(QStringLiteral("https://mcp.example.com/mcp"));
    m_bearerEnvironment = new QLineEdit(httpPage);
    m_bearerEnvironment->setPlaceholderText(QStringLiteral("MCP_BEARER_TOKEN"));
    m_headers = createJsonEditor(QStringLiteral("{}"), httpPage);
    m_environmentHeaders = createJsonEditor(QStringLiteral("{}"), httpPage);
    httpForm->addRow(QStringLiteral("URL"), m_url);
    httpForm->addRow(QStringLiteral("Bearer 令牌环境变量"), m_bearerEnvironment);
    httpForm->addRow(QStringLiteral("标头（JSON 对象）"), m_headers);
    httpForm->addRow(QStringLiteral("来自环境变量的标头（JSON 对象）"),
                     m_environmentHeaders);

    m_pages->addWidget(stdioPage);
    m_pages->addWidget(httpPage);
    rootLayout->addWidget(m_pages, 1);

    auto* buttons = new QDialogButtonBox(  // 提供标准保存与取消操作
        QDialogButtonBox::Save | QDialogButtonBox::Cancel,
        this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    rootLayout->addWidget(buttons);
    connect(m_type,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            &McpClientConfigDialog::updateTransportPage);
    connect(buttons,
            &QDialogButtonBox::accepted,
            this,
            &McpClientConfigDialog::validateAndAccept);
    connect(buttons,
            &QDialogButtonBox::rejected,
            this,
            &QDialog::reject);
    updateTransportPage();
}

void McpClientConfigDialog::setConfiguration(
    const LibMcp::McpClientConfig& configuration)  // 使用现有配置预填编辑表单
{
    setWindowTitle(QStringLiteral("编辑 MCP Client"));
    m_name->setText(configuration.name);
    const int typeIndex = m_type->findData(configuration.transportType);  // 查找配置对应的类型页
    m_type->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    const QJsonObject& values = configuration.transportConfig;  // 缩短后续配置字段访问
    m_command->setText(values.value(QStringLiteral("command")).toString());
    m_arguments->setPlainText(compactJson(values.value(QStringLiteral("arguments")).toArray()));
    m_environment->setPlainText(compactJson(values.value(QStringLiteral("environment")).toObject()));
    m_inheritEnvironment->setPlainText(
        compactJson(values.value(QStringLiteral("inheritEnvironment")).toArray()));
    m_workingDirectory->setText(
        values.value(QStringLiteral("workingDirectory")).toString());
    m_url->setText(values.value(QStringLiteral("url")).toString());
    m_bearerEnvironment->setText(
        values.value(QStringLiteral("bearerTokenEnvironment")).toString());
    m_headers->setPlainText(compactJson(values.value(QStringLiteral("headers")).toObject()));
    m_environmentHeaders->setPlainText(
        compactJson(values.value(QStringLiteral("environmentHeaders")).toObject()));
}

LibMcp::McpClientConfig McpClientConfigDialog::configuration() const  // 返回经过表单解析的配置
{
    LibMcp::McpClientConfig result;  // 汇总当前表单中的持久化配置
    result.id = m_name->text().trimmed();
    result.name = result.id;
    result.transportType = m_type->currentData().toString();
    bool valid = false;  // 接收已在接受前验证过的 JSON 解析状态
    if (result.transportType == QStringLiteral("stdio")) {
        result.transportConfig = {
            {QStringLiteral("command"), m_command->text().trimmed()},
            {QStringLiteral("arguments"),
             parseJson(m_arguments, QString{}, QJsonValue::Array, &valid)},
            {QStringLiteral("environment"),
             parseJson(m_environment, QString{}, QJsonValue::Object, &valid)},
            {QStringLiteral("inheritEnvironment"),
             parseJson(m_inheritEnvironment, QString{}, QJsonValue::Array, &valid)},
            {QStringLiteral("workingDirectory"),
             m_workingDirectory->text().trimmed()}};
    } else {
        result.transportConfig = {
            {QStringLiteral("url"), m_url->text().trimmed()},
            {QStringLiteral("bearerTokenEnvironment"),
             m_bearerEnvironment->text().trimmed()},
            {QStringLiteral("headers"),
             parseJson(m_headers, QString{}, QJsonValue::Object, &valid)},
            {QStringLiteral("environmentHeaders"),
             parseJson(m_environmentHeaders, QString{}, QJsonValue::Object, &valid)}};
    }
    return result;
}

void McpClientConfigDialog::validateAndAccept()  // 校验必填字段和 JSON 后接受对话框
{
    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("配置无效"), QStringLiteral("名称不能为空。"));
        m_name->setFocus();
        return;
    }
    if (m_type->currentData().toString() == QStringLiteral("stdio")
        && m_command->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("配置无效"), QStringLiteral("启动命令不能为空。"));
        m_command->setFocus();
        return;
    }
    const QUrl endpoint(m_url->text().trimmed());  // 解析并校验 Streamable HTTP 绝对端点
    if (m_type->currentData().toString() == QStringLiteral("streamable-http")
        && (!endpoint.isValid() || endpoint.host().isEmpty()
            || (endpoint.scheme() != QStringLiteral("http")
                && endpoint.scheme() != QStringLiteral("https")))) {
        QMessageBox::warning(this, QStringLiteral("配置无效"), QStringLiteral("请输入有效 URL。"));
        m_url->setFocus();
        return;
    }

    bool valid = false;  // 逐项接收 JSON 字段校验结果
    if (m_type->currentData().toString() == QStringLiteral("stdio")) {
        parseJson(m_arguments, QStringLiteral("参数"), QJsonValue::Array, &valid);
        if (!valid) return;
        parseJson(m_environment, QStringLiteral("环境变量"), QJsonValue::Object, &valid);
        if (!valid) return;
        parseJson(m_inheritEnvironment, QStringLiteral("环境变量传递"), QJsonValue::Array, &valid);
        if (!valid) return;
    } else {
        parseJson(m_headers, QStringLiteral("标头"), QJsonValue::Object, &valid);
        if (!valid) return;
        parseJson(m_environmentHeaders, QStringLiteral("环境变量标头"), QJsonValue::Object, &valid);
        if (!valid) return;
    }
    accept();
}

void McpClientConfigDialog::updateTransportPage()  // 根据类型切换 STDIO 或 HTTP 表单
{
    m_pages->setCurrentIndex(
        m_type->currentData().toString() == QStringLiteral("stdio") ? 0 : 1);
}

QJsonValue McpClientConfigDialog::parseJson(
    QPlainTextEdit* editor,            // 需要解析的 JSON 编辑框
    const QString& fieldName,          // 错误提示使用的字段名称
    QJsonValue::Type expectedType,     // 期望的 JSON 顶层类型
    bool* valid) const                 // 返回解析是否成功
{                                     // 解析并验证一个 JSON 配置字段
    QJsonParseError error;  // 保存 JSON 语法错误位置与说明
    const QJsonDocument document =  // 解析编辑器中的 UTF-8 JSON
        QJsonDocument::fromJson(editor->toPlainText().trimmed().toUtf8(), &error);
    const QJsonValue value = document.isArray()
                                 ? QJsonValue(document.array())
                                 : QJsonValue(document.object());  // 保留顶层数组或对象
    *valid = error.error == QJsonParseError::NoError
             && ((expectedType == QJsonValue::Array && document.isArray())
                 || (expectedType == QJsonValue::Object && document.isObject()));
    if (!*valid && !fieldName.isEmpty()) {
        QMessageBox::warning(
            const_cast<McpClientConfigDialog*>(this),
            QStringLiteral("JSON 无效"),
            QStringLiteral("%1必须是合法的 JSON %2。")
                .arg(fieldName,
                     expectedType == QJsonValue::Array
                         ? QStringLiteral("数组")
                         : QStringLiteral("对象")));
        editor->setFocus();
    }
    return value;
}
