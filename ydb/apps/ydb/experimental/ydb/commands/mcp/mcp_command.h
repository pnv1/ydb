#pragma once

#include <ydb/public/lib/ydb_cli/commands/interactive/ai/tools/tool_interface.h>
#include <ydb/public/lib/ydb_cli/commands/ydb_command.h>

#include <util/generic/string.h>
#include <util/system/types.h>

#include <utility>
#include <vector>

namespace NYdb::NConsoleClient {

// Starts an MCP (Model Context Protocol) server that exposes the YDB tools
// reused from the interactive AI mode. Reuses the standard connection options of
// the experimental CLI (endpoint, database, auth). Transports: stdio (default)
// and Streamable HTTP (--port / --transport http).
class TCommandMcp: public TYdbCommand {
public:
    TCommandMcp();

    void Config(TConfig& config) override;
    void Parse(TConfig& config) override;
    int Run(TConfig& config) override;

private:
    enum class ETransport {
        Stdio,
        Http,
    };

    std::vector<std::pair<TString, NAi::ITool::TPtr>> SelectMcpTools(
        std::vector<std::pair<TString, NAi::ITool::TPtr>> builtinTools) const;

    TString TransportName = "stdio";
    ui16 Port = 0;
    TString BindAddress = "127.0.0.1";
    bool ReadOnly = false;

    ETransport Transport = ETransport::Stdio;
};

} // namespace NYdb::NConsoleClient
