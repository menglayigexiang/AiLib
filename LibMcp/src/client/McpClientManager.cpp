#include <LibMcp/McpClientManager.h>
#include <LibMcp/StreamableHttpTransport.h>

#include "../core/FutureUtils_p.h"

#include <QHash>
#include <QJsonArray>
#include <QUrl>

namespace LibMcp {

class McpClientManager::Private
{
public:
    struct Entry
    {
        McpClientConfig config;
        std::unique_ptr<McpClient> client;
    };

    QHash<QString, std::shared_ptr<Entry>> entries;
};

McpClientManager::McpClientManager(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

McpClientManager::~McpClientManager() = default;

QJsonObject McpClientManager::serialize() const
{
    QJsonArray clients;
    QStringList ids = d->entries.keys();
    ids.sort();
    for (const QString &id : ids) {
        const McpClientConfig &config = d->entries.value(id)->config;
        clients.append(
            QJsonObject{{QStringLiteral("id"), config.id},
                        {QStringLiteral("name"), config.name},
                        {QStringLiteral("enabled"), config.enabled},
                        {QStringLiteral("transport"),
                         QJsonObject{
                             {QStringLiteral("type"), config.transportType},
                             {QStringLiteral("config"),
                              config.transportConfig}}},
                        {QStringLiteral("extensions"), config.extensions}});
    }
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("clients"), clients}};
}

McpResult<void> McpClientManager::deserialize(const QJsonObject &json)
{
    for (const auto &entry : d->entries) {
        if (entry->client && entry->client->isRunning()) {
            return McpResult<void>::failure(
                {McpErrorCode::InvalidState,
                 QStringLiteral("存在运行中的 Client，不能替换配置")});
        }
    }

    if (json.value(QStringLiteral("version")).toInt() != 1
        || !json.value(QStringLiteral("clients")).isArray()) {
        return McpResult<void>::failure(
            {McpErrorCode::InvalidMessage,
             QStringLiteral("不支持的 Client 配置格式")});
    }

    QHash<QString, std::shared_ptr<Private::Entry>> replacement;
    for (const QJsonValue &value :
         json.value(QStringLiteral("clients")).toArray()) {
        if (!value.isObject()) {
            return McpResult<void>::failure(
                {McpErrorCode::InvalidMessage,
                 QStringLiteral("Client 配置项必须是对象")});
        }
        const QJsonObject object = value.toObject();
        const QJsonObject transport =
            object.value(QStringLiteral("transport")).toObject();

        McpClientConfig config;
        config.id = object.value(QStringLiteral("id")).toString();
        config.name = object.value(QStringLiteral("name")).toString();
        config.enabled =
            object.value(QStringLiteral("enabled")).toBool(true);
        config.transportType =
            transport.value(QStringLiteral("type")).toString();
        config.transportConfig =
            transport.value(QStringLiteral("config")).toObject();
        config.extensions =
            object.value(QStringLiteral("extensions")).toObject();

        if (config.id.isEmpty() || replacement.contains(config.id)) {
            return McpResult<void>::failure(
                {McpErrorCode::InvalidMessage,
                 QStringLiteral("Client ID 为空或重复")});
        }
        auto entry = std::make_shared<Private::Entry>();
        entry->config = std::move(config);
        replacement.insert(entry->config.id, entry);
    }

    d->entries = std::move(replacement);
    emit configsChanged();
    return McpResult<void>::success();
}

bool McpClientManager::addConfig(const McpClientConfig &config)
{
    if (config.id.isEmpty() || d->entries.contains(config.id)) {
        return false;
    }
    auto entry = std::make_shared<Private::Entry>();
    entry->config = config;
    d->entries.insert(config.id, entry);
    emit configsChanged();
    return true;
}

bool McpClientManager::removeConfig(const QString &id)
{
    const bool removed = d->entries.remove(id) > 0;
    if (removed) {
        emit configsChanged();
    }
    return removed;
}

QList<McpClientConfig> McpClientManager::configs() const
{
    QList<McpClientConfig> result;
    QStringList ids = d->entries.keys();
    ids.sort();
    for (const QString &id : ids) {
        result.append(d->entries.value(id)->config);
    }
    return result;
}

McpClient *McpClientManager::client(const QString &id) const
{
    const auto entry = d->entries.value(id);
    return entry ? entry->client.get() : nullptr;
}

QFuture<McpResult<void>> McpClientManager::startClient(const QString &id)
{
    const auto entry = d->entries.value(id);
    if (!entry) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::InvalidState,
             QStringLiteral("Client 配置不存在")}));
    }
    if (!entry->config.enabled) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::InvalidState,
             QStringLiteral("Client 配置已禁用")}));
    }
    if (entry->config.transportType != QStringLiteral("streamable-http")) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::InvalidState,
             QStringLiteral("不支持的 Transport 类型: %1")
                 .arg(entry->config.transportType)}));
    }
    if (!entry->client) {
        const QUrl endpoint(
            entry->config.transportConfig.value(QStringLiteral("url"))
                .toString());
        const QJsonObject headers =
            entry->config.transportConfig.value(QStringLiteral("headers"))
                .toObject();
        entry->client = std::make_unique<McpClient>(
            std::make_unique<StreamableHttpClientTransport>(endpoint,
                                                            headers));
    }
    return entry->client->start();
}

QFuture<McpResult<void>> McpClientManager::stopClient(const QString &id)
{
    McpClient *managedClient = client(id);
    if (!managedClient) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::InvalidState,
             QStringLiteral("Client 尚未启动")}));
    }
    return managedClient->stop();
}

} // namespace LibMcp
