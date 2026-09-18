#pragma once

#include <AiLib/Export.h>
#include <atomic>
#include <memory>

namespace AiLib {
class CancellationSource;

class AILIB_EXPORT CancellationToken {
public:
    CancellationToken() = default;                  // 创建不关联取消源的默认令牌
    bool isCancellationRequested() const noexcept;  // 查询共享状态是否已请求取消

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state);  // 使用传入的共享原子状态创建令牌
    std::shared_ptr<std::atomic_bool> m_state;                            // 与 Source 和其他 Token 共享的原子取消状态
};

class AILIB_EXPORT CancellationSource {
public:
    CancellationSource();             // 创建独立的共享取消状态
    CancellationToken token() const;  // 获取关联本取消源的令牌
    void cancel() noexcept;           // 发起不可重置的协作取消请求

private:
    std::shared_ptr<std::atomic_bool> m_state;  // 与 Source 和其他 Token 共享的原子取消状态
};
}  // AiLib 命名空间结束
