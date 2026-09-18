#include <AiLib/agent/Agent.h>
#include <AiLib/tools/ToolErrorCodes.h>
#include <QUuid>
#include <QSet>
#include <limits>

namespace AiLib {
namespace {
// 保证所有返回路径都释放实例运行标记。
struct RunningGuard {
    std::atomic_bool& flag;  // 借用 Agent 的运行标记
    ~RunningGuard()          // 离开本次运行时恢复可调用状态
    {
        flag.store(false);
    }
};
bool failure(SdkError& error,         // 异常故障输出
             ErrorCategory category,  // 故障分类
             const QString& code,     // 稳定错误码
             const QString& message)  // 设置异常失败，AgentResult 默认保持 Failed
{
    error = {};
    error.category = category;
    error.code = code;
    error.message = message;
    return false;
}
bool notify(const AgentCallback& callback,  // 应用同步事件接收器
            const AgentEvent& event,        // 本次标准化事件
            SdkError& error)                // 捕获应用事件接收器异常，不丢失已产生消息
{
    if (!callback)
        return true;
    try {
        callback(event);
    } catch (...) {
        return failure(error, ErrorCategory::Internal, QStringLiteral("AgentCallbackFailed"),
                       QStringLiteral("Agent 事件回调抛出异常"));
    }
    return true;
}
bool stopRequested(const ToolExecutionContext& context,  // 来源和停止上下文
                   AgentResult& result)                  // 将主动取消和总截止时间转换为正常结束原因
{
    if (context.cancellation.isCancellationRequested()) {
        result.finishReason = AgentFinishReason::Cancelled;
        return true;
    }
    if (context.isTimedOut()) {
        result.finishReason = AgentFinishReason::Timeout;
        return true;
    }
    return false;
}
void addUsageField(std::optional<qint64>& total,        // 当前字段累计输出
                   const std::optional<qint64>& value)  // 某轮未知或加法溢出时累计值保持未知
{
    if (!total || !value || *value < 0 || *total > std::numeric_limits<qint64>::max() - *value) {
        total.reset();
        return;
    }
    *total += *value;
}
void addUsage(AgentResult& result, const Usage& usage)  // 累计每轮的已报告用量，不推算未知字段
{
    result.turnUsages.append(usage);
    addUsageField(result.totalUsage.inputTokens, usage.inputTokens);
    addUsageField(result.totalUsage.outputTokens, usage.outputTokens);
    addUsageField(result.totalUsage.totalTokens, usage.totalTokens);
}
void appendAssistant(const ChatResponse& response,  // 本轮模型响应
                     AgentResult& result,           // 本次运行的增量和结束原因
                     ChatRequest& chat)             // 保存完整消息或有效部分内容，不制造空的部分消息
{
    Message message = response.message;  // 保留规范化内容的有序副本
    const bool incomplete =              // 协议是否完整获得
        response.completionState != CompletionState::Complete;
    if (incomplete && message.contents.isEmpty())
        return;
    if (incomplete)
        message.status = MessageStatus::Incomplete;
    result.newMessages.append(message);
    chat.messages.append(message);
}
}
Agent::Agent(std::unique_ptr<LLMClient> client,        // 转移模型客户端所有权
             ToolRegistry& registry,                   // 应用拥有的注册表
             IToolApprovalProvider* approvalProvider,  // 可空的应用确认策略
             QString id)                               // 接收 Client 所有权并借用应用工具组件
    : m_id(id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : std::move(id)),
      m_client(std::move(client)), m_registry(registry), m_toolExecutor(registry, approvalProvider)
{
}
QString Agent::id() const  // 返回实例身份副本
{
    return m_id;
}
bool Agent::run(const AgentRequest& request,  // 本次运行的输入历史和配置
                AgentResult& result,          // 本次运行的增量和结束原因
                SdkError& error)              // 在当前线程同步完成模型与工具循环
{
    result = {};
    error = {};
    if (m_running.exchange(true))
        return failure(error, ErrorCategory::Configuration, QStringLiteral("AgentBusy"),
                       QStringLiteral("同一 Agent 不允许并发或重入运行"));
    const auto started = std::chrono::steady_clock::now();  // 总运行时间包含配置校验和事件等待
    RunningGuard guard{m_running};                          // 任何返回路径都复位运行标记
    result.totalUsage = Usage{qint64(0), qint64(0), qint64(0)};
    if (!m_client)
        return failure(error, ErrorCategory::Configuration, QStringLiteral("MissingClient"),
                       QStringLiteral("Agent 未提供模型客户端"));
    if (request.limits.maxTurns < 1 || request.limits.maxToolCalls < -1 ||
        (request.limits.totalTimeoutSeconds != -1 && request.limits.totalTimeoutSeconds < 1) ||
        (request.limits.llmTimeoutSeconds != -1 && request.limits.llmTimeoutSeconds < 1)) {
        return failure(error, ErrorCategory::Configuration, QStringLiteral("InvalidAgentLimits"),
                       QStringLiteral("Agent 轮数和时间限制非法"));
    }
    ChatRequest chat = request.chat;  // 私有的本次运行历史，不修改应用输入
    chat.tools.clear();
    QSet<QString> enabled;                           // 执行阶段共用的严格名称白名单
    for (const auto& name : request.enabledTools) {  // 开始前检查所有工具名称
        FunctionToolDefinition definition;           // Registry 提供的唯一工具描述来源
        if (!m_registry.definition(name, definition))
            return failure(error, ErrorCategory::Configuration,
                           QStringLiteral("UnknownEnabledTool"),
                           QStringLiteral("工具白名单包含未注册名称：") + name);
        if (!enabled.contains(name))
            chat.tools.append(definition);
        enabled.insert(name);
    }
    ToolExecutionContext context;  // 传递身份及唯一取消来源，不绑定执行器
    context.agentId = m_id;
    context.cancellation = request.requestOptions.cancellation;
    if (request.limits.totalTimeoutSeconds != -1)
        context.deadline = started + std::chrono::seconds(request.limits.totalTimeoutSeconds);
    qint64 executedToolCalls = 0;                                 // 所有工具实际 Handler 执行次数，失败也计数
    for (int turn = 0; turn < request.limits.maxTurns; ++turn) {  // 每轮代表一次逻辑 LLM 调用
        if (stopRequested(context, result))
            return true;
        RequestOptions options = request.requestOptions;  // 每轮局部覆盖，不改 Client 或调用方配置
        options.timeoutSeconds = request.limits.llmTimeoutSeconds;
        options.retryPolicy = request.limits.llmRetryPolicy;
        if (context.deadline) {
            if (!options.deadline || *context.deadline < *options.deadline)
                options.deadline = context.deadline;
            const auto remaining =  // 当前单调时钟剩余时间
                *context.deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                result.finishReason = AgentFinishReason::Timeout;
                return true;
            }
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     remaining)  // 秒级 Transport 接口的剩余整秒
                                     .count();
            const int timeout =  // 向上取整，亚秒预算仍能发起请求
                static_cast<int>(seconds + (remaining > std::chrono::seconds(seconds) ? 1 : 0));
            if (options.timeoutSeconds == -1 || timeout < options.timeoutSeconds)
                options.timeoutSeconds = timeout;
        }
        ChatResponse response;  // 完整或部分有效的模型输出
        const bool received =   // Streaming 仅聚合与通知，尚不执行工具
            m_client->chat(chat, response, error, options);
        appendAssistant(response, result, chat);
        addUsage(result, response.usage);
        if (stopRequested(context, result)) {
            error = {};
            return true;
        }
        if (!received) {
            if (error.category == ErrorCategory::Cancelled) {
                result.finishReason = AgentFinishReason::Cancelled;
                error = {};
                return true;
            }
            return false;
        }
        if (response.completionState != CompletionState::Complete ||
            response.message.role != Role::Assistant)
            return failure(error, ErrorCategory::Protocol,
                           QStringLiteral("InvalidAssistantResponse"),
                           QStringLiteral("Client 成功但未提供完整 Assistant 响应"));
        if (response.finishReason == FinishReason::Length) {
            result.finishReason = AgentFinishReason::Length;
            return true;
        }
        const auto calls = response.message.toolCalls();  // 只消费完整响应中规范化后的全部调用
        if (calls.isEmpty()) {
            result.finishReason = AgentFinishReason::Completed;
            result.finalMessage = response.message;
            return true;
        }
        QSet<QString> ids;                // 当前响应范围内的调用 ID 集合
        for (const auto& call : calls) {  // 批量检查 ID，任何非法关联都不执行本批工具
            if (call.id.isEmpty() || ids.contains(call.id))
                return failure(error, ErrorCategory::Protocol, QStringLiteral("InvalidToolCallId"),
                               QStringLiteral("ToolCall ID 为空或在当前响应内重复"));
            ids.insert(call.id);
        }
        if (turn + 1 == request.limits.maxTurns) {
            result.finishReason = AgentFinishReason::MaxTurns;
            return true;
        }
        if (request.limits.maxToolCalls != -1 &&
            calls.size() > request.limits.maxToolCalls - executedToolCalls) {
            result.finishReason = AgentFinishReason::MaxToolCalls;
            return true;
        }
        for (const auto& call : calls) {  // 顺序执行本轮调用，结果顺序与模型调用一致
            if (stopRequested(context, result))
                return true;
            AgentEvent event;  // 模型完整生成后开始客户端工具执行流程
            event.agentId = m_id;
            event.call = call;
            if (!notify(request.callback, event, error))
                return false;
            ToolResult toolResult;         // 反馈模型的统一工具结果
            bool handlerExecuted = false;  // 工具是否已经实际进入业务执行
            bool executed = true;          // 工具流程是否正常返回
            if (!enabled.contains(call.name)) {
                toolResult.callId = call.id;
                toolResult.toolName = call.name;
                toolResult.success = false;
                toolResult.errorCode = ToolErrorCodes::ToolNotEnabled;
                toolResult.errorMessage = QStringLiteral("工具不在本次运行白名单中");
            } else {
                executed =
                    m_toolExecutor.execute(call, context, toolResult, error, handlerExecuted);
            }
            if (handlerExecuted)
                ++executedToolCalls;
            if (executed || handlerExecuted) {
                Message message;  // 一个实际工具结果对应一条工具消息
                message.role = Role::Tool;
                message.contents.append(ToolResultContent{toolResult});
                result.newMessages.append(message);
                chat.messages.append(message);
            }
            event.type = AgentEventType::ToolExecutionFinished;
            event.result = toolResult;
            event.handlerExecuted = handlerExecuted;
            if (!notify(request.callback, event, error))
                return false;
            if (!executed) {
                if (error.category == ErrorCategory::Cancelled ||
                    error.category == ErrorCategory::Timeout) {
                    result.finishReason = error.category == ErrorCategory::Cancelled
                                              ? AgentFinishReason::Cancelled
                                              : AgentFinishReason::Timeout;
                    error = {};
                    return true;
                }
                return false;
            }
        }
    }
    result.finishReason = AgentFinishReason::MaxTurns;
    return true;
}
}  // AiLib 命名空间结束
