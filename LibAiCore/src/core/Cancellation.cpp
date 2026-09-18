#include <AiLib/core/Cancellation.h>
#include <utility>

namespace AiLib {
CancellationToken::CancellationToken(std::shared_ptr<std::atomic_bool> state)  // 以传入的共享取消状态构造令牌
    : m_state(std::move(state)) {}

bool CancellationToken::isCancellationRequested() const noexcept  // 查询共享原子状态，默认令牌保持未取消
{
    return m_state && m_state->load(std::memory_order_relaxed);
}

CancellationSource::CancellationSource()  // 创建初始未取消的独立共享状态
    : m_state(std::make_shared<std::atomic_bool>(false)) {}

CancellationToken CancellationSource::token() const  // 返回与当前取消源共享状态的令牌
{
    return CancellationToken(m_state);
}

void CancellationSource::cancel() noexcept  // 原子标记取消，不创建线程或主动中断执行
{
    if (m_state)
        m_state->store(true, std::memory_order_relaxed);
}
}  // AiLib 命名空间结束
