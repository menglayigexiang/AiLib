#include <AiLib/provider/ModelRegistry.h>
#include <AiLib/client/LLMClientFactory.h>
#include <QtTest>
#include <thread>

using namespace AiLib;
namespace {
ProviderEntry entry(const QString& id = QStringLiteral("provider"))  // 构造有效的最小目录条目
{
    ProviderEntry value;  // 待注册的无凭据 Provider
    value.configTemplate.id = id;
    value.configTemplate.name = QStringLiteral("Provider");
    value.configTemplate.baseUrl = QUrl(QStringLiteral("https://example.com/v1"));
    value.models = {ModelInfo{QStringLiteral("model"), QStringLiteral("Model"), {}, {}, {}}};
    return value;
}
}  // 测试数据辅助函数命名空间结束

// 验证模型目录的校验、复合查询键和线程安全快照。
class ModelRegistryTest final : public QObject {
    Q_OBJECT
private slots:
    void registerAndFind()  // 注册后按 Provider 与模型复合键查询
    {
        ModelRegistry registry;  // 当前测试独立目录
        SdkError error;          // 注册错误输出
        QVERIFY(registry.registerProvider(entry(), error));
        QVERIFY(registry.findProvider(QStringLiteral("provider")));
        QVERIFY(registry.findModel(QStringLiteral("provider"), QStringLiteral("model")));
        QVERIFY(!registry.findModel(QStringLiteral("other"), QStringLiteral("model")));
        QCOMPARE(registry.providers().size(), 1);
        QVERIFY(!registry.registerProvider(entry(), error));
        QCOMPARE(error.code, QStringLiteral("DuplicateProvider"));
    }

    void rejectsCredentialsAndUnsupportedProtocol()  // 阻止凭据和未实现协议进入目录
    {
        ModelRegistry registry;  // 当前测试独立目录
        SdkError error;          // 注册错误输出
        ProviderEntry value = entry();  // 待破坏的有效条目
        value.configTemplate.apiKey = QStringLiteral("secret");
        QVERIFY(!registry.registerProvider(value, error));
        QCOMPARE(error.code, QStringLiteral("CredentialInTemplate"));
        value = entry();
        value.configTemplate.protocol = ProtocolType::GeminiGenerateContent;
        QVERIFY(!registry.registerProvider(value, error));
        QCOMPARE(error.code, QStringLiteral("UnsupportedProtocol"));
        QVERIFY(LLMClientFactory::supportsProtocol(ProtocolType::OpenAIResponses));
        QVERIFY(!LLMClientFactory::supportsProtocol(ProtocolType::GeminiGenerateContent));
    }

    void concurrentRegistrationAndQuery()  // 并发读写后目录保持完整
    {
        ModelRegistry registry;  // 多线程共享目录
        SdkError firstError;     // 首个注册错误
        QVERIFY(registry.registerProvider(entry(QStringLiteral("base")), firstError));
        std::thread writer([&registry]() {  // 后台注册不同 Provider
            for (int index = 0; index < 20; ++index) {  // 当前 Provider 序号
                SdkError error;  // 当前注册错误
                registry.registerProvider(entry(QStringLiteral("p%1").arg(index)), error);
            }
        });
        for (int index = 0; index < 100; ++index)  // 主线程并发获取值快照
            QVERIFY(registry.findProvider(QStringLiteral("base")));
        writer.join();
        QCOMPARE(registry.providers().size(), 21);
    }
};

QTEST_APPLESS_MAIN(ModelRegistryTest)
#include "tst_ModelRegistry.moc"
