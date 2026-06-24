#include "tool_factory.h"

#include "describe_tool.h"
#include "docs_search_tool.h"
#include "exec_query_tool.h"
#include "exec_shell_tool.h"
#include "explain_query_tool.h"
#include "list_directory_tool.h"
#include "ydb_help_tool.h"

namespace NYdb::NConsoleClient::NAi {

std::vector<std::pair<TString, ITool::TPtr>> CreateBuiltinTools(const TBuiltinToolsSettings& settings) {
    return {
        {"list_directory", CreateListDirectoryTool({.Database = settings.Database, .LazyDriver = settings.LazyDriver})},
        {"exec_query", CreateExecQueryTool({.Prompt = settings.Prompt, .Database = settings.Database, .LazyDriver = settings.LazyDriver})},
        {"explain_query", CreateExplainQueryTool({.LazyDriver = settings.LazyDriver})},
        {"describe", CreateDescribeTool({.Database = settings.Database, .LazyDriver = settings.LazyDriver})},
        {"ydb_help", CreateYdbHelpTool({.UsageInfoGetter = settings.UsageInfoGetter})},
        {"docs_search", CreateDocsSearchTool()},
        {"exec_shell", CreateExecShellTool({.Prompt = settings.Prompt})},
    };
}

} // namespace NYdb::NConsoleClient::NAi
