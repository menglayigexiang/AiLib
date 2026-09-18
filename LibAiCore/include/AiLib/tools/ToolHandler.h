#pragma once
#include <AiLib/core/Error.h>
#include <AiLib/tools/ToolCall.h>
#include <AiLib/tools/ToolResult.h>
#include <AiLib/tools/ToolExecutionContext.h>
#include <functional>
namespace AiLib {
using ToolHandler =
    std::function<bool(const ToolCall& call,                 // 规范化后的工具调用
                       const ToolExecutionContext& context,  // 来源、取消及截止时间
                       ToolResult& result,                   // 输出业务结果，关联字段由 Executor 保证
                       SdkError& error)>;                    // 同步业务函数；false 时错误转换为失败 ToolResult
}
