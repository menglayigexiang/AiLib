#include <LibMcp/McpClient.h>
#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>
#include <LibMcp/InMemoryTransport.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFutureWatcher>

#include <iostream>

using namespace LibMcp;

template<typename T>
T waitFor(QFuture<T> future)
{
    QFutureWatcher<T> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<T>::finished,
                     &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    if (!watcher.isFinished()) loop.exec();
    return watcher.result();
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    int failures = 0;

    McpClientManager manager;
    McpClientConfig config;
    config.id = QStringLiteral("remote");
    config.transportType = QStringLiteral("streamable-http");
    config.transportConfig = {
        {QStringLiteral("url"), QStringLiteral("http://127.0.0.1:8080/mcp")}};
    if (!manager.addConfig(config)) ++failures;

    McpClientManager restored;
    if (restored.deserialize(manager.serialize()).isError()) ++failures;
    if (restored.configs().size() != 1) ++failures;

    auto pair = createInMemoryTransportPair();
    McpServer server(std::move(pair.second),
                     {QStringLiteral("test-server"),
                      QStringLiteral("1.0"),
                      QStringLiteral("Test Server")});

    McpTool tool;
    tool.name = QStringLiteral("echo");
    tool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    if (!server.addTool(tool, [](const QJsonArray &input) { return input; }))
        ++failures;

    McpTool validatedTool;
    validatedTool.name = QStringLiteral("required-value");
    validatedTool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("value")}},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("value"),
                      QJsonObject{{QStringLiteral("type"),
                                   QStringLiteral("string")}}}}}};
    if (!server.addTool(validatedTool,
                        [](const QJsonArray &input) { return input; })) {
        ++failures;
    }

    McpResource resource;
    resource.name = QStringLiteral("status");
    resource.uri = QStringLiteral("test://status");
    server.addResource(resource, [](const QString &uri) {
        return QList<McpResourceContent>{McpTextResourceContent{
            uri, QStringLiteral("text/plain"), QStringLiteral("running"), {}}};
    });

    McpResourceTemplate resourceTemplate;
    resourceTemplate.name = QStringLiteral("user");
    resourceTemplate.uriTemplate = QStringLiteral("test://users/{id}");
    if (!server.addResourceTemplate(
            resourceTemplate,
            [](const QString &uri) {
                return QList<McpResourceContent>{McpTextResourceContent{
                    uri,
                    QStringLiteral("text/plain"),
                    QStringLiteral("templated"),
                    {}}};
            })) {
        ++failures;
    }

    McpPrompt prompt;
    prompt.name = QStringLiteral("greet");
    prompt.arguments.append(
        {QStringLiteral("name"), {}, true});
    if (!server.addPrompt(prompt, [](const QJsonObject &arguments) {
            return QList<McpPromptMessage>{McpPromptMessage{
                McpRole::User,
                QJsonObject{{QStringLiteral("type"),
                             QStringLiteral("text")},
                            {QStringLiteral("text"),
                             QStringLiteral("Hello %1")
                                 .arg(arguments.value(
                                                   QStringLiteral("name"))
                                          .toString())}}}};
        })) {
        ++failures;
    }

    McpClient client(std::move(pair.first),
                     {QStringLiteral("test-client"), QStringLiteral("1.0")});
    if (waitFor(server.start()).isError()) ++failures;
    if (waitFor(client.start()).isError()) ++failures;

    const auto tools = waitFor(client.listTools());
    if (tools.isError() || tools.value().size() != 2) ++failures;
    const auto toolResult = waitFor(client.callTool(
        QStringLiteral("echo"),
        QJsonArray{QJsonObject{{QStringLiteral("value"), 42}}}));
    if (toolResult.isError() || toolResult.value().size() != 1) ++failures;
    const auto invalidToolResult = waitFor(client.callTool(
        QStringLiteral("required-value"),
        QJsonArray{QJsonObject{}}));
    if (invalidToolResult.isSuccess()) ++failures;
    const auto resources = waitFor(client.listResources());
    if (resources.isError() || resources.value().size() != 1) ++failures;
    const auto contents = waitFor(client.readResource(QStringLiteral("test://status")));
    if (contents.isError() || contents.value().size() != 1) ++failures;
    const auto templates = waitFor(client.listResourceTemplates());
    if (templates.isError() || templates.value().size() != 1) ++failures;
    const auto templatedContents = waitFor(
        client.readResource(QStringLiteral("test://users/7")));
    if (templatedContents.isError() || templatedContents.value().size() != 1)
        ++failures;
    const auto prompts = waitFor(client.listPrompts());
    if (prompts.isError() || prompts.value().size() != 1) ++failures;
    const auto promptMessages = waitFor(client.getPrompt(
        QStringLiteral("greet"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("Qt")}}));
    if (promptMessages.isError() || promptMessages.value().size() != 1)
        ++failures;

    waitFor(client.stop());
    waitFor(server.stop());

    if (failures == 0) std::cout << "All LibMcp tests passed\n";
    return failures == 0 ? 0 : 1;
}
