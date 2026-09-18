#pragma once

#include <AiLib/protocol/IProtocolAdapter.h>
#include <AiLib/network/ITransport.h>
#include <memory>

namespace AiLib {
// 协调同步普通或流式请求，独占组件；每次调用的可变状态独立。
class AILIB_EXPORT LLMClient {
public:
    LLMClient(ProviderConfig provider,                    // 按值保存且不可动态修改的服务配置
              std::unique_ptr<IProtocolAdapter> adapter,  // 转移协议实现所有权
              std::unique_ptr<ITransport> transport,      // 转移传输实现所有权
              RequestOptions defaults = {});              // 保存默认请求选项，使用默认自动重试策略
    bool chat(const ChatRequest& request,                 // 本次 canonical 模型请求
              ChatResponse& response,                     // 输出响应，调用开始时重置
              SdkError& error) const;                     // 使用 Client 默认选项同步请求
    bool chat(const ChatRequest& request,                 // 本次 canonical 模型请求
              ChatResponse& response,                     // 输出响应，解析失败保留已获得的有效内容
              SdkError& error,                            // 输出流程故障详情
              const RequestOptions& options) const;       // 覆盖默认配置，同步请求并按策略重试

private:
    bool chatStream(const TransportRequest& encoded,       // Adapter 已生成的流式 HTTP 请求
                    ChatResponse& response,                // 输出完整或部分有效数据
                    SdkError& error,                       // 流程故障输出
                    const RequestOptions& options) const;  // 协调 Decoder、Session 和同步实时通知
    ProviderConfig m_provider;                             // 构造时保存的只读服务配置副本
    std::unique_ptr<IProtocolAdapter> m_adapter;           // 本 Client 独占的无状态协议实现
    std::unique_ptr<ITransport> m_transport;               // 本 Client 独占的可并发传输实现
    RequestOptions m_defaults;                             // 未传入本次选项时使用的默认配置
};
}  // AiLib 命名空间结束
