#include "../../examples/support/DemoSupport.h"
#include <QCoreApplication>
#include <QTextStream>

// 手动验收的同步取消策略，记录工具确认是否实际发生。
class CancelApproval final : public AiLib::IToolApprovalProvider {
public:
    bool requestApproval(const AiLib::ToolCall& call,                      // 完整的调用关联
                         const AiLib::FunctionToolDefinition& definition,  // 已注册工具描述
                         const AiLib::ToolExecutionContext& context,       // Agent 来源和停止上下文
                         AiLib::ToolApprovalResult& result,                // 输出取消决定
                         AiLib::SdkError& error) override                  // 取消整个 Run，不执行 Handler
    {
        Q_UNUSED(definition)
        Q_UNUSED(error)
        ++requests;
        associated = !call.id.isEmpty() && context.agentId == "live-cancel-agent";
        result.decision = AiLib::ToolApprovalDecision::Cancel;
        return true;
    }
    int requests = 0;         // 实际触发的确认次数
    bool associated = false;  // 确认是否携带有效 Agent 和 ToolCall 身份
};
int main(int argc, char* argv[])  // 用显式 Provider 参数执行真实取消验收，不加入 CTest
{
    QCoreApplication application(argc, argv);                               // 当前线程 Qt 网络环境
    QTextStream log(stdout);                                                // 只输出不含凭据的验收摘要
    const QString provider = application.arguments().value(1, "deepseek");  // 待验收服务
    AiLib::SdkError error;                                                  // SDK 流程故障
    QString model;                                                          // 当前模型名称
    std::unique_ptr<AiLib::LLMClient> client;                               // 验收用的独占 Client
    if (!Demo::createClient(provider, client, model, error)) {
        log << "factory_failed code=" << error.code << '\n';
        return 2;
    }
    AiLib::ToolRegistry registry;  // 应用拥有的本地工具
    if (!Demo::registerTools(registry, error))
        return 3;
    CancelApproval approval;  // 应用实现的同步取消决定
    AiLib::Agent agent(  // 借用工具和确认策略
        std::move(client), registry, &approval, "live-cancel-agent");
    AiLib::AgentRequest request;              // 当前取消工具测试，不修改普通 Auto 使用方式
    request.chat =
        Demo::chatRequest(model, {AiLib::Message::user("请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。")}, true);
    request.chat.toolChoice.mode = AiLib::ToolChoiceMode::Specific;
    request.chat.toolChoice.toolName = "add";
    request.enabledTools = {QStringLiteral("add")};
    request.limits.llmRetryPolicy.maxRetries = 0;
    request.limits.totalTimeoutSeconds = 45;
    int handlerExecutions = 0;                                // 实际进入 Handler 的执行次数
    request.callback = [&](const AiLib::AgentEvent& event) {  // 只记录副作用执行边界
        if (event.type == AiLib::AgentEventType::ToolExecutionFinished && event.handlerExecuted)
            ++handlerExecutions;
    };
    AiLib::AgentResult result;                    // 本轮已实际产生的消息
    bool ok = agent.run(request, result, error);  // 强制调用工具，仅用于 Cancel 场景，无下一轮请求
    const bool approvalPassed =                   // 确认和关联必须实际发生
        ok && result.finishReason == AiLib::AgentFinishReason::Cancelled &&
        approval.requests == 1 && approval.associated && handlerExecutions == 0 &&
        result.newMessages.size() == 1 &&
        result.newMessages.first().toolCalls().size() == 1;
    log << "approval_cancel passed=" << approvalPassed
        << " reason=" << Demo::finishName(result.finishReason) << " approvals=" << approval.requests
        << " handlers=" << handlerExecutions << " code=" << error.code << '\n'
        << Qt::flush;
    request = {};
    request.chat = Demo::chatRequest(
        model,
        {AiLib::Message::user("请详细介绍 C++17 的十个特性，每个特性至少五句话，不要省略说明。")},
        true);
    request.limits.llmRetryPolicy.maxRetries = 0;
    request.limits.totalTimeoutSeconds = 45;
    AiLib::CancellationSource cancellation;  // 唯一主动取消来源
    request.requestOptions.cancellation = cancellation.token();
    int textCharacters = 0;  // 取消之前已经生成的有效文字长度
    request.requestOptions.streamCallback =
        [&](const AiLib::StreamEvent& event) {  // 收到有效文字后主动取消正在传输的请求
            if (event.type == AiLib::StreamEventType::TextDelta) {
                textCharacters += event.delta.size();
                if (textCharacters >= 5)
                    cancellation.cancel();
            }
        };
    ok = agent.run(request, result, error);
    const bool streamPassed = ok && result.finishReason == AiLib::AgentFinishReason::Cancelled &&  // 取消须保留有效的未完整消息
                              textCharacters >= 5 && result.newMessages.size() == 1 &&
                              !result.newMessages.first().text().isEmpty() &&
                              result.newMessages.first().status ==
                                  AiLib::MessageStatus::Incomplete;
    log << "stream_cancel passed=" << streamPassed
        << " reason=" << Demo::finishName(result.finishReason) << " text_chars=" << textCharacters
        << " new_messages=" << result.newMessages.size() << " code=" << error.code << '\n';
    return approvalPassed && streamPassed ? 0 : 5;
}
