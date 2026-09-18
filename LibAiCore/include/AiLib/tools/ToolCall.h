#pragma once

#include <QString>
#include <QJsonObject>

namespace AiLib {
struct ToolCall {
    QString id;             // 非空调用标识，在当前助手响应内唯一
    QString name;           // 本次请求执行的工具名称
    QJsonObject arguments;  // 完整解析后的工具参数，不存放流式分片
};
}  // AiLib 命名空间结束
