#pragma once
#include <AiLib/core/Error.h>
#include <AiLib/tools/FunctionToolDefinition.h>
#include <AiLib/tools/ToolCall.h>
#include <AiLib/tools/ToolExecutionContext.h>
namespace AiLib {
enum class ToolApprovalDecision { Allow, Deny, Cancel };
// 保存一次同步确认的决定及说明。
struct ToolApprovalResult {
    ToolApprovalDecision decision = ToolApprovalDecision::Deny;  // 默认拒绝，不能隐式批准
    QString reason;                                              // 用户决定或确认机制的说明
};
// 应用实现的同步确认策略；等待时应协作检查 Context 的取消和截止时间。
class IToolApprovalProvider {
public:
    virtual ~IToolApprovalProvider() = default;                             // 允许通过接口安全销毁应用实现
    virtual bool requestApproval(const ToolCall& call,                      // 待确认的调用
                                 const FunctionToolDefinition& definition,  // 工具声明
                                 const ToolExecutionContext& context,       // 来源及停止上下文
                                 ToolApprovalResult& result,                // 输出用户决定
                                 SdkError& error) = 0;                      // 返回确认机制是否成功，区别于是否允许
};
}
