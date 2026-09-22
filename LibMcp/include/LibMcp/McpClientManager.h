#pragma once

#include <LibMcp/McpClient.h>

#include <QJsonObject>

namespace LibMcp {

/// 管理一组 Client 配置和按需创建的运行实例。
class LIBMCP_EXPORT McpClientManager final : public QObject
{
    Q_OBJECT
public:
    explicit McpClientManager(QObject *parent = nullptr);
    ~McpClientManager() override;

    /// 将全部持久化配置序列化为带版本的 JSON 对象。
    QJsonObject serialize() const;
    /// 事务式加载配置；失败时保留原有集合。
    McpResult<void> deserialize(const QJsonObject &json);

    /// 添加一项配置；ID 为空或重复时返回 false。
    bool addConfig(const McpClientConfig &config);
    /// 删除一项未使用的配置；ID 不存在时返回 false。
    bool removeConfig(const QString &id);
    /// 返回当前全部配置的副本。
    QList<McpClientConfig> configs() const;
    /// 返回 Manager 拥有的运行实例；尚未创建时返回 nullptr。
    McpClient *client(const QString &id) const;
    /// 按配置创建并启动指定 Client。
    QFuture<McpResult<void>> startClient(const QString &id);
    /// 停止指定 Client；实例尚未创建时返回错误结果。
    QFuture<McpResult<void>> stopClient(const QString &id);

signals:
    /// 配置集合成功改变后发出。
    void configsChanged();

private:
    class Private;
    std::unique_ptr<Private> d; ///< 配置项及其可选运行实例。
};

} // namespace LibMcp
