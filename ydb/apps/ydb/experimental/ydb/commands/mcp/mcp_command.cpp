#include "mcp_command.h"

#include "mcp_server.h"
#include "mcp_transport_http.h"
#include "mcp_transport_stdio.h"

#include <ydb/public/lib/ydb_cli/commands/interactive/ai/tools/tool_factory.h>
#include <ydb/public/lib/ydb_cli/commands/interactive/common/interactive_config.h>
#include <ydb/public/lib/ydb_cli/common/common.h>
#include <ydb/public/lib/ydb_cli/common/lazy_driver.h>

#include <util/generic/hash_set.h>
#include <util/generic/scope.h>

namespace NYdb::NConsoleClient {

namespace {

// Tools exposed over MCP. This is an explicit allow-list rather than an
// "exclude exec_shell" deny-list on purpose: MCP runs tools non-interactively
// (AutoAction=Execute, no human confirmation) and can be reachable over HTTP,
// so a tool newly added to the shared factory must be reviewed before it is
// exposed here instead of being published automatically.
const THashSet<TString>& McpToolAllowList() {
    static const THashSet<TString> allowList = {
        "list_directory",
        "describe",
        "explain_query",
        "docs_search",
        "ydb_help",
        "exec_query",
    };
    return allowList;
}

} // anonymous namespace

TCommandMcp::TCommandMcp()
    : TYdbCommand("mcp", {}, "Start an MCP (Model Context Protocol) server exposing YDB tools")
{
}

void TCommandMcp::Config(TConfig& config) {
    TYdbCommand::Config(config);

    config.Opts->AddLongOption("transport",
            "MCP transport: stdio (newline-delimited JSON-RPC over stdin/stdout) "
            "or http (Streamable HTTP, JSON-RPC over POST to /mcp)")
        .RequiredArgument("STRING")
        .DefaultValue("stdio")
        .CompletionArgHelp("MCP transport")
        .ChoicesWithCompletion({
            { "stdio", "Newline-delimited JSON-RPC over stdin/stdout (default)" },
            { "http", "Streamable HTTP: JSON-RPC over POST to /mcp" },
        })
        .StoreResult(&TransportName);
    config.Opts->AddLongOption("port", "TCP port for the http transport (implies --transport http; required for http)")
        .RequiredArgument("PORT")
        .StoreResult(&Port);
    config.Opts->AddLongOption("bind", "Address the http transport listens on. Default: 127.0.0.1 (loopback only). "
            "Use 0.0.0.0 to accept connections from other hosts (less secure).")
        .RequiredArgument("ADDRESS")
        .DefaultValue("127.0.0.1")
        .StoreResult(&BindAddress);
    config.Opts->AddLongOption("read-only", "Expose only read-only tools (exclude exec_query)")
        .StoreTrue(&ReadOnly);

    config.SetFreeArgsNum(0);
}

void TCommandMcp::Parse(TConfig& config) {
    TClientCommand::Parse(config);

    const bool transportProvided = config.ParseResult->Has("transport");
    const bool portProvided = config.ParseResult->Has("port");
    const bool bindProvided = config.ParseResult->Has("bind");

    // --transport accepts only stdio/http (enforced by ChoicesWithCompletion).
    if (TransportName == "http") {
        Transport = ETransport::Http;
    } else {
        Transport = ETransport::Stdio;
    }

    if (portProvided && !transportProvided) {
        // A port only makes sense for the http transport, so accept it as a shortcut.
        Transport = ETransport::Http;
    }

    if (Transport == ETransport::Stdio) {
        if (portProvided) {
            throw TMisuseException() << "--port is only valid for the http transport.";
        }
        if (bindProvided) {
            throw TMisuseException() << "--bind is only valid for the http transport.";
        }
    } else if (!portProvided) {
        // There is no universally safe well-known MCP port, so require an explicit
        // one instead of silently grabbing a popular port like 8080.
        throw TMisuseException() << "--port is required for the http transport.";
    }
}

std::vector<std::pair<TString, NAi::ITool::TPtr>> TCommandMcp::SelectMcpTools(
        std::vector<std::pair<TString, NAi::ITool::TPtr>> builtinTools) const {
    const auto& allowList = McpToolAllowList();

    std::vector<std::pair<TString, NAi::ITool::TPtr>> tools;
    tools.reserve(builtinTools.size());
    for (auto& [name, tool] : builtinTools) {
        if (!tool || !allowList.contains(name)) {
            continue;
        }
        if (ReadOnly && name == "exec_query") {
            continue;
        }
        // MCP runs non-interactively: tools must execute without asking.
        tool->SetAutoAction(TInteractiveConfigurationManager::EToolAutoAction::Execute);
        tools.emplace_back(name, std::move(tool));
    }
    return tools;
}

int TCommandMcp::Run(TConfig& config) {
    auto lazyDriver = std::make_shared<TLazyDriver>(
        [&config] { return TDriver(config.CreateDriverConfigWithBuildInfo("mcp")); });
    Y_DEFER {
        lazyDriver->Stop(true);
    };

    auto builtinTools = NAi::CreateBuiltinTools({
        .Database = config.Database,
        .LazyDriver = lazyDriver,
        .Prompt = TString(),
        .UsageInfoGetter = config.UsageInfoGetter,
    });

    NMcp::TMcpServer::TSettings serverSettings;
    serverSettings.ServerName = "ydb";
    serverSettings.ServerVersion = config.GetBuildInfo().Version;
    serverSettings.Tools = SelectMcpTools(std::move(builtinTools));

    NMcp::TMcpServer server(std::move(serverSettings));

    if (Transport == ETransport::Http) {
        NMcp::THttpTransportSettings httpSettings;
        httpSettings.BindAddress = BindAddress;
        httpSettings.Port = Port;
        return NMcp::RunHttpTransport(server, httpSettings);
    }

    return NMcp::RunStdioTransport(server);
}

} // namespace NYdb::NConsoleClient
