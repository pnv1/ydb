LIBRARY()

SRCS(
    mcp_command.cpp
    mcp_server.cpp
    mcp_stdout_guard.cpp
    mcp_transport_http.cpp
    mcp_transport_stdio.cpp
)

PEERDIR(
    library/cpp/http/io
    library/cpp/http/misc
    library/cpp/http/server
    library/cpp/json
    ydb/public/lib/ydb_cli/commands
    ydb/public/lib/ydb_cli/commands/interactive/ai/tools
    ydb/public/lib/ydb_cli/commands/interactive/common
    ydb/public/lib/ydb_cli/common
    ydb/public/sdk/cpp/src/client/driver
)

END()
