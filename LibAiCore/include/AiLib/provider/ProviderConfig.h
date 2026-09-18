#pragma once

#include <QString>
#include <QUrl>
#include <QHash>

namespace AiLib {
enum class ProtocolType {
    OpenAIChatCompletions, OpenAIResponses, AnthropicMessages,
    GeminiGenerateContent, GeminiInteractions, Custom
};

struct ProviderConfig {
    QString id;                                                   // 服务配置的逻辑标识
    QString name;                                                 // 服务的展示名称
    QUrl baseUrl;                                                 // API 根地址，后续追加端点并保留路径前缀
    QString apiKey;                                               // 默认认证密钥，允许为空
    ProtocolType protocol = ProtocolType::OpenAIChatCompletions;  // 显式指定的请求协议，不根据 URL 猜测
    QHash<QString, QString> customHeaders;                        // 自定义请求头，可覆盖默认认证但不可覆盖协议保护头
};
}  // AiLib 命名空间结束
