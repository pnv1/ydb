#include "mcp_transport_http.h"

#include "mcp_server.h"
#include "mcp_stdout_guard.h"

#include <library/cpp/http/io/headers.h>
#include <library/cpp/http/io/stream.h>
#include <library/cpp/http/misc/httpcodes.h>
#include <library/cpp/http/misc/parsed_request.h>
#include <library/cpp/http/server/http.h>
#include <library/cpp/http/server/response.h>

#include <util/datetime/base.h>
#include <util/generic/strbuf.h>
#include <util/generic/string.h>
#include <util/stream/output.h>
#include <util/string/ascii.h>

#include <atomic>
#include <csignal>
#include <optional>
#include <stdlib.h>

namespace NYdb::NConsoleClient::NMcp {

namespace {

constexpr TStringBuf MCP_PATH = "/mcp";
constexpr TStringBuf JSON_CONTENT_TYPE = "application/json";

std::atomic_bool StopRequested{false};

void HandleStopSignal(int) {
    StopRequested.store(true);
}

TStringBuf ExtractOriginHost(TStringBuf origin) {
    TStringBuf rest = origin;
    if (const auto schemeEnd = rest.find(TStringBuf("://")); schemeEnd != TStringBuf::npos) {
        rest = rest.SubStr(schemeEnd + 3);
    }
    rest = rest.Before('/');
    if (const auto at = rest.find('@'); at != TStringBuf::npos) {
        rest = rest.SubStr(at + 1);
    }
    if (rest.StartsWith('[')) {
        // IPv6 literal: [::1]:port
        if (const auto close = rest.find(']'); close != TStringBuf::npos) {
            return rest.SubStr(1, close - 1);
        }
        return rest;
    }
    return rest.Before(':');
}

// Accepts only loopback origins to mitigate DNS-rebinding from browsers.
// Non-browser MCP clients normally omit the Origin header entirely.
bool IsLoopbackOrigin(TStringBuf origin) {
    const TStringBuf host = ExtractOriginHost(origin);
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

class TMcpHttpRequest: public TRequestReplier {
public:
    explicit TMcpHttpRequest(TMcpServer& server)
        : Server_(server)
    {
    }

    bool DoReply(const TReplyParams& params) override {
        const THttpResponse response = BuildResponse(params);
        response.OutTo(params.Output);
        return true;
    }

private:
    THttpResponse BuildResponse(const TReplyParams& params) {
        const TParsedHttpFull http(params.Input.FirstLine());

        if (!AsciiEqualsIgnoreCase(http.Method, TStringBuf("POST"))) {
            // The Streamable HTTP transport also defines GET for server-initiated
            // SSE streams, which this server does not provide.
            return THttpResponse(HTTP_METHOD_NOT_ALLOWED)
                .AddHeader("Allow", "POST")
                .SetContent(R"({"error":"Only POST is supported"})", JSON_CONTENT_TYPE);
        }

        if (http.Path != MCP_PATH && http.Path != TStringBuf("/")) {
            return THttpResponse(HTTP_NOT_FOUND)
                .SetContent(R"({"error":"Not found"})", JSON_CONTENT_TYPE);
        }

        if (const THttpInputHeader* origin = params.Input.Headers().FindHeader("Origin")) {
            if (!IsLoopbackOrigin(origin->Value())) {
                return THttpResponse(HTTP_FORBIDDEN)
                    .SetContent(R"({"error":"Origin not allowed"})", JSON_CONTENT_TYPE);
            }
        }

        const TString body = params.Input.ReadAll();

        std::optional<TString> rpcResponse;
        try {
            rpcResponse = Server_.ProcessMessage(body);
        } catch (const std::exception& e) {
            Cerr << "MCP HTTP: failed to process message: " << e.what() << Endl;
            return THttpResponse(HTTP_INTERNAL_SERVER_ERROR)
                .SetContent(R"({"error":"Internal server error"})", JSON_CONTENT_TYPE);
        }

        if (!rpcResponse) {
            // The payload contained only notifications/responses.
            return THttpResponse(HTTP_ACCEPTED);
        }
        return THttpResponse(HTTP_OK).SetContent(*rpcResponse, JSON_CONTENT_TYPE);
    }

private:
    TMcpServer& Server_;
};

class TMcpHttpCallback: public THttpServer::ICallBack {
public:
    explicit TMcpHttpCallback(TMcpServer& server)
        : Server_(server)
    {
    }

    TClientRequest* CreateClient() override {
        return new TMcpHttpRequest(Server_);
    }

private:
    TMcpServer& Server_;
};

} // anonymous namespace

int RunHttpTransport(TMcpServer& server, const THttpTransportSettings& settings) {
    // Tool/FTXUI chatter goes to stdout; keep it off the server console (HTTP
    // responses are written to sockets, so stdout is not used for protocol).
    TProtocolStdoutGuard stdoutGuard;

    TMcpHttpCallback callback(server);

    THttpServerOptions options(settings.Port);
    options.SetHost(settings.BindAddress);
    options.SetThreads(2);

    THttpServer httpServer(&callback, options);
    if (!httpServer.Start()) {
        Cerr << "Failed to start MCP HTTP server on " << settings.BindAddress << ":" << settings.Port
             << ": " << httpServer.GetError() << Endl;
        return EXIT_FAILURE;
    }

    Cerr << "MCP HTTP server listening on http://" << settings.BindAddress << ":" << settings.Port << MCP_PATH << Endl;
    Cerr << "Press Ctrl+C to stop." << Endl;

    StopRequested.store(false);
    const auto prevSigInt = std::signal(SIGINT, HandleStopSignal);
    const auto prevSigTerm = std::signal(SIGTERM, HandleStopSignal);

    while (!StopRequested.load()) {
        Sleep(TDuration::MilliSeconds(200));
    }

    std::signal(SIGINT, prevSigInt);
    std::signal(SIGTERM, prevSigTerm);

    httpServer.Stop();
    return EXIT_SUCCESS;
}

} // namespace NYdb::NConsoleClient::NMcp
