#include <AiLib/tools/ToolExecutor.h>
#include <AiLib/tools/ToolErrorCodes.h>
#include "SchemaValidator.h"
#include <exception>
namespace AiLib {
namespace {
bool checkStop(const ToolExecutionContext& context, SdkError& error)  // 检查协作取消及总截止时间
{
    if (!context.cancellation.isCancellationRequested() && !context.isTimedOut())
        return false;
    const bool cancelled =  // 主动取消优先于同时到达的截止时间
        context.cancellation.isCancellationRequested();
    error.category = cancelled ? ErrorCategory::Cancelled : ErrorCategory::Timeout;
    error.code = cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Timeout");
    error.message =
        cancelled ? QStringLiteral("调用方取消工具流程") : QStringLiteral("总运行截止时间已到");
    return true;
}
void failTool(ToolResult& result,      // 规范化业务结果输出
              const QString& code,     // 稳定错误码
              const QString& message)  // 规范化工具失败，不把 SDK 错误对象嵌入结果
{
    result.success = false;
    result.data = QJsonValue();
    result.errorCode = code;
    result.errorMessage = message;
}
}
ToolExecutor::ToolExecutor(ToolRegistry& registry,
                           IToolApprovalProvider* approvalProvider)  // 借用应用管理的依赖
    : m_registry(registry), m_approvalProvider(approvalProvider)
{
}
bool ToolExecutor::execute(const ToolCall& call,                 // 待执行的规范化调用
                           const ToolExecutionContext& context,  // 来源和停止上下文
                           ToolResult& result,                   // 规范化业务结果输出
                           SdkError& error) const                // 同步完成调用或报告停止信号
{
    bool handlerExecuted = false;  // 简单调用不需要读取的执行标记
    return execute(call, context, result, error, handlerExecuted);
}
bool ToolExecutor::execute(const ToolCall& call,                 // 输入规范化调用
                           const ToolExecutionContext& context,  // 来源及停止条件
                           ToolResult& result,                   // 输出实际业务结果
                           SdkError& error,                      // 流程停止或故障输出
                           bool& handlerExecuted) const          // 区分前置停止与业务执行后的停止
{
    handlerExecuted = false;
    error = {};
    result = {};
    result.callId = call.id;
    result.toolName = call.name;
    if (call.id.isEmpty() || call.name.isEmpty()) {
        error.category = ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("InvalidToolCall");
        error.message = QStringLiteral("核心 ToolCall 的 id 和 name 必须非空");
        failTool(result, ToolErrorCodes::InvalidArguments, error.message);
        return false;
    }
    if (checkStop(context, error)) {
        failTool(result, error.code, error.message);
        return false;
    }
    const auto it = m_registry.m_entries.constFind(call.name);  // 单次调用查询条目，不创建 Run 快照
    if (it == m_registry.m_entries.cend()) {
        failTool(result, ToolErrorCodes::ToolNotFound, QStringLiteral("工具未注册"));
        return true;
    }
    const auto entry = it.value();  // 保持本次执行条目及共享锁的生命周期
    QString reason;                 // 参数验证失败说明
    if (!validateToolArguments(entry->definition.inputSchema, call.arguments, reason)) {
        failTool(result, ToolErrorCodes::InvalidArguments, reason);
        return true;
    }
    if (checkStop(context, error)) {
        failTool(result, error.code, error.message);
        return false;
    }
    if (entry->definition.approvalPolicy == ToolApprovalPolicy::Always) {
        if (!m_approvalProvider) {
            failTool(result, ToolErrorCodes::ApprovalUnavailable,
                     QStringLiteral("工具需要确认，但未提供确认策略"));
            return true;
        }
        ToolApprovalResult approval;  // 应用输出的确认决定
        SdkError approvalError;       // 确认机制故障
        bool confirmed = false;       // 确认机制是否正常返回
        try {
            confirmed = m_approvalProvider->requestApproval(call, entry->definition, context,
                                                            approval, approvalError);
        } catch (const std::exception& exception) {
            approvalError.message = QString::fromUtf8(exception.what());
        }  // 捕获应用确认异常
        catch (...) {
            approvalError.message = QStringLiteral("确认策略抛出未知异常");
        }
        if (checkStop(context, error)) {
            failTool(result, error.code, error.message);
            return false;
        }
        if (!confirmed) {
            failTool(result, ToolErrorCodes::ApprovalUnavailable,
                     approvalError.message.isEmpty() ? QStringLiteral("确认机制无法完成")
                                                     : approvalError.message);
            return true;
        }
        if (approval.decision == ToolApprovalDecision::Cancel) {
            error.category = ErrorCategory::Cancelled;
            error.code = QStringLiteral("Cancelled");
            error.message =
                approval.reason.isEmpty() ? QStringLiteral("用户取消本次运行") : approval.reason;
            failTool(result, error.code, error.message);
            return false;
        }
        if (approval.decision != ToolApprovalDecision::Allow) {
            failTool(result, ToolErrorCodes::ApprovalDenied, approval.reason);
            return true;
        }
    }
    std::unique_lock<std::timed_mutex> lock(entry->executionMutex,
                                            std::defer_lock);  // 确认结束后才持有串行执行锁
    if (entry->definition.concurrency == ToolConcurrency::Serialized) {
        while (!lock.try_lock_for(std::chrono::milliseconds(20))) {
            if (checkStop(context, error)) {
                failTool(result, error.code, error.message);
                return false;
            }
        }
    }
    if (checkStop(context, error)) {
        failTool(result, error.code, error.message);
        return false;
    }
    SdkError handlerError;  // Handler 返回的业务故障，转换为工具失败
    bool executed = false;  // Handler 是否成功完成业务执行
    handlerExecuted = true;
    try {
        executed = entry->handler(call, context, result, handlerError);
    } catch (const std::exception& exception) {
        handlerError.code = ToolErrorCodes::ExecutionFailed;
        handlerError.message = QString::fromUtf8(exception.what());
    }  // 捕获业务异常，不终止 SDK 调用链
    catch (...) {
        handlerError.code = ToolErrorCodes::ExecutionFailed;
        handlerError.message = QStringLiteral("Handler 抛出未知异常");
    }
    result.callId = call.id;
    result.toolName = call.name;
    if (!executed) {
        failTool(result,
                 handlerError.code.isEmpty() ? ToolErrorCodes::ExecutionFailed : handlerError.code,
                 handlerError.message.isEmpty() ? QStringLiteral("工具执行失败")
                                                : handlerError.message);
    } else if (result.success) {
        result.errorCode.clear();
        result.errorMessage.clear();
    } else {
        result.data = QJsonValue();
        if (result.errorCode.isEmpty())
            result.errorCode = ToolErrorCodes::ExecutionFailed;
        if (result.errorMessage.isEmpty())
            result.errorMessage = QStringLiteral("工具返回业务失败");
    }
    if (checkStop(context, error))
        return false;
    return true;
}
}
