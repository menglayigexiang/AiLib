#pragma once
#include <AiLib/tools/ToolRegistry.h>
#include <AiLib/tools/IToolApprovalProvider.h>
namespace AiLib {
// 同步执行单次工具调用；工具业务失败返回 true，取消和总截止时间返回 false。
class AILIB_EXPORT ToolExecutor {
public:
    explicit ToolExecutor(
        ToolRegistry& registry,                              // 使用应用拥有的注册表
        IToolApprovalProvider* approvalProvider = nullptr);  // 使用应用拥有的注册表和可空确认策略
    bool
    execute(const ToolCall& call,                 // 规范化后的工具调用
            const ToolExecutionContext& context,  // 来源、取消和总截止时间
            ToolResult& result,                   // 输出带关联信息的业务结果
            SdkError& error) const;               // false 只表达停止信号或非法核心调用，Agent 后续决定运行状态
    bool
    execute(const ToolCall& call,                 // 规范化后的调用
            const ToolExecutionContext& context,  // 来源和停止上下文
            ToolResult& result,                   // 输出实际业务结果
            SdkError& error,                      // 输出停止或故障信息
            bool& handlerExecuted) const;         // 额外报告 Handler 是否已进入，便于保留取消时的实际结果
private:
    ToolRegistry& m_registry;                   // 非拥有的应用注册表引用
    IToolApprovalProvider* m_approvalProvider;  // 非拥有且可空的同步确认策略
};
}
