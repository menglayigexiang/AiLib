#pragma once

#include <AiLib/protocol/IProtocolAdapter.h>

namespace AiLib {

// 在 canonical Chat 类型与 OpenAI Responses API 之间转换。
class AILIB_EXPORT OpenAIResponsesAdapter final : public IProtocolAdapter {
public:
    bool encodeChatRequest(const ProviderConfig& provider,  // OpenAI 或兼容服务配置
                           const ChatRequest& request,      // canonical 模型请求
                           TransportRequest& output,        // 输出 Responses HTTP 请求
                           SdkError& error) const override; // 校验并编码完整请求
    bool decodeChatResponse(const TransportResponse& input,  // Responses HTTP 响应
                            ChatResponse& output,             // 输出 canonical 助手响应
                            SdkError& error) const override;  // 严格解析文本、工具和用量
    std::unique_ptr<IStreamDecoder>
    createStreamDecoder(SdkError& error) const override;     // 创建独立的 Responses SSE 解析器
};

}  // AiLib 命名空间结束
