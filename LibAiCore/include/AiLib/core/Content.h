#pragma once

#include <AiLib/core/MediaResource.h>
#include <AiLib/tools/ToolCall.h>
#include <AiLib/tools/ToolResult.h>
#include <variant>

namespace AiLib {
struct TextContent {
    QString text;  // 当前内容块的文本，不与其他类型混合
};
struct ReasoningContent {
    QString text;  // 当前内容块的文本，不与其他类型混合
};
struct ImageContent {
    MediaResource resource;  // 媒体资源来源，与输入输出角色无关
    int width = -1;          // 图片宽度像素数，-1 表示未知
    int height = -1;         // 图片高度像素数，-1 表示未知
};
struct AudioContent {
    MediaResource resource;  // 媒体资源来源，与输入输出角色无关
};
struct VideoContent {
    MediaResource resource;  // 媒体资源来源，与输入输出角色无关
};
struct FileContent {
    MediaResource resource;  // 媒体资源来源，与输入输出角色无关
    QString fileName;        // 文件展示名称，未知时为空
};
struct ToolCallContent {
    ToolCall call;  // 包装已有工具调用，不复制调用字段
};
struct ToolResultContent {
    ToolResult result;  // 包装已有工具结果，不复制结果字段
};

using MessageContent = std::variant<TextContent, ReasoningContent, ImageContent,
    AudioContent, VideoContent, FileContent, ToolCallContent, ToolResultContent>;
}  // AiLib 命名空间结束
