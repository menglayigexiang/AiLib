#include "../support/DemoSupport.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTextStream>

// CLI 应用在当前线程读取确认；阻塞输入不能被令牌强制打断。
class CliApproval final : public AiLib::IToolApprovalProvider {
public:
    bool requestApproval(const AiLib::ToolCall& call,                      // 待执行的规范化调用
                         const AiLib::FunctionToolDefinition& definition,  // 工具纯描述
                         const AiLib::ToolExecutionContext& context,       // 来源和停止上下文
                         AiLib::ToolApprovalResult& result,                // 输出用户确认决定
                         AiLib::SdkError& error) override                  // 同步读取允许、拒绝或取消
    {
        Q_UNUSED(definition)
        Q_UNUSED(error)
        QTextStream output(stdout);  // 当前线程的提示输出
        QTextStream input(stdin);    // 当前线程的阻塞输入
        output << "\nApproval agent=" << context.agentId << " call=" << call.id
               << " tool=" << call.name
               << " args=" << QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)
               << "\n1 Allow / 2 Deny / 3 Cancel: " << Qt::flush;
        const QString answer = input.readLine().trimmed();  // 用户输入，EOF 默认拒绝
        result.decision = answer == "1"   ? AiLib::ToolApprovalDecision::Allow
                          : answer == "3" ? AiLib::ToolApprovalDecision::Cancel
                                          : AiLib::ToolApprovalDecision::Deny;
        return true;
    }
};
int main(int argc, char* argv[])  // 解析命令行参数，展示普通、流式和 Agent 调用
{
    QCoreApplication application(argc, argv);  // 由调用方提供 Qt 运行环境
    QCommandLineParser parser;                 // 命令行配置
    parser.setApplicationDescription("AiLib synchronous CLI example");
    parser.addHelpOption();
    parser.addOption({{"p", "provider"}, "deepseek / kimi / openai", "provider", "deepseek"});
    parser.addOption({{"M", "model"}, "Model ID; defaults to the first catalog model", "model"});
    parser.addOption({{"m", "mode"}, "chat / stream / agent", "mode", "agent"});
    parser.addOption({"prompt", "User input", "text", "请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。"});
    parser.addOption({"no-stream", "Disable streaming in Agent mode"});
    parser.process(application);
    QTextStream output(stdout);                 // 示例结果输出，不打印请求 Header 或凭据
    const QString mode = parser.value("mode");  // 本次演示模式
    if (mode != "chat" && mode != "stream" && mode != "agent")
        parser.showHelp(2);
    AiLib::SdkError error;                     // 本次流程错误
    if (!Demo::ensureBuiltinCatalog(error)) {
        output << error.code << '\n';
        return 2;
    }
    const QString providerId = parser.value("provider");  // 选定 Provider ID
    const auto providerEntry = AiLib::ModelRegistry::instance().findProvider(providerId);  // Provider 目录副本
    if (!providerEntry) {
        output << "UnknownDemoProvider\n";
        return 2;
    }
    QString model = parser.value("model").trimmed();  // 用户指定或目录默认模型 ID
    if (model.isEmpty() && !providerEntry->models.isEmpty())
        model = providerEntry->models.first().id;
    std::unique_ptr<AiLib::LLMClient> client;  // 将所有权移交给 Agent 或独立使用
    if (!Demo::createClient(providerId, model, Demo::demoApiKeyFromEnv(providerId), client, error)) {
        output << error.code << '\n';
        return 2;
    }
    QList<AiLib::Message> history{  // 应用拥有的完整历史
        AiLib::Message::user(parser.value("prompt"))};
    AiLib::RequestOptions options;                                   // 本次局部请求配置
    options.streamCallback = [&](const AiLib::StreamEvent& event) {  // 实时展示，不承担响应聚合
        if (event.type == AiLib::StreamEventType::TextDelta)
            output << event.delta << Qt::flush;
    };
    if (mode != "agent") {
        const auto request =  // 普通 Chat 的请求
            Demo::chatRequest(model, history, mode == "stream");
        AiLib::ChatResponse response;                                     // 完整或部分响应
        const bool ok = client->chat(request, response, error, options);  // 当前线程同步调用
        if (mode == "chat")
            output << response.message.text();
        output << '\n' << (ok ? "Complete" : error.code) << '\n';
        return ok ? 0 : 1;
    }
    AiLib::ToolRegistry registry;  // 应用独立管理的工具集合
    CliApproval approval;          // 应用提供的同步确认策略
    if (!Demo::registerTools(registry, error))
        return 2;
    AiLib::Agent agent(  // Agent 独占 Client，借用工具和确认策略
        std::move(client), registry, &approval, "cli-agent");
    AiLib::AgentRequest request;  // 本轮 Agent 配置
    request.chat = Demo::chatRequest(model, history, !parser.isSet("no-stream"));
    request.requestOptions = options;
    request.enabledTools = {QStringLiteral("add")};
    request.callback = [&](const AiLib::AgentEvent& event) {  // 工具执行事件与模型流事件分开展示
        output << '\n'
               << (event.type == AiLib::AgentEventType::ToolExecutionStarted ? "ToolStarted "
                                                                             : "ToolFinished ")
               << event.call.id;
        if (event.result)
            output << " success=" << event.result->success << " error=" << event.result->errorCode;
        output << '\n' << Qt::flush;
    };
    AiLib::AgentResult result;                          // 本轮实际产生的增量
    const bool ok = agent.run(request, result, error);  // 当前线程同步运行完整工具闭环
    if (!request.chat.stream && result.finalMessage)
        output << result.finalMessage->text();
    if (ok && result.finishReason == AiLib::AgentFinishReason::Completed)
        history.append(result.newMessages);
    output << '\n'
           << Demo::finishName(result.finishReason) << " newMessages=" << result.newMessages.size();
    if (!ok)
        output << " code=" << error.code;
    output << '\n';
    return ok ? 0 : 1;
}
