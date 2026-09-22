#pragma once

#include <QMainWindow>
#include <QString>

class LogWidget;

// 统一承载 LibAiCore 与 LibMcp 人工测试页面的主窗口。
class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(
        const QString& initialProvider,  // LibAiCore 页面的初始 Provider 标识
        LogWidget* logWidget,            // 已开始截获进程日志的底部日志控件
        QWidget* parent = nullptr);      // 创建统一测试窗口，parent 为可选父控件
};
