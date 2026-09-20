#pragma once

#include <AiLib/provider/ModelRegistry.h>

namespace Demo {

QList<AiLib::ProviderEntry> builtinProviders();                 // 构造示例内置的无凭据 Provider 目录
bool registerBuiltinModels(AiLib::ModelRegistry& registry,     // 接收内置条目的模型目录
                           AiLib::SdkError& error);             // 注册全部条目并返回首个错误
QString demoApiKeyFromEnv(const QString& providerId);          // 读取对应 Provider 的示例环境变量

}  // Demo 命名空间结束
