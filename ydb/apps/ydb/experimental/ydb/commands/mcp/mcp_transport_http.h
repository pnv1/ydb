#pragma once

#include <util/generic/string.h>
#include <util/system/types.h>

namespace NYdb::NConsoleClient::NMcp {

class TMcpServer;

struct THttpTransportSettings {
    TString BindAddress = "127.0.0.1";
    ui16 Port = 8080;
};

// Runs the MCP server over the Streamable HTTP transport (single /mcp endpoint,
// JSON-RPC over POST with application/json responses). Blocks until the process
// receives SIGINT/SIGTERM. Returns the process exit code.
int RunHttpTransport(TMcpServer& server, const THttpTransportSettings& settings);

} // namespace NYdb::NConsoleClient::NMcp
