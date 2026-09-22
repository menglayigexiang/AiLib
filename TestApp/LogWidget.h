#pragma once

#include <QDateTime>
#include <QMessageLogContext>
#include <QWidget>

#include <memory>

class QPushButton;
class QTextBrowser;

// 集中截获、格式化并展示 TestApp 进程日志的控件。
class LogWidget final : public QWidget
{
public:
    explicit LogWidget(QWidget* parent = nullptr);  // 创建日志控件并开始截获 Qt 与 C++ 标准流
    ~LogWidget() override;                          // 恢复原有日志处理器和标准流缓冲区

private:
    // 描述一条等待在界面中展示的日志记录。
    struct LogEntry
    {
        QDateTime timestamp;       // 记录日志产生的本地时间
        QtMsgType type;            // 记录映射后的 Qt 日志级别
        QString category;          // 记录日志分类或标准流来源
        QString message;           // 记录不含格式前缀的原始消息
    };

    // 截获单个 C++ 标准流并将内容同时转发给原始缓冲区。
    class StreamRedirectBuffer;

    static void handleQtMessage(
        QtMsgType type,                     // Qt 消息的严重级别
        const QMessageLogContext& context,  // Qt 消息的分类及源码上下文
        const QString& message);            // 截获 Qt 消息并保留原有控制台输出
    static void enqueueStandardMessage(
        QtMsgType type,           // 标准流映射后的日志级别
        const QString& category,  // 标准流对应的日志分类
        const QString& message);  // 将标准流消息投递给当前日志控件

    void installMessageCapture();                  // 安装 Qt 消息处理器和标准流缓冲区
    void uninstallMessageCapture();                // 恢复安装前的 Qt 消息处理器和标准流缓冲区
    void enqueueEntry(const LogEntry& entry);      // 将任意线程产生的日志安全投递到界面线程
    void appendEntry(const LogEntry& entry);       // 格式化并追加一条日志记录
    QString levelName(QtMsgType type) const;       // 返回用于显示的日志级别名称
    QColor levelColor(QtMsgType type) const;       // 返回适配当前主题的日志级别颜色

    QTextBrowser* m_textBrowser = nullptr;                              // 显示格式化日志内容
    QPushButton* m_clearButton = nullptr;                               // 清空当前日志内容
    QtMessageHandler m_previousMessageHandler = nullptr;                // 保存安装前的 Qt 消息处理器
    std::unique_ptr<StreamRedirectBuffer> m_stdoutRedirect;             // 截获并转发 std::cout
    std::unique_ptr<StreamRedirectBuffer> m_stderrRedirect;             // 截获并转发 std::cerr
    std::unique_ptr<StreamRedirectBuffer> m_stdlogRedirect;             // 截获并转发 std::clog
};
