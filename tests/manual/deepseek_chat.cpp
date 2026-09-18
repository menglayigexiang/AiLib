#include <AiLib/client/LLMClientFactory.h>
#include <QCoreApplication>
#include <QTextStream>
#include <QJsonArray>
#include <algorithm>

using namespace AiLib;
namespace {
bool runChat(LLMClient& client, const ChatRequest& request, ChatResponse& output, QTextStream& log)  // 执行一次真实请求，只输出不含凭据的结果摘要
{
    SdkError error;  // SDK 流程故障，不打印原始请求或服务端正文
    if (!client.chat(request, output, error)) {
        log << "request_failed http=" << error.httpStatus.value_or(0) << " code=" << error.code << '\n';
        return false;
    }
    log << "request_ok text_chars=" << output.message.text().size() << " tool_calls=" << output.message.toolCalls().size()
        << " input_tokens=" << output.usage.inputTokens.value_or(-1) << " output_tokens=" << output.usage.outputTokens.value_or(-1) << '\n';
    return output.completionState == CompletionState::Complete && output.finishReason != FinishReason::Length;
}
}  // 内部测试辅助函数命名空间结束

int main(int argc, char* argv[])  // 手动验证 DeepSeek OpenAI Compatible 普通 Chat 和一次无副作用工具往返
{
    QCoreApplication application(argc, argv);  // 当前线程的 Qt 网络运行环境
    QTextStream log(stdout);                   // 只输出测试结果摘要
    ProviderConfig config;                     // 凭据只从调用方环境读取，不保存在代码中
    config.protocol = ProtocolType::OpenAIChatCompletions;
    config.baseUrl = QUrl(QStringLiteral("https://api.deepseek.com"));
    config.apiKey = QString::fromUtf8(qgetenv("DEEPSEEK_API_KEY"));
    config.customHeaders.insert(QStringLiteral("User-Agent"), QStringLiteral("AiLib/0.1.0 (SDK integration test)"));
    if (config.apiKey.isEmpty()) { log << "missing DEEPSEEK_API_KEY\n"; return 2; }
    int streamEvents = 0;  // 本次手动验证中收到的标准化事件数
    RequestOptions defaults;  // 普通和流式调用共用的 Client 默认选项
    defaults.streamCallback = [&](const StreamEvent&) {  // 只计数通知，不承担最终响应聚合
        ++streamEvents;
    };
    SdkError error;                     // Factory 配置故障
    std::unique_ptr<LLMClient> client;  // 独占实际 Adapter 和 Qt Transport 的测试 Client
    if (!LLMClientFactory::create(config, client, error, defaults)) return 3;
    ChatRequest chat;  // 先验证普通编程问题
    chat.model = QStringLiteral("deepseek-flash");
    chat.maxOutputTokens = 1024;
    chat.stream = application.arguments().contains(QStringLiteral("--stream"));
    chat.extraParameters = {{"thinking", QJsonObject{{"type", "disabled"}}}};
    chat.messages = {Message::user(QStringLiteral("C++17 中 std::unique_ptr 是什么？用一句中文回答。"))};
    ChatResponse output;  // 每次请求独立替换的结果
    if (!runChat(*client, chat, output, log) || output.message.text().isEmpty()) return 4;
    FunctionToolDefinition tool;  // 无副作用的测试函数定义
    tool.name = QStringLiteral("add");
    tool.description = QStringLiteral("Add two integers for a C++ unit test.");
    tool.inputSchema = {{"type", "object"}, {"properties", QJsonObject{{"a", QJsonObject{{"type", "integer"}}}, {"b", QJsonObject{{"type", "integer"}}}}}, {"required", QJsonArray{"a", "b"}}};
    chat.tools = {tool};
    chat.toolChoice.mode = ToolChoiceMode::Specific;
    chat.toolChoice.toolName = tool.name;
    chat.messages = {Message::user(QStringLiteral("请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。"))};
    if (!runChat(*client, chat, output, log) || output.message.toolCalls().size() != 1) return 5;
    const ToolCall call = output.message.toolCalls().first();  // 已完整生成的测试调用
    if (call.name != tool.name || call.arguments.value("a").toInt(-1) != 19 || call.arguments.value("b").toInt(-1) != 23) return 6;
    if (std::any_of(output.message.contents.cbegin(), output.message.contents.cend(), [](const MessageContent& part) { return std::holds_alternative<ReasoningContent>(part); })) {  // 检测无签名可回传的推理块
        log << "unexpected_reasoning_history\n";
        return 7;
    }
    chat.messages.append(output.message);
    Message result;  // 测试程序人工执行加法得到的工具结果，不代表 Agent 已实现
    result.role = Role::Tool;
    result.contents.append(ToolResultContent{ToolResult{call.id, call.name, true, QJsonObject{{"sum", 42}}, {}, {}}});
    chat.messages.append(result);
    chat.toolChoice.mode = ToolChoiceMode::Auto;
    if (!runChat(*client, chat, output, log) || !output.message.toolCalls().isEmpty() || !output.message.text().contains(QStringLiteral("42"))) return 8;
    log << "plain_chat_and_tool_round_trip_passed stream=" << chat.stream << " events=" << streamEvents << '\n';
    return 0;
}
