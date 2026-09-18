#pragma once

#include <QString>
#include <QJsonObject>

namespace AiLib {
enum class ToolApprovalPolicy { Never, Always };
enum class ToolConcurrency { Serialized, Concurrent };

// 描述 Function Tool 的参数契约和执行策略，不包含 Handler 或运行时锁。
struct FunctionToolDefinition {
    QString name;                                                   // 唯一的 Function Tool 名称
    QString description;                                            // 提供给模型的工具用途说明
    QJsonObject inputSchema;                                        // 工具参数的 JSON Schema 描述
    ToolApprovalPolicy approvalPolicy = ToolApprovalPolicy::Never;  // 工具确认策略，默认无需确认
    ToolConcurrency concurrency = ToolConcurrency::Serialized;      // 跨执行器的并发策略，默认串行保护
};
}  // AiLib 命名空间结束
