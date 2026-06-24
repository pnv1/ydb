#pragma once

#include <util/generic/strbuf.h>

namespace NYdb::NConsoleClient::NMcp {

// Redirects process stdout (fd 1) to stderr (fd 2) for the lifetime of the
// guard, keeping a private duplicate of the original stdout. MCP tools and
// FTXUI write to stdout, which must not be mixed with the JSON-RPC channel
// (stdio transport) or the server console (http transport). The saved
// descriptor is exposed so the stdio transport can write framed responses to
// the real stdout. CLI logs already go to stderr.
class TProtocolStdoutGuard {
public:
    TProtocolStdoutGuard();
    ~TProtocolStdoutGuard();

    TProtocolStdoutGuard(const TProtocolStdoutGuard&) = delete;
    TProtocolStdoutGuard& operator=(const TProtocolStdoutGuard&) = delete;

    bool IsValid() const {
        return ProtocolFd_ >= 0;
    }

    // Writes all bytes to the saved real stdout. Returns false on error.
    bool WriteToRealStdout(TStringBuf data);

private:
    int ProtocolFd_ = -1;
};

} // namespace NYdb::NConsoleClient::NMcp
