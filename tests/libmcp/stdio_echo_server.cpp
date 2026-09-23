#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>

int main(
    int argc,      // 进程参数数量
    char** argv)   // 进程参数数组
{                  // 提供 STDIO Transport 测试使用的最小协议 Server
    QCoreApplication application(argc, argv);  // 初始化 Qt Core 运行环境
    std::string inputLine;  // 保存阻塞读取的一条完整 JSON-RPC 请求
    while (std::getline(std::cin, inputLine)) {
        const QString line = QString::fromStdString(inputLine);  // 转换当前协议行
        const QJsonDocument request =  // 解析当前请求以回显 ID
            QJsonDocument::fromJson(line.toUtf8());
        if (!request.isObject()) {
            continue;
        }
        const QJsonObject response{  // 返回合法的空工具列表结果
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), request.object().value(QStringLiteral("id"))},
            {QStringLiteral("result"),
             QJsonObject{
                 {QStringLiteral("resultType"), QStringLiteral("complete")},
                 {QStringLiteral("ttlMs"), 0},
                 {QStringLiteral("cacheScope"), QStringLiteral("private")},
                 {QStringLiteral("tools"), QJsonArray{}}}}};
        std::cout << QJsonDocument(response)
                         .toJson(QJsonDocument::Compact)
                         .constData()
                  << std::endl;
    }
    return 0;
}
