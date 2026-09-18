#include <AiLib/tools/ToolRegistry.h>
#include "SchemaValidator.h"
namespace AiLib {
bool ToolRegistry::registerTool(const FunctionToolDefinition& definition,  // 工具纯描述
                                ToolHandler handler,                       // 同步业务执行函数
                                SdkError& error)                           // 验证后注册，失败不改原工具集合
{
    error = {};
    if (definition.name.trimmed().isEmpty() || !handler || m_entries.contains(definition.name)) {
        error.category = ErrorCategory::Configuration;
        error.code = QStringLiteral("InvalidToolRegistration");
        error.message = QStringLiteral("工具名称不能为空、重复，Handler 不能为空");
        return false;
    }
    if ((definition.approvalPolicy != ToolApprovalPolicy::Never &&
         definition.approvalPolicy != ToolApprovalPolicy::Always) ||
        (definition.concurrency != ToolConcurrency::Serialized &&
         definition.concurrency != ToolConcurrency::Concurrent)) {
        error.category = ErrorCategory::Configuration;
        error.code = QStringLiteral("InvalidToolPolicy");
        error.message = QStringLiteral("工具确认或并发策略非法");
        return false;
    }
    if (!validateToolSchema(definition.inputSchema, error))
        return false;
    const auto entry = std::make_shared<Entry>();  // 供所有执行器共用的工具条目
    entry->definition = definition;
    entry->handler = std::move(handler);
    m_entries.insert(definition.name, entry);
    return true;
}
bool ToolRegistry::removeTool(const QString& name)  // 删除注册项，调用方同步修改
{
    return m_entries.remove(name) != 0;
}
bool ToolRegistry::contains(const QString& name) const  // 只读查询名称
{
    return m_entries.contains(name);
}
bool ToolRegistry::definition(
    const QString& name, FunctionToolDefinition& output) const  // 返回描述副本而不暴露运行时条目
{
    const auto it = m_entries.constFind(name);  // 查找结果
    if (it == m_entries.cend())
        return false;
    output = it.value()->definition;
    return true;
}
}
