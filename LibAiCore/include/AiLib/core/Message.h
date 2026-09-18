#pragma once

#include <AiLib/Export.h>
#include <AiLib/core/Content.h>
#include <QList>

namespace AiLib {
enum class Role { System, User, Assistant, Tool };
enum class MessageStatus { Complete, Incomplete };

struct AILIB_EXPORT Message {
    Role role = Role::User;                          // 消息发送者角色，默认用户
    QList<MessageContent> contents;                  // 唯一的有序内容容器，保留原始内容顺序
    MessageStatus status = MessageStatus::Complete;  // 消息内容是否完整，不表示终止原因

    static Message user(const QString& text);    // 使用给定文本构造用户消息
    static Message system(const QString& text);  // 使用给定文本构造系统消息
    // 普通文本按内容顺序拼接，不添加分隔符。
    QString text() const;               // 按顺序拼接普通文字，不插入分隔符或推理内容
    QList<ToolCall> toolCalls() const;  // 按内容顺序提取完整工具调用的值副本
};
}  // AiLib 命名空间结束
