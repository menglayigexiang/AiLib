#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpServerTransport.h>

#include <QHostAddress>
#include <QUrl>

#include <memory>

namespace LibMcp {

class StreamableHttpClientTransportPrivate;
class StreamableHttpServerTransportPrivate;

/// 基于 HTTP POST 的 Streamable HTTP Client Transport。
class LIBMCP_EXPORT StreamableHttpClientTransport final : public McpClientTransport
{
    Q_OBJECT
public:
    explicit StreamableHttpClientTransport(
        QUrl endpoint, QJsonObject headers = {}, QObject *parent = nullptr);
    ~StreamableHttpClientTransport() override;
    QFuture<McpResult<void>> start() override;
    QFuture<McpResult<void>> stop() override;
    QFuture<McpResult<void>> sendMessage(const QJsonObject &message) override;

private:
    std::unique_ptr<StreamableHttpClientTransportPrivate> d;
};

/// 可配置监听地址、端口和路径的 Streamable HTTP Server Transport。
class LIBMCP_EXPORT StreamableHttpServerTransport final : public McpServerTransport
{
    Q_OBJECT
public:
    explicit StreamableHttpServerTransport(
        QHostAddress address = QHostAddress::LocalHost,
        quint16 port = 8080,
        QString path = QStringLiteral("/mcp"),
        QObject *parent = nullptr);
    ~StreamableHttpServerTransport() override;
    QFuture<McpResult<void>> start() override;
    QFuture<McpResult<void>> stop() override;
    QFuture<McpResult<void>> sendMessage(
        const McpSessionId &sessionId, const QJsonObject &message) override;
    QHostAddress address() const;
    quint16 port() const;
    QString path() const;

private:
    std::unique_ptr<StreamableHttpServerTransportPrivate> d;
};

} // namespace LibMcp
