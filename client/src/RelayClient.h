#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ix {
class HttpClient;
}

struct SearchResult {
    std::string videoId;
    std::string title;
    std::string channel;
    std::optional<double> durationSec;
    std::string thumbnailUrl;
};

// The relay's plain HTTP endpoints: song search (the relay holds the YouTube
// API key; the app never sees it) and room creation.
//
// THREADING: like SessionConnection, HTTP replies arrive on a worker thread
// and are handed to `post` to reach the GUI thread.
class RelayClient {
public:
    using Post = std::function<void(std::function<void()>)>;
    // Exactly one of results / error is meaningful.
    using SearchCallback = std::function<void(const std::vector<SearchResult>& results, const std::string& error)>;
    using CreateRoomCallback = std::function<void(const std::string& code, const std::string& error)>;

    // relayUrl is the WebSocket URL ("wss://host" or "ws://host:port"); the
    // HTTP endpoints live on the same host.
    RelayClient(std::string relayUrl, Post post);
    ~RelayClient();

    // Only the newest search's reply is delivered: a reply to a superseded
    // search is dropped.
    void Search(const std::string& query, SearchCallback callback);

    // Asks the relay for a new room with a fresh, random code.
    void CreateRoom(CreateRoomCallback callback);

    const std::string& RelayUrl() const { return m_relayUrl; }

    // "wss://h/x" -> "https://h", "ws://h:1" -> "http://h:1".
    static std::string HttpBaseFor(const std::string& relayUrl);
    static std::string UrlEncode(const std::string& s);

private:
    std::string m_relayUrl;
    Post m_post;
    std::unique_ptr<ix::HttpClient> m_http;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    int m_searchGeneration = 0;
};
