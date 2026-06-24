#pragma once

namespace NYdb::NConsoleClient::NMcp {

class TMcpServer;

// Runs the MCP server over the stdio transport: newline-delimited JSON-RPC read
// from stdin, responses written to stdout. Blocks until stdin reaches EOF.
int RunStdioTransport(TMcpServer& server);

} // namespace NYdb::NConsoleClient::NMcp
