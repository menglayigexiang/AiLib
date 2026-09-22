#include <LibMcp/McpClient.h>

#include "../core/FutureUtils_p.h"
#include "../core/JsonRpcCodec_p.h"
#include "../core/McpProtocolCodec_p.h"

#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

#include <functional>

namespace LibMcp {
namespace {

using Internal::Promise;
using Internal::makeNotification;
using Internal::makeRequest;
using Internal::readyFuture;

template<typename T, typename Transform>
QFuture<McpResult<T>> mapResult(
    QObject *context,
    QFuture<McpResult<QJsonObject>> source,
    Transform transform)
{
    Promise<McpResult<T>> promise;
    const QFuture<McpResult<T>> future = promise.future();
    auto *watcher = new QFutureWatcher<McpResult<QJsonObject>>(context);
    QObject::connect(
        watcher,
        &QFutureWatcher<McpResult<QJsonObject>>::finished,
        context,
        [watcher, promise, transform] {
            const McpResult<QJsonObject> result = watcher->result();
            watcher->deleteLater();
            if (result.isError()) {
                promise.finish(McpResult<T>::failure(result.error()));
                return;
            }
            promise.finish(transform(result.value()));
        });
    watcher->setFuture(source);
    return future;
}

template<typename T, typename Decoder>
McpResult<QList<T>> decodeArray(
    const QJsonObject &result, const QString &field, Decoder decoder)
{
    if (!result.value(field).isArray()) {
        return McpResult<QList<T>>::failure(
            {McpErrorCode::InvalidResponse,
             QStringLiteral("响应字段 %1 不是数组").arg(field)});
    }

    QList<T> values;
    for (const QJsonValue &item : result.value(field).toArray()) {
        if (!item.isObject()) {
            return McpResult<QList<T>>::failure(
                {McpErrorCode::InvalidResponse,
                 QStringLiteral("响应数组 %1 包含非对象项").arg(field)});
        }
        McpResult<T> decoded = decoder(item.toObject());
        if (decoded.isError()) {
            return McpResult<QList<T>>::failure(decoded.error());
        }
        values.append(decoded.value());
    }
    return McpResult<QList<T>>::success(std::move(values));
}

} // namespace

class McpClient::Private
{
public:
    struct PendingRequest
    {
        Internal::Promise<McpResult<QJsonObject>> promise;
        QTimer *timer = nullptr;
    };

    McpClient *owner = nullptr;
    std::unique_ptr<McpClientTransport> transport;
    ClientInfo clientInfo;
    McpServerInfo serverInfo;
    QJsonObject capabilities;
    QHash<QString, std::shared_ptr<PendingRequest>> pending;
    quint64 nextRequestId = 1;
    bool running = false;

    QFuture<McpResult<QJsonObject>> request(
        const QString &method,
        QJsonObject params = {},
        int timeoutMs = 30000)
    {
        const QString id = QString::number(nextRequestId++);
        QJsonObject meta = Internal::requestMeta();
        meta.insert(QStringLiteral(
                        "io.modelcontextprotocol/clientCapabilities"),
                    QJsonObject{});
        meta.insert(
            QStringLiteral("io.modelcontextprotocol/clientInfo"),
            QJsonObject{{QStringLiteral("name"), clientInfo.name},
                        {QStringLiteral("version"), clientInfo.version}});
        params.insert(QStringLiteral("_meta"), meta);

        auto state = std::make_shared<PendingRequest>();
        const QFuture<McpResult<QJsonObject>> future = state->promise.future();
        state->timer = new QTimer(owner);
        state->timer->setSingleShot(true);
        QObject::connect(state->timer, &QTimer::timeout, owner, [this, id] {
            const auto timedOut = pending.take(id);
            if (timedOut) {
                timedOut->promise.finish(McpResult<QJsonObject>::failure(
                    {McpErrorCode::RequestTimeout,
                     QStringLiteral("MCP 请求超时")}));
            }
        });
        pending.insert(id, state);
        state->timer->start(timeoutMs);

        auto *sendWatcher = new QFutureWatcher<McpResult<void>>(owner);
        QObject::connect(
            sendWatcher,
            &QFutureWatcher<McpResult<void>>::finished,
            owner,
            [this, id, sendWatcher] {
                const McpResult<void> result = sendWatcher->result();
                sendWatcher->deleteLater();
                if (result.isSuccess()) {
                    return;
                }
                const auto failed = pending.take(id);
                if (failed) {
                    failed->timer->stop();
                    failed->timer->deleteLater();
                    failed->promise.finish(
                        McpResult<QJsonObject>::failure(result.error()));
                }
            });
        sendWatcher->setFuture(transport->sendMessage(
            makeRequest(id, method, params)));
        return future;
    }

    void receive(const QJsonObject &message)
    {
        if (message.contains(QStringLiteral("method"))) {
            emit owner->notificationReceived(
                message.value(QStringLiteral("method")).toString(),
                message.value(QStringLiteral("params")).toObject());
            return;
        }

        const QString id = message.value(QStringLiteral("id"))
                               .toVariant()
                               .toString();
        const auto state = pending.take(id);
        if (!state) {
            return;
        }
        state->timer->stop();
        state->timer->deleteLater();

        if (message.value(QStringLiteral("error")).isObject()) {
            const QJsonObject error =
                message.value(QStringLiteral("error")).toObject();
            state->promise.finish(McpResult<QJsonObject>::failure(
                {McpErrorCode::RemoteError,
                 error.value(QStringLiteral("message")).toString(),
                 error.value(QStringLiteral("code")).toInt(),
                 error.value(QStringLiteral("data"))}));
            return;
        }
        if (!message.value(QStringLiteral("result")).isObject()) {
            state->promise.finish(McpResult<QJsonObject>::failure(
                {McpErrorCode::InvalidResponse,
                 QStringLiteral("JSON-RPC 响应缺少 result 对象")}));
            return;
        }
        state->promise.finish(McpResult<QJsonObject>::success(
            message.value(QStringLiteral("result")).toObject()));
    }

    void failPending(const McpError &error)
    {
        const auto requests = pending;
        pending.clear();
        for (const auto &state : requests) {
            state->timer->stop();
            state->timer->deleteLater();
            state->promise.finish(McpResult<QJsonObject>::failure(error));
        }
    }

    template<typename T, typename Decoder>
    QFuture<McpResult<QList<T>>> listAll(
        const QString &method,
        const QString &field,
        Decoder decoder)
    {
        struct PaginationState
        {
            Internal::Promise<McpResult<QList<T>>> promise;
            QList<T> values;
            QSet<QString> cursors;
            int pageCount = 0;
        };

        auto state = std::make_shared<PaginationState>();
        auto loadPage = std::make_shared<std::function<void(QString)>>();
        *loadPage = [this, method, field, decoder, state, loadPage](
                        const QString &cursor) {
            QJsonObject params;
            if (!cursor.isEmpty()) {
                params.insert(QStringLiteral("cursor"), cursor);
            }
            auto *watcher =
                new QFutureWatcher<McpResult<QJsonObject>>(owner);
            QObject::connect(
                watcher,
                &QFutureWatcher<McpResult<QJsonObject>>::finished,
                owner,
                [field, decoder, state, loadPage, watcher] {
                    const auto page = watcher->result();
                    watcher->deleteLater();
                    if (page.isError()) {
                        state->promise.finish(
                            McpResult<QList<T>>::failure(page.error()));
                        return;
                    }

                    const auto decoded = decodeArray<T>(
                        page.value(), field, decoder);
                    if (decoded.isError()) {
                        state->promise.finish(decoded);
                        return;
                    }
                    state->values.append(decoded.value());

                    const QString nextCursor =
                        page.value()
                            .value(QStringLiteral("nextCursor"))
                            .toString();
                    ++state->pageCount;
                    if (nextCursor.isEmpty()) {
                        state->promise.finish(
                            McpResult<QList<T>>::success(state->values));
                        return;
                    }
                    if (state->pageCount >= 64
                        || state->cursors.contains(nextCursor)) {
                        state->promise.finish(
                            McpResult<QList<T>>::failure(
                                {McpErrorCode::InvalidResponse,
                                 QStringLiteral(
                                     "分页游标重复或页数超过安全上限")}));
                        return;
                    }
                    state->cursors.insert(nextCursor);
                    (*loadPage)(nextCursor);
                });
            watcher->setFuture(request(method, params));
        };

        const QFuture<McpResult<QList<T>>> future = state->promise.future();
        (*loadPage)({});
        return future;
    }
};

McpClient::McpClient(
    std::unique_ptr<McpClientTransport> transport,
    ClientInfo clientInfo,
    QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->owner = this;
    d->transport = std::move(transport);
    d->clientInfo = std::move(clientInfo);

    connect(d->transport.get(),
            &McpClientTransport::messageReceived,
            this,
            [this](const QJsonObject &message) { d->receive(message); });
    connect(d->transport.get(),
            &McpClientTransport::disconnected,
            this,
            [this](const McpError &error) {
                d->running = false;
                d->failPending(error);
            });
}

McpClient::~McpClient() = default;

QFuture<McpResult<void>> McpClient::start()
{
    if (d->running) {
        return readyFuture(McpResult<void>::success());
    }

    Promise<McpResult<void>> promise;
    const QFuture<McpResult<void>> future = promise.future();
    auto *transportWatcher = new QFutureWatcher<McpResult<void>>(this);
    connect(transportWatcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, transportWatcher, promise] {
                const McpResult<void> transportResult =
                    transportWatcher->result();
                transportWatcher->deleteLater();
                if (transportResult.isError()) {
                    promise.finish(transportResult);
                    return;
                }

                const QJsonObject params{
                    {QStringLiteral("protocolVersion"),
                     QStringLiteral(LIBMCP_PROTOCOL_VERSION)},
                    {QStringLiteral("capabilities"), QJsonObject{}},
                    {QStringLiteral("clientInfo"),
                     QJsonObject{
                         {QStringLiteral("name"), d->clientInfo.name},
                         {QStringLiteral("version"), d->clientInfo.version}}}};
                auto *watcher =
                    new QFutureWatcher<McpResult<QJsonObject>>(this);
                connect(watcher,
                        &QFutureWatcher<McpResult<QJsonObject>>::finished,
                        this,
                        [this, watcher, promise] {
                            const auto result = watcher->result();
                            watcher->deleteLater();
                            if (result.isError()) {
                                promise.finish(McpResult<void>::failure(
                                    result.error()));
                                return;
                            }

                            const QJsonObject response = result.value();
                            const QString version =
                                response.value(QStringLiteral("protocolVersion"))
                                    .toString();
                            if (version.isEmpty()) {
                                promise.finish(McpResult<void>::failure(
                                    {McpErrorCode::InvalidResponse,
                                     QStringLiteral(
                                         "initialize 响应缺少协议版本")}));
                                return;
                            }
                            const QJsonObject info =
                                response.value(QStringLiteral("serverInfo"))
                                    .toObject();
                            d->serverInfo.name =
                                info.value(QStringLiteral("name")).toString();
                            d->serverInfo.version =
                                info.value(QStringLiteral("version")).toString();
                            d->serverInfo.title =
                                info.value(QStringLiteral("title")).toString();
                            d->capabilities =
                                response.value(QStringLiteral("capabilities"))
                                    .toObject();
                            d->running = true;
                            d->transport->sendMessage(makeNotification(
                                QStringLiteral("notifications/initialized")));
                            promise.finish(McpResult<void>::success());
                        });
                watcher->setFuture(d->request(QStringLiteral("initialize"),
                                               params));
            });
    transportWatcher->setFuture(d->transport->start());
    return future;
}

QFuture<McpResult<void>> McpClient::stop()
{
    d->running = false;
    d->failPending({McpErrorCode::ConnectionClosed,
                    QStringLiteral("MCP Client 已停止")});
    return d->transport->stop();
}

bool McpClient::isRunning() const
{
    return d->running;
}

McpClient::ClientInfo McpClient::clientInfo() const
{
    return d->clientInfo;
}

McpServerInfo McpClient::serverInfo() const
{
    return d->serverInfo;
}

QJsonObject McpClient::serverCapabilities() const
{
    return d->capabilities;
}

QFuture<McpResult<QList<McpTool>>> McpClient::listTools()
{
    return d->listAll<McpTool>(QStringLiteral("tools/list"),
                               QStringLiteral("tools"),
                               Internal::decodeTool);
}

QFuture<McpResult<QJsonArray>> McpClient::callTool(
    const QString &name, const QJsonArray &input)
{
    QJsonObject arguments;
    if (input.size() == 1 && input.first().isObject()) {
        arguments = input.first().toObject();
    } else {
        for (int index = 0; index < input.size(); ++index) {
            arguments.insert(QStringLiteral("arg%1").arg(index),
                             input.at(index));
        }
    }

    return mapResult<QJsonArray>(
        this,
        d->request(QStringLiteral("tools/call"),
                   {{QStringLiteral("name"), name},
                    {QStringLiteral("arguments"), arguments}}),
        [](const QJsonObject &result) {
            if (!result.value(QStringLiteral("content")).isArray()) {
                return McpResult<QJsonArray>::failure(
                    {McpErrorCode::InvalidResponse,
                     QStringLiteral("tools/call 响应缺少 content 数组")});
            }
            if (result.value(QStringLiteral("isError")).toBool()) {
                return McpResult<QJsonArray>::failure(
                    {McpErrorCode::ToolError,
                     QStringLiteral("远端工具执行失败"),
                     0,
                     result});
            }
            return McpResult<QJsonArray>::success(
                result.value(QStringLiteral("content")).toArray());
        });
}

QFuture<McpResult<QList<McpResource>>> McpClient::listResources()
{
    return d->listAll<McpResource>(QStringLiteral("resources/list"),
                                   QStringLiteral("resources"),
                                   Internal::decodeResource);
}

QFuture<McpResult<QList<McpResourceTemplate>>>
McpClient::listResourceTemplates()
{
    return d->listAll<McpResourceTemplate>(
        QStringLiteral("resources/templates/list"),
        QStringLiteral("resourceTemplates"),
        Internal::decodeResourceTemplate);
}

QFuture<McpResult<QList<McpResourceContent>>> McpClient::readResource(
    const QString &uri)
{
    return mapResult<QList<McpResourceContent>>(
        this,
        d->request(QStringLiteral("resources/read"),
                   {{QStringLiteral("uri"), uri}}),
        [](const QJsonObject &result) {
            return decodeArray<McpResourceContent>(
                result,
                QStringLiteral("contents"),
                Internal::decodeResourceContent);
        });
}

QFuture<McpResult<QList<McpPrompt>>> McpClient::listPrompts()
{
    return d->listAll<McpPrompt>(QStringLiteral("prompts/list"),
                                 QStringLiteral("prompts"),
                                 Internal::decodePrompt);
}

QFuture<McpResult<QList<McpPromptMessage>>> McpClient::getPrompt(
    const QString &name, const QJsonObject &arguments)
{
    return mapResult<QList<McpPromptMessage>>(
        this,
        d->request(QStringLiteral("prompts/get"),
                   {{QStringLiteral("name"), name},
                    {QStringLiteral("arguments"), arguments}}),
        [](const QJsonObject &result) {
            return decodeArray<McpPromptMessage>(
                result,
                QStringLiteral("messages"),
                Internal::decodePromptMessage);
        });
}

} // namespace LibMcp
