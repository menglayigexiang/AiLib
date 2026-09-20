#pragma once
#include <AiLib/agent/Agent.h>
#include <AiLib/client/LLMClientFactory.h>
#include "BuiltinCatalog.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <mutex>

namespace Demo {
inline bool ensureBuiltinCatalog(AiLib::SdkError& error)  // 每个进程只注册一次内置模型目录
{
    static std::once_flag once;       // 保护首次注册
    static bool success = false;      // 首次注册结果
    static AiLib::SdkError failure;   // 首次注册错误副本
    std::call_once(once, [&]() {  // 在并发入口下只执行一次注册
        success = registerBuiltinModels(AiLib::ModelRegistry::instance(), failure);
    });
    error = failure;
    return success;
}

inline bool createClient(const QString& providerId,                 // 已注册 Provider 的稳定标识
                         const QString& modelId,                    // 本次调用的模型标识，可不在目录中
                         const QString& apiKey,                     // 调用方提供的运行时认证密钥
                         std::unique_ptr<AiLib::LLMClient>& client, // 输出独占 Client
                         AiLib::SdkError& error)                    // 从目录模板创建真实服务 Client
{
    const auto entry = AiLib::ModelRegistry::instance().findProvider(providerId);  // Provider 目录值副本
    if (!entry) {
        error = {};
        error.category = AiLib::ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("UnknownDemoProvider");
        return false;
    }
    if (modelId.trimmed().isEmpty()) {
        error = {};
        error.category = AiLib::ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("MissingModelId");
        return false;
    }
    if (apiKey.isEmpty()) {
        error = {};
        error.category = AiLib::ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("MissingApiKey");
        return false;
    }
    AiLib::ProviderConfig config = entry->configTemplate;  // 注入凭据前的无秘密模板副本
    config.apiKey = apiKey;
    return AiLib::LLMClientFactory::create(config, client, error);
}

inline bool registerTools(AiLib::ToolRegistry& registry,  // 非拥有的应用注册表
                          AiLib::SdkError& error)         // 注册需要确认的无副作用加法工具
{
    AiLib::FunctionToolDefinition definition;  // 纯工具描述
    definition.name = QStringLiteral("add");
    definition.description =
        QStringLiteral("Add two integers. Use a=19 and b=23 to verify the SDK example.");
    definition.approvalPolicy = AiLib::ToolApprovalPolicy::Always;
    definition.inputSchema = {{"type", "object"},
                              {"properties", QJsonObject{{"a", QJsonObject{{"type", "integer"}}},
                                                         {"b", QJsonObject{{"type", "integer"}}}}},
                              {"required", QJsonArray{"a", "b"}}};
    return registry.registerTool(
        definition,
        [](const AiLib::ToolCall& call,                 // 完整的业务参数
           const AiLib::ToolExecutionContext& context,  // 执行来源和停止上下文
           AiLib::ToolResult& result,                   // 输出业务计算结果
           AiLib::SdkError& failure) {                  // 执行业务计算，关联字段由 Executor 填写
            Q_UNUSED(context)
            Q_UNUSED(failure)
            result.data = QJsonObject{{"sum", call.arguments.value("a").toDouble() +
                                                  call.arguments.value("b").toDouble()}};
            return true;
        },
        error);
}
inline QString finishName(AiLib::AgentFinishReason reason)  // 将正常停止原因转换为示例展示名称
{
    switch (reason) {
    case AiLib::AgentFinishReason::Completed:
        return QStringLiteral("Completed");
    case AiLib::AgentFinishReason::Length:
        return QStringLiteral("Length");
    case AiLib::AgentFinishReason::Cancelled:
        return QStringLiteral("Cancelled");
    case AiLib::AgentFinishReason::MaxTurns:
        return QStringLiteral("MaxTurns");
    case AiLib::AgentFinishReason::MaxToolCalls:
        return QStringLiteral("MaxToolCalls");
    case AiLib::AgentFinishReason::Timeout:
        return QStringLiteral("Timeout");
    case AiLib::AgentFinishReason::Failed:
        return QStringLiteral("Failed");
    }
    return QStringLiteral("Failed");
}
inline AiLib::ChatRequest chatRequest(const QString& model,                  // 当前选定模型
                                      const QList<AiLib::Message>& history,  // 应用历史值对象
                                      bool stream)                           // 生成两种协议均可编码的示例请求
{
    AiLib::ChatRequest request;  // 本次请求的值对象
    request.model = model;
    request.messages = history;
    request.stream = stream;
    request.maxOutputTokens = 1024;
    if (model == "kimi-for-coding" || model == "deepseek-flash")
        request.extraParameters = {{"thinking", QJsonObject{{"type", "disabled"}}}};
    return request;
}
}  // 示例辅助命名空间结束
