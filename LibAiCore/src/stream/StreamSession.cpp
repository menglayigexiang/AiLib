#include <AiLib/stream/StreamSession.h>
#include <QJsonDocument>
#include <QJsonParseError>

namespace AiLib {
StreamSession::StreamSession()  // 初始化请求级助手响应，默认未完成
{
    m_response.message.role = Role::Assistant;
    m_response.message.status = MessageStatus::Incomplete;
}
bool StreamSession::apply(const StreamEvent& event,  // 标准化流事件
                          SdkError& error)           // 聚合事件并拒绝非法 Part 状态转换
{
    const auto invalid = [&](const QString& message) {  // 保存标准化事件契约故障
        error = {};
        error.category = ErrorCategory::Protocol;
        error.code = QStringLiteral("InvalidStreamEvent");
        error.message = message;
        error.details.insert(QStringLiteral("partIndex"), event.partIndex);
        fail(error);
        return false;
    };
    if (m_response.completionState == CompletionState::Complete)
        return invalid(QStringLiteral("Events arrived after response completion"));
    const bool partEvent =  // 判断是否需要逻辑 Part 索引
        event.type == StreamEventType::PartStarted || event.type == StreamEventType::TextDelta ||
        event.type == StreamEventType::ReasoningDelta ||
        event.type == StreamEventType::ToolCallDelta ||
        event.type == StreamEventType::PartCompleted;
    if ((partEvent && event.partIndex < 0) || (!partEvent && event.partIndex != -1))
        return invalid(QStringLiteral("Part and response event indices do not match their scope"));
    if (event.type == StreamEventType::PartStarted) {
        if (m_parts.contains(event.partIndex))
            return invalid(QStringLiteral("Part index already exists"));
        if (event.partType != StreamPartType::Text && event.partType != StreamPartType::Reasoning &&
            event.partType != StreamPartType::ToolCall)
            return invalid(QStringLiteral("Unknown Part type"));
        Part part;  // 新建的内部聚合状态
        part.type = event.partType;
        m_parts.insert(event.partIndex, part);
        m_order.append(event.partIndex);
        return true;
    }
    if (partEvent) {
        auto it = m_parts.find(event.partIndex);  // 当前事件对应的请求级 Part
        if (it == m_parts.end() || it->completed)
            return invalid(QStringLiteral("Part is missing or already completed"));
        Part& part = it.value();  // 需要更新的内容聚合状态
        if (event.type == StreamEventType::PartCompleted) {
            if (part.type == StreamPartType::ToolCall) {
                if (part.callId.trimmed().isEmpty() || part.toolName.trimmed().isEmpty() ||
                    m_callIds.contains(part.callId))
                    return invalid(QStringLiteral(
                        "ToolCall IDs must be nonempty and unique; tool names must be nonempty"));
                QJsonParseError parseError;  // 完整工具参数的 JSON 解析故障
                const QJsonDocument document =
                    QJsonDocument::fromJson(  // 完整参数树，禁止宽松截断解析
                        part.text.toUtf8(), &parseError);
                part.argumentsValid =
                    parseError.error == QJsonParseError::NoError && document.isObject();
                if (!part.argumentsValid && m_response.finishReason != FinishReason::Unknown &&
                    m_response.finishReason != FinishReason::Length)
                    return invalid(
                        QStringLiteral("Completed ToolCall arguments must be a JSON object"));
                if (part.argumentsValid)
                    part.arguments = document.object();
                m_callIds.insert(part.callId);
            }
            part.completed = true;
            return true;
        }
        if (event.type == StreamEventType::ToolCallDelta) {
            if (part.type != StreamPartType::ToolCall)
                return invalid(QStringLiteral("Tool delta belongs to a different Part type"));
            if (event.toolCallId) {
                if (!part.callId.isEmpty() && part.callId != *event.toolCallId)
                    return invalid(QStringLiteral("ToolCall ID changed within a Part"));
                part.callId = *event.toolCallId;
            }
            part.toolName += event.toolNameDelta;
        } else if ((event.type == StreamEventType::TextDelta &&
                    part.type != StreamPartType::Text) ||
                   (event.type == StreamEventType::ReasoningDelta &&
                    part.type != StreamPartType::Reasoning)) {
            return invalid(QStringLiteral("Text delta belongs to a different Part type"));
        }
        part.text += event.delta;
        return true;
    }
    switch (event.type) {
    case StreamEventType::ResponseMetadataUpdated:
        if (!m_response.id.isEmpty() && !event.responseId.isEmpty() &&
            m_response.id != event.responseId)
            return invalid(QStringLiteral("Response ID changed"));
        if (!event.responseId.isEmpty())
            m_response.id = event.responseId;
        if (!m_response.model.isEmpty() && !event.model.isEmpty() &&
            m_response.model != event.model)
            return invalid(QStringLiteral("Response model changed"));
        if (!event.model.isEmpty())
            m_response.model = event.model;
        break;
    case StreamEventType::UsageUpdated:
        if (event.usage.inputTokens)
            m_response.usage.inputTokens = event.usage.inputTokens;
        if (event.usage.outputTokens)
            m_response.usage.outputTokens = event.usage.outputTokens;
        if (event.usage.totalTokens)
            m_response.usage.totalTokens = event.usage.totalTokens;
        break;
    case StreamEventType::FinishReasonReceived:
        if (m_response.finishReason != FinishReason::Unknown &&
            m_response.finishReason != event.finishReason)
            return invalid(QStringLiteral("FinishReason changed"));
        m_response.finishReason = event.finishReason;
        break;
    case StreamEventType::ResponseCompleted:
        if (m_response.finishReason == FinishReason::Unknown)
            return invalid(QStringLiteral("Completed response is missing FinishReason"));
        if (m_response.finishReason == FinishReason::ToolCalls && m_callIds.isEmpty())
            return invalid(QStringLiteral("ToolCalls FinishReason requires completed calls"));
        for (auto it = m_parts.constBegin(); it != m_parts.constEnd();
             ++it) {  // 当前 Part 的完整性状态
            if (it->type == StreamPartType::ToolCall &&
                m_response.finishReason == FinishReason::Length)
                continue;
            if (!it->completed)
                return invalid(QStringLiteral("Response contains unfinished Parts"));
            if (it->type == StreamPartType::ToolCall && !it->argumentsValid)
                return invalid(
                    QStringLiteral("Completed ToolCall arguments must be a JSON object"));
        }
        m_response.completionState = CompletionState::Complete;
        m_response.message.status = m_response.finishReason == FinishReason::Length
                                        ? MessageStatus::Incomplete
                                        : MessageStatus::Complete;
        break;
    case StreamEventType::Error:
        error = event.error;
        fail(error);
        return false;
    default:
        return invalid(QStringLiteral("Unknown response event"));
    }
    return true;
}
ChatResponse StreamSession::response() const  // 按首次出现顺序生成包含有效内容的值快照
{
    ChatResponse output = m_response;       // 响应级数据副本
    for (const int index : m_order) {       // 当前逻辑内容的出现顺序
        const Part& part = m_parts[index];  // 只读聚合状态
        if (part.type == StreamPartType::Text && !part.text.isEmpty())
            output.message.contents.append(TextContent{part.text});
        else if (part.type == StreamPartType::Reasoning && !part.text.isEmpty())
            output.message.contents.append(ReasoningContent{part.text});
        else if (part.type == StreamPartType::ToolCall && part.completed && part.argumentsValid)
            output.message.contents.append(
                ToolCallContent{ToolCall{part.callId, part.toolName, part.arguments}});
    }
    return output;
}
void StreamSession::fail(const SdkError& error)  // 标记故障，不删除已聚合数据
{
    m_error = error;
    m_response.completionState = CompletionState::Incomplete;
    m_response.message.status = MessageStatus::Incomplete;
}
const SdkError& StreamSession::error() const  // 返回流程故障的只读引用
{
    return m_error;
}
}  // AiLib 命名空间结束
