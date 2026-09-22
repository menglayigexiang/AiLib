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

/// 初始化阶段获取的 Server 标识。
struct McpServerInfo
{
    QString name;                         ///< Server 程序名称。
    QString version;                      ///< Server 程序版本。
    QString title;                        ///< 可选显示标题。
};

/// McpClientManager 中的一项持久化 Client 配置。
struct McpClientConfig
{
    QString id;                           ///< Manager 内唯一 ID。
    QString name;                         ///< 用户可读名称。
    bool enabled = true;                  ///< 是否允许启动此 Client。
    QString transportType;                ///< Transport 类型标识。
    QJsonObject transportConfig;          ///< Transport 专属配置。
    QJsonObject extensions;               ///< 应用自定义扩展字段。
};

} // namespace LibMcp
