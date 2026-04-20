#include "../Common/Protocol.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
constexpr auto kPollInterval = std::chrono::seconds(1);
constexpr auto kPunchInterval = std::chrono::milliseconds(250);
constexpr int kPunchAttempts = 20;

struct SessionInfo
{
    std::string roomCode;
    std::string sessionToken;
    std::uint16_t udpServerPort = 0;
};

struct RuntimeState
{
    std::mutex mutex;
    std::optional<windup::Endpoint> peerEndpoint;
    bool punchTriggered = false;
    bool connected = false;
    bool relayMode = false;
    std::atomic<bool> running{true};
};

std::string ReceiveAll(SOCKET socketHandle)
{
    std::string response;
    char buffer[4096];
    while (true)
    {
        const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            break;
        }
        response.append(buffer, received);
    }
    return response;
}

std::unordered_map<std::string, std::string> ParseHttpJsonBody(const std::string& response)
{
    const std::size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        return {};
    }
    return windup::ParseSimpleJsonObject(response.substr(headerEnd + 4));
}

std::unordered_map<std::string, std::string> SendHttpRequest(
    const std::string& serverHost,
    std::uint16_t httpPort,
    const std::string& method,
    const std::string& target,
    const std::string& body)
{
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        throw std::runtime_error("Failed to create HTTP socket");
    }

    sockaddr_in address{};
    if (!windup::InitializeSocketAddress(serverHost, httpPort, address))
    {
        closesocket(socketHandle);
        throw std::runtime_error("Invalid server host");
    }

    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(socketHandle);
        throw std::runtime_error("HTTP connect failed: " + windup::SocketErrorMessage(error));
    }

    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: " + serverHost + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Connection: close\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += body;

    send(socketHandle, request.c_str(), static_cast<int>(request.size()), 0);
    const std::string response = ReceiveAll(socketHandle);
    closesocket(socketHandle);
    return ParseHttpJsonBody(response);
}

SessionInfo CreateRoom(const std::string& serverHost, std::uint16_t httpPort)
{
    const auto json = SendHttpRequest(serverHost, httpPort, "POST", "/rooms", "{}");
    return SessionInfo{
        json.at("roomCode"),
        json.at("sessionToken"),
        static_cast<std::uint16_t>(std::stoi(json.at("udpServerPort"))),
    };
}

SessionInfo JoinRoom(const std::string& serverHost, std::uint16_t httpPort, const std::string& roomCode)
{
    const auto json = SendHttpRequest(
        serverHost,
        httpPort,
        "POST",
        "/rooms/join",
        windup::BuildJsonObject({{"roomCode", roomCode}}));

    const auto errorIt = json.find("errorCode");
    if (errorIt != json.end())
    {
        throw std::runtime_error("Join room failed: " + errorIt->second);
    }

    return SessionInfo{
        roomCode,
        json.at("sessionToken"),
        static_cast<std::uint16_t>(std::stoi(json.at("udpServerPort"))),
    };
}

std::unordered_map<std::string, std::string> GetStatus(
    const std::string& serverHost,
    std::uint16_t httpPort,
    const std::string& roomCode,
    const std::string& sessionToken)
{
    return SendHttpRequest(serverHost, httpPort, "GET", "/rooms/" + roomCode + "/status?sessionToken=" + sessionToken, {});
}

void SendUdpLine(SOCKET socketHandle, const sockaddr_in& address, const std::string& message)
{
    sendto(
        socketHandle,
        message.c_str(),
        static_cast<int>(message.size()),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
}

void StartPunchThread(SOCKET udpSocket, windup::Endpoint peerEndpoint, std::string sessionToken)
{
    std::thread([udpSocket, peerEndpoint = std::move(peerEndpoint), sessionToken = std::move(sessionToken)]() {
        sockaddr_in peerAddress{};
        if (!windup::InitializeSocketAddress(peerEndpoint.ip, peerEndpoint.port, peerAddress))
        {
            return;
        }

        for (int attempt = 0; attempt < kPunchAttempts; ++attempt)
        {
            SendUdpLine(udpSocket, peerAddress, "PUNCH " + sessionToken);
            std::this_thread::sleep_for(kPunchInterval);
        }
    }).detach();
}

void StartRelayThread(SOCKET udpSocket, sockaddr_in serverAddress, std::string sessionToken)
{
    std::thread([udpSocket, serverAddress, sessionToken = std::move(sessionToken)]() {
        for (int attempt = 0; attempt < kPunchAttempts; ++attempt)
        {
            SendUdpLine(udpSocket, serverAddress, "RELAY " + sessionToken + " HELLO");
            std::this_thread::sleep_for(kPunchInterval);
        }
    }).detach();
}

void UdpReceiveLoop(
    SOCKET udpSocket,
    const std::string& sessionToken,
    const std::string& serverHost,
    std::uint16_t udpServerPort,
    RuntimeState& runtimeState,
    bool forceRelay)
{
    sockaddr_in serverAddress{};
    windup::InitializeSocketAddress(serverHost, udpServerPort, serverAddress);

    char buffer[1024];
    while (runtimeState.running.load())
    {
        sockaddr_in sender{};
        int senderLength = sizeof(sender);
        const int received = recvfrom(
            udpSocket,
            buffer,
            static_cast<int>(sizeof(buffer) - 1),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLength);
        if (received == SOCKET_ERROR)
        {
            break;
        }

        buffer[received] = '\0';
        const std::vector<std::string> parts = windup::SplitString(windup::Trim(std::string(buffer, received)), ' ');
        if (parts.empty())
        {
            continue;
        }

        if (parts[0] == "REGISTERED")
        {
            std::cout << "UDP registered via server response.\n";
            continue;
        }

        if (parts[0] == "PUNCH_START" && parts.size() >= 3)
        {
            std::lock_guard lock(runtimeState.mutex);
            runtimeState.peerEndpoint = windup::Endpoint{parts[1], static_cast<std::uint16_t>(std::stoi(parts[2]))};
            if (!runtimeState.punchTriggered && !forceRelay)
            {
                runtimeState.punchTriggered = true;
                std::cout << "Punch start against " << parts[1] << ":" << parts[2] << '\n';
                StartPunchThread(udpSocket, *runtimeState.peerEndpoint, sessionToken);
            }
            continue;
        }

        if (parts[0] == "RELAY_READY" && parts.size() >= 2)
        {
            bool shouldStart = false;
            {
                std::lock_guard lock(runtimeState.mutex);
                if (!runtimeState.relayMode)
                {
                    runtimeState.relayMode = true;
                    shouldStart = true;
                }
            }

            if (shouldStart)
            {
                std::cout << "Relay fallback enabled via server.\n";
                StartRelayThread(udpSocket, serverAddress, sessionToken);
            }
            continue;
        }

        if (parts[0] == "PUNCH")
        {
            bool firstHit = false;
            {
                std::lock_guard lock(runtimeState.mutex);
                if (!runtimeState.connected)
                {
                    runtimeState.connected = true;
                    firstHit = true;
                }
            }

            if (firstHit)
            {
                std::cout << "Received peer UDP from " << windup::SockaddrToString(sender) << '\n';
                SendUdpLine(udpSocket, serverAddress, "CONNECTED " + sessionToken);
            }
        }

        if (parts[0] == "RELAY_FROM" && parts.size() >= 3)
        {
            bool firstHit = false;
            {
                std::lock_guard lock(runtimeState.mutex);
                if (!runtimeState.connected)
                {
                    runtimeState.connected = true;
                    firstHit = true;
                }
            }

            std::cout << "Received relayed UDP from " << parts[1] << '\n';
            if (firstHit)
            {
                SendUdpLine(udpSocket, serverAddress, "CONNECTED " + sessionToken);
            }
        }
    }
}

int RunClient(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage:\n";
        std::cerr << "  MatchMakingDummyCli host [serverHost] [httpPort] [--force-relay]\n";
        std::cerr << "  MatchMakingDummyCli guest <roomCode> [serverHost] [httpPort] [--force-relay]\n";
        return 1;
    }

    const std::string mode = argv[1];
    const bool isGuest = mode == "guest";
    bool forceRelay = false;
    std::vector<std::string> positionalArgs;
    for (int index = 2; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--force-relay")
        {
            forceRelay = true;
        }
        else
        {
            positionalArgs.push_back(argument);
        }
    }

    if (isGuest && positionalArgs.empty())
    {
        std::cerr << "guest mode requires roomCode\n";
        return 1;
    }

    const std::string serverHost = positionalArgs.size() >= (isGuest ? 2U : 1U) ? positionalArgs[isGuest ? 1 : 0] : "127.0.0.1";
    const std::uint16_t httpPort = positionalArgs.size() >= (isGuest ? 3U : 2U) ? static_cast<std::uint16_t>(std::stoi(positionalArgs[isGuest ? 2 : 1])) : 8080;

    SessionInfo session{};
    if (mode == "host")
    {
        session = CreateRoom(serverHost, httpPort);
        std::cout << "Created room code: " << session.roomCode << '\n';
    }
    else if (mode == "guest")
    {
        session = JoinRoom(serverHost, httpPort, positionalArgs[0]);
        std::cout << "Joined room code: " << session.roomCode << '\n';
    }
    else
    {
        std::cerr << "Unknown mode: " << mode << '\n';
        return 1;
    }

    std::cout << "Session token: " << session.sessionToken << '\n';

    SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
        throw std::runtime_error("Failed to create UDP socket");
    }

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(0);
    localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udpSocket, reinterpret_cast<const sockaddr*>(&localAddress), sizeof(localAddress)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(udpSocket);
        throw std::runtime_error("UDP bind failed: " + windup::SocketErrorMessage(error));
    }

    RuntimeState runtimeState{};
    std::thread receiver(UdpReceiveLoop, udpSocket, session.sessionToken, serverHost, session.udpServerPort, std::ref(runtimeState), forceRelay);

    sockaddr_in serverUdpAddress{};
    windup::InitializeSocketAddress(serverHost, session.udpServerPort, serverUdpAddress);
    SendUdpLine(udpSocket, serverUdpAddress, "REGISTER " + session.sessionToken);

    while (true)
    {
        const auto status = GetStatus(serverHost, httpPort, session.roomCode, session.sessionToken);
        const auto statusIt = status.find("status");
        if (statusIt == status.end())
        {
            const auto errorIt = status.find("errorCode");
            throw std::runtime_error("Status query failed: " + (errorIt == status.end() ? std::string("unknown_error") : errorIt->second));
        }

        std::cout << "Status: " << statusIt->second << '\n';
        if (statusIt->second == "connected")
        {
            std::cout << "UDP session established.\n";
            break;
        }
        if (statusIt->second == "relay")
        {
            std::cout << "Server relay mode active.\n";
        }
        if (statusIt->second == "failed" || statusIt->second == "expired")
        {
            std::cout << "Session ended with state: " << statusIt->second << '\n';
            break;
        }

        std::this_thread::sleep_for(kPollInterval);
    }

    runtimeState.running.store(false);
    shutdown(udpSocket, SD_BOTH);
    closesocket(udpSocket);
    if (receiver.joinable())
    {
        receiver.join();
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        const windup::WinSockRuntime winSock;
        return RunClient(argc, argv);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "MatchMakingDummyCli failed: " << exception.what() << '\n';
        return 1;
    }
}
