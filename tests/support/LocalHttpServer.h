#pragma once

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QList>
#include <functional>
#include <memory>

class LocalHttpServer {  // 同调用线程的本地 HTTP 测试设施，不进入 SDK 正式架构
public:
    using Handler = std::function<void(QTcpSocket*, const QByteArray&)>;       // 应用测试逻辑处理完整请求并模拟响应
    explicit LocalHttpServer(Handler handler) : m_handler(std::move(handler))  // 保存响应处理器，连接仅由测试事件循环驱动
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {  // 接受本地客户端请求，为每个连接创建独立缓冲
            while (m_server.hasPendingConnections()) {
                QTcpSocket* socket = m_server.nextPendingConnection();  // 测试服务拥有的当前客户端连接
                auto pending = std::make_shared<Pending>();             // 每个连接独立的请求缓冲和完成状态
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, &m_server, [this, socket, pending] {  // 请求头与请求体可能拆包，完整后只处理一次
                    pending->bytes += socket->readAll();
                    if (pending->handled) return;
                    const int end = pending->bytes.indexOf("\r\n\r\n");  // HTTP 请求头的结束位置
                    if (end < 0) return;
                    int length = 0;                                                  // 请求头声明的消息体字节数
                    for (const auto& line : pending->bytes.left(end).split('\n')) {  // 当前待解析的测试请求头行
                        if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (pending->bytes.size() < end + 4 + length) return;
                    pending->handled = true;
                    m_requests.append(pending->bytes);
                    m_handler(socket, pending->bytes);
                });
            }
        });
    }
    bool listen()  // 绑定环回地址的随机端口，避免依赖外部网络和固定端口
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QUrl url(const QString& path = QStringLiteral("/v1")) const  // 根据实际监听端口构造测试 API 根地址
    {
        return QUrl(QStringLiteral("http://127.0.0.1:") + QString::number(m_server.serverPort()) + path);
    }
    QList<QByteArray> requests() const  // 返回当前测试服务收到的完整请求，调用方仅在服务线程读取
    {
        return m_requests;
    }
    static QByteArray httpResponse(int status, const QByteArray& body, const QByteArray& extraHeaders = {}) // 构造带正确长度和连接关闭语义的测试响应
    {
        return "HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    }

private:
    struct Pending {           // 一个测试 TCP 连接的请求组装状态
        QByteArray bytes;      // 已收到的原始请求字节
        bool handled = false;  // 是否已完整处理，防止重复发送响应
    };
    QTcpServer m_server;           // 当前测试线程的本地监听服务
    Handler m_handler;             // 测试指定的响应或异常模拟逻辑
    QList<QByteArray> m_requests;  // 仅在服务线程更新和读取的请求记录
};
