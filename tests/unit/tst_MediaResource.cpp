#include <AiLib/core/MediaResource.h>
#include <QtTest>

using namespace AiLib;
class MediaResourceTest : public QObject {
    Q_OBJECT
private slots:
    void validBase64_data()  // 准备合法 Base64 的数据驱动测试样本
    {
        QTest::addColumn<QByteArray>("encoded");
        QTest::addColumn<QByteArray>("decoded");
        QTest::newRow("empty") << QByteArray() << QByteArray();
        QTest::newRow("one") << QByteArray("Zg==") << QByteArray("f");
        QTest::newRow("two") << QByteArray("Zm8=") << QByteArray("fo");
        QTest::newRow("three") << QByteArray("Zm9v") << QByteArray("foo");
        QTest::newRow("binary") << QByteArray("AP/+AA==") << QByteArray::fromHex("00fffe00");
    }
    void validBase64()  // 验证严格解码、空输入及旧错误清理
    {
        QFETCH(QByteArray, encoded);                                                           // 当前数据驱动行的 Base64 编码输入
        QFETCH(QByteArray, decoded);                                                           // 当前数据驱动行期望的原始字节
        MediaResource output = MediaResource::fromUrl(QStringLiteral("https://example.com"));  // 用于验证成功后替换来源的输出资源
        SdkError error;                                                                        // 本次操作的流程错误输出或默认无错误状态
        error.code = QStringLiteral("previous");
        QVERIFY(MediaResource::fromBase64(encoded, QStringLiteral("image/png"), output, error));
        QCOMPARE(output.storageType, StorageType::Bytes);
        QCOMPARE(output.data, decoded);
        QCOMPARE(output.mimeType, QStringLiteral("image/png"));
        QVERIFY(output.url.isEmpty());
        QVERIFY(error.code.isEmpty());
        QCOMPARE(error.category, ErrorCategory::None);
    }
    void invalidBase64_data()  // 准备非法字母表、填充和长度等测试样本
    {
        QTest::addColumn<QByteArray>("encoded");
        QTest::newRow("missing-padding") << QByteArray("Zg");
        QTest::newRow("space") << QByteArray("Z g=");
        QTest::newRow("newline") << QByteArray("Zm9v\n");
        QTest::newRow("invalid-alphabet") << QByteArray("Zm$v");
        QTest::newRow("url-alphabet") << QByteArray("____");
        QTest::newRow("middle-padding") << QByteArray("Z=8=");
        QTest::newRow("too-much-padding") << QByteArray("Z===");
        QTest::newRow("only-padding") << QByteArray("====");
        QTest::newRow("padding-bits-one-byte") << QByteArray("Zh==");
        QTest::newRow("padding-bits-two-bytes") << QByteArray("Zm9=");
        QTest::newRow("data-url") << QByteArray("data:image/png;base64,Zg==");
        QTest::newRow("trailing-junk") << QByteArray("Zg==AAAA");
    }
    void invalidBase64()  // 验证非法输入明确失败且输出对象不变
    {
        QFETCH(QByteArray, encoded);                                                      // 当前数据驱动行的 Base64 编码输入
        MediaResource output = MediaResource::fromLocalFile(QStringLiteral("keep.png"));  // 用于验证解析失败不修改输出的既有资源
        SdkError error;                                                                   // 本次操作的流程错误输出或默认无错误状态
        QVERIFY(!MediaResource::fromBase64(encoded, QStringLiteral("image/png"), output, error));
        QCOMPARE(error.category, ErrorCategory::InvalidArgument);
        QCOMPARE(error.code, QStringLiteral("InvalidBase64"));
        QCOMPARE(output.storageType, StorageType::LocalFile);
        QCOMPARE(output.filePath, QStringLiteral("keep.png"));
        QVERIFY(output.data.isEmpty());
    }
    void validSources()  // 验证基本来源校验不执行文件或网络访问
    {
        SdkError error;  // 本次操作的流程错误输出或默认无错误状态
        QVERIFY(validateMediaResource(MediaResource::fromUrl(QStringLiteral("https://example.com/a")), error));
        QVERIFY(validateMediaResource(MediaResource::fromBytes({}, QStringLiteral("image/png")), error));
        // 基本来源校验不访问文件系统。
        QVERIFY(validateMediaResource(MediaResource::fromLocalFile(QStringLiteral("/not/existing/image.png")), error));
        QVERIFY(validateMediaResource(MediaResource::fromFileReference(QStringLiteral("file_1")), error));
        QVERIFY(error.code.isEmpty());
    }
    void invalidSources()  // 验证来源缺失、冲突、非法 URL 和未知来源类型
    {
        SdkError error;                                                                // 本次操作的流程错误输出或默认无错误状态
        auto resource = MediaResource::fromUrl(QStringLiteral("relative/image.png"));  // 依次设置非法来源的媒体测试对象
        QVERIFY(!validateMediaResource(resource, error));
        resource.url = QStringLiteral("https://");
        QVERIFY(!validateMediaResource(resource, error));
        resource.url = QStringLiteral("https://example.com/%GG");
        QVERIFY(!validateMediaResource(resource, error));
        resource = MediaResource::fromUrl(QStringLiteral("https://example.com/a"));
        resource.fileId = QStringLiteral("conflicting");
        QVERIFY(!validateMediaResource(resource, error));
        resource = MediaResource::fromBytes(QByteArray("bytes"), {});
        resource.url = QStringLiteral("https://example.com");
        QVERIFY(!validateMediaResource(resource, error));
        resource = MediaResource::fromLocalFile({});
        QVERIFY(!validateMediaResource(resource, error));
        resource = MediaResource::fromFileReference({});
        QVERIFY(!validateMediaResource(resource, error));
        resource.fileId = QStringLiteral("file_1");
        resource.data = QByteArray("conflicting");
        QVERIFY(!validateMediaResource(resource, error));
        resource = {};
        resource.storageType = static_cast<StorageType>(999);
        QVERIFY(!validateMediaResource(resource, error));
        QCOMPARE(error.category, ErrorCategory::InvalidArgument);
    }
};
QTEST_GUILESS_MAIN(MediaResourceTest)
#include "tst_MediaResource.moc"
