#pragma once

#include <AiLib/core/Message.h>
#include <AiLib/tools/FunctionToolDefinition.h>
#include <AiLib/tools/ToolChoice.h>
#include <optional>

namespace AiLib {
struct ChatRequest {
    QString model;                        // 本次请求的模型标识
    QList<Message> messages;              // 输入消息历史，保持调用方提供的顺序
    std::optional<double> temperature;    // 采样温度，未设置时不发送
    std::optional<double> topP;           // 概率采样参数，未设置时不发送
    std::optional<int> maxOutputTokens;   // 输出 Token 上限，未设置时不发送
    QList<FunctionToolDefinition> tools;  // 可提供给模型的 Function Tool 定义
    ToolChoice toolChoice;                // 本次请求的工具选择方式
    bool stream = false;                  // 是否开启流式请求，不由 Callback 决定
    // 扩展参数不能包含 Adapter 保留字段，即使标准参数尚未设置。
    QJsonObject extraParameters;  // 厂商扩展 JSON 参数，不允许出现 Adapter 保留字段
};
}  // AiLib 命名空间结束
