#pragma once
#include "../ai_broker.h"

namespace pulse::extensions {
app::ai::ToolOutcome DescribeExtensionApiTool(const app::ai::ToolCall& call);
app::ai::ToolOutcome ValidateExtensionManifestTool(const app::ai::ToolCall& call);
app::ai::ToolOutcome GetExtensionTemplateTool(const app::ai::ToolCall& call);
}
