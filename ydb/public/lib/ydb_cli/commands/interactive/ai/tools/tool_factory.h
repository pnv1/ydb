#pragma once

#include "tool_interface.h"

#include <ydb/public/lib/ydb_cli/common/command.h>
#include <ydb/public/lib/ydb_cli/common/lazy_driver.h>

#include <util/generic/string.h>

#include <utility>
#include <vector>

namespace NYdb::NConsoleClient::NAi {

struct TBuiltinToolsSettings {
    TString Database;
    TLazyDriver::TPtr LazyDriver;
    TString Prompt; // Current interactive CLI prompt; used by tools that offer an interactive edit dialog
    TClientCommand::TConfig::TUsageInfoGetter UsageInfoGetter;
};

// Builds the canonical ordered list of built-in YDB tools shared between the
// interactive AI mode and the MCP server. Some entries may be null when a tool
// is unavailable in the current build (e.g. docs_search without the bundled
// archive), so callers must skip null entries.
std::vector<std::pair<TString, ITool::TPtr>> CreateBuiltinTools(const TBuiltinToolsSettings& settings);

} // namespace NYdb::NConsoleClient::NAi
