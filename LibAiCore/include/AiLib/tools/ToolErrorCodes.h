#pragma once

#include <QString>

namespace AiLib::ToolErrorCodes {
inline const QString InvalidArguments = QStringLiteral("InvalidArguments");        // 参数 Schema 验证失败的稳定错误码
inline const QString ToolNotFound = QStringLiteral("ToolNotFound");                // 工具未注册的稳定错误码
inline const QString ToolNotEnabled = QStringLiteral("ToolNotEnabled");            // 工具不在本次运行白名单中的稳定错误码
inline const QString ApprovalDenied = QStringLiteral("ApprovalDenied");            // 用户拒绝本次调用的稳定错误码
inline const QString ApprovalUnavailable = QStringLiteral("ApprovalUnavailable");  // 确认提供者不可用的稳定错误码
inline const QString ExecutionFailed = QStringLiteral("ExecutionFailed");          // 业务执行失败或 Handler 抛出异常的稳定错误码
inline const QString ExecutionTimeout = QStringLiteral("ExecutionTimeout");        // 工具自身执行超时的稳定错误码
inline const QString Cancelled = QStringLiteral("Cancelled");                      // 工具局部操作取消，不替代 Agent 取消判定的稳定错误码
}                                                                                  // 工具错误码命名空间结束
