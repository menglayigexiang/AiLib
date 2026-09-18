#include <AiLib/agent/AgentRequest.h>
#include <AiLib/agent/AgentResult.h>
#include <AiLib/client/ChatResponse.h>
#include <AiLib/core/ModelInfo.h>
#include <AiLib/provider/ProviderConfig.h>
#include <AiLib/tools/ToolErrorCodes.h>
#include <QtTest>

using namespace AiLib;
class PublicModelsTest : public QObject {
    Q_OBJECT
private slots:
    void requestDefaultsAndCopies()  // 验证公共配置默认值、未设置参数及值复制语义
    {
        AgentRequest request;  // 用于验证默认配置和历史复制的运行请求
        QCOMPARE(request.limits.maxTurns, 20);
        QCOMPARE(request.limits.maxToolCalls, -1);
        QCOMPARE(request.limits.totalTimeoutSeconds, 600);
        QCOMPARE(request.limits.llmTimeoutSeconds, 120);
        QCOMPARE(request.requestOptions.timeoutSeconds, 120);
        QCOMPARE(request.requestOptions.retryPolicy.maxRetries, 5);
        QCOMPARE(request.requestOptions.retryPolicy.retryIntervalMs, 30000);
        QVERIFY(request.enabledTools.isEmpty());
        QVERIFY(!request.chat.temperature.has_value());
        request.chat.temperature = 0.0;
        QVERIFY(request.chat.temperature.has_value());
        QCOMPARE(*request.chat.temperature, 0.0);
        request.chat.messages.append(Message::user(QStringLiteral("输入")));
        auto copy = request;  // 用于验证修改副本不影响原请求的值副本
        copy.chat.messages[0] = Message::user(QStringLiteral("修改"));
        QCOMPARE(request.chat.messages.at(0).text(), QStringLiteral("输入"));
    }
    void unknownIsNotZeroOrFalse()  // 验证用量未知与零、能力未知与不支持的区别
    {
        Usage usage;  // 用于区分未报告用量和明确零值的统计对象
        QVERIFY(!usage.inputTokens.has_value());
        usage.inputTokens = 0;
        QVERIFY(usage.inputTokens.has_value());
        QCOMPARE(*usage.inputTokens, qint64(0));
        QVERIFY(!usage.totalTokens.has_value());
        ModelInfo model;  // 用于验证能力提示三态及枚举哈希的模型描述
        QVERIFY(!model.capabilities.contains(Capability::ImageInput));
        model.capabilities.insert(Capability::ImageInput, std::nullopt);
        QVERIFY(!model.capabilities.value(Capability::ImageInput).has_value());
        model.capabilities[Capability::ImageInput] = false;
        QVERIFY(model.capabilities.value(Capability::ImageInput).has_value());
        QVERIFY(!*model.capabilities.value(Capability::ImageInput));
    }
    void completionIsIndependentFromReason()  // 验证协议完整性、模型结束原因和消息完整性分离
    {
        ChatResponse response;  // 构造正常传输但输出截断的响应模型
        response.completionState = CompletionState::Complete;
        response.finishReason = FinishReason::Length;
        response.message.status = MessageStatus::Incomplete;
        QCOMPARE(response.completionState, CompletionState::Complete);
        QCOMPARE(response.finishReason, FinishReason::Length);
        QCOMPARE(response.message.status, MessageStatus::Incomplete);
        AgentResult result;  // 用于验证停止原因和增量消息的运行结果
        QVERIFY(!result.finalMessage.has_value());
        result.finishReason = AgentFinishReason::Length;
        result.newMessages.append(response.message);
        QCOMPARE(result.newMessages.size(), 1);
    }
    void toolFailuresAreSeparateFromSdkErrors()  // 验证工具业务失败不等同 SDK 流程故障
    {
        ToolResult result;  // 用于验证结果包装或工具失败语义的业务结果
        result.success = false;
        result.errorCode = ToolErrorCodes::ApprovalDenied;
        result.errorMessage = QStringLiteral("用户拒绝");
        SdkError error;  // 本次操作的流程错误输出或默认无错误状态
        QCOMPARE(error.category, ErrorCategory::None);
        QCOMPARE(result.errorCode, QStringLiteral("ApprovalDenied"));
        FunctionToolDefinition definition;  // 用于检查纯声明性工具策略默认值的定义
        QCOMPARE(definition.approvalPolicy, ToolApprovalPolicy::Never);
        QCOMPARE(definition.concurrency, ToolConcurrency::Serialized);
        ProviderConfig provider;  // 用于检查显式协议选择默认值的服务配置
        QCOMPARE(provider.protocol, ProtocolType::OpenAIChatCompletions);
    }
};
QTEST_GUILESS_MAIN(PublicModelsTest)
#include "tst_PublicModels.moc"
