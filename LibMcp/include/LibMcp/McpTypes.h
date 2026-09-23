#pragma once

#include <LibMcp/LibMcpGlobal.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>
#include <variant>

namespace LibMcp {

/// MCP 消息参与者角色。
enum class McpRole { User, Assistant };

/// Server 对外公布的工具描述。
struct McpTool
{
    QString name;                         ///< 协议内唯一的工具名称。
    std::optional<QString> title;         ///< 可选的人类可读标题。
    std::optional<QString> description;   ///< 可选的工具用途说明。
    QJsonObject inputSchema;              ///< 工具输入 JSON Schema。
    QJsonObject outputSchema;             ///< 可选的工具输出 JSON Schema。
    QJsonObject annotations;              ///< MCP 工具注解。
    QJsonObject meta;                     ///< 未建模的 `_meta` 扩展字段。
};

// 表示一次工具调用的完整业务结果，协议错误不使用此类型表达。
struct McpToolCallResult
{
    QJsonArray content;                 // 面向模型的非结构化内容块
    QJsonValue structuredContent;       // 可选结构化结果，Undefined 表示未提供
    bool isError = false;               // 工具业务本身是否执行失败
    QJsonObject inputRequests;          // input_required 时需要 Client 完成的输入请求映射
    QString requestState;               // 无状态重试时由 Client 原样回传的不透明状态
};

// 描述一次工具调用及其可选 MRTR 重试输入。
struct McpToolCallRequest
{
    QString name;                 // 需要调用的工具名称
    QJsonObject arguments;        // 通过 inputSchema 校验的工具参数
    QJsonObject inputResponses;   // 对上一轮 inputRequests 的响应映射
    QString requestState;         // 上一轮返回的不透明重试状态
};

/// 一个可直接读取的 MCP Resource。
struct McpResource
{
    QString name;                         ///< 程序使用的资源名称。
    QString uri;                          ///< 保持原样的资源 URI。
    std::optional<QString> title;         ///< 可选显示标题。
    std::optional<QString> description;   ///< 可选资源说明。
    std::optional<QString> mimeType;      ///< 可选 MIME 类型。
    std::optional<qint64> size;           ///< 编码前的可选字节数。
    QJsonObject meta;                     ///< 未建模的 `_meta` 扩展字段。
};

/// 使用 RFC 6570 URI Template 描述的一组动态资源。
struct McpResourceTemplate
{
    QString name;                         ///< 程序使用的模板名称。
    QString uriTemplate;                  ///< 保持原样的 URI Template。
    std::optional<QString> title;         ///< 可选显示标题。
    std::optional<QString> description;   ///< 可选模板说明。
    std::optional<QString> mimeType;      ///< 模板结果的可选 MIME 类型。
    QJsonObject meta;                     ///< 未建模的 `_meta` 扩展字段。
};

/// 文本资源内容。
struct McpTextResourceContent
{
    QString uri;                          ///< 此内容对应的资源 URI。
    std::optional<QString> mimeType;      ///< 可选 MIME 类型。
    QString text;                         ///< UTF-16 文本内容。
    QJsonObject meta;                     ///< 内容级 `_meta` 扩展字段。
};

/// 二进制资源内容；Codec 负责与 Base64 wire 值转换。
struct McpBlobResourceContent
{
    QString uri;                          ///< 此内容对应的资源 URI。
    std::optional<QString> mimeType;      ///< 可选 MIME 类型。
    QByteArray data;                      ///< 已解码的二进制内容。
    QJsonObject meta;                     ///< 内容级 `_meta` 扩展字段。
};

using McpResourceContent = std::variant<McpTextResourceContent, McpBlobResourceContent>;

/// Prompt 对外声明的一个参数。
struct McpPromptArgument
{
    QString name;                         ///< 参数名称。
    std::optional<QString> description;   ///< 可选参数说明。
    bool required = false;                ///< 参数是否必须提供。
};

/// Server 对外公布的 Prompt 描述。
struct McpPrompt
{
    QString name;                         ///< 协议内唯一的 Prompt 名称。
    std::optional<QString> title;         ///< 可选显示标题。
    std::optional<QString> description;   ///< 可选用途说明。
    QList<McpPromptArgument> arguments;   ///< Prompt 参数声明。
    QJsonObject meta;                     ///< 未建模的 `_meta` 扩展字段。
};

/// Prompt 展开后生成的一条消息。
struct McpPromptMessage
{
    McpRole role = McpRole::User;         ///< 消息角色。
    QJsonObject content;                  ///< MCP 内容块。
};

/// 补全请求引用的 MCP 对象种类。
enum class McpCompletionReferenceType
{
    Prompt,            // 引用 Prompt 名称
    ResourceTemplate   // 引用 Resource URI Template
};

// 描述一次 Prompt 参数或资源模板变量补全请求。
struct McpCompletionRequest
{
    McpCompletionReferenceType referenceType =  // 被补全对象的种类
        McpCompletionReferenceType::Prompt;
    QString reference;       // Prompt 名称或资源 URI Template
    QString argumentName;    // 当前需要补全的参数名称
    QString argumentValue;   // 调用方已经输入的前缀
    QJsonObject context;     // 已解析的其他参数上下文
};

// 保存一次补全请求返回的候选值及分页提示。
struct McpCompletionResult
{
    QStringList values;           // 候选值，协议限制最多一百项
    std::optional<int> total;     // 可选的全部候选数量
    std::optional<bool> hasMore;  // 是否仍有未返回的候选值
};

/// 从结果元数据或能力发现中获取的 Server 标识。
struct McpServerInfo
{
    QString name;                         ///< Server 程序名称。
    QString version;                      ///< Server 程序版本。
    QString title;                        ///< 可选显示标题。
};

// 保存 server/discover 返回的版本、能力和可选使用说明。
struct McpDiscoveryResult
{
    QStringList supportedVersions;  // Server 声明支持的协议版本
    QJsonObject capabilities;       // Server 当前能力对象
    McpServerInfo serverInfo;       // 结果元数据携带的 Server 身份
    QString instructions;           // 可选的自然语言使用说明
};

// 描述 Client 在长寿命订阅流中显式选择的通知类型。
struct McpSubscriptionFilter
{
    bool toolsListChanged = false;      // 是否接收工具列表变更通知
    bool resourcesListChanged = false;  // 是否接收资源列表变更通知
    bool promptsListChanged = false;    // 是否接收 Prompt 列表变更通知
    QStringList resourceSubscriptions;  // 需要监听内容变更的资源 URI
};

/// McpClientManager 中的一项持久化 Client 配置。
struct McpClientConfig
{
    QString id;                           ///< Manager 内唯一 ID。
    QString name;                         ///< 用户可读名称。
    QString transportType;                ///< Transport 类型标识。
    QJsonObject transportConfig;          ///< Transport 专属配置。
    QJsonObject extensions;               ///< 应用自定义扩展字段。
};

} // namespace LibMcp
