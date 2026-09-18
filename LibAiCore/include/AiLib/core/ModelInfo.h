#pragma once

#include <QString>
#include <QHash>
#include <optional>

namespace AiLib {
enum class Capability {
    TextInput, TextOutput, ImageInput, ImageOutput, AudioInput, AudioOutput,
    VideoInput, VideoOutput, FileInput, Streaming, ToolCalling,
    StructuredOutput, Reasoning, Embedding
};

// 为能力枚举提供 ADL 哈希重载，兼容 Qt 5 与 Qt 6 的种子类型。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline size_t qHash(Capability value, size_t seed = 0) noexcept  // 根据能力枚举值和哈希种子生成 Qt 容器键哈希
#else
inline uint qHash(Capability value, uint seed = 0) noexcept  // 根据能力枚举值和哈希种子生成 Qt 容器键哈希
#endif
{
    return ::qHash(static_cast<int>(value), seed);
}

struct ModelInfo {
    QString id;           // 模型标识，不通过名称推断能力
    QString displayName;  // 供界面展示的模型名称
    // 缺失键和 nullopt 都表示未知；能力提示不阻止请求发送。
    QHash<Capability, std::optional<bool>> capabilities;  // 模型能力提示，缺失或 nullopt 为未知，不作强制准入
    std::optional<qint64> contextWindow;                  // 上下文 Token 上限，未知用 nullopt
    std::optional<qint64> maxOutputTokens;                // 输出 Token 上限，未知用 nullopt
};
}  // AiLib 命名空间结束
