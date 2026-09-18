#pragma once

#include <AiLib/Export.h>
#include <AiLib/stream/StreamEvent.h>

namespace AiLib {
// 单次请求的协议解析器，将原始字节转换为事件，不聚合最终内容。
class AILIB_EXPORT IStreamDecoder {
public:
    virtual ~IStreamDecoder() = default;              // 通过接口销毁请求级解析器
    virtual bool feed(const QByteArray& bytes,        // 本次收到的原始网络片段
                      const StreamEventSink& sink,    // 标准化事件的接收端
                      SdkError& error) = 0;           // 增量解析，协议或聚合故障时返回失败
    virtual bool finish(const StreamEventSink& sink,  // 标准化事件接收端
                        SdkError& error) = 0;         // 交付最后的完整事件并验证协议结束，sink 接收标准事件
};
}  // AiLib 命名空间结束
