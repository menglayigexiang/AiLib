#pragma once

#include <AiLib/client/LLMClient.h>

namespace AiLib {
class AILIB_EXPORT LLMClientFactory {  // 便利组装内置 Adapter 与 Qt Transport，不参与请求执行
public:
    static bool supportsProtocol(ProtocolType protocol);  // 查询内置工厂是否实现指定协议
    static bool create(
        const ProviderConfig& provider,        // 显式选择协议的服务配置
        std::unique_ptr<LLMClient>& output,    // 成功时移交 Client 所有权，失败不修改既有对象
        SdkError& error,                       // 输出不支持协议或配置错误
        const RequestOptions& defaults = {});  // 新 Client 的默认请求选项
};
}  // AiLib 命名空间结束
