#pragma once

#include <ydb/public/lib/ydb_cli/commands/interactive/ai/tools/tool_interface.h>

#include <library/cpp/json/writer/json_value.h>

#include <util/generic/strbuf.h>
#include <util/generic/string.h>
#include <util/system/mutex.h>

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NYdb::NConsoleClient::NMcp {

// Minimal MCP (Model Context Protocol) server: JSON-RPC 2.0 dispatch over a
// transport-agnostic interface. Exposes a fixed set of YDB tools reused from
// the interactive AI mode. Thread-safe: tool executions are serialized because
// tools keep per-call state in member fields.
class TMcpServer {
public:
    struct TSettings {
        TString ServerName = "ydb";
        TString ServerVersion;
        // Ordered, non-null tools to expose. Each tool must already have its
        // AutoAction set to Execute (MCP runs non-interactively).
        std::vector<std::pair<TString, NAi::ITool::TPtr>> Tools;
    };

    explicit TMcpServer(TSettings settings);

    // Processes one raw JSON-RPC message (a single object or a batch array) and
    // returns the serialized JSON response, or std::nullopt when no response is
    // due (e.g. the message was a notification).
    std::optional<TString> ProcessMessage(TStringBuf rawMessage);

private:
    NJson::TJsonValue HandleSingle(const NJson::TJsonValue& message, bool& hasResponse);

    NJson::TJsonValue HandleInitialize(const NJson::TJsonValue& params) const;
    NJson::TJsonValue HandleToolsList() const;
    NJson::TJsonValue HandleToolsCall(const NJson::TJsonValue& params);

    NAi::ITool* FindTool(const TString& name) const;

private:
    const TSettings Settings_;
    std::unordered_map<TString, NAi::ITool::TPtr> ToolsByName_;
    TMutex CallMutex_;
};

} // namespace NYdb::NConsoleClient::NMcp
