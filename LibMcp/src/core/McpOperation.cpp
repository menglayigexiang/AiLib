#include <LibMcp/McpOperation.h>

namespace LibMcp {

McpOperationBase::McpOperationBase()  // 创建无 parent 且处于运行状态的操作
    : QObject(nullptr)
{
}

McpOperationBase::~McpOperationBase() = default;  // 释放操作对象，不管理 QObject parent

McpOperationBase::Status McpOperationBase::status() const  // 返回当前操作状态
{
    return m_status;
}

bool McpOperationBase::isFinished() const  // 查询操作是否已经进入最终状态
{
    return m_status != Status::Running;
}

McpError McpOperationBase::error() const  // 返回失败或取消错误，成功时为空错误
{
    return m_error;
}

void McpOperationBase::cancel()  // 请求取消仍在执行的操作
{
    if (m_status != Status::Running) {
        return;
    }
    emit cancellationRequested();
}

void McpOperationBase::finishSuccess()  // 将运行中操作标记为成功
{
    if (m_status != Status::Running) {
        return;
    }
    m_status = Status::Succeeded;
    emit finished();
}

void McpOperationBase::finishFailure(const McpError& error)  // 将运行中操作标记为失败
{
    if (m_status != Status::Running) {
        return;
    }
    m_error = error;
    m_status = Status::Failed;
    emit finished();
}

void McpOperationBase::finishCancelled()  // 将运行中操作标记为已取消
{
    if (m_status != Status::Running) {
        return;
    }
    m_error = {McpErrorCode::Cancelled, QStringLiteral("MCP 操作已取消")};
    m_status = Status::Cancelled;
    emit finished();
}

} // namespace LibMcp
