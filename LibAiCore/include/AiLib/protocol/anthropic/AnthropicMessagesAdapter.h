#pragma once

#include <AiLib/protocol/IProtocolAdapter.h>

namespace AiLib {
// 将 Anthropic Messages 协议转换为 canonical 模型，不执行网络或工具。
class AILIB_EXPORT AnthropicMessagesAdapter final : public IProtocolAdapter {
public:
    bool encodeChatRequest(
        const ProviderConfig& provider,   // 当前服务的地址及认证配置
        const ChatRequest& request,       // 有序 canonical 请求
        TransportRequest& output,         // 成功后输出 HTTP 请求
        SdkError& error) const override;  // 校验并编码 Messages 请求
    bool decodeChatResponse(
        const TransportResponse& input,   // 完整 HTTP 响应
        ChatResponse& output,             // 输出助手响应，失败保留有效数据
        SdkError& error) const override;  // 解码内容块、工具调用及 Provider 错误
    std::unique_ptr<IStreamDecoder> createStreamDecoder(SdkError& error) const override;  // 创建当前请求独立的 SSE 协议解析器
};
}  // AiLib 命名空间结束
