#pragma once

#include <AiLib/Export.h>
#include <AiLib/core/Error.h>
#include <QByteArray>
#include <QString>

namespace AiLib {
enum class StorageType { Url, Bytes, LocalFile, FileReference };

struct AILIB_EXPORT MediaResource {
    StorageType storageType = StorageType::Bytes;  // 媒体当前存储来源，默认原始字节
    QString url;                                   // URL 来源地址，仅 Url 类型使用
    QByteArray data;                               // 原始媒体字节，仅 Bytes 类型使用，可为空
    QString filePath;                              // 本地文件路径，仅 LocalFile 类型使用
    QString fileId;                                // 服务端文件引用，仅 FileReference 类型使用
    QString mimeType;                              // 媒体 MIME 类型，未知时为空

    static MediaResource fromUrl(const QString& url, const QString& mimeType = {});               // 以 URL 和可选 MIME 类型构造媒体资源，不联网
    static MediaResource fromBytes(const QByteArray& data, const QString& mimeType);              // 以原始字节和 MIME 类型构造媒体资源
    static MediaResource fromLocalFile(const QString& filePath, const QString& mimeType = {});    // 以本地路径和可选 MIME 类型构造资源，不读取文件
    static MediaResource fromFileReference(const QString& fileId, const QString& mimeType = {});  // 以服务端文件引用和可选 MIME 类型构造资源
    // 严格标准 Base64：不接受空白、URL-safe 字母表或缺失填充。
    // 失败时输出不变；空 Base64 合法并得到空字节。
    static bool fromBase64(const QByteArray& encodedData, const QString& mimeType,  // 待严格解析的标准 Base64 数据及对应 MIME 类型
                           MediaResource& output, SdkError& error);                 // 严格解码为 Bytes，输出资源和错误，失败不修改输出
};

// 仅作结构校验：不读取文件、不联网、不检查模型能力。
AILIB_EXPORT bool validateMediaResource(const MediaResource& resource, SdkError& error);  // 检查资源来源字段合法性，输出错误，不执行 I/O
}                                                                                         // AiLib 命名空间结束
