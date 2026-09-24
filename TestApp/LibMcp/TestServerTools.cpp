#include "TestServerTools.h"

#include <LibMcp/McpServer.h>

#include <QDateTime>
#include <QJsonDocument>

using namespace LibMcp;

namespace {

QJsonObject readOnlyAnnotations(
    bool openWorld)  // 创建只读、幂等测试工具共用的行为注解
{
    return {{QStringLiteral("readOnlyHint"), true},
            {QStringLiteral("destructiveHint"), false},
            {QStringLiteral("idempotentHint"), true},
            {QStringLiteral("openWorldHint"), openWorld}};
}

McpToolCallResult structuredResult(
    const QJsonObject& value)  // 同时生成面向模型的文本和可校验结构化结果
{
    McpToolCallResult result;  // 保存符合 outputSchema 的成功结果
    const QString text = QString::fromUtf8(
        QJsonDocument(value).toJson(QJsonDocument::Compact));  // 生成与结构化内容一致的文本
    result.content = QJsonArray{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), text}}};
    result.structuredContent = value;
    return result;
}

McpToolCallResult businessErrorResult(
    const QString& message)  // 创建不升级为协议错误的工具业务失败结果
{
    McpToolCallResult result;  // 保存供 Client 和模型直接处理的业务错误
    result.isError = true;
    result.content = QJsonArray{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), message}}};
    return result;
}

bool registerEchoTool(McpServer& server)  // 注册任意对象参数的结构化回显工具
{
    McpTool tool;  // 描述最宽松的对象输入与同形输出
    tool.name = QStringLiteral("echo");
    tool.title = QStringLiteral("参数回显");
    tool.description = QStringLiteral("原样返回调用方提交的 JSON 对象，用于检查调用链和结构化输出。");
    tool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    tool.outputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    tool.annotations = readOnlyAnnotations(false);
    return server.addTool(
        tool,
        [](const McpToolCallRequest& request,  // 调用方提交的任意对象参数
           const McpRequestContext&) {        // 返回未经修改的输入对象
            return structuredResult(request.arguments);
        });
}

bool registerCurrentTimeTool(McpServer& server)  // 注册官方推荐形式的无参数工具
{
    McpTool tool;  // 描述只接受空对象并返回 UTC 时间的工具
    tool.name = QStringLiteral("current_time");
    tool.title = QStringLiteral("当前时间");
    tool.description = QStringLiteral("返回 TestApp Server 当前 UTC 时间，演示无参数 Tool。");
    tool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false}};
    tool.outputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("iso8601"), QStringLiteral("timeZone")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("iso8601"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("timeZone"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
    tool.annotations = readOnlyAnnotations(false);
    return server.addTool(
        tool,
        [](const McpToolCallRequest&,   // 无参数工具已校验为空对象的请求
           const McpRequestContext&) { // 生成每次请求发生时的 UTC 时间
            const QJsonObject value{
                {QStringLiteral("iso8601"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {QStringLiteral("timeZone"), QStringLiteral("UTC")}};  // 当前时间结构化结果
            return structuredResult(value);
        });
}

bool registerCalculateSumTool(McpServer& server)  // 注册带两个必填数值的计算工具
{
    McpTool tool;  // 描述严格数值输入和数值输出
    tool.name = QStringLiteral("calculate_sum");
    tool.title = QStringLiteral("两数求和");
    tool.description = QStringLiteral("计算两个数字之和，用于验证必填参数、数值 Schema 和结构化输出。");
    tool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("a"), QStringLiteral("b")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("a"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                          {QStringLiteral("description"), QStringLiteral("第一个加数")}}},
             {QStringLiteral("b"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                          {QStringLiteral("description"), QStringLiteral("第二个加数")}}}}}};
    tool.outputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("sum")}},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("sum"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}}};
    tool.annotations = readOnlyAnnotations(false);
    return server.addTool(
        tool,
        [](const McpToolCallRequest& request,  // 已通过 Schema 校验的两个加数
           const McpRequestContext&) {        // 对两个数字求和并返回结构化结果
            const double sum = request.arguments.value(QStringLiteral("a")).toDouble()
                               + request.arguments.value(QStringLiteral("b")).toDouble();  // 计算数值结果
            return structuredResult(QJsonObject{{QStringLiteral("sum"), sum}});
        });
}

bool registerWeatherTool(McpServer& server)  // 注册带枚举参数的确定性模拟天气工具
{
    McpTool tool;  // 描述地点、温度单位和完整结构化天气结果
    tool.name = QStringLiteral("get_weather");
    tool.title = QStringLiteral("模拟天气查询");
    tool.description = QStringLiteral("返回确定性的模拟天气数据，用于验证枚举、默认值和多字段输出。");
    tool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("location")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("location"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("minLength"), 1},
                          {QStringLiteral("description"), QStringLiteral("城市或地点名称")}}},
             {QStringLiteral("units"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"),
                           QJsonArray{QStringLiteral("celsius"),
                                      QStringLiteral("fahrenheit")}},
                          {QStringLiteral("default"), QStringLiteral("celsius")}}}}}};
    tool.outputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("location"),
                    QStringLiteral("temperature"),
                    QStringLiteral("units"),
                    QStringLiteral("conditions")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("location"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("temperature"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
             {QStringLiteral("units"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("conditions"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
    tool.annotations = readOnlyAnnotations(false);
    return server.addTool(
        tool,
        [](const McpToolCallRequest& request,  // 已通过 Schema 校验的地点和单位
           const McpRequestContext&) {        // 返回无需外部网络的稳定模拟数据
            const QString units = request.arguments
                                      .value(QStringLiteral("units"))
                                      .toString(QStringLiteral("celsius"));  // 请求温度单位
            const double temperature = units == QStringLiteral("fahrenheit")
                ? 74.3
                : 23.5;  // 与单位对应的模拟温度
            const QJsonObject value{
                {QStringLiteral("location"), request.arguments.value(QStringLiteral("location"))},
                {QStringLiteral("temperature"), temperature},
                {QStringLiteral("units"), units},
                {QStringLiteral("conditions"), QStringLiteral("晴")}};  // 完整模拟天气结果
            return structuredResult(value);
        });
}

bool registerBusinessErrorTool(McpServer& server)  // 注册返回工具级错误的诊断工具
{
    McpTool tool;  // 描述可自定义错误消息的无副作用测试工具
    tool.name = QStringLiteral("simulate_error");
    tool.title = QStringLiteral("模拟业务错误");
    tool.description = QStringLiteral("始终返回 isError=true，用于验证业务失败与协议错误的区分。");
    tool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("message"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("default"), QStringLiteral("模拟业务失败")}}}}}};
    tool.annotations = readOnlyAnnotations(false);
    return server.addTool(
        tool,
        [](const McpToolCallRequest& request,  // 可选的自定义错误消息
           const McpRequestContext&) {        // 返回 Client 可直接展示和处理的工具级错误
            const QString message = request.arguments
                                        .value(QStringLiteral("message"))
                                        .toString(QStringLiteral("模拟业务失败"));  // 调用方指定或默认错误消息
            return businessErrorResult(message);
        });
}

} // namespace

bool registerTestServerTools(McpServer& server)  // 返回全部工具是否成功注册
{
    bool success = true;  // 汇总所有固定测试工具的注册结果
    success = registerEchoTool(server) && success;
    success = registerCurrentTimeTool(server) && success;
    success = registerCalculateSumTool(server) && success;
    success = registerWeatherTool(server) && success;
    success = registerBusinessErrorTool(server) && success;
    return success;
}
