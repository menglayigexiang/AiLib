#include <LibMcp/McpClientManager.h>
#include <LibMcp/StreamableHttpTransport.h>
#include <LibMcp/StdioTransport.h>

#include "../core/FutureUtils_p.h"
#include "../core/McpOperationAccess_p.h"

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

bool McpClientManager::updateConfig(
    const QString& originalId,             // 编辑前的稳定配置 ID
    const McpClientConfig& configuration)  // 完整替换的新配置
{
    const auto entry = d->entries.value(originalId);  // 查找需要替换的配置项
    if (!entry || (entry->client && entry->client->isRunning())
        || configuration.id.isEmpty()
        || (configuration.id != originalId
            && d->entries.contains(configuration.id))) {
        return false;
    }
    d->entries.remove(originalId);
    entry->config = configuration;
    entry->client.reset();
    d->entries.insert(configuration.id, entry);
    emit configsChanged();
    return true;
}

bool McpClientManager::removeConfig(const QString &id)
{
    const auto entry = d->entries.value(id);  // 查找并检查待删除配置的运行状态
    if (entry && entry->client && entry->client->isRunning()) {
        return false;
    }
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

QSharedPointer<McpOperation<void>> McpClientManager::startClient(const QString &id)
{
    const auto entry = d->entries.value(id);
    if (!entry) {
        auto operation = QSharedPointer<McpOperation<void>>::create();  // 保存立即失败的启动操作
        Internal::McpOperationAccess::fail(
            operation,
            {McpErrorCode::InvalidState, QStringLiteral("Client 配置不存在")});
        return operation;
    }
    if (entry->config.transportType != QStringLiteral("streamable-http")
        && entry->config.transportType != QStringLiteral("stdio")) {
        auto operation = QSharedPointer<McpOperation<void>>::create();  // 保存立即失败的不支持传输操作
        Internal::McpOperationAccess::fail(
            operation,
            {McpErrorCode::InvalidState,
             QStringLiteral("不支持的 Transport 类型: %1")
                 .arg(entry->config.transportType)});
        return operation;
    }
    if (!entry->client) {
        std::unique_ptr<McpClientTransport> transport;  // 保存按配置创建的具体传输
        if (entry->config.transportType == QStringLiteral("streamable-http")) {
            const QUrl endpoint(  // 读取 Streamable HTTP Endpoint
                entry->config.transportConfig.value(QStringLiteral("url"))
                    .toString());
            QJsonObject headers =  // 读取固定 HTTP Headers
                entry->config.transportConfig.value(QStringLiteral("headers"))
                    .toObject();
            const QString bearerEnvironment =  // 读取可选 Bearer Token 环境变量名
                entry->config.transportConfig
                    .value(QStringLiteral("bearerTokenEnvironment"))
                    .toString();
            if (!bearerEnvironment.isEmpty()) {
                const QByteArray token = qgetenv(bearerEnvironment.toUtf8().constData());  // 从当前进程环境解析 Token
                if (!token.isEmpty()) {
                    headers.insert(QStringLiteral("Authorization"),
                                   QStringLiteral("Bearer ")
                                       + QString::fromUtf8(token));
                }
            }
            const QJsonObject environmentHeaders =  // 读取 Header 到环境变量名的映射
                entry->config.transportConfig
                    .value(QStringLiteral("environmentHeaders"))
                    .toObject();
            for (auto iterator = environmentHeaders.constBegin();
                 iterator != environmentHeaders.constEnd();
                 ++iterator) {
                const QByteArray value =  // 解析当前 Header 对应的环境变量值
                    qgetenv(iterator.value().toString().toUtf8().constData());
                if (!value.isEmpty()) {
                    headers.insert(iterator.key(), QString::fromUtf8(value));
                }
            }
            transport = std::make_unique<StreamableHttpClientTransport>(
                endpoint,
                headers);
        } else {
            StdioClientConfig stdioConfig;  // 解码 STDIO 子进程启动配置
            stdioConfig.command =
                entry->config.transportConfig.value(QStringLiteral("command"))
                    .toString();
            stdioConfig.workingDirectory =
                entry->config.transportConfig
                    .value(QStringLiteral("workingDirectory"))
                    .toString();
            for (const QJsonValue& argument :
                 entry->config.transportConfig.value(QStringLiteral("arguments"))
                     .toArray()) {
                stdioConfig.arguments.append(argument.toString());
            }
            const QJsonObject environment =  // 读取显式环境变量键值
                entry->config.transportConfig.value(QStringLiteral("environment"))
                    .toObject();
            for (auto iterator = environment.constBegin();
                 iterator != environment.constEnd();
                 ++iterator) {
                stdioConfig.environment.insert(iterator.key(),
                                               iterator.value().toString());
            }
            for (const QJsonValue& name :
                 entry->config.transportConfig
                     .value(QStringLiteral("inheritEnvironment"))
                     .toArray()) {
                stdioConfig.inheritedEnvironmentNames.append(name.toString());
            }
            transport = std::make_unique<StdioClientTransport>(
                std::move(stdioConfig));
        }
        entry->client =
            std::make_unique<McpClient>(std::move(transport));
    }
    return entry->client->start();
}

QSharedPointer<McpOperation<void>> McpClientManager::stopClient(const QString &id)
{
    McpClient *managedClient = client(id);
    if (!managedClient) {
        auto operation = QSharedPointer<McpOperation<void>>::create();  // 保存立即失败的停止操作
        Internal::McpOperationAccess::fail(
            operation,
            {McpErrorCode::InvalidState, QStringLiteral("Client 尚未启动")});
        return operation;
    }
    return managedClient->stop();
}

} // namespace LibMcp
