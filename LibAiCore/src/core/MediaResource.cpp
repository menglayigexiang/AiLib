#include <AiLib/core/MediaResource.h>
#include <QUrl>

namespace AiLib {
namespace {
bool invalid(SdkError& error, const QString& code, const QString& message)  // 用给定错误码和说明填写参数错误并返回失败
{
    error = {};
    error.category = ErrorCategory::InvalidArgument;
    error.code = code;
    error.message = message;
    return false;
}

int base64Value(char ch)  // 将标准 Base64 字符转换为六位数值，非法字符返回 -1
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}
}  // 内部辅助函数命名空间结束

MediaResource MediaResource::fromUrl(const QString& url, const QString& mimeType)  // 以给定 URL 和 MIME 类型构造资源，不发起请求
{
    MediaResource resource;  // 待返回的媒体值对象，仅填写当前来源字段
    resource.storageType = StorageType::Url;
    resource.url = url;
    resource.mimeType = mimeType;
    return resource;
}

MediaResource MediaResource::fromBytes(const QByteArray& data, const QString& mimeType)  // 以给定原始字节和 MIME 类型构造资源
{
    MediaResource resource;  // 待返回的媒体值对象，仅填写当前来源字段
    resource.data = data;
    resource.mimeType = mimeType;
    return resource;
}

MediaResource MediaResource::fromLocalFile(const QString& filePath, const QString& mimeType)  // 以给定本地路径和 MIME 类型构造资源，不读取文件
{
    MediaResource resource;  // 待返回的媒体值对象，仅填写当前来源字段
    resource.storageType = StorageType::LocalFile;
    resource.filePath = filePath;
    resource.mimeType = mimeType;
    return resource;
}

MediaResource MediaResource::fromFileReference(const QString& fileId, const QString& mimeType)  // 以给定服务端引用和 MIME 类型构造资源
{
    MediaResource resource;  // 待返回的媒体值对象，仅填写当前来源字段
    resource.storageType = StorageType::FileReference;
    resource.fileId = fileId;
    resource.mimeType = mimeType;
    return resource;
}

bool MediaResource::fromBase64(const QByteArray& encodedData, const QString& mimeType,  // 需要严格解码的标准 Base64 输入及对应 MIME 类型
                               MediaResource& output, SdkError& error)                  // 校验并解码为 Bytes，输出资源和错误，失败不修改输出
{
    // 先校验规范 RFC 4648 标准 Base64，再调用 Qt 解码。
    // 显式校验保证 Qt 5.15 与 Qt 6 使用相同的严格输入规则。
    const auto size = encodedData.size();  // 编码输入字节数，必须是四的倍数
    if (size % 4 != 0)
        return invalid(error, QStringLiteral("InvalidBase64"),
                       QStringLiteral("Base64 length must be a multiple of four"));

    int padding = 0;  // 尾部等号填充数量，合法范围为零到二
    if (size > 0 && encodedData.at(size - 1) == '=') ++padding;
    if (size > 1 && encodedData.at(size - 2) == '=') ++padding;
    const auto payloadSize = size - padding;                          // 需要逐字符验证的非填充部分长度
    for (decltype(encodedData.size()) i = 0; i < payloadSize; ++i) {  // 当前校验字符的位置，不遍历末尾填充
        if (base64Value(encodedData.at(i)) < 0)
            return invalid(error, QStringLiteral("InvalidBase64"),
                           QStringLiteral("Invalid Base64 alphabet or padding"));
    }
    // 未使用的填充位必须为零，否则属于非规范编码。
    if ((padding == 2 && (base64Value(encodedData.at(size - 3)) & 15) != 0)
        || (padding == 1 && (base64Value(encodedData.at(size - 2)) & 3) != 0)) {
        return invalid(error, QStringLiteral("InvalidBase64"),
                       QStringLiteral("Non-zero Base64 padding bits"));
    }

    output = fromBytes(QByteArray::fromBase64(encodedData), mimeType);
    error = {};
    return true;
}

bool validateMediaResource(const MediaResource& resource, SdkError& error)  // 检查资源来源字段和 URL 基本合法性，输出参数错误
{
    bool missing = false;   // 当前来源是否缺失必要信息或格式非法
    bool conflict = false;  // 是否同时填写了其他不匹配的来源字段
    switch (resource.storageType) {
    case StorageType::Url: {
        const QUrl url(resource.url, QUrl::StrictMode);  // 严格解析的媒体 URL，仅用于结构校验
        missing = resource.url.isEmpty() || !url.isValid() || url.isRelative();
        // HTTP(S) 必须有主机，其他协议是否支持由 Adapter 决定。
        if ((url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
             || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
            && url.host().isEmpty()) missing = true;
        conflict = !resource.data.isEmpty() || !resource.filePath.isEmpty()
                   || !resource.fileId.isEmpty();
        break;
    }
    case StorageType::Bytes:
        conflict = !resource.url.isEmpty() || !resource.filePath.isEmpty()
                   || !resource.fileId.isEmpty();
        break;
    case StorageType::LocalFile:
        missing = resource.filePath.trimmed().isEmpty();
        conflict = !resource.url.isEmpty() || !resource.data.isEmpty()
                   || !resource.fileId.isEmpty();
        break;
    case StorageType::FileReference:
        missing = resource.fileId.trimmed().isEmpty();
        conflict = !resource.url.isEmpty() || !resource.data.isEmpty()
                   || !resource.filePath.isEmpty();
        break;
    default:
        return invalid(error, QStringLiteral("InvalidMediaResource"),
                       QStringLiteral("Unknown media storage type"));
    }
    if (conflict)
        return invalid(error, QStringLiteral("InvalidMediaResource"),
                       QStringLiteral("Media source fields conflict with storageType"));
    if (missing)
        return invalid(error, QStringLiteral("InvalidMediaResource"),
                       QStringLiteral("Media source is missing or invalid"));
    error = {};
    return true;
}
}  // AiLib 命名空间结束
