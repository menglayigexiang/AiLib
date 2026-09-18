#pragma once
#include <AiLib/network/TransportResponse.h>
#include <QDateTime>
#include <QLocale>
#include <limits>
#include <optional>
namespace AiLib::ProtocolDetails {
inline std::optional<qint64> retryAfter(
    const TransportResponse& response)  // 统一解析整数秒和三种 HTTP 日期形式，非法建议保持未知
{
    for (const auto& header : response.headers) {  // 响应 Header 名称不区分大小写
        if (header.first.compare("Retry-After", Qt::CaseInsensitive) != 0)
            continue;
        const QByteArray raw = header.second.trimmed();  // 去除 Header 边缘空白
        bool digits = !raw.isEmpty();                    // 秒数必须由十进制数字组成，不接受符号或小数
        for (char byte : raw) {                          // 检查秒数字符是否合法
            if (byte < '0' || byte > '9')
                digits = false;
        }
        if (digits) {
            bool parsed = false;                             // 受 qint64 范围约束的秒数解析状态
            const qint64 seconds = raw.toLongLong(&parsed);  // 服务端建议的非负整数秒数
            if (parsed && seconds <= std::numeric_limits<qint64>::max() / 1000)
                return seconds * 1000;
            return std::nullopt;
        }
        const QString text = QString::fromLatin1(raw);  // 英文 HTTP 日期原文
        const int comma = text.indexOf(',');            // IMF-fixdate 和 RFC850 的星期分隔符
        QDateTime date;                                 // 当前已解析的服务端恢复时间
        if (comma >= 0) {
            const QString tail = text.mid(comma + 1).trimmed();  // 不依赖星期名称计算日期
            date = QLocale::c().toDateTime(tail, QStringLiteral("dd MMM yyyy HH:mm:ss 'GMT'"));
            if (!date.isValid() && tail.size() == 22) {
                bool parsed = false;                                  // RFC850 两位年份是否为有效数字
                const int shortYear = tail.mid(7, 2).toInt(&parsed);  // 服务端给定的两位年份
                if (parsed && shortYear >= 0 && shortYear <= 99) {
                    const int currentYear = QDateTime::currentDateTimeUtc()  // 按当前世纪及五十年规则解释年份
                                                .date()
                                                .year();
                    int year =  // 完整年份，先替换再解析以正确处理闰日
                        currentYear / 100 * 100 + shortYear;
                    if (year > currentYear + 50)
                        year -= 100;
                    const QString expanded =  // 已展开年份的原始日期
                        tail.left(7) + QString::number(year) + tail.mid(9);
                    date = QLocale::c().toDateTime(expanded,
                                                   QStringLiteral("dd-MMM-yyyy HH:mm:ss 'GMT'"));
                }
            }
        } else if (text.size() > 4) {
            date = QLocale::c().toDateTime(text.mid(4).simplified(),
                                           QStringLiteral("MMM d HH:mm:ss yyyy"));
        }
        if (!date.isValid())
            return std::nullopt;
        date.setTimeSpec(Qt::UTC);
        return qMax<qint64>(0, QDateTime::currentDateTimeUtc().msecsTo(date));
    }
    return std::nullopt;
}
}  // 协议内部 HTTP 辅助函数命名空间结束
