#pragma once

#include <AiLib/Export.h>
#include <AiLib/core/Error.h>
#include <AiLib/core/ModelInfo.h>
#include <AiLib/provider/ProviderConfig.h>
#include <QList>
#include <QReadWriteLock>
#include <optional>

namespace AiLib {

// 描述一个 Provider 的无凭据连接模板及其已知模型。
struct ProviderEntry {
    ProviderConfig configTemplate;  // Provider 固有的无凭据连接配置
    QList<ModelInfo> models;        // 当前目录收录的模型
};

// 保存线程安全的 Provider 与模型目录，不限制未收录模型的调用。
class AILIB_EXPORT ModelRegistry {
public:
    ModelRegistry() = default;                                      // 创建空的独立模型目录
    static ModelRegistry& instance();                               // 获取进程级便利目录
    bool registerProvider(const ProviderEntry& entry,               // 待原子注册的完整 Provider 条目
                          SdkError& error);                          // 返回定义或重复错误
    QList<ProviderEntry> providers() const;                         // 获取当前目录的值快照
    std::optional<ProviderEntry> findProvider(
        const QString& providerId) const;                            // 按稳定 Provider ID 查询值副本
    std::optional<ModelInfo> findModel(
        const QString& providerId,                                  // 模型所属 Provider ID
        const QString& modelId) const;                              // 按 Provider 和模型 ID 查询值副本

private:
    mutable QReadWriteLock m_lock;  // 保护运行期注册和并发查询
    QList<ProviderEntry> m_entries; // 已完成校验的 Provider 条目
};

}  // AiLib 命名空间结束
