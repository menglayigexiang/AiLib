#pragma once
#include <AiLib/client/RequestOptions.h>
inline AiLib::RequestOptions singleAttemptOptions()  // 既有单次错误测试明确禁用自动重试
{
    AiLib::RequestOptions options;  // 测试专用的完整请求选项
    options.retryPolicy.maxRetries = 0;
    return options;
}
