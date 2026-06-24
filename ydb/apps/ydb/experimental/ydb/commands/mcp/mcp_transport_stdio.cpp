#include "mcp_transport_stdio.h"

#include "mcp_server.h"
#include "mcp_stdout_guard.h"

#include <util/generic/string.h>
#include <util/stream/input.h>
#include <util/stream/output.h>
#include <util/string/strip.h>

#include <optional>
#include <stdlib.h>

namespace NYdb::NConsoleClient::NMcp {

int RunStdioTransport(TMcpServer& server) {
    // stdout carries the JSON-RPC channel; redirect tool/FTXUI output to stderr
    // and keep a private handle on the real stdout for protocol responses.
    TProtocolStdoutGuard stdoutGuard;
    if (!stdoutGuard.IsValid()) {
        Cerr << "Failed to set up stdout for MCP stdio transport" << Endl;
        return EXIT_FAILURE;
    }

    int exitCode = EXIT_SUCCESS;
    TString line;
    while (Cin.ReadLine(line)) {
        if (Strip(line).empty()) {
            continue;
        }

        std::optional<TString> response;
        try {
            response = server.ProcessMessage(line);
        } catch (const std::exception& e) {
            Cerr << "MCP stdio: failed to process message: " << e.what() << Endl;
            continue;
        }

        if (response) {
            TString framed = std::move(*response);
            framed += '\n';
            if (!stdoutGuard.WriteToRealStdout(framed)) {
                Cerr << "MCP stdio: failed to write response to stdout" << Endl;
                exitCode = EXIT_FAILURE;
                break;
            }
        }
    }

    return exitCode;
}

} // namespace NYdb::NConsoleClient::NMcp
