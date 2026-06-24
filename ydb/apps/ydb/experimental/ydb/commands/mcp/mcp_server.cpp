#include "mcp_server.h"

#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>

#include <util/generic/hash_set.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/system/guard.h>

namespace NYdb::NConsoleClient::NMcp {

namespace {

// JSON-RPC 2.0 error codes
constexpr int JSON_RPC_PARSE_ERROR = -32700;
constexpr int JSON_RPC_INVALID_REQUEST = -32600;
constexpr int JSON_RPC_METHOD_NOT_FOUND = -32601;
constexpr int JSON_RPC_INVALID_PARAMS = -32602;
constexpr int JSON_RPC_INTERNAL_ERROR = -32603;

// Latest MCP protocol version we advertise when the client does not request a
// version we recognize. Our request handlers behave identically across the
// listed versions, so any of them is safe to echo back.
constexpr TStringBuf DEFAULT_PROTOCOL_VERSION = "2025-06-18";

bool IsSupportedProtocolVersion(TStringBuf version) {
    static const THashSet<TStringBuf> supported = {
        "2024-11-05",
        "2025-03-26",
        "2025-06-18",
        "2025-11-25",
    };
    return supported.contains(version);
}

// Carries a JSON-RPC error code out of a request handler.
class TJsonRpcError: public yexception {
public:
    explicit TJsonRpcError(int code)
        : Code_(code)
    {
    }

    int GetCode() const {
        return Code_;
    }

private:
    int Code_;
};

NJson::TJsonValue MakeResult(const NJson::TJsonValue& id, NJson::TJsonValue result) {
    NJson::TJsonValue response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

NJson::TJsonValue MakeError(const NJson::TJsonValue& id, int code, const TString& message) {
    NJson::TJsonValue response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    auto& error = response["error"];
    error["code"] = code;
    error["message"] = message;
    return response;
}

TString Serialize(const NJson::TJsonValue& value) {
    return NJson::WriteJson(&value, /* formatOutput */ false);
}

} // anonymous namespace

TMcpServer::TMcpServer(TSettings settings)
    : Settings_(std::move(settings))
{
    for (const auto& [name, tool] : Settings_.Tools) {
        ToolsByName_.emplace(name, tool);
    }
}

std::optional<TString> TMcpServer::ProcessMessage(TStringBuf rawMessage) {
    NJson::TJsonValue parsed;
    if (!NJson::ReadJsonTree(rawMessage, &parsed, /* throwOnError */ false)) {
        const NJson::TJsonValue idNull(NJson::JSON_NULL);
        return Serialize(MakeError(idNull, JSON_RPC_PARSE_ERROR, "Parse error"));
    }

    if (parsed.IsArray()) {
        const auto& batch = parsed.GetArraySafe();
        if (batch.empty()) {
            const NJson::TJsonValue idNull(NJson::JSON_NULL);
            return Serialize(MakeError(idNull, JSON_RPC_INVALID_REQUEST, "Invalid Request"));
        }

        NJson::TJsonValue responses(NJson::JSON_ARRAY);
        for (const auto& item : batch) {
            bool hasResponse = false;
            NJson::TJsonValue response = HandleSingle(item, hasResponse);
            if (hasResponse) {
                responses.AppendValue(std::move(response));
            }
        }

        if (responses.GetArraySafe().empty()) {
            return std::nullopt;
        }
        return Serialize(responses);
    }

    bool hasResponse = false;
    NJson::TJsonValue response = HandleSingle(parsed, hasResponse);
    if (!hasResponse) {
        return std::nullopt;
    }
    return Serialize(response);
}

NJson::TJsonValue TMcpServer::HandleSingle(const NJson::TJsonValue& message, bool& hasResponse) {
    hasResponse = false;

    if (!message.IsMap()) {
        hasResponse = true;
        return MakeError(NJson::TJsonValue(NJson::JSON_NULL), JSON_RPC_INVALID_REQUEST, "Invalid Request");
    }

    const bool hasId = message.Has("id");
    const NJson::TJsonValue id = hasId ? message["id"] : NJson::TJsonValue(NJson::JSON_NULL);

    if (!message.Has("method") || !message["method"].IsString()) {
        // A message without a method is not a request. It is either a response
        // from the client (ignored) or malformed.
        if (hasId && !message.Has("result") && !message.Has("error")) {
            hasResponse = true;
            return MakeError(id, JSON_RPC_INVALID_REQUEST, "Invalid Request");
        }
        return {};
    }

    const TString method = message["method"].GetString();
    const NJson::TJsonValue params = message.Has("params") ? message["params"] : NJson::TJsonValue(NJson::JSON_MAP);
    const bool isNotification = !hasId;

    try {
        NJson::TJsonValue result;
        if (method == "initialize") {
            result = HandleInitialize(params);
        } else if (method == "ping") {
            result.SetType(NJson::JSON_MAP);
        } else if (method == "tools/list") {
            result = HandleToolsList();
        } else if (method == "tools/call") {
            result = HandleToolsCall(params);
        } else if (method.StartsWith("notifications/")) {
            // Notifications require no action for the methods we receive
            // (e.g. notifications/initialized, notifications/cancelled).
            return {};
        } else {
            if (isNotification) {
                return {};
            }
            hasResponse = true;
            return MakeError(id, JSON_RPC_METHOD_NOT_FOUND, TStringBuilder() << "Method not found: " << method);
        }

        if (isNotification) {
            return {};
        }
        hasResponse = true;
        return MakeResult(id, std::move(result));
    } catch (const TJsonRpcError& e) {
        if (isNotification) {
            return {};
        }
        hasResponse = true;
        return MakeError(id, e.GetCode(), e.what());
    } catch (const std::exception& e) {
        if (isNotification) {
            return {};
        }
        hasResponse = true;
        return MakeError(id, JSON_RPC_INTERNAL_ERROR, e.what());
    }
}

NJson::TJsonValue TMcpServer::HandleInitialize(const NJson::TJsonValue& params) const {
    NJson::TJsonValue result;

    TString protocolVersion(DEFAULT_PROTOCOL_VERSION);
    if (params.IsMap() && params.Has("protocolVersion") && params["protocolVersion"].IsString()) {
        if (const TString requested = params["protocolVersion"].GetString(); IsSupportedProtocolVersion(requested)) {
            protocolVersion = requested;
        }
    }
    result["protocolVersion"] = protocolVersion;

    auto& capabilities = result["capabilities"];
    capabilities.SetType(NJson::JSON_MAP);
    auto& toolsCapability = capabilities["tools"];
    toolsCapability.SetType(NJson::JSON_MAP);
    toolsCapability["listChanged"] = false;

    auto& serverInfo = result["serverInfo"];
    serverInfo["name"] = Settings_.ServerName;
    serverInfo["version"] = Settings_.ServerVersion;

    return result;
}

NJson::TJsonValue TMcpServer::HandleToolsList() const {
    NJson::TJsonValue result;
    auto& tools = result["tools"];
    tools.SetType(NJson::JSON_ARRAY);
    for (const auto& [name, tool] : Settings_.Tools) {
        NJson::TJsonValue entry;
        entry["name"] = name;
        entry["description"] = tool->GetDescription();
        entry["inputSchema"] = tool->GetParametersSchema();
        tools.AppendValue(std::move(entry));
    }
    return result;
}

NJson::TJsonValue TMcpServer::HandleToolsCall(const NJson::TJsonValue& params) {
    if (!params.IsMap()) {
        throw TJsonRpcError(JSON_RPC_INVALID_PARAMS) << "params must be an object";
    }
    if (!params.Has("name") || !params["name"].IsString()) {
        throw TJsonRpcError(JSON_RPC_INVALID_PARAMS) << "missing required string parameter 'name'";
    }
    const TString name = params["name"].GetString();

    NJson::TJsonValue arguments;
    if (params.Has("arguments") && !params["arguments"].IsNull()) {
        arguments = params["arguments"];
        if (!arguments.IsMap()) {
            throw TJsonRpcError(JSON_RPC_INVALID_PARAMS) << "'arguments' must be an object";
        }
    } else {
        arguments.SetType(NJson::JSON_MAP);
    }

    NAi::ITool* tool = FindTool(name);
    if (!tool) {
        throw TJsonRpcError(JSON_RPC_INVALID_PARAMS) << "Unknown tool: " << name;
    }

    const NAi::ITool::TResponse toolResponse = [&] {
        TGuard<TMutex> guard(CallMutex_);
        return tool->Execute(arguments);
    }();

    NJson::TJsonValue result;
    auto& content = result["content"];
    content.SetType(NJson::JSON_ARRAY);
    NJson::TJsonValue textItem;
    textItem["type"] = "text";
    textItem["text"] = toolResponse.ToolResult;
    content.AppendValue(std::move(textItem));
    result["isError"] = !toolResponse.IsSuccess;
    return result;
}

NAi::ITool* TMcpServer::FindTool(const TString& name) const {
    const auto it = ToolsByName_.find(name);
    return it == ToolsByName_.end() ? nullptr : it->second.get();
}

} // namespace NYdb::NConsoleClient::NMcp
