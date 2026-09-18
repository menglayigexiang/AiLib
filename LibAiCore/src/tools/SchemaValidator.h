#pragma once
#include <AiLib/core/Error.h>
#include <QJsonObject>
namespace AiLib {
bool validateToolSchema(const QJsonObject& schema,        // 当前参数约束
                        SdkError& error);                 // 检查受支持的 Schema 子集及声明合法性
bool validateToolArguments(const QJsonObject& schema,     // 当前参数约束
                           const QJsonObject& arguments,  // 待验证的对象参数
                           QString& reason);              // 验证参数，失败说明包含字段路径
}
