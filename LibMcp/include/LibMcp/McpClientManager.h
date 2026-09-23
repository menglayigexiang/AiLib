#pragma once

#include <LibMcp/McpClient.h>

#include <QJsonObject>

namespace LibMcp {

/// 管理一组 Client 配置和按需创建的运行实例。
class LIBMCP_EXPORT McpClientManager final : public QObject
{
    Q_OBJECT
public:
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
    /// 按配置创建并启动指定 Client。
    QSharedPointer<McpOperation<void>> startClient(const QString &id);  // 按配置创建并启动 Client
    /// 停止指定 Client；实例尚未创建时返回错误结果。
    QSharedPointer<McpOperation<void>> stopClient(const QString &id);   // 停止指定 Client 实例

signals:
    /// 配置集合成功改变后发出。
    void configsChanged();                                      // 通知持久化配置已改变

private:
    // 隐藏配置集合和运行实例的内部存储。
    class Private;
    std::unique_ptr<Private> d; ///< 配置项及其可选运行实例。
};

} // namespace LibMcp
