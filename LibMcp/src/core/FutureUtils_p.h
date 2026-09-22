#pragma once

#include <QFuture>
#include <QFutureInterface>

#include <memory>
#include <utility>

namespace LibMcp::Internal {

/// Qt 5 与 Qt 6 共用的最小 Promise 封装。
template<typename T>
class Promise
{
public:
    Promise()
        : m_interface(std::make_shared<QFutureInterface<T>>())
    {
        m_interface->reportStarted();
    }

    QFuture<T> future() const
    {
        return m_interface->future();
    }

    void finish(T value) const
    {
        m_interface->reportResult(std::move(value));
        m_interface->reportFinished();
    }

private:
    std::shared_ptr<QFutureInterface<T>> m_interface;
};

/// 创建一个已经完成的 Future。
template<typename T>
QFuture<T> readyFuture(T value)
{
    Promise<T> promise;
    const QFuture<T> future = promise.future();
    promise.finish(std::move(value));
    return future;
}

} // namespace LibMcp::Internal
