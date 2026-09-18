#include <AiLib/core/Message.h>
#include <QtTest>

using namespace AiLib;
class MessageTest : public QObject {
    Q_OBJECT
private slots:
    void orderedQueries()  // 验证内容原始顺序及便利查询无副作用
    {
        Message message;  // 包含多种交错内容的助手消息测试对象
        message.role = Role::Assistant;
        message.contents = {
            TextContent{QStringLiteral("前")},
            ImageContent{MediaResource::fromUrl(QStringLiteral("https://example.com/a.png"))},
            ReasoningContent{QStringLiteral("推理不能混入回答")},
            ToolCallContent{ToolCall{QStringLiteral("a"), QStringLiteral("first"), {}}},
            TextContent{QStringLiteral("后")},
            ToolCallContent{ToolCall{QStringLiteral("b"), QStringLiteral("second"), {}}}
        };
        QCOMPARE(message.text(), QStringLiteral("前后"));
        const auto calls = message.toolCalls();  // 按原始顺序提取的工具调用值副本
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls.at(0).id, QStringLiteral("a"));
        QCOMPARE(calls.at(1).id, QStringLiteral("b"));
        QCOMPARE(message.contents.size(), 6);
        QVERIFY(std::holds_alternative<ImageContent>(message.contents.at(1)));
        QVERIFY(std::holds_alternative<TextContent>(message.contents.at(4)));
        auto queryCopy = message.toolCalls();  // 用于验证查询结果修改不影响原消息的副本
        queryCopy[0].name = QStringLiteral("changed");
        QCOMPARE(message.toolCalls().at(0).name, QStringLiteral("first"));
    }
    void wrappingAndValueCopies()  // 验证工具结果包装和消息值复制相互独立
    {
        Message original = Message::user(QStringLiteral("你好"));  // 用于验证值复制语义的原始用户消息
        QCOMPARE(original.role, Role::User);
        QCOMPARE(Message::system(QStringLiteral("规则")).role, Role::System);
        Message copy = original;  // 可独立修改内容与状态的消息副本
        std::get<TextContent>(copy.contents[0]).text = QStringLiteral("修改");
        copy.status = MessageStatus::Incomplete;
        QCOMPARE(original.text(), QStringLiteral("你好"));
        QCOMPARE(original.status, MessageStatus::Complete);
        ToolResult result;  // 用于验证结果包装或工具失败语义的业务结果
        result.callId = QStringLiteral("a");
        result.data = QJsonObject{{QStringLiteral("temperature"), 38.6}};
        Message tool{Role::Tool, {ToolResultContent{result}}, MessageStatus::Complete};  // 包装已有工具结果的完整工具消息
        const auto& stored = std::get<ToolResultContent>(tool.contents.at(0)).result;    // 消息实际保存的工具结果只读引用
        QCOMPARE(stored.callId, result.callId);
        QCOMPARE(stored.data, result.data);
        QVERIFY(tool.text().isEmpty());
        QVERIFY(tool.toolCalls().isEmpty());
    }
};
QTEST_GUILESS_MAIN(MessageTest)
#include "tst_Message.moc"
