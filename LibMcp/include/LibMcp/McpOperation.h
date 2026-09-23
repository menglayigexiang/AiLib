#pragma once

#include <LibMcp/McpResult.h>

#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>

#include <optional>
#include <utility>

namespace LibMcp {

namespace Internal {
class McpOperationAccess;  // 允许 Client 调度器完成 Operation，不对外暴露写接口
}

// 承载所有异步 MCP 操作共有的状态、取消和过程事件。
class LIBMCP_EXPORT McpOperationBase : public QObject
{
    Q_OBJECT
public:
    /// 表示异步 MCP 操作当前所处的最终或运行状态。
    enum class Status
    {
        Running,    // 操作仍在执行
        Succeeded,  // 操作已成功完成
        Failed,     // 操作以错误结束
        Cancelled   // 操作被调用方取消
    };
    Q_ENUM(Status)

    ~McpOperationBase() override;             // 释放操作对象，不管理 QObject parent
    Status status() const;                    // 返回当前操作状态
    bool isFinished() const;                  // 查询操作是否已经进入最终状态
    McpError error() const;                   // 返回失败或取消错误，成功时为空错误
    void cancel();                            // 请求取消仍在执行的操作

signals:
    void finished();                          // 操作进入任一最终状态后发出一次
    void cancellationRequested();             // 调用方首次请求取消时发出
    void progressChanged(
        double progress,                      // 当前进度值
        double total,                         // 总量，未知时为负数
        const QString& message);              // 可选的人类可读进度说明
    void inputRequired(
        const QJsonObject& request);           // 操作需要调用方提供额外输入

protected:
    explicit McpOperationBase();               // 创建无 parent 且处于运行状态的操作
    void finishSuccess();                      // 将运行中操作标记为成功

private:
    friend class Internal::McpOperationAccess;
    void finishFailure(const McpError& error); // 将运行中操作标记为失败
    void finishCancelled();                    // 将运行中操作标记为已取消

    Status m_status = Status::Running;          // 当前操作状态
    McpError m_error;                           // 失败或取消时的最终错误
};

// 保存一个具有类型化最终值的 MCP 异步操作。
template<typename T>
class McpOperation final : public McpOperationBase
{
public:
    McpOperation() = default;                   // 创建尚未完成的类型化操作
    const std::optional<T>& result() const      // 返回成功结果，未成功时为空
    {
        return m_result;
    }

private:
    friend class Internal::McpOperationAccess;
    void setResult(T value)                     // 保存最终值并将操作标记为成功
    {
        m_result = std::move(value);
        finishSuccess();
    }

    std::optional<T> m_result;                  // 成功完成后的类型化结果
};

// 表示不携带成功值的 MCP 异步操作。
template<>
class McpOperation<void> final : public McpOperationBase
{
public:
    McpOperation() = default;                   // 创建尚未完成的无值操作

private:
    friend class Internal::McpOperationAccess;
    void setResult()                            // 将无值操作标记为成功
    {
        finishSuccess();
    }
};

} // namespace LibMcp
