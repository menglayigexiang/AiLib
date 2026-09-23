#include <LibMcp/McpClient.h>

#include "../core/FutureUtils_p.h"
#include "../core/JsonRpcCodec_p.h"
#include "../core/McpProtocolCodec_p.h"
#include "../core/McpOperationAccess_p.h"
#include "../core/ProtocolMetadata_p.h"
#include "../core/ProtocolSchemaValidator_p.h"

#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

#include <functional>
#include <limits>
#include <type_traits>

namespace LibMcp {
namespace {

using Internal::Promise;
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
    QJsonObject clientCapabilities;  // 每次请求都显式携带的 Client 能力
    McpServerInfo serverInfo;
    QJsonObject capabilities;
    QHash<QString, std::shared_ptr<PendingRequest>> pending;
    QHash<McpOperationBase*, QSharedPointer<McpOperationBase>> activeOperations;  // Client 持有的活动操作
    QHash<QString, QSharedPointer<McpOperationBase>> progressOperations;  // 按不透明 Token 路由请求内进度
    quint64 nextRequestId = 1;
    bool running = false;

    template<typename T>
    QSharedPointer<McpOperation<T>> track(
        QFuture<McpResult<T>> future,  // 需要映射为公共 Operation 的内部 Future
        QString requestId = {})       // 可选的底层 JSON-RPC 请求 ID
    {                                  // 跟踪 Future 并维护 Operation 共享所有权
        const QSharedPointer<McpOperation<T>> operation =  // 创建无 QObject parent 的公共操作
            QSharedPointer<McpOperation<T>>::create();
        activeOperations.insert(operation.data(), operation);
        if (!requestId.isEmpty()) {
            progressOperations.insert(requestId, operation);
        }
        QObject::connect(operation.data(),
                         &McpOperationBase::finished,
                         owner,
                         [this, operation, requestId] {  // 完成后释放 Client 持有的共享引用
                             activeOperations.remove(operation.data());
                             if (!requestId.isEmpty()) {
                                 progressOperations.remove(requestId);
                             }
                         });
        QObject::connect(operation.data(),
                         &McpOperationBase::cancellationRequested,
                         owner,
                         [this, operation, requestId] {  // 结束公共操作并通知远端放弃请求
                             if (!requestId.isEmpty()) {
                                 cancelPendingRequest(requestId);
                             }
                             Internal::McpOperationAccess::cancel(operation);
                         });

        auto* watcher = new QFutureWatcher<McpResult<T>>(owner);  // 监视内部 Future 最终结果
        QObject::connect(
            watcher,
            &QFutureWatcher<McpResult<T>>::finished,
            owner,
            [watcher, operation] {  // 将内部 Future 结果写入仍在运行的 Operation
                const McpResult<T> result = watcher->result();  // 读取内部异步结果
                watcher->deleteLater();
                if (operation->isFinished()) {
                    return;
                }
                if (result.isError()) {
                    Internal::McpOperationAccess::fail(operation,
                                                       result.error());
                } else {
                    if constexpr (std::is_same_v<T, McpToolCallResult>) {
                        if (!result.value().inputRequests.isEmpty()) {
                            Internal::McpOperationAccess::requireInput(
                                operation,
                                result.value().inputRequests);
                        }
                    }
                    Internal::McpOperationAccess::succeed(operation,
                                                          result.value());
                }
            });
        watcher->setFuture(future);
        return operation;
    }

    QSharedPointer<McpOperation<void>> track(
        QFuture<McpResult<void>> future,  // 需要映射为公共无值 Operation 的内部 Future
        QString requestId = {})          // 可选的底层 JSON-RPC 请求 ID
    {                                     // 跟踪 Future 并维护 Operation 共享所有权
        const QSharedPointer<McpOperation<void>> operation =  // 创建无 QObject parent 的公共操作
            QSharedPointer<McpOperation<void>>::create();
        activeOperations.insert(operation.data(), operation);
        if (!requestId.isEmpty()) {
            progressOperations.insert(requestId, operation);
        }
        QObject::connect(operation.data(),
                         &McpOperationBase::finished,
                         owner,
                         [this, operation, requestId] {  // 完成后释放 Client 持有的共享引用
                             activeOperations.remove(operation.data());
                             if (!requestId.isEmpty()) {
                                 progressOperations.remove(requestId);
                             }
                         });
        QObject::connect(operation.data(),
                         &McpOperationBase::cancellationRequested,
                         owner,
                         [this, operation, requestId] {  // 结束公共操作并通知远端放弃请求
                             if (!requestId.isEmpty()) {
                                 cancelPendingRequest(requestId);
                             }
                             Internal::McpOperationAccess::cancel(operation);
                         });
        auto* watcher = new QFutureWatcher<McpResult<void>>(owner);  // 监视内部 Future 最终结果
        QObject::connect(
            watcher,
            &QFutureWatcher<McpResult<void>>::finished,
            owner,
            [watcher, operation] {  // 将内部 Future 结果写入仍在运行的 Operation
                const McpResult<void> result = watcher->result();  // 读取内部异步结果
                watcher->deleteLater();
                if (operation->isFinished()) {
                    return;
                }
                if (result.isError()) {
                    Internal::McpOperationAccess::fail(operation,
                                                       result.error());
                } else {
                    Internal::McpOperationAccess::succeed(operation);
                }
            });
        watcher->setFuture(future);
        return operation;
    }

    void cancelOperations()  // 在 Client 销毁时终止全部未完成 Operation
    {
        const auto operations = activeOperations;  // 固定本轮需要取消的 Operation 集合
        for (const QSharedPointer<McpOperationBase>& operation : operations) {
            Internal::McpOperationAccess::cancel(operation);
        }
        activeOperations.clear();
    }

    QFuture<McpResult<QJsonObject>> request(
        const QString &method,
        QJsonObject params = {},
        int timeoutMs = 30000,
        QString* requestIdOutput = nullptr)  // 可选返回生成的请求 ID 以支持精确取消
    {
        const QString id = QString::number(nextRequestId++);
        if (requestIdOutput) {
            *requestIdOutput = id;
        }
        const QJsonObject meta =  // 保存本次请求自包含的协议版本、能力和客户端身份
            Internal::makeRequestMetadata(
                clientCapabilities,
                QJsonObject{{QStringLiteral("name"), clientInfo.name},
                            {QStringLiteral("version"), clientInfo.version}});
        QJsonObject requestMeta = meta;  // 扩展当前请求的进度路由信息
        requestMeta.insert(QStringLiteral("progressToken"), id);
        params.insert(QStringLiteral("_meta"), requestMeta);

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

    void cancelPendingRequest(const QString& id)  // 终止指定待处理请求并发送标准取消通知
    {
        const auto state = pending.take(id);  // 移除不再接受结果的请求
        if (!state) {
            return;
        }
        state->timer->stop();
        state->timer->deleteLater();
        state->promise.finish(McpResult<QJsonObject>::failure(
            {McpErrorCode::Cancelled, QStringLiteral("MCP 请求已取消")}));
        transport->sendMessage(Internal::makeNotification(
            QStringLiteral("notifications/cancelled"),
            QJsonObject{{QStringLiteral("requestId"), id}}));
    }

    void receive(const QJsonObject &message)
    {
        if (message.contains(QStringLiteral("method"))) {
            const McpResult<void> notificationValidation =  // 按官方 Schema 校验 Server 通知
                Internal::validateProtocolDefinition(
                    QStringLiteral("ServerNotification"),
                    message);
            if (notificationValidation.isError()) {
                return;
            }
            const QString method = message.value(QStringLiteral("method")).toString();  // 读取待分发的 Server 通知方法
            const QJsonObject params = message.value(QStringLiteral("params")).toObject();  // 读取通知参数和可选路由 Token
            if (method == QStringLiteral("notifications/progress")) {
                const QString token = params.value(QStringLiteral("progressToken"))
                                          .toVariant()
                                          .toString();  // 规范化字符串或整数进度 Token
                const auto operation = progressOperations.value(token);  // 查找当前进度所属的操作
                if (operation && !operation->isFinished()) {
                    Internal::McpOperationAccess::reportProgress(
                        operation,
                        params.value(QStringLiteral("progress")).toDouble(),
                        params.contains(QStringLiteral("total"))
                            ? params.value(QStringLiteral("total")).toDouble()
                            : -1.0,
                        params.value(QStringLiteral("message")).toString());
                }
            }
            emit owner->notificationReceived(method, params);
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
            const McpResult<void> errorValidation =  // 按官方 Schema 校验 JSON-RPC 错误响应
                Internal::validateProtocolDefinition(
                    QStringLiteral("JSONRPCErrorResponse"),
                    message);
            if (errorValidation.isError()) {
                state->promise.finish(McpResult<QJsonObject>::failure(
                    errorValidation.error()));
                return;
            }
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
        const McpResult<void> resultValidation =  // 按官方 Schema 校验 Server 结果公共结构
            Internal::validateProtocolDefinition(
                QStringLiteral("JSONRPCResultResponse"),
                message);
        if (resultValidation.isError()) {
            state->promise.finish(McpResult<QJsonObject>::failure(
                resultValidation.error()));
            return;
        }
        const QJsonObject result = message.value(QStringLiteral("result")).toObject();  // 保存已通过 Schema 校验的业务结果
        const QJsonObject serverInfo =  // 读取每个结果建议携带的 Server 身份
            result.value(QStringLiteral("_meta"))
                .toObject()
                .value(QStringLiteral("io.modelcontextprotocol/serverInfo"))
                .toObject();
        if (!serverInfo.isEmpty()) {
            this->serverInfo.name = serverInfo.value(QStringLiteral("name")).toString();
            this->serverInfo.version = serverInfo.value(QStringLiteral("version")).toString();
            this->serverInfo.title = serverInfo.value(QStringLiteral("title")).toString();
        }
        state->promise.finish(McpResult<QJsonObject>::success(result));
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
    QJsonObject capabilities,
    QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->owner = this;
    d->transport = std::move(transport);
    d->clientInfo = std::move(clientInfo);
    d->clientCapabilities = std::move(capabilities);

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

McpClient::~McpClient()  // 取消活动操作并释放 Transport
{
    QObject::disconnect(d->transport.get(), nullptr, this, nullptr);
    d->failPending({McpErrorCode::Cancelled,
                    QStringLiteral("MCP Client 已销毁")});
    d->cancelOperations();
}

QSharedPointer<McpOperation<void>> McpClient::start()  // 启动 Transport，新协议不执行连接级握手
{
    if (d->running) {
        return d->track(readyFuture(McpResult<void>::success()));
    }

    Promise<McpResult<void>> promise;
    const QFuture<McpResult<void>> future = promise.future();
    auto *transportWatcher = new QFutureWatcher<McpResult<void>>(this);
    connect(transportWatcher,
            &QFutureWatcher<McpResult<void>>::finished,
            this,
            [this, transportWatcher, promise] {
                const McpResult<void> transportResult =  // 保存 Transport 的最终启动结果
                    transportWatcher->result();
                transportWatcher->deleteLater();
                if (transportResult.isError()) {
                    promise.finish(transportResult);
                    return;
                }
                d->running = true;
                promise.finish(McpResult<void>::success());
            });
    transportWatcher->setFuture(d->transport->start());
    return d->track(future);
}

QSharedPointer<McpOperation<void>> McpClient::stop()  // 停止 Transport 并终止未完成请求
{
    d->running = false;
    d->failPending({McpErrorCode::ConnectionClosed,
                    QStringLiteral("MCP Client 已停止")});
    return d->track(d->transport->stop());
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

QSharedPointer<McpOperation<McpDiscoveryResult>> McpClient::discover()  // 显式发现 Server 版本和能力
{
    return d->track(mapResult<McpDiscoveryResult>(
        this,
        d->request(QStringLiteral("server/discover")),
        [this](const QJsonObject& result) {  // 解码发现结果并更新可查询的最近能力快照
            if (!result.value(QStringLiteral("supportedVersions")).isArray()
                || !result.value(QStringLiteral("capabilities")).isObject()) {
                return McpResult<McpDiscoveryResult>::failure(
                    {McpErrorCode::InvalidResponse,
                     QStringLiteral("server/discover 响应缺少版本或能力")});
            }
            McpDiscoveryResult decoded;  // 保存公共发现结果
            for (const QJsonValue& version :
                 result.value(QStringLiteral("supportedVersions")).toArray()) {
                if (!version.isString()) {
                    return McpResult<McpDiscoveryResult>::failure(
                        {McpErrorCode::InvalidResponse,
                         QStringLiteral("Server 支持版本必须是字符串")});
                }
                decoded.supportedVersions.append(version.toString());
            }
            decoded.capabilities = result.value(QStringLiteral("capabilities")).toObject();
            decoded.serverInfo = d->serverInfo;
            decoded.instructions = result.value(QStringLiteral("instructions")).toString();
            d->capabilities = decoded.capabilities;
            return McpResult<McpDiscoveryResult>::success(std::move(decoded));
        }));
}

QSharedPointer<McpOperation<QList<McpTool>>> McpClient::listTools()  // 列出全部远端工具
{
    return d->track(
        d->listAll<McpTool>(QStringLiteral("tools/list"),
                            QStringLiteral("tools"),
                            Internal::decodeTool));
}

QSharedPointer<McpOperation<McpToolCallResult>> McpClient::callTool(
    const QString& name,                 // 需要调用的工具名称
    const QJsonObject& arguments,        // 传给工具 inputSchema 校验的参数对象
    const QString& requestState,         // 上一轮 input_required 返回的不透明状态
    const QJsonObject& inputResponses)   // 对上一轮输入请求的响应映射
{
    QJsonObject params{{QStringLiteral("name"), name},
                       {QStringLiteral("arguments"), arguments}};  // 编码工具名称和输入参数
    if (!requestState.isEmpty()) {
        params.insert(QStringLiteral("requestState"), requestState);
    }
    if (!inputResponses.isEmpty()) {
        params.insert(QStringLiteral("inputResponses"), inputResponses);
    }
    QString requestId;  // 保存工具调用 ID 以便 Operation 精确取消
    const QFuture<McpResult<QJsonObject>> requestFuture =  // 保存待解码的工具调用响应
        d->request(QStringLiteral("tools/call"), params, 30000, &requestId);
    return d->track(mapResult<McpToolCallResult>(
        this,
        requestFuture,
        [](const QJsonObject &result) {
            const QString resultType =  // 区分最终结果与需要补充输入的中间结果
                result.value(QStringLiteral("resultType")).toString();
            if (resultType == QStringLiteral("input_required")) {
                const QJsonObject inputRequests =  // 读取 Server 需要 Client 完成的请求
                    result.value(QStringLiteral("inputRequests")).toObject();
                const QString requestState =  // 读取下一次重试必须原样回传的状态
                    result.value(QStringLiteral("requestState")).toString();
                if (inputRequests.isEmpty() && requestState.isEmpty()) {
                    return McpResult<McpToolCallResult>::failure(
                        {McpErrorCode::InvalidResponse,
                         QStringLiteral("input_required 结果缺少输入请求或重试状态")});
                }
                McpToolCallResult decoded;  // 保存需要调用方继续提供输入的结果
                decoded.inputRequests = inputRequests;
                decoded.requestState = requestState;
                return McpResult<McpToolCallResult>::success(std::move(decoded));
            }
            if (resultType != QStringLiteral("complete")
                || !result.value(QStringLiteral("content")).isArray()) {
                return McpResult<McpToolCallResult>::failure(
                    {McpErrorCode::InvalidResponse,
                     QStringLiteral("tools/call 完成响应缺少正确类型或 content 数组")});
            }
            McpToolCallResult decoded;  // 保存解码后的完整工具业务结果
            decoded.content = result.value(QStringLiteral("content")).toArray();
            decoded.structuredContent =
                result.value(QStringLiteral("structuredContent"));
            decoded.isError = result.value(QStringLiteral("isError")).toBool();
            decoded.inputRequests =
                result.value(QStringLiteral("inputRequests")).toObject();
            decoded.requestState =
                result.value(QStringLiteral("requestState")).toString();
            return McpResult<McpToolCallResult>::success(std::move(decoded));
        }),
        requestId);
}

QSharedPointer<McpOperation<QList<McpResource>>> McpClient::listResources()  // 列出全部固定资源
{
    return d->track(
        d->listAll<McpResource>(QStringLiteral("resources/list"),
                                QStringLiteral("resources"),
                                Internal::decodeResource));
}

QSharedPointer<McpOperation<QList<McpResourceTemplate>>>
McpClient::listResourceTemplates()
{
    return d->track(d->listAll<McpResourceTemplate>(
        QStringLiteral("resources/templates/list"),
        QStringLiteral("resourceTemplates"),
        Internal::decodeResourceTemplate));
}

QSharedPointer<McpOperation<QList<McpResourceContent>>> McpClient::readResource(
    const QString &uri)
{
    return d->track(mapResult<QList<McpResourceContent>>(
        this,
        d->request(QStringLiteral("resources/read"),
                   {{QStringLiteral("uri"), uri}}),
        [](const QJsonObject &result) {
            return decodeArray<McpResourceContent>(
                result,
                QStringLiteral("contents"),
                Internal::decodeResourceContent);
        }));
}

QSharedPointer<McpOperation<QList<McpPrompt>>> McpClient::listPrompts()  // 列出全部 Prompt
{
    return d->track(
        d->listAll<McpPrompt>(QStringLiteral("prompts/list"),
                              QStringLiteral("prompts"),
                              Internal::decodePrompt));
}

QSharedPointer<McpOperation<QList<McpPromptMessage>>> McpClient::getPrompt(
    const QString &name, const QJsonObject &arguments)
{
    return d->track(mapResult<QList<McpPromptMessage>>(
        this,
        d->request(QStringLiteral("prompts/get"),
                   {{QStringLiteral("name"), name},
                    {QStringLiteral("arguments"), arguments}}),
        [](const QJsonObject &result) {
            return decodeArray<McpPromptMessage>(
                result,
                QStringLiteral("messages"),
                Internal::decodePromptMessage);
        }));
}

QSharedPointer<McpOperation<McpCompletionResult>> McpClient::complete(
    const McpCompletionRequest& request)  // 描述引用、当前参数和已知上下文
{
    QJsonObject reference;  // 编码 Prompt 或资源模板引用
    if (request.referenceType == McpCompletionReferenceType::Prompt) {
        reference = {{QStringLiteral("type"), QStringLiteral("ref/prompt")},
                     {QStringLiteral("name"), request.reference}};
    } else {
        reference = {{QStringLiteral("type"), QStringLiteral("ref/resource")},
                     {QStringLiteral("uri"), request.reference}};
    }
    QJsonObject params{  // 编码 completion/complete 请求参数
        {QStringLiteral("ref"), reference},
        {QStringLiteral("argument"),
         QJsonObject{{QStringLiteral("name"), request.argumentName},
                     {QStringLiteral("value"), request.argumentValue}}}};
    if (!request.context.isEmpty()) {
        params.insert(QStringLiteral("context"),
                      QJsonObject{{QStringLiteral("arguments"), request.context}});
    }

    return d->track(mapResult<McpCompletionResult>(
        this,
        d->request(QStringLiteral("completion/complete"), params),
        [](const QJsonObject& result) {  // 解码补全候选值和可选数量提示
            const QJsonObject completion =  // 读取协议 Completion 对象
                result.value(QStringLiteral("completion")).toObject();
            if (!completion.value(QStringLiteral("values")).isArray()) {
                return McpResult<McpCompletionResult>::failure(
                    {McpErrorCode::InvalidResponse,
                     QStringLiteral("completion/complete 响应缺少 values 数组")});
            }
            McpCompletionResult decoded;  // 保存公共补全结果
            for (const QJsonValue& value :
                 completion.value(QStringLiteral("values")).toArray()) {
                if (!value.isString()) {
                    return McpResult<McpCompletionResult>::failure(
                        {McpErrorCode::InvalidResponse,
                         QStringLiteral("补全候选值必须是字符串")});
                }
                decoded.values.append(value.toString());
            }
            if (completion.value(QStringLiteral("total")).isDouble()) {
                decoded.total = completion.value(QStringLiteral("total")).toInt();
            }
            if (completion.value(QStringLiteral("hasMore")).isBool()) {
                decoded.hasMore = completion.value(QStringLiteral("hasMore")).toBool();
            }
            return McpResult<McpCompletionResult>::success(std::move(decoded));
        }));
}

QSharedPointer<McpOperation<void>> McpClient::listen(
    const McpSubscriptionFilter& filter)  // 打开按类型筛选的长寿命通知订阅
{
    QJsonObject notifications;  // 编码 Client 显式选择的通知类型
    if (filter.toolsListChanged) {
        notifications.insert(QStringLiteral("toolsListChanged"), true);
    }
    if (filter.resourcesListChanged) {
        notifications.insert(QStringLiteral("resourcesListChanged"), true);
    }
    if (filter.promptsListChanged) {
        notifications.insert(QStringLiteral("promptsListChanged"), true);
    }
    if (!filter.resourceSubscriptions.isEmpty()) {
        QJsonArray uris;  // 保存需要监听更新的资源 URI
        for (const QString& uri : filter.resourceSubscriptions) {
            uris.append(uri);
        }
        notifications.insert(QStringLiteral("resourceSubscriptions"), uris);
    }
    QString requestId;  // 保存订阅请求 ID 以支持精确取消
    const QFuture<McpResult<QJsonObject>> future =  // 订阅只在 Server 正常关闭时返回最终结果
        d->request(QStringLiteral("subscriptions/listen"),
                   QJsonObject{{QStringLiteral("notifications"), notifications}},
                   std::numeric_limits<int>::max(),
                   &requestId);
    return d->track(mapResult<void>(
                        this,
                        future,
                        [](const QJsonObject&) {  // 将正常关闭结果转换为无值成功
                            return McpResult<void>::success();
                        }),
                    requestId);
}

} // namespace LibMcp
