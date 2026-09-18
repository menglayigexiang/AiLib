#pragma once

#include <QString>
#include <QJsonValue>

namespace AiLib {
struct ToolResult {
    // 调用关联由 Executor 保证，Handler 不负责填写。
    QString callId;        // 由 Executor 根据输入调用标识统一填写
    QString toolName;      // 由 Executor 根据输入工具名称统一填写
    bool success = true;   // 本次调用是否成功产生可用业务结果
    QJsonValue data;       // 成功时的业务结果，不用于承载错误信息
    QString errorCode;     // 失败的开放字符串错误码，成功时为空
    QString errorMessage;  // 失败的可读说明，成功时为空
};
}  // AiLib 命名空间结束
