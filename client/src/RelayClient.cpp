#include "RelayClient.h"

#include <cctype>

#include <nlohmann/json.hpp>
#include <ixwebsocket/IXHttpClient.h>

namespace {

// Turns a reply into (parsed body, error message). Error is empty on success.
std::pair<nlohmann::json, std::string> ParseReply(const ix::HttpResponsePtr& response) {
    if (response->statusCode == 0)
        return {{}, "Couldn't reach the Planar server. Check your internet connection."};
    auto body = nlohmann::json::parse(response->body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return {{}, "Unexpected reply from the server (HTTP " + std::to_string(response->statusCode) + ")."};
    if (response->statusCode != 200)
        return {{}, body.value("error", "Request failed (HTTP " + std::to_string(response->statusCode) + ").")};
    return {std::move(body), {}};
}

} // namespace

RelayClient::RelayClient(std::string relayUrl, Post post)
    : m_relayUrl(std::move(relayUrl)), m_post(std::move(post)),
      m_http(std::make_unique<ix::HttpClient>(/*async=*/true)) {
    // Default TLS options verify against the Windows certificate store, as
    // for the WebSocket.
}

RelayClient::~RelayClient() = default; // HttpClient's destructor joins its thread

std::string RelayClient::HttpBaseFor(const std::string& relayUrl) {
    std::string base = relayUrl;
    if (base.rfind("wss://", 0) == 0)
        base = "https://" + base.substr(6);
    else if (base.rfind("ws://", 0) == 0)
        base = "http://" + base.substr(5);
    // Keep scheme + host[:port] only.
    const auto schemeEnd = base.find("://");
    const auto pathStart = schemeEnd == std::string::npos ? std::string::npos : base.find('/', schemeEnd + 3);
    if (pathStart != std::string::npos)
        base.erase(pathStart);
    return base;
}

std::string RelayClient::UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void RelayClient::Search(const std::string& query, SearchCallback callback) {
    const int generation = ++m_searchGeneration;
    auto args = m_http->createRequest(HttpBaseFor(m_relayUrl) + "/search?q=" + UrlEncode(query));
    args->connectTimeout = 10;
    args->transferTimeout = 15;

    m_http->performRequest(args, [this, generation, alive = std::weak_ptr<int>(m_alive),
                                  callback = std::move(callback)](const ix::HttpResponsePtr& response) {
        // Worker thread: parse here, deliver on the GUI thread.
        std::vector<SearchResult> results;
        auto [body, error] = ParseReply(response);
        if (error.empty()) {
            for (const auto& r : body.value("results", nlohmann::json::array())) {
                SearchResult result;
                result.videoId = r.value("videoId", "");
                result.title = r.value("title", "");
                result.channel = r.value("channel", "");
                if (r.contains("durationSec") && r["durationSec"].is_number())
                    result.durationSec = r["durationSec"].get<double>();
                result.thumbnailUrl = r.value("thumbnailUrl", "");
                if (!result.videoId.empty())
                    results.push_back(std::move(result));
            }
        }
        m_post([this, generation, alive, callback, results = std::move(results), error = std::move(error)] {
            if (alive.lock() && generation == m_searchGeneration)
                callback(results, error);
        });
    });
}

void RelayClient::CreateRoom(CreateRoomCallback callback) {
    auto args = m_http->createRequest(HttpBaseFor(m_relayUrl) + "/rooms", ix::HttpClient::kPost);
    args->connectTimeout = 10;
    args->transferTimeout = 15;

    m_http->performRequest(args, [this, alive = std::weak_ptr<int>(m_alive),
                                  callback = std::move(callback)](const ix::HttpResponsePtr& response) {
        auto [body, error] = ParseReply(response);
        std::string code = error.empty() ? body.value("code", "") : "";
        if (error.empty() && code.empty())
            error = "The server didn't return a room code.";
        m_post([alive, callback, code = std::move(code), error = std::move(error)] {
            if (alive.lock())
                callback(code, error);
        });
    });
}
