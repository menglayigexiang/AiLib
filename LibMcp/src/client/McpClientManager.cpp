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
        McpClientConfig config;                         // 持久化 Client 配置
        std::unique_ptr<McpClient> client;              // 按需创建的底层协议 Client
        ClientState state = ClientState::Disabled;      // 当前完整启用流程阶段
        TransportState transportState = TransportState::Stopped;  // 底层 Transport 独立状态
        ProtocolState protocolState = ProtocolState::NotChecked;  // 固定协议独立验证状态
        bool enabled = false;                           // 用户是否希望启用该 Client
        McpError error;                                 // 最近一次启用失败详情
        McpDiscoveryResult discovery;                   // 最近成功的 Server 能力发现快照
        QList<McpTool> tools;                           // Ready 后可对外使用的工具快照
        QList<McpResource> resources;                   // Ready 后可对外使用的固定资源快照
        QList<McpResourceTemplate> resourceTemplates;   // Ready 后可对外使用的资源模板快照
        QList<McpPrompt> prompts;                       // Ready 后可对外使用的 Prompt 快照
        QSharedPointer<McpOperation<void>> activation;  // 当前完整启用操作
    };

    // 汇总并行基础能力请求的剩余数量，最后一个成功请求负责进入 Ready。
    struct InitialCapabilityLoad
    {
        int pending = 0;  // 尚未完成的能力列表请求数量
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
        if (entry->enabled) {
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
    if (!entry || entry->enabled
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
    if (entry && entry->enabled) {
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

McpClientManager::ClientState McpClientManager::clientState(
    const QString& id) const  // 返回 Client 当前启用与验证阶段
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->state : ClientState::Disabled;
}

McpClientManager::TransportState McpClientManager::clientTransportState(
    const QString& id) const  // 返回独立 Transport 可达状态
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->transportState : TransportState::Stopped;
}

McpClientManager::ProtocolState McpClientManager::clientProtocolState(
    const QString& id) const  // 返回固定 MCP 协议验证状态
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->protocolState : ProtocolState::NotChecked;
}

bool McpClientManager::clientEnabled(
    const QString& id) const  // 返回用户是否希望启用该 Client
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry && entry->enabled;
}

McpError McpClientManager::clientError(
    const QString& id) const  // 返回最近一次启用失败的详细错误
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->error : McpError{};
}

McpDiscoveryResult McpClientManager::clientDiscovery(
    const QString& id) const  // 返回最近成功的能力发现快照
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->discovery : McpDiscoveryResult{};
}

QList<McpTool> McpClientManager::clientTools(
    const QString& id) const  // 返回 Ready Client 已校验的工具快照
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->tools : QList<McpTool>{};
}

QList<McpResource> McpClientManager::clientResources(
    const QString& id) const  // 返回 Ready Client 的固定资源快照
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->resources : QList<McpResource>{};
}

QList<McpResourceTemplate> McpClientManager::clientResourceTemplates(
    const QString& id) const  // 返回 Ready Client 的资源模板快照
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->resourceTemplates : QList<McpResourceTemplate>{};
}

QList<McpPrompt> McpClientManager::clientPrompts(
    const QString& id) const  // 返回 Ready Client 的 Prompt 快照
{
    const auto entry = d->entries.value(id);  // 查找需要查询的运行项
    return entry ? entry->prompts : QList<McpPrompt>{};
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
    if (entry->activation && !entry->activation->isFinished()) {
        return entry->activation;
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
    entry->enabled = true;
    entry->error = {};
    entry->discovery = {};
    entry->tools.clear();
    entry->resources.clear();
    entry->resourceTemplates.clear();
    entry->prompts.clear();
    entry->state = ClientState::Starting;
    entry->transportState = TransportState::Starting;
    entry->protocolState = ProtocolState::NotChecked;
    emit clientStateChanged(id, entry->state);
    emit clientTransportStateChanged(id, entry->transportState);
    emit clientProtocolStateChanged(id, entry->protocolState);
    emit clientToolsChanged(id);
    emit clientCapabilitiesChanged(id);

    const auto activation = QSharedPointer<McpOperation<void>>::create();  // 表示完整启用流程的最终操作
    entry->activation = activation;
    const auto failActivation =  // 统一停止 Transport 并进入可诊断错误状态
        [this, id, entry, activation](McpError error) {
            if (activation->isFinished()) {
                return;
            }
            entry->error = std::move(error);
            entry->tools.clear();
            entry->resources.clear();
            entry->resourceTemplates.clear();
            entry->prompts.clear();
            entry->state = ClientState::Error;
            if (entry->transportState != TransportState::Reachable) {
                entry->transportState = TransportState::Error;
            }
            if (entry->client && entry->client->isRunning()) {
                entry->client->stop();
            }
            emit clientToolsChanged(id);
            emit clientCapabilitiesChanged(id);
            emit clientTransportStateChanged(id, entry->transportState);
            emit clientStateChanged(id, entry->state);
            Internal::McpOperationAccess::fail(activation, entry->error);
        };
    const auto startOperation = entry->client->start();  // 启动底层 Transport
    const auto continueDiscovery =  // Transport 成功后执行固定版本协议发现
        [this, id, entry, activation, startOperation, failActivation] {
            if (activation->isFinished()) {
                return;
            }
            if (startOperation->status()
                != McpOperationBase::Status::Succeeded) {
                failActivation(startOperation->error());
                return;
            }
            entry->transportState = TransportState::Active;
            entry->state = ClientState::Discovering;
            entry->protocolState = ProtocolState::Checking;
            emit clientTransportStateChanged(id, entry->transportState);
            emit clientProtocolStateChanged(id, entry->protocolState);
            emit clientStateChanged(id, entry->state);
            const auto discoverOperation = entry->client->discover();  // 验证现代协议发现响应
            const auto continueTools =  // 发现成功并匹配固定版本后加载工具
                [this,
                 id,
                 entry,
                 activation,
                 discoverOperation,
                 failActivation] {
                    if (activation->isFinished()) {
                        return;
                    }
                    if (discoverOperation->status()
                        != McpOperationBase::Status::Succeeded) {
                        McpError error = discoverOperation->error();  // 归一化协议发现失败
                        if (error.code == McpErrorCode::RemoteError
                            && error.remoteCode == -32601) {
                            entry->transportState = TransportState::Reachable;
                            entry->protocolState = ProtocolState::Incompatible;
                            error.code = McpErrorCode::UnsupportedProtocolVersion;
                            error.message = QStringLiteral(
                                "Server 不支持 MCP 2026-07-28：server/discover 不可用");
                        } else if (error.code == McpErrorCode::RemoteError
                                   || error.code == McpErrorCode::InvalidMessage
                                   || error.code == McpErrorCode::InvalidResponse
                                   || error.code == McpErrorCode::ProtocolError
                                   || error.code == McpErrorCode::UnsupportedProtocolVersion) {
                            entry->transportState = TransportState::Reachable;
                            entry->protocolState =
                                error.code == McpErrorCode::UnsupportedProtocolVersion
                                    ? ProtocolState::Incompatible
                                    : ProtocolState::Invalid;
                        }
                        emit clientProtocolStateChanged(id, entry->protocolState);
                        failActivation(std::move(error));
                        return;
                    }
                    const McpDiscoveryResult& discovery =  // 读取 Server 声明的支持版本
                        *discoverOperation->result();
                    if (!discovery.supportedVersions.contains(
                            QStringLiteral(LIBMCP_PROTOCOL_VERSION))) {
                        entry->transportState = TransportState::Reachable;
                        entry->protocolState = ProtocolState::Incompatible;
                        emit clientProtocolStateChanged(id, entry->protocolState);
                        failActivation(
                            {McpErrorCode::UnsupportedProtocolVersion,
                             QStringLiteral("Server 不支持 MCP %1")
                                 .arg(QStringLiteral(LIBMCP_PROTOCOL_VERSION))});
                        return;
                    }
                    entry->transportState = TransportState::Reachable;
                    entry->protocolState = ProtocolState::Compatible;
                    entry->discovery = discovery;
                    emit clientTransportStateChanged(id, entry->transportState);
                    emit clientProtocolStateChanged(id, entry->protocolState);
                    emit clientCapabilitiesChanged(id);
                    const bool hasTools =  // Server 是否声明 Tools 能力
                        discovery.capabilities.contains(QStringLiteral("tools"));
                    const bool hasResources =  // Server 是否声明 Resources 与 Templates 能力
                        discovery.capabilities.contains(QStringLiteral("resources"));
                    const bool hasPrompts =  // Server 是否声明 Prompts 能力
                        discovery.capabilities.contains(QStringLiteral("prompts"));
                    const auto load = std::make_shared<Private::InitialCapabilityLoad>();  // 汇总并行列表请求
                    load->pending = (hasTools ? 1 : 0)
                                    + (hasResources ? 2 : 0)
                                    + (hasPrompts ? 1 : 0);
                    const auto completeOne =  // 最后一个成功列表请求负责进入 Ready
                        [this, id, entry, activation, load] {
                            if (activation->isFinished()) {
                                return;
                            }
                            --load->pending;
                            if (load->pending > 0) {
                                return;
                            }
                            entry->error = {};
                            entry->state = ClientState::Ready;
                            emit clientToolsChanged(id);
                            emit clientCapabilitiesChanged(id);
                            emit clientStateChanged(id, entry->state);
                            Internal::McpOperationAccess::succeed(activation);
                        };
                    if (load->pending == 0) {
                        entry->error = {};
                        entry->state = ClientState::Ready;
                        emit clientStateChanged(id, entry->state);
                        Internal::McpOperationAccess::succeed(activation);
                        return;
                    }
                    entry->state = ClientState::LoadingCapabilities;
                    emit clientStateChanged(id, entry->state);
                    if (hasTools) {
                        const auto operation = entry->client->listTools();  // 并行加载全部工具页
                        const auto finish =  // 保存工具快照或终止启用
                            [entry,
                             activation,
                             operation,
                             failActivation,
                             completeOne] {
                            if (activation->isFinished()) {
                                return;
                            }
                            if (operation->status()
                                != McpOperationBase::Status::Succeeded) {
                                failActivation(operation->error());
                                return;
                            }
                            entry->tools = *operation->result();
                            completeOne();
                        };
                        QObject::connect(operation.data(),
                                         &McpOperationBase::finished,
                                         this,
                                         finish);
                        if (operation->isFinished()) {
                            finish();
                        }
                    }
                    if (hasResources && !activation->isFinished()) {
                        const auto resourcesOperation = entry->client->listResources();  // 并行加载固定资源
                        const auto finishResources =  // 保存资源快照或终止启用
                            [entry,
                             activation,
                             resourcesOperation,
                             failActivation,
                             completeOne] {
                            if (activation->isFinished()) {
                                return;
                            }
                            if (resourcesOperation->status()
                                != McpOperationBase::Status::Succeeded) {
                                failActivation(resourcesOperation->error());
                                return;
                            }
                            entry->resources = *resourcesOperation->result();
                            completeOne();
                        };
                        QObject::connect(resourcesOperation.data(),
                                         &McpOperationBase::finished,
                                         this,
                                         finishResources);
                        if (resourcesOperation->isFinished()) {
                            finishResources();
                        }
                        const auto templatesOperation = entry->client->listResourceTemplates();  // 并行加载资源模板
                        const auto finishTemplates =  // 保存模板快照或终止启用
                            [entry,
                             activation,
                             templatesOperation,
                             failActivation,
                             completeOne] {
                            if (activation->isFinished()) {
                                return;
                            }
                            if (templatesOperation->status()
                                != McpOperationBase::Status::Succeeded) {
                                failActivation(templatesOperation->error());
                                return;
                            }
                            entry->resourceTemplates = *templatesOperation->result();
                            completeOne();
                        };
                        QObject::connect(templatesOperation.data(),
                                         &McpOperationBase::finished,
                                         this,
                                         finishTemplates);
                        if (templatesOperation->isFinished()) {
                            finishTemplates();
                        }
                    }
                    if (hasPrompts && !activation->isFinished()) {
                        const auto operation = entry->client->listPrompts();  // 并行加载全部 Prompt 页
                        const auto finish =  // 保存 Prompt 快照或终止启用
                            [entry,
                             activation,
                             operation,
                             failActivation,
                             completeOne] {
                            if (activation->isFinished()) {
                                return;
                            }
                            if (operation->status()
                                != McpOperationBase::Status::Succeeded) {
                                failActivation(operation->error());
                                return;
                            }
                            entry->prompts = *operation->result();
                            completeOne();
                        };
                        QObject::connect(operation.data(),
                                         &McpOperationBase::finished,
                                         this,
                                         finish);
                        if (operation->isFinished()) {
                            finish();
                        }
                    }
                };
            QObject::connect(discoverOperation.data(),
                             &McpOperationBase::finished,
                             this,
                             continueTools);
            if (discoverOperation->isFinished()) {
                continueTools();
            }
        };
    QObject::connect(startOperation.data(),
                     &McpOperationBase::finished,
                     this,
                     continueDiscovery);
    if (startOperation->isFinished()) {
        continueDiscovery();
    }
    QObject::connect(activation.data(),
                     &McpOperationBase::cancellationRequested,
                     this,
                     [this, id] { stopClient(id); });
    return activation;
}

QSharedPointer<McpOperation<void>> McpClientManager::stopClient(const QString &id)
{
    const auto entry = d->entries.value(id);  // 查找需要禁用的 Client 运行项
    if (!entry) {
        auto operation = QSharedPointer<McpOperation<void>>::create();  // 保存立即失败的停止操作
        Internal::McpOperationAccess::fail(
            operation,
            {McpErrorCode::InvalidState, QStringLiteral("Client 尚未启动")});
        return operation;
    }
    entry->enabled = false;
    if (entry->activation && !entry->activation->isFinished()) {
        Internal::McpOperationAccess::cancel(entry->activation);
    }
    entry->activation.reset();
    entry->discovery = {};
    entry->tools.clear();
    entry->resources.clear();
    entry->resourceTemplates.clear();
    entry->prompts.clear();
    entry->error = {};
    entry->state = ClientState::Disabled;
    entry->transportState = TransportState::Stopped;
    entry->protocolState = ProtocolState::NotChecked;
    emit clientToolsChanged(id);
    emit clientCapabilitiesChanged(id);
    emit clientTransportStateChanged(id, entry->transportState);
    emit clientProtocolStateChanged(id, entry->protocolState);
    emit clientStateChanged(id, entry->state);
    if (!entry->client || !entry->client->isRunning()) {
        auto operation = QSharedPointer<McpOperation<void>>::create();  // 返回已禁用 Client 的成功操作
        Internal::McpOperationAccess::succeed(operation);
        return operation;
    }
    return entry->client->stop();
}

} // namespace LibMcp
