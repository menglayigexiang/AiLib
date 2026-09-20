#include "BuiltinCatalog.h"

namespace Demo {
namespace {
AiLib::ModelInfo model(const QString& id, const QString& name)  // 构造仅含稳定身份的目录模型
{
    AiLib::ModelInfo value;  // 待返回的模型目录值
    value.id = id;
    value.displayName = name;
    return value;
}
}  // 内置目录构造辅助函数命名空间结束

QList<AiLib::ProviderEntry> builtinProviders()  // 构造 DeepSeek、Kimi 与 OpenAI 条目
{
    AiLib::ProviderEntry deepseek;  // DeepSeek 无凭据连接模板
    deepseek.configTemplate.id = QStringLiteral("deepseek");
    deepseek.configTemplate.name = QStringLiteral("DeepSeek");
    deepseek.configTemplate.baseUrl = QUrl(QStringLiteral("https://api.deepseek.com"));
    deepseek.configTemplate.protocol = AiLib::ProtocolType::OpenAIChatCompletions;
    deepseek.models.append(model(QStringLiteral("deepseek-flash"), QStringLiteral("DeepSeek Flash")));

    AiLib::ProviderEntry kimi;  // Kimi Code 无凭据连接模板
    kimi.configTemplate.id = QStringLiteral("kimi");
    kimi.configTemplate.name = QStringLiteral("Kimi");
    kimi.configTemplate.baseUrl = QUrl(QStringLiteral("https://api.kimi.com/coding/"));
    kimi.configTemplate.protocol = AiLib::ProtocolType::AnthropicMessages;
    kimi.configTemplate.customHeaders.insert(QStringLiteral("User-Agent"), QStringLiteral("AiLib/0.1.0 (SDK example)"));
    kimi.models.append(model(QStringLiteral("kimi-for-coding"), QStringLiteral("Kimi for Coding")));

    AiLib::ProviderEntry openai;  // OpenAI Responses 无凭据连接模板
    openai.configTemplate.id = QStringLiteral("openai");
    openai.configTemplate.name = QStringLiteral("OpenAI");
    openai.configTemplate.baseUrl = QUrl(QStringLiteral("https://api.openai.com/v1"));
    openai.configTemplate.protocol = AiLib::ProtocolType::OpenAIResponses;
    openai.models = {
        model(QStringLiteral("gpt-5.6-sol"), QStringLiteral("GPT-5.6 Sol")),
        model(QStringLiteral("gpt-5.6-terra"), QStringLiteral("GPT-5.6 Terra")),
        model(QStringLiteral("gpt-5.6-luna"), QStringLiteral("GPT-5.6 Luna"))};
    return {deepseek, kimi, openai};
}

bool registerBuiltinModels(AiLib::ModelRegistry& registry,  // 待填充的模型目录
                           AiLib::SdkError& error)          // 返回首个注册错误
{
    for (const AiLib::ProviderEntry& entry : builtinProviders()) {  // 当前内置 Provider
        if (!registry.registerProvider(entry, error))
            return false;
    }
    return true;
}

QString demoApiKeyFromEnv(const QString& providerId)  // 读取 Provider 对应的环境变量
{
    if (providerId == QStringLiteral("deepseek"))
        return QString::fromUtf8(qgetenv("DEEPSEEK_API_KEY"));
    if (providerId == QStringLiteral("kimi"))
        return QString::fromUtf8(qgetenv("KIMI_CODE_API_KEY"));
    if (providerId == QStringLiteral("openai"))
        return QString::fromUtf8(qgetenv("OPENAI_API_KEY"));
    return {};
}

}  // Demo 命名空间结束
