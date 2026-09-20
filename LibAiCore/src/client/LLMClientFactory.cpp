#include <AiLib/client/LLMClientFactory.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <AiLib/protocol/openai/OpenAIResponsesAdapter.h>
#include <AiLib/network/QtHttpTransport.h>
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>

namespace AiLib {
bool LLMClientFactory::supportsProtocol(ProtocolType protocol)  // 返回协议到内置 Adapter 的实际支持状态
{
    switch (protocol) {
    case ProtocolType::OpenAIChatCompletions:
    case ProtocolType::OpenAIResponses:
    case ProtocolType::AnthropicMessages:
        return true;
    default:
        return false;
    }
}

bool LLMClientFactory::create(
    const ProviderConfig& provider,      // 明确指定协议的服务配置
    std::unique_ptr<LLMClient>& output,  // 成功后接收 Client，失败保持原对象
    SdkError& error,                     // 工厂配置错误输出
    const RequestOptions& defaults)      // 创建后使用的默认选项
{
    error = {};
    std::unique_ptr<IProtocolAdapter> adapter;  // 按显式协议选择并移交所有权的实现
    if (provider.protocol == ProtocolType::OpenAIChatCompletions) {
        adapter = std::make_unique<OpenAIChatCompatibleAdapter>();
    } else if (provider.protocol == ProtocolType::OpenAIResponses) {
        adapter = std::make_unique<OpenAIResponsesAdapter>();
    } else if (provider.protocol == ProtocolType::AnthropicMessages) {
        adapter = std::make_unique<AnthropicMessagesAdapter>();
    } else {
        error.category = ErrorCategory::Unsupported;
        error.code = QStringLiteral("UnsupportedProtocol");
        error.message = QStringLiteral("Requested built-in protocol is not implemented");
        return false;
    }
    output = std::make_unique<LLMClient>(provider,
        std::move(adapter),
        std::make_unique<QtHttpTransport>(), defaults);
    return true;
}
}  // AiLib 命名空间结束
