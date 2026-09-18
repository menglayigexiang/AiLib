#pragma once

#include <AiLib/protocol/IProtocolAdapter.h>

namespace AiLib {
// 编解码 OpenAI Chat Compatible 协议，流式请求使用独立解析器。
class AILIB_EXPORT OpenAIChatCompatibleAdapter final : public IProtocolAdapter {
public:
    bool encodeChatRequest(
        const ProviderConfig& provider,   // 服务地址、认证和自定义 Header
        const ChatRequest& request,       // 消息、函数工具及可选模型参数
        TransportRequest& output,         // 输出 chat/completions 请求
        SdkError& error) const override;  // 校验保留字段和协议支持范围后编码
    bool decodeChatResponse(
        const TransportResponse& input,   // 原始 HTTP 响应
        ChatResponse& output,             // 输出唯一助手响应和已报告用量
        SdkError& error) const override;  // 解析普通文字、函数调用及服务端故障
    std::unique_ptr<IStreamDecoder> createStreamDecoder(SdkError& error) const override;  // 创建当前请求独立的 SSE 协议解析器
};
}  // AiLib 命名空间结束
