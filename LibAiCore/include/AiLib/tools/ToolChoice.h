#pragma once

#include <QString>

namespace AiLib {
enum class ToolChoiceMode { Auto, None, Required, Specific };
struct ToolChoice {
    ToolChoiceMode mode = ToolChoiceMode::Auto;  // 模型选择工具的方式，默认自动
    QString toolName;                            // Specific 模式指定的工具名称
};
}  // AiLib 命名空间结束
