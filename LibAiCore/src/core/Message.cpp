#include <AiLib/core/Message.h>

namespace AiLib {
Message Message::user(const QString& text)  // 以给定文本创建完整用户消息
{
    return Message{Role::User, {TextContent{text}}, MessageStatus::Complete};
}

Message Message::system(const QString& text)  // 以给定文本创建完整系统消息
{
    return Message{Role::System, {TextContent{text}}, MessageStatus::Complete};
}

QString Message::text() const  // 仅按原顺序汇总普通文本内容
{
    QString result;                                              // 累计普通文本，不包含推理或媒体内容
    for (const auto& part : contents) {                          // 当前按原始顺序遍历的内容块
        if (const auto* text = std::get_if<TextContent>(&part))  // 当前内容块的普通文本视图，其他类型为空
            result += text->text;
    }
    return result;
}

QList<ToolCall> Message::toolCalls() const  // 按原顺序提取已有完整工具调用
{
    QList<ToolCall> calls;                                           // 提取出的工具调用值副本
    for (const auto& part : contents) {                              // 当前按原始顺序遍历的内容块
        if (const auto* tool = std::get_if<ToolCallContent>(&part))  // 当前内容块的工具调用视图，其他类型为空
            calls.append(tool->call);
    }
    return calls;
}
}  // AiLib 命名空间结束
