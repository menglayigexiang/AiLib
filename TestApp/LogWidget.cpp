#include "LogWidget.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cstdio>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>

namespace {
QMutex g_logWidgetMutex;                    // 保护全局日志控件及旧处理器的生命周期
QPointer<LogWidget> g_logWidget;            // 指向当前接收进程日志的日志控件
QtMessageHandler g_previousHandler = nullptr;  // 保存供全局回调转发的原有 Qt 消息处理器
}  // namespace

// 截获单个 C++ 标准流并将内容同时转发给原始缓冲区。
class LogWidget::StreamRedirectBuffer final : public std::streambuf
{
public:
    StreamRedirectBuffer(
        std::ostream& stream,        // 需要截获的 C++ 标准输出流
        QtMsgType type,              // 输出流映射后的 Qt 日志级别
        const QString& category)     // 创建标准流截获器并替换原缓冲区
        : m_stream(stream)
        , m_originalBuffer(stream.rdbuf(this))
        , m_type(type)
        , m_category(category)
    {
    }

    ~StreamRedirectBuffer() override  // 输出残留内容并恢复原标准流缓冲区
    {
        flushPendingMessage();
        m_stream.rdbuf(m_originalBuffer);
    }

protected:
    int_type overflow(int_type character) override  // 转发并收集单个输出字符
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }

        const char value = traits_type::to_char_type(character);  // 当前写入标准流的字符
        std::lock_guard<std::mutex> lock(m_mutex);                 // 串行保护转发及行缓冲区
        if (traits_type::eq_int_type(m_originalBuffer->sputc(value), traits_type::eof())) {
            return traits_type::eof();
        }
        appendCharacter(value);
        return character;
    }

    std::streamsize xsputn(
        const char* text,             // 本次写入的字节序列
        std::streamsize count) override  // 转发并收集一段输出内容
    {
        std::lock_guard<std::mutex> lock(m_mutex);  // 串行保护转发及行缓冲区
        const std::streamsize written =             // 原标准流实际接收的字节数量
            m_originalBuffer->sputn(text, count);
        for (std::streamsize index = 0; index < written; ++index) {  // 按字符识别完整日志行
            appendCharacter(text[index]);
        }
        return written;
    }

    int sync() override  // 刷新原始标准流并提交尚未换行的消息
    {
        std::lock_guard<std::mutex> lock(m_mutex);  // 串行保护刷新及行缓冲区
        const int result = m_originalBuffer->pubsync();  // 保存原标准流刷新结果
        emitPendingMessage();
        return result;
    }

private:
    void appendCharacter(char character)  // 将字符加入行缓冲区并在换行时提交消息
    {
        if (character == '\n') {
            emitPendingMessage();
            return;
        }
        if (character != '\r') {
            m_pending.push_back(character);
        }
    }

    void emitPendingMessage()  // 将当前 UTF-8 行缓冲区提交给日志控件
    {
        if (m_pending.empty()) {
            return;
        }
        const QString message = QString::fromUtf8(  // 将标准流 UTF-8 字节转换为界面文本
            m_pending.data(),
            static_cast<int>(m_pending.size()));
        m_pending.clear();
        LogWidget::enqueueStandardMessage(m_type, m_category, message);
    }

    void flushPendingMessage()  // 线程安全地提交析构时残留的标准流内容
    {
        std::lock_guard<std::mutex> lock(m_mutex);  // 串行保护析构阶段的行缓冲区
        emitPendingMessage();
    }

    std::ostream& m_stream;                 // 当前被截获的标准输出流
    std::streambuf* m_originalBuffer;       // 安装前用于继续输出到控制台的缓冲区
    QtMsgType m_type;                       // 当前标准流映射后的日志级别
    QString m_category;                     // 当前标准流在日志中的分类名称
    std::mutex m_mutex;                     // 保护多线程写入与待提交内容
    std::string m_pending;                  // 暂存尚未提交的 UTF-8 日志行
};

LogWidget::LogWidget(QWidget* parent)  // 创建日志控件并开始截获 Qt 与 C++ 标准流
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);                          // 纵向排列操作区与日志正文
    auto* actionLayout = new QHBoxLayout;                          // 承载日志操作按钮
    m_clearButton = new QPushButton(tr("清空日志"), this);
    m_textBrowser = new QTextBrowser(this);

    m_clearButton->setToolTip(tr("清空当前显示的全部日志"));
    m_textBrowser->setReadOnly(true);
    m_textBrowser->setLineWrapMode(QTextEdit::NoWrap);
    m_textBrowser->document()->setMaximumBlockCount(50000);
    m_textBrowser->setAccessibleName(tr("运行日志"));

    actionLayout->addStretch();
    actionLayout->addWidget(m_clearButton);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(actionLayout);
    layout->addWidget(m_textBrowser);

    connect(m_clearButton, &QPushButton::clicked,
            m_textBrowser, &QTextBrowser::clear);
    installMessageCapture();
}

LogWidget::~LogWidget()  // 恢复原有日志处理器和标准流缓冲区
{
    uninstallMessageCapture();
}

void LogWidget::handleQtMessage(
    QtMsgType type,                     // Qt 消息的严重级别
    const QMessageLogContext& context,  // Qt 消息的分类及源码上下文
    const QString& message)             // 截获 Qt 消息并保留原有控制台输出
{
    QtMessageHandler previousHandler = nullptr;  // 保存本次需要调用的原消息处理器
    {
        QMutexLocker locker(&g_logWidgetMutex);  // 防止控件析构与消息投递并发
        previousHandler = g_previousHandler;
        if (g_logWidget) {
            const LogEntry entry{QDateTime::currentDateTime(),
                                 type,
                                 QString::fromUtf8(context.category != nullptr
                                                       ? context.category
                                                       : "default"),
                                 message};  // 汇总本次 Qt 日志记录
            g_logWidget->enqueueEntry(entry);
        }
    }

    if (previousHandler != nullptr) {
        previousHandler(type, context, message);
        return;
    }

    const QByteArray formattedMessage =  // 使用 Qt 默认格式继续写入调试控制台
        qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fwrite(formattedMessage.constData(), 1,
                static_cast<std::size_t>(formattedMessage.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void LogWidget::enqueueStandardMessage(
    QtMsgType type,           // 标准流映射后的日志级别
    const QString& category,  // 标准流对应的日志分类
    const QString& message)   // 将标准流消息投递给当前日志控件
{
    QMutexLocker locker(&g_logWidgetMutex);  // 防止控件析构与消息投递并发
    if (!g_logWidget) {
        return;
    }
    const LogEntry entry{QDateTime::currentDateTime(),
                         type,
                         category,
                         message};  // 汇总标准流日志记录
    g_logWidget->enqueueEntry(entry);
}

void LogWidget::installMessageCapture()  // 安装 Qt 消息处理器和标准流缓冲区
{
    QMutexLocker locker(&g_logWidgetMutex);  // 串行保护全局处理器安装过程
    g_logWidget = this;
    m_previousMessageHandler = qInstallMessageHandler(&LogWidget::handleQtMessage);
    g_previousHandler = m_previousMessageHandler;
    m_stdoutRedirect = std::make_unique<StreamRedirectBuffer>(
        std::cout, QtInfoMsg, QStringLiteral("stdout"));
    m_stderrRedirect = std::make_unique<StreamRedirectBuffer>(
        std::cerr, QtCriticalMsg, QStringLiteral("stderr"));
    m_stdlogRedirect = std::make_unique<StreamRedirectBuffer>(
        std::clog, QtDebugMsg, QStringLiteral("stdlog"));
}

void LogWidget::uninstallMessageCapture()  // 恢复安装前的 Qt 消息处理器和标准流缓冲区
{
    m_stdoutRedirect.reset();
    m_stderrRedirect.reset();
    m_stdlogRedirect.reset();

    QMutexLocker locker(&g_logWidgetMutex);  // 串行保护全局处理器恢复过程
    if (g_logWidget == this) {
        qInstallMessageHandler(m_previousMessageHandler);
        g_previousHandler = nullptr;
        g_logWidget.clear();
    }
}

void LogWidget::enqueueEntry(const LogEntry& entry)  // 将任意线程产生的日志安全投递到界面线程
{
    QMetaObject::invokeMethod(
        this,
        [this, entry] { appendEntry(entry); },  // 在所属界面线程中安全更新日志控件
        Qt::QueuedConnection);
}

void LogWidget::appendEntry(const LogEntry& entry)  // 格式化并追加一条日志记录
{
    QScrollBar* scrollBar = m_textBrowser->verticalScrollBar();  // 读取并恢复用户当前阅读位置
    const int previousValue = scrollBar->value();                // 保存追加日志前的滚动位置
    const bool wasAtBottom = previousValue >= scrollBar->maximum();  // 判断用户此前是否位于底部
    const QString line = QStringLiteral("[%1][%2][%3] %4")          // 生成统一格式的日志文本
                             .arg(entry.timestamp.time().toString(QStringLiteral("HH:mm:ss.zzz")),
                                  levelName(entry.type),
                                  entry.category,
                                  entry.message);
    QTextCursor cursor(m_textBrowser->document());  // 在文档末尾追加带级别颜色的纯文本
    cursor.movePosition(QTextCursor::End);
    if (!m_textBrowser->document()->isEmpty()) {
        cursor.insertBlock();
    }
    QTextCharFormat format;  // 设置当前日志级别对应的文本样式
    format.setForeground(levelColor(entry.type));
    if (entry.type == QtCriticalMsg || entry.type == QtFatalMsg) {
        format.setFontWeight(QFont::Bold);
    }
    cursor.insertText(line, format);

    if (wasAtBottom) {
        scrollBar->setValue(scrollBar->maximum());
    } else {
        scrollBar->setValue(previousValue);
    }
}

QString LogWidget::levelName(QtMsgType type) const  // 返回用于显示的日志级别名称
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("Debug");
    case QtInfoMsg:
        return QStringLiteral("Info");
    case QtWarningMsg:
        return QStringLiteral("Warning");
    case QtCriticalMsg:
        return QStringLiteral("Critical");
    case QtFatalMsg:
        return QStringLiteral("Fatal");
    }
    return QStringLiteral("Unknown");
}

QColor LogWidget::levelColor(QtMsgType type) const  // 返回适配当前主题的日志级别颜色
{
    const QColor textColor = palette().color(QPalette::Text);  // 获取当前主题的默认文本颜色
    const bool darkTheme = textColor.lightness() > 128;        // 判断当前主题是否使用深色背景
    switch (type) {
    case QtDebugMsg:
        return darkTheme ? QColor(QStringLiteral("#A0A0A0"))
                         : QColor(QStringLiteral("#6B7280"));
    case QtInfoMsg:
        return textColor;
    case QtWarningMsg:
        return darkTheme ? QColor(QStringLiteral("#F59E0B"))
                         : QColor(QStringLiteral("#B45309"));
    case QtCriticalMsg:
    case QtFatalMsg:
        return darkTheme ? QColor(QStringLiteral("#FF6B6B"))
                         : QColor(QStringLiteral("#B91C1C"));
    }
    return textColor;
}
