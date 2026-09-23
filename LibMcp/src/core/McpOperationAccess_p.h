#pragma once

#include <LibMcp/McpOperation.h>

namespace LibMcp::Internal {

// 为 Client 内部调度器提供完成 Operation 的最小写接口。
class McpOperationAccess final
{
public:
    template<typename T>
    static void succeed(
        const QSharedPointer<McpOperation<T>>& operation,  // 需要完成的类型化操作
        T value)                                           // 操作的最终成功值
    {                                                       // 保存成功值并结束操作
        operation->setResult(std::move(value));
    }

    static void succeed(
        const QSharedPointer<McpOperation<void>>& operation)  // 需要完成的无值操作
    {                                                         // 将无值操作标记为成功
        operation->setResult();
    }

    static void fail(
        const QSharedPointer<McpOperationBase>& operation,  // 需要结束的操作
        const McpError& error)                             // 操作的最终错误
    {                                                      // 将操作标记为失败
        operation->finishFailure(error);
    }

    static void cancel(
        const QSharedPointer<McpOperationBase>& operation)  // 需要结束的操作
    {                                                       // 将操作标记为已取消
        operation->finishCancelled();
    }

    static void requireInput(
        const QSharedPointer<McpOperationBase>& operation,  // 需要向调用方请求输入的操作
        const QJsonObject& requests)                        // Server 返回的输入请求映射
    {                                                       // 转发 MRTR 输入需求且不结束操作对象
        emit operation->inputRequired(requests);
    }

    static void reportProgress(
        const QSharedPointer<McpOperationBase>& operation,  // 需要向调用方报告进度的操作
        double progress,                                  // 当前已完成的工作量
        double total,                                     // 总工作量，未知时为负数
        const QString& message)                            // 可选的人类可读说明
    {                                                      // 转发请求内的进度通知
        emit operation->progressChanged(progress, total, message);
    }
};

} // namespace LibMcp::Internal
