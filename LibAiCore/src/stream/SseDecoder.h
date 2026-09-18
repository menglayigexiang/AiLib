#pragma once

#include <AiLib/core/Error.h>
#include <functional>

namespace AiLib {
// 处理 SSE 行边界和多行 data，不解释任何模型厂商事件。
class SseDecoder {
public:
    using Handler = std::function<bool(
        const QString&, const QByteArray&, SdkError&)>;  // 完整事件名称和原始 data 的接收器
    bool feed(const QByteArray& bytes,                   // 原始网络片段
              const Handler& handler,                    // 完整 SSE 事件接收器
              SdkError& error)                           // 缓冲拆包字节并交付完整事件，支持 CR/LF/CRLF
    {
        m_pending += bytes;
        while (true) {
            int end = -1;                                 // 当前完整行结束位置
            for (int i = 0; i < m_pending.size(); ++i) {  // 当前扫描的原始字节位置
                if (m_pending[i] == '\n' || m_pending[i] == '\r') {
                    end = i;
                    break;
                }
            }
            if (end < 0 || (m_pending[end] == '\r' && end + 1 == m_pending.size()))
                break;
            const int separator =  // 实际行结束字符数
                m_pending[end] == '\r' && m_pending[end + 1] == '\n' ? 2 : 1;
            QByteArray line = m_pending.left(end);  // 完整原始行，不提前解码 UTF-8 分片
            m_pending.remove(0, end + separator);
            if (m_firstLine) {
                if (line.startsWith("\xEF\xBB\xBF"))
                    line.remove(0, 3);
                m_firstLine = false;
            }
            if (line.isEmpty()) {
                if (m_hasData && !handler(m_event, m_data, error))
                    return false;
                m_event.clear();
                m_data.clear();
                m_hasData = false;
                continue;
            }
            if (line.startsWith(':'))
                continue;
            const int colon = line.indexOf(':');                          // SSE 字段和值的分隔位置
            const QByteArray name = colon < 0 ? line : line.left(colon);  // SSE 字段名称
            QByteArray value =                                            // 去掉首个可选空格后的原始字段值
                colon < 0 ? QByteArray() : line.mid(colon + 1);
            if (value.startsWith(' '))
                value.remove(0, 1);
            if (name == "event")
                m_event = QString::fromUtf8(value);
            else if (name == "data") {
                if (m_hasData)
                    m_data += '\n';
                m_data += value;
                m_hasData = true;
            }
            if (m_data.size() > 8 * 1024 * 1024)
                return tooLarge(error);
        }
        if (m_pending.size() > 8 * 1024 * 1024)
            return tooLarge(error);
        return true;
    }
    bool finish(const Handler& handler,  // 完整 SSE 事件接收器
                SdkError& error)         // EOF 确认末尾 CR 是完整行结束，不交付未闭合事件
    {
        if (m_pending.endsWith('\r'))
            return feed("\n", handler, error);
        return true;
    }

private:
    bool tooLarge(SdkError& error)  // 防止异常 SSE 行或事件无限增长
    {
        error = {};
        error.category = ErrorCategory::Protocol;
        error.code = QStringLiteral("StreamFrameTooLarge");
        error.message = QStringLiteral("SSE event exceeds 8 MiB");
        return false;
    }
    QByteArray m_pending;     // 尚未收到完整行的原始字节
    QByteArray m_data;        // 当前事件的多行 data 字段
    QString m_event;          // 当前事件名，未提供时为空
    bool m_hasData = false;   // 是否存在 data 字段，区别空数据和无数据
    bool m_firstLine = true;  // 是否需要检查 UTF-8 BOM
};
}  // AiLib 命名空间结束
