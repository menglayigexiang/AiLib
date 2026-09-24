#pragma once

#include <LibMcp/McpClient.h>

#include <QJsonObject>

namespace LibMcp {

/// 管理一组 Client 配置和按需创建的运行实例。
class LIBMCP_EXPORT McpClientManager final : public QObject
{
    Q_OBJECT
public:
    // 表示持久化 Client 从未启用到协议能力可用的运行阶段。
    enum class ClientState
    {
        Disabled,    // 用户未启用 Client
        Starting,    // 正在启动底层 Transport
        Discovering, // 正在验证 MCP 2026-07-28
        LoadingCapabilities, // 正在加载并校验 Server 声明的基础能力
        Ready,       // 协议与声明能力已经验证，可以对外使用
        Error        // 启用流程失败，错误详情由 clientError 返回
    };
    Q_ENUM(ClientState)

    // 表示底层 Transport 从未启动到已收到远端响应的可观测状态。
    enum class TransportState
    {
        Stopped,   // Transport 未启动或已经停止
        Starting,  // 正在启动本地进程或准备 HTTP Transport
        Active,    // Transport 已启动，但尚未证明远端 Endpoint 可达
        Reachable, // 已收到可归因于当前 Endpoint 的响应
        Error      // Transport 启动或通信失败
    };
    Q_ENUM(TransportState)

    // 表示固定 MCP 2026-07-28 协议的独立验证结果。
    enum class ProtocolState
    {
        NotChecked,   // 尚未取得足以判断协议的响应
        Checking,     // 正在执行 server/discover
        Compatible,   // Server 明确支持 MCP 2026-07-28
        Incompatible, // Server 明确不支持固定协议版本
        Invalid       // Endpoint 有响应，但响应不是有效的当前 MCP 协议
    };
    Q_ENUM(ProtocolState)

    explicit McpClientManager(QObject *parent = nullptr);  // 创建 Client 配置与运行实例管理器
    ~McpClientManager() override;                          // 停止并释放所有 Client 实例

    /// 将全部持久化配置序列化为带版本的 JSON 对象。
    QJsonObject serialize() const;                        // 序列化全部持久化配置
    /// 事务式加载配置；失败时保留原有集合。
    McpResult<void> deserialize(const QJsonObject &json);  // 事务式加载完整配置集合

    /// 添加一项配置；ID 为空或重复时返回 false。
    bool addConfig(const McpClientConfig &config);         // 添加唯一 Client 配置
    /// 替换一项未连接配置；ID 不存在或 Client 正在运行时返回 false。
    bool updateConfig(
        const QString& originalId,             // 编辑前的稳定配置 ID
        const McpClientConfig& configuration); // 完整替换的新配置
    /// 删除一项未使用的配置；ID 不存在时返回 false。
    bool removeConfig(const QString &id);                  // 删除指定未运行配置
    /// 返回当前全部配置的副本。
    QList<McpClientConfig> configs() const;                // 返回当前配置快照
    /// 返回 Manager 拥有的运行实例；尚未创建时返回 nullptr。
    McpClient *client(const QString &id) const;             // 查找 Manager 拥有的 Client 实例
    ClientState clientState(const QString& id) const;       // 返回 Client 当前启用与验证阶段
    TransportState clientTransportState(const QString& id) const;  // 返回独立 Transport 可达状态
    ProtocolState clientProtocolState(const QString& id) const;    // 返回固定 MCP 协议验证状态
    bool clientEnabled(const QString& id) const;            // 返回用户是否希望启用该 Client
    McpError clientError(const QString& id) const;          // 返回最近一次启用失败的详细错误
    McpDiscoveryResult clientDiscovery(const QString& id) const;  // 返回最近成功的能力发现快照
    QList<McpTool> clientTools(const QString& id) const;     // 返回 Ready Client 已校验的工具快照
    QList<McpResource> clientResources(const QString& id) const;  // 返回 Ready Client 的固定资源快照
    QList<McpResourceTemplate> clientResourceTemplates(const QString& id) const;  // 返回 Ready Client 的资源模板快照
    QList<McpPrompt> clientPrompts(const QString& id) const;  // 返回 Ready Client 的 Prompt 快照
    /// 启动 Transport，验证固定协议版本并加载工具；全部成功后才进入 Ready。
    QSharedPointer<McpOperation<void>> startClient(const QString &id);  // 完成完整 Client 启用流程
    /// 停止指定 Client，并清除协议能力和工具缓存。
    QSharedPointer<McpOperation<void>> stopClient(const QString &id);   // 禁用并停止指定 Client

signals:
    /// 配置集合成功改变后发出。
    void configsChanged();                                      // 通知持久化配置已改变
    void clientStateChanged(
        const QString& id,             // 状态发生变化的 Client ID
        ClientState state);            // Client 最新运行阶段
    void clientTransportStateChanged(
        const QString& id,              // Transport 状态发生变化的 Client ID
        TransportState state);          // Client 最新 Transport 状态
    void clientProtocolStateChanged(
        const QString& id,              // 协议状态发生变化的 Client ID
        ProtocolState state);           // Client 最新协议验证状态
    void clientToolsChanged(
        const QString& id);            // 通知指定 Client 的工具快照已改变
    void clientCapabilitiesChanged(
        const QString& id);            // 通知发现结果或基础能力快照已改变

private:
    // 隐藏配置集合和运行实例的内部存储。
    class Private;
    std::unique_ptr<Private> d; ///< 配置项及其可选运行实例。
};

} // namespace LibMcp
