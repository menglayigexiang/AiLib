#include <AiLib/core/Cancellation.h>
#include <QtTest>
#include <thread>

using namespace AiLib;
class CancellationTest : public QObject {
    Q_OBJECT
private slots:
    void defaultAndIndependentSources()  // 验证默认令牌、独立取消源及副本语义
    {
        CancellationToken empty;  // 不关联取消源的默认令牌
        QVERIFY(!empty.isCancellationRequested());
        CancellationSource a;    // 用于触发取消的第一组独立状态
        CancellationSource b;    // 用于验证取消隔离的第二组独立状态
        auto first = a.token();  // 第一取消源生成的令牌
        auto copy = first;       // 共享同一取消状态的令牌副本
        a.cancel();
        a.cancel();
        QVERIFY(first.isCancellationRequested());
        QVERIFY(copy.isCancellationRequested());
        QVERIFY(a.token().isCancellationRequested());
        QVERIFY(!b.token().isCancellationRequested());
        QVERIFY(!empty.isCancellationRequested());
    }
    void survivesSourceLifetime()  // 验证令牌状态不依赖取消源对象的存活
    {
        CancellationToken token;  // 离开取消源作用域后继续保留的令牌
        {
            CancellationSource source;  // 本测试拥有的取消源
            token = source.token();
            source.cancel();
        }
        QVERIFY(token.isCancellationRequested());
    }
    void crossThreadPropagation()  // 验证取消状态跨线程可见，线程仅由测试创建
    {
        CancellationSource source;          // 本测试拥有的取消源
        const auto token = source.token();  // 交给工作线程读取的共享取消令牌
        std::atomic_bool ready{false};      // 工作线程是否已经开始读取状态
        std::atomic_bool observed{false};   // 工作线程是否观察到取消请求
        // 只有测试创建线程，SDK 本身不创建线程。
        std::thread worker([&] {  // 测试工作线程及执行体，用于观察跨线程取消
            ready.store(true);
            QElapsedTimer timer;  // 限制测试等待时长，避免异常情况下无限等待
            timer.start();
            while (!token.isCancellationRequested() && timer.elapsed() < 3000)
                std::this_thread::yield();
            observed.store(token.isCancellationRequested());
        });
        QElapsedTimer timer;  // 限制测试等待时长，避免异常情况下无限等待
        timer.start();
        while (!ready.load() && timer.elapsed() < 3000)
            std::this_thread::yield();
        source.cancel();
        worker.join();
        QVERIFY(ready.load());
        QVERIFY(observed.load());
    }
};
QTEST_GUILESS_MAIN(CancellationTest)
#include "tst_Cancellation.moc"
