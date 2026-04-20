#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace windup
{
class WinSockRuntime
{
public:
    WinSockRuntime();
    ~WinSockRuntime();

    WinSockRuntime(const WinSockRuntime&) = delete;
    WinSockRuntime& operator=(const WinSockRuntime&) = delete;
};

struct HttpRequest
{
    std::string method;
    std::string target;
    std::string path;
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse
{
    int statusCode = 200;
    std::string statusText = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct Endpoint
{
    std::string ip;
    std::uint16_t port = 0;
};

bool InitializeSocketAddress(const std::string& host, std::uint16_t port, sockaddr_in& outAddress);
Endpoint EndpointFromSockaddr(const sockaddr_in& address);
std::string EndpointToString(const Endpoint& endpoint);
std::string SockaddrToString(const sockaddr_in& address);

std::optional<HttpRequest> ParseHttpRequest(const std::string& rawRequest);
std::string BuildHttpResponse(const HttpResponse& response);

std::unordered_map<std::string, std::string> ParseSimpleJsonObject(const std::string& text);
std::string BuildJsonObject(const std::vector<std::pair<std::string, std::string>>& fields);
std::string EscapeJson(const std::string& input);

std::unordered_map<std::string, std::string> ParseQueryString(const std::string& queryText);
std::vector<std::string> SplitString(const std::string& text, char delimiter);
std::string Trim(const std::string& text);
std::string GenerateToken(std::size_t length);
std::string FormatUtcIso8601(std::chrono::system_clock::time_point timePoint);
std::string SocketErrorMessage(int errorCode);
} // namespace windup
