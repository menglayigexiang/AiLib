#include <AiLib/provider/ModelRegistry.h>
#include <AiLib/client/LLMClientFactory.h>
#include <QSet>

namespace AiLib {
namespace {
bool fail(SdkError& error,                         // 目录注册错误输出
          const QString& code,                     // 稳定错误码
          const QString& message)                  // 保存配置错误并返回 false
{
    error = {};
    error.category = ErrorCategory::Configuration;
    error.code = code;
    error.message = message;
    return false;
}
}  // 内部目录校验辅助函数命名空间结束

ModelRegistry& ModelRegistry::instance()  // 获取线程安全的进程级模型目录
{
    static ModelRegistry registry;  // 首次使用时线程安全初始化的目录
    return registry;
}

bool ModelRegistry::registerProvider(const ProviderEntry& entry,  // 待注册的 Provider 完整值
                                     SdkError& error)              // 注册失败的结构化原因
{
    error = {};
    const ProviderConfig& config = entry.configTemplate;  // 无凭据连接模板
    if (config.id.trimmed().isEmpty())
        return fail(error, QStringLiteral("InvalidProviderId"), QStringLiteral("Provider ID is required"));
    if (config.name.trimmed().isEmpty())
        return fail(error, QStringLiteral("InvalidProviderName"), QStringLiteral("Provider name is required"));
    if (!config.apiKey.isEmpty())
        return fail(error, QStringLiteral("CredentialInTemplate"), QStringLiteral("Provider templates must not contain an API key"));
    if (!config.baseUrl.isValid() || config.baseUrl.host().isEmpty()
        || (config.baseUrl.scheme() != QStringLiteral("http") && config.baseUrl.scheme() != QStringLiteral("https"))
        || config.baseUrl.hasQuery() || config.baseUrl.hasFragment() || !config.baseUrl.userInfo().isEmpty())
        return fail(error, QStringLiteral("InvalidBaseUrl"), QStringLiteral("Provider endpoint must be a credential-free HTTP(S) URL"));
    if (!LLMClientFactory::supportsProtocol(config.protocol))
        return fail(error, QStringLiteral("UnsupportedProtocol"), QStringLiteral("Provider protocol is not implemented"));
    QSet<QString> modelIds;  // 当前 Provider 内已见的模型 ID
    for (const ModelInfo& model : entry.models) {  // 验证模型身份完整且无重复
        if (model.id.trimmed().isEmpty())
            return fail(error, QStringLiteral("InvalidModelId"), QStringLiteral("Model ID is required"));
        if (modelIds.contains(model.id))
            return fail(error, QStringLiteral("DuplicateModel"), QStringLiteral("Model ID is duplicated within the Provider"));
        modelIds.insert(model.id);
    }
    QWriteLocker locker(&m_lock);  // 将重复检查与写入放在同一临界区
    for (const ProviderEntry& current : m_entries) {  // 已注册条目只读检查
        if (current.configTemplate.id == config.id)
            return fail(error, QStringLiteral("DuplicateProvider"), QStringLiteral("Provider ID is already registered"));
    }
    m_entries.append(entry);
    return true;
}

QList<ProviderEntry> ModelRegistry::providers() const  // 返回不受后续注册影响的目录快照
{
    QReadLocker locker(&m_lock);  // 复制期间阻止容器写入
    return m_entries;
}

std::optional<ProviderEntry> ModelRegistry::findProvider(
    const QString& providerId) const  // 按稳定 ID 查询 Provider 值副本
{
    QReadLocker locker(&m_lock);  // 查询期间保护内部容器
    for (const ProviderEntry& entry : m_entries) {  // 当前目录条目
        if (entry.configTemplate.id == providerId)
            return entry;
    }
    return std::nullopt;
}

std::optional<ModelInfo> ModelRegistry::findModel(
    const QString& providerId,  // 模型所属 Provider
    const QString& modelId) const  // 待查询模型 ID
{
    QReadLocker locker(&m_lock);  // 查询期间保护 Provider 与模型列表
    for (const ProviderEntry& entry : m_entries) {  // 当前 Provider 条目
        if (entry.configTemplate.id != providerId)
            continue;
        for (const ModelInfo& model : entry.models) {  // 当前 Provider 的模型
            if (model.id == modelId)
                return model;
        }
        break;
    }
    return std::nullopt;
}

}  // AiLib 命名空间结束
