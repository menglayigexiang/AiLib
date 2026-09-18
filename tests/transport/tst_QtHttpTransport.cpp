#include <AiLib/network/QtHttpTransport.h>
#include <AiLib/client/LLMClientFactory.h>
#include "../support/LocalHttpServer.h"
#include <QtTest>
#include <thread>
#include <atomic>
#include <algorithm>

using namespace AiLib;
namespace {
QByteArray answer()  // 构造本地服务返回的单候选普通 Chat 响应
{
    return R"({"id":"local_1","model":"test-model","choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"local answer"}}]})";
}
TransportRequest request(const QUrl& url)  // 构造与模型无关的原始 POST 请求
{
    TransportRequest input;  // 包含本地服务端点的原始传输数据
    input.url = url;
    input.headers.append(qMakePair(QByteArray("content-type"), QByteArray("application/json")));
    input.body = "{\"test\":true}";
    return input;
}
}  // 本地传输测试辅助函数命名空间结束

class QtHttpTransportTest : public QObject {  // 使用真实 Qt 网络栈验证普通 HTTP 和模型调用闭环
    Q_OBJECT
private slots:
    void factoryChat()  // 内置 Factory、Client、Adapter、真实 Transport 的完整本地普通 Chat
    {
        LocalHttpServer server([](QTcpSocket* socket, const QByteArray&) {  // 本地服务收到模型请求后发送标准单候选响应
            socket->write(LocalHttpServer::httpResponse(200, answer()));
            socket->disconnectFromHost();
        });
        QVERIFY(server.listen());
        ProviderConfig provider;  // 使用明确协议及网关前缀的本地服务配置
        provider.baseUrl = server.url(QStringLiteral("/gateway/v1/"));
        std::unique_ptr<LLMClient> client;  // Factory 生成并转移所有权的 Client
        SdkError error;                     // 创建和调用中的错误输出
        QVERIFY(LLMClientFactory::create(provider, client, error));
        ChatRequest input;  // 最小普通模型请求
        input.model = QStringLiteral("test-model");
        input.messages.append(Message::user(QStringLiteral("hello")));
        ChatResponse output;  // 通过实际网络获得的规范模型响应
        QVERIFY2(client->chat(input, output, error), qPrintable(error.message));
        QCOMPARE(output.message.text(), QStringLiteral("local answer"));
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(server.requests().size(), 1);
        QVERIFY(server.requests().first().startsWith("POST /gateway/v1/chat/completions HTTP/1.1"));
    }
    void httpStatuses_data()  // HTTP 失败属于响应数据，不能误判为 Transport 网络错误
    {
        QTest::addColumn<int>("status");
        QTest::newRow("200") << 200;
        QTest::newRow("400") << 400;
        QTest::newRow("401") << 401;
        QTest::newRow("429") << 429;
        QTest::newRow("503") << 503;
    }
    void httpStatuses()  // 保留状态码、错误响应体和原始 Retry-After Header
    {
        QFETCH(int, status);                                                      // 当前模拟的 HTTP 状态
        LocalHttpServer server([status](QTcpSocket* socket, const QByteArray&) {  // 当前状态的原始 HTTP 响应发送器
            socket->write(LocalHttpServer::httpResponse(status, "{\"error\":\"test\"}", "Retry-After: 30\r\n"));
            socket->disconnectFromHost();
        });
        QVERIFY(server.listen());
        QtHttpTransport transport;  // 实际 Qt 同步传输实现
        TransportResponse output;   // 原始 HTTP 响应输出
        SdkError error;             // 不应产生的网络错误
        QVERIFY2(transport.send(request(server.url()), {}, output, error), qPrintable(error.message));
        QCOMPARE(*output.statusCode, status);
        QCOMPARE(output.body, QByteArray("{\"error\":\"test\"}"));
        QCOMPARE(error.category, ErrorCategory::None);
        QVERIFY(std::any_of(output.headers.begin(), output.headers.end(), [](const auto& header) {  // 检查原始响应头未被协议层提前转换
            return header.first.compare("Retry-After", Qt::CaseInsensitive) == 0 && header.second == "30";
        }));
    }
    void fragmentedResponse()  // 响应拆成多个 TCP 写入，Transport 汇集完整原始消息体
    {
        LocalHttpServer server([](QTcpSocket* socket, const QByteArray&) {        // 模拟请求头及正文交错拆包的本地响应
            const QByteArray raw = LocalHttpServer::httpResponse(200, answer());  // 正确声明长度的完整原始响应
            socket->write(raw.left(23));
            QTimer::singleShot(20, socket, [socket, raw] {  // 稍后发送后续头和部分正文
                socket->write(raw.mid(23, raw.size() - 30));
                QTimer::singleShot(20, socket, [socket, raw] {  // 最后一个正文片段到达后正常关闭连接
                    socket->write(raw.right(7));
                    socket->disconnectFromHost();
                });
            });
        });
        QVERIFY(server.listen());
        QtHttpTransport transport;  // 实际 Qt 传输
        TransportResponse output;   // 已聚合的原始数据
        SdkError error;             // 网络错误输出
        QVERIFY(transport.send(request(server.url()), {}, output, error));
        QCOMPARE(output.body, answer());
    }
    void timeout()  // 单次 HTTP 尝试超时可以中断不返回响应的服务
    {
        LocalHttpServer server([](QTcpSocket*, const QByteArray&) {  // 接收完整请求但不返回数据，模拟阻塞服务
        });
        QVERIFY(server.listen());
        QtHttpTransport transport;  // 实际 Qt 传输
        RequestOptions options;     // 测试使用的单次尝试超时
        options.timeoutSeconds = 1;
        TransportResponse output;  // 超时前已经收到的原始数据
        SdkError error;            // 应报告 Timeout 而非 Cancelled
        QElapsedTimer elapsed;     // 验证调用不会无限阻塞的测试计时器
        elapsed.start();
        QVERIFY(!transport.send(request(server.url()), options, output, error));
        QCOMPARE(error.category, ErrorCategory::Timeout);
        QVERIFY(elapsed.elapsed() >= 900);
        QVERIFY(elapsed.elapsed() < 4000);
    }
    void cancellation()  // 请求期间的统一 Token 取消及时中止连接，不依赖 SDK 工作线程
    {
        LocalHttpServer server([](QTcpSocket*, const QByteArray&) {  // 服务保持连接但不回应，等待测试取消
        });
        QVERIFY(server.listen());
        CancellationSource source;  // 由测试调用方控制的取消源
        RequestOptions options;     // 本次请求共享取消 Token，禁用超时确保原因可区分
        options.timeoutSeconds = -1;
        options.cancellation = source.token();
        QTimer::singleShot(50, [&source] { source.cancel(); });  // 在调用方事件循环中发起取消
        QtHttpTransport transport;                               // 实际 Qt 传输
        TransportResponse output;                                // 取消前收到的数据
        SdkError error;                                          // 应报告 Cancelled
        QVERIFY(!transport.send(request(server.url()), options, output, error));
        QCOMPARE(error.category, ErrorCategory::Cancelled);
    }
    void preCancelled()  // 发送前已取消不产生实际网络连接
    {
        LocalHttpServer server([](QTcpSocket*, const QByteArray&) {  // 任何收到的请求都说明取消检查过晚
        });
        QVERIFY(server.listen());
        CancellationSource source;  // 已取消的调用方取消源
        source.cancel();
        RequestOptions options;  // 携带预取消 Token 的请求选项
        options.cancellation = source.token();
        QtHttpTransport transport;  // 实际 Qt 传输
        TransportResponse output;   // 预取消时为空的响应
        SdkError error;             // 取消错误
        QVERIFY(!transport.send(request(server.url()), options, output, error));
        QCOMPARE(error.category, ErrorCategory::Cancelled);
        QVERIFY(server.requests().isEmpty());
    }
    void droppedConnectionKeepsPartialBody()  // 连接提前关闭，保留已收到的状态及部分正文并报告网络故障
    {
        LocalHttpServer server([](QTcpSocket* socket, const QByteArray&) {  // 声明较长消息体但只发送有效前缀
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial");
            socket->disconnectFromHost();
        });
        QVERIFY(server.listen());
        QtHttpTransport transport;  // 实际 Qt 传输
        TransportResponse output;   // 即使失败也保留原始部分数据
        SdkError error;             // 远端连接提前关闭的网络错误
        QVERIFY(!transport.send(request(server.url()), {}, output, error));
        QCOMPARE(error.category, ErrorCategory::Network);
        QCOMPARE(*output.statusCode, 200);
        QCOMPARE(output.body, QByteArray("partial"));
    }
    void realConcurrentClient()  // 同一个 Client 的网络对象在两个调用线程分别创建，不发生 QObject 跨线程使用
    {
        LocalHttpServer server([](QTcpSocket* socket, const QByteArray&) {  // 本地事件循环分别处理两个工作线程的请求
            socket->write(LocalHttpServer::httpResponse(200, answer()));
            socket->disconnectFromHost();
        });
        QVERIFY(server.listen());
        ProviderConfig provider;  // 两个请求共享的只读本地服务配置
        provider.baseUrl = server.url();
        RequestOptions options;  // 为异常情况设置有限尝试超时，避免测试无限等待
        options.timeoutSeconds = 2;
        std::unique_ptr<LLMClient> client;  // 由 Factory 创建且支持并发的 Client
        SdkError error;                     // Factory 错误输出
        QVERIFY(LLMClientFactory::create(provider, client, error, options));
        std::atomic<int> finished{0};    // 已结束的工作线程数量
        std::atomic<int> successful{0};  // 得到正确模型回答的调用数量
        const auto run = [&] {           // 测试应用创建的同步调用执行体，每个线程保存独立结果
            ChatRequest input;           // 本线程模型请求
            input.model = QStringLiteral("test-model");
            input.messages.append(Message::user(QStringLiteral("hello")));
            ChatResponse output;  // 本线程独立助手响应
            SdkError failure;     // 本线程独立错误输出
            if (client->chat(input, output, failure) && output.message.text() == QStringLiteral("local answer")) ++successful;
            ++finished;
        };
        std::thread first(run);   // 测试调用方创建的第一工作线程
        std::thread second(run);  // 测试调用方创建的第二工作线程
        QElapsedTimer elapsed;    // 限制主线程处理测试服务事件的等待时间
        elapsed.start();
        while (finished.load() != 2 && elapsed.elapsed() < 5000) QTest::qWait(10);
        first.join();
        second.join();
        QCOMPARE(successful.load(), 2);
        QCOMPARE(server.requests().size(), 2);
    }
};
QTEST_GUILESS_MAIN(QtHttpTransportTest)
#include "tst_QtHttpTransport.moc"
