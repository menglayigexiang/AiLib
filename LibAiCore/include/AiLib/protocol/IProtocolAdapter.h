#pragma once

#include <AiLib/Export.h>
#include <AiLib/provider/ProviderConfig.h>
#include <AiLib/client/ChatRequest.h>
#include <AiLib/client/ChatResponse.h>
#include <AiLib/network/TransportResponse.h>
#include <AiLib/core/Error.h>
#include <AiLib/stream/IStreamDecoder.h>
#include <memory>

namespace AiLib {
// 转换 canonical model 与厂商协议，不执行网络或工具；流解析器按请求创建。
class AILIB_EXPORT IProtocolAdapter {
public:
    virtual ~IProtocolAdapter() = default;                          // 支持通过接口销毁具体协议实现
    virtual bool encodeChatRequest(const ProviderConfig& provider,  // 当前 Client 的只读服务配置
                                   const ChatRequest& request,      // 单候选普通模型请求
                                   TransportRequest& output,        // 输出厂商 HTTP 请求
                                   SdkError& error) const = 0;      // 发送前验证并编码，无对象级请求状态
    virtual bool
    decodeChatResponse(const TransportResponse& input,  // 完整 HTTP 响应或服务端错误响应
                       ChatResponse& output,            // 输出 canonical 响应，失败可保留有效数据
                       SdkError& error) const = 0;      // 普通响应及 HTTP/Provider 错误规范化
    virtual std::unique_ptr<IStreamDecoder>
    createStreamDecoder(SdkError& error) const  // 创建独立请求解析器，旧 Adapter 默认不支持流式
    {
        error = {};
        error.category = ErrorCategory::Unsupported;
        error.code = QStringLiteral("UnsupportedStreamingProtocol");
        error.message = QStringLiteral("Adapter does not implement streaming");
        return {};
    }
};
}  // AiLib 命名空间结束
