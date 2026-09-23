#pragma once

#include <LibMcp/McpResult.h>
#include <LibMcp/McpTypes.h>

namespace LibMcp::Internal {

QJsonObject encodeTool(const McpTool &tool);
McpResult<McpTool> decodeTool(const QJsonObject &object);

QJsonObject encodeResource(const McpResource &resource);
McpResult<McpResource> decodeResource(const QJsonObject &object);

QJsonObject encodeResourceTemplate(
    const McpResourceTemplate &resourceTemplate);
McpResult<McpResourceTemplate> decodeResourceTemplate(
    const QJsonObject &object);

QJsonObject encodeResourceContent(const McpResourceContent &content);
McpResult<McpResourceContent> decodeResourceContent(
    const QJsonObject &object);

QJsonObject encodePrompt(const McpPrompt &prompt);
McpResult<McpPrompt> decodePrompt(const QJsonObject &object);

QJsonObject encodePromptMessage(const McpPromptMessage &message);
McpResult<McpPromptMessage> decodePromptMessage(
    const QJsonObject &object);

} // namespace LibMcp::Internal
