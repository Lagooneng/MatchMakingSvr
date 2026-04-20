#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace windup
{
namespace
{
std::string StatusTextForCode(int statusCode)
{
    switch (statusCode)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 410:
        return "Gone";
    case 500:
        return "Internal Server Error";
    default:
        return "OK";
    }
}
} // namespace

WinSockRuntime::WinSockRuntime()
{
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
    {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
    }
}

WinSockRuntime::~WinSockRuntime()
{
    WSACleanup();
}

bool InitializeSocketAddress(const std::string& host, std::uint16_t port, sockaddr_in& outAddress)
{
    outAddress = {};
    outAddress.sin_family = AF_INET;
    outAddress.sin_port = htons(port);
    return InetPtonA(AF_INET, host.c_str(), &outAddress.sin_addr) == 1;
}

Endpoint EndpointFromSockaddr(const sockaddr_in& address)
{
    char buffer[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, const_cast<IN_ADDR*>(&address.sin_addr), buffer, INET_ADDRSTRLEN);
    return {buffer, ntohs(address.sin_port)};
}

std::string EndpointToString(const Endpoint& endpoint)
{
    return endpoint.ip + ":" + std::to_string(endpoint.port);
}

std::string SockaddrToString(const sockaddr_in& address)
{
    return EndpointToString(EndpointFromSockaddr(address));
}

std::optional<HttpRequest> ParseHttpRequest(const std::string& rawRequest)
{
    const std::size_t headerEnd = rawRequest.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        return std::nullopt;
    }

    std::istringstream stream(rawRequest.substr(0, headerEnd));
    HttpRequest request{};
    std::string startLine;
    if (!std::getline(stream, startLine))
    {
        return std::nullopt;
    }

    if (!startLine.empty() && startLine.back() == '\r')
    {
        startLine.pop_back();
    }

    std::istringstream startLineStream(startLine);
    startLineStream >> request.method >> request.target;
    if (request.method.empty() || request.target.empty())
    {
        return std::nullopt;
    }

    const std::size_t queryPos = request.target.find('?');
    request.path = queryPos == std::string::npos ? request.target : request.target.substr(0, queryPos);
    if (queryPos != std::string::npos)
    {
        request.query = ParseQueryString(request.target.substr(queryPos + 1));
    }

    std::string headerLine;
    while (std::getline(stream, headerLine))
    {
        if (!headerLine.empty() && headerLine.back() == '\r')
        {
            headerLine.pop_back();
        }

        const std::size_t colonPos = headerLine.find(':');
        if (colonPos == std::string::npos)
        {
            continue;
        }

        request.headers.emplace(Trim(headerLine.substr(0, colonPos)), Trim(headerLine.substr(colonPos + 1)));
    }

    request.body = rawRequest.substr(headerEnd + 4);
    return request;
}

std::string BuildHttpResponse(const HttpResponse& response)
{
    std::ostringstream builder;
    builder << "HTTP/1.1 " << response.statusCode << ' '
            << (response.statusText.empty() ? StatusTextForCode(response.statusCode) : response.statusText) << "\r\n";
    builder << "Content-Length: " << response.body.size() << "\r\n";
    builder << "Content-Type: application/json\r\n";
    builder << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers)
    {
        builder << key << ": " << value << "\r\n";
    }
    builder << "\r\n";
    builder << response.body;
    return builder.str();
}

std::unordered_map<std::string, std::string> ParseSimpleJsonObject(const std::string& text)
{
    std::unordered_map<std::string, std::string> result;
    const std::string trimmed = Trim(text);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}')
    {
        return result;
    }

    std::size_t index = 1;
    while (index + 1 < trimmed.size())
    {
        while (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index])))
        {
            ++index;
        }

        if (index >= trimmed.size() || trimmed[index] == '}')
        {
            break;
        }

        if (trimmed[index] != '"')
        {
            return {};
        }

        const std::size_t keyEnd = trimmed.find('"', index + 1);
        if (keyEnd == std::string::npos)
        {
            return {};
        }

        const std::string key = trimmed.substr(index + 1, keyEnd - index - 1);
        index = trimmed.find(':', keyEnd);
        if (index == std::string::npos)
        {
            return {};
        }
        ++index;

        while (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index])))
        {
            ++index;
        }

        std::string value;
        if (index < trimmed.size() && trimmed[index] == '"')
        {
            const std::size_t valueEnd = trimmed.find('"', index + 1);
            if (valueEnd == std::string::npos)
            {
                return {};
            }
            value = trimmed.substr(index + 1, valueEnd - index - 1);
            index = valueEnd + 1;
        }
        else
        {
            std::size_t valueEnd = index;
            while (valueEnd < trimmed.size() && trimmed[valueEnd] != ',' && trimmed[valueEnd] != '}')
            {
                ++valueEnd;
            }
            value = Trim(trimmed.substr(index, valueEnd - index));
            index = valueEnd;
        }

        result.emplace(key, value);

        while (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index])))
        {
            ++index;
        }

        if (index < trimmed.size() && trimmed[index] == ',')
        {
            ++index;
        }
    }

    return result;
}

std::string BuildJsonObject(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::ostringstream builder;
    builder << '{';
    bool first = true;
    for (const auto& [key, value] : fields)
    {
        if (!first)
        {
            builder << ',';
        }
        first = false;
        builder << '"' << EscapeJson(key) << "\":\"" << EscapeJson(value) << '"';
    }
    builder << '}';
    return builder.str();
}

std::string EscapeJson(const std::string& input)
{
    std::ostringstream builder;
    for (const char ch : input)
    {
        switch (ch)
        {
        case '\\':
            builder << "\\\\";
            break;
        case '"':
            builder << "\\\"";
            break;
        case '\n':
            builder << "\\n";
            break;
        case '\r':
            builder << "\\r";
            break;
        case '\t':
            builder << "\\t";
            break;
        default:
            builder << ch;
            break;
        }
    }
    return builder.str();
}

std::unordered_map<std::string, std::string> ParseQueryString(const std::string& queryText)
{
    std::unordered_map<std::string, std::string> result;
    for (const std::string& part : SplitString(queryText, '&'))
    {
        const std::size_t equalPos = part.find('=');
        if (equalPos == std::string::npos)
        {
            continue;
        }
        result.emplace(part.substr(0, equalPos), part.substr(equalPos + 1));
    }
    return result;
}

std::vector<std::string> SplitString(const std::string& text, char delimiter)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(delimiter, start);
        if (end == std::string::npos)
        {
            result.push_back(text.substr(start));
            break;
        }
        result.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

std::string Trim(const std::string& text)
{
    const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (begin >= end)
    {
        return {};
    }
    return std::string(begin, end);
}

std::string GenerateToken(std::size_t length)
{
    static constexpr std::string_view alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::size_t> distribution(0, alphabet.size() - 1);

    std::string token;
    token.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        token.push_back(alphabet[distribution(generator)]);
    }
    return token;
}

std::string FormatUtcIso8601(std::chrono::system_clock::time_point timePoint)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm utcTime{};
    gmtime_s(&utcTime, &time);

    std::ostringstream builder;
    builder << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return builder.str();
}

std::string SocketErrorMessage(int errorCode)
{
    char* message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(errorCode),
        0,
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    const std::string result = length == 0 ? "unknown socket error" : Trim(message);
    if (message != nullptr)
    {
        LocalFree(message);
    }
    return result;
}
} // namespace windup
