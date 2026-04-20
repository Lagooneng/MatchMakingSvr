#include "MatchmakingServer.h"

#include <array>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace windup
{
namespace
{
constexpr auto kRoomLifetime = std::chrono::minutes(5);
constexpr auto kPunchTimeout = std::chrono::seconds(15);
constexpr int kReceiveBufferSize = 4096;

std::optional<std::string> ExtractRoomCodeFromPath(const std::string& path)
{
    const std::vector<std::string> segments = SplitString(path, '/');
    if (segments.size() == 4 && segments[1] == "rooms" && segments[3] == "status")
    {
        return segments[2];
    }
    return std::nullopt;
}
} // namespace

MatchmakingServer::MatchmakingServer(std::uint16_t httpPort, std::uint16_t udpPort)
    : httpPort_(httpPort), udpPort_(udpPort)
{
}

MatchmakingServer::~MatchmakingServer()
{
    Stop();
}

void MatchmakingServer::Run()
{
    SetupSockets();
    running_.store(true);
    httpThread_ = std::thread(&MatchmakingServer::RunHttpLoop, this);
    udpThread_ = std::thread(&MatchmakingServer::RunUdpLoop, this);
    maintenanceThread_ = std::thread(&MatchmakingServer::RunMaintenanceLoop, this);
}

void MatchmakingServer::Stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (httpSocket_ != INVALID_SOCKET)
    {
        closesocket(httpSocket_);
        httpSocket_ = INVALID_SOCKET;
    }

    if (udpSocket_ != INVALID_SOCKET)
    {
        closesocket(udpSocket_);
        udpSocket_ = INVALID_SOCKET;
    }

    if (httpThread_.joinable())
    {
        httpThread_.join();
    }
    if (udpThread_.joinable())
    {
        udpThread_.join();
    }
    if (maintenanceThread_.joinable())
    {
        maintenanceThread_.join();
    }
}

void MatchmakingServer::SetupSockets()
{
    httpSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (httpSocket_ == INVALID_SOCKET || udpSocket_ == INVALID_SOCKET)
    {
        throw std::runtime_error("Failed to create server sockets");
    }

    const BOOL reuseAddress = TRUE;
    setsockopt(httpSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));
    setsockopt(udpSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

    sockaddr_in httpAddress{};
    httpAddress.sin_family = AF_INET;
    httpAddress.sin_port = htons(httpPort_);
    httpAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    sockaddr_in udpAddress{};
    udpAddress.sin_family = AF_INET;
    udpAddress.sin_port = htons(udpPort_);
    udpAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(httpSocket_, reinterpret_cast<const sockaddr*>(&httpAddress), sizeof(httpAddress)) == SOCKET_ERROR)
    {
        throw std::runtime_error("HTTP bind failed: " + SocketErrorMessage(WSAGetLastError()));
    }

    if (listen(httpSocket_, SOMAXCONN) == SOCKET_ERROR)
    {
        throw std::runtime_error("HTTP listen failed: " + SocketErrorMessage(WSAGetLastError()));
    }

    if (bind(udpSocket_, reinterpret_cast<const sockaddr*>(&udpAddress), sizeof(udpAddress)) == SOCKET_ERROR)
    {
        throw std::runtime_error("UDP bind failed: " + SocketErrorMessage(WSAGetLastError()));
    }
}

void MatchmakingServer::RunHttpLoop()
{
    while (running_.load())
    {
        sockaddr_in remoteAddress{};
        int remoteLength = sizeof(remoteAddress);
        const SOCKET clientSocket = accept(httpSocket_, reinterpret_cast<sockaddr*>(&remoteAddress), &remoteLength);
        if (clientSocket == INVALID_SOCKET)
        {
            if (running_.load())
            {
                std::cerr << "HTTP accept failed: " << SocketErrorMessage(WSAGetLastError()) << '\n';
            }
            break;
        }

        std::thread(&MatchmakingServer::HandleHttpClient, this, clientSocket).detach();
    }
}

void MatchmakingServer::RunUdpLoop()
{
    std::array<char, kReceiveBufferSize> buffer{};
    while (running_.load())
    {
        sockaddr_in remoteAddress{};
        int remoteLength = sizeof(remoteAddress);
        const int received = recvfrom(
            udpSocket_,
            buffer.data(),
            static_cast<int>(buffer.size() - 1),
            0,
            reinterpret_cast<sockaddr*>(&remoteAddress),
            &remoteLength);

        if (received == SOCKET_ERROR)
        {
            if (running_.load())
            {
                std::cerr << "UDP recvfrom failed: " << SocketErrorMessage(WSAGetLastError()) << '\n';
            }
            break;
        }

        buffer[received] = '\0';
        HandleUdpPacket(std::string(buffer.data(), received), remoteAddress);
    }
}

void MatchmakingServer::RunMaintenanceLoop()
{
    while (running_.load())
    {
        CleanupExpiredRooms();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void MatchmakingServer::HandleHttpClient(SOCKET clientSocket)
{
    std::string rawRequest;
    std::array<char, kReceiveBufferSize> buffer{};
    int expectedBodyLength = -1;

    while (true)
    {
        const int received = recv(clientSocket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0)
        {
            break;
        }

        rawRequest.append(buffer.data(), received);
        const std::size_t headerEnd = rawRequest.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
        {
            continue;
        }

        if (expectedBodyLength < 0)
        {
            const std::string headerBlock = rawRequest.substr(0, headerEnd);
            const std::size_t contentLengthPos = headerBlock.find("Content-Length:");
            if (contentLengthPos == std::string::npos)
            {
                expectedBodyLength = 0;
            }
            else
            {
                const std::size_t valueStart = contentLengthPos + std::string("Content-Length:").size();
                const std::size_t lineEnd = headerBlock.find("\r\n", valueStart);
                expectedBodyLength = std::stoi(Trim(headerBlock.substr(valueStart, lineEnd - valueStart)));
            }
        }

        const std::size_t totalExpected = headerEnd + 4 + static_cast<std::size_t>(expectedBodyLength);
        if (rawRequest.size() >= totalExpected)
        {
            break;
        }
    }

    HttpResponse response{};
    const std::optional<HttpRequest> request = ParseHttpRequest(rawRequest);
    if (!request.has_value())
    {
        response.statusCode = 400;
        response.body = BuildJsonObject({{"errorCode", "invalid_http_request"}});
    }
    else
    {
        response = HandleHttpRequest(request.value());
    }

    const std::string payload = BuildHttpResponse(response);
    send(clientSocket, payload.c_str(), static_cast<int>(payload.size()), 0);
    closesocket(clientSocket);
}

HttpResponse MatchmakingServer::HandleHttpRequest(const HttpRequest& request)
{
    if (request.method == "POST" && request.path == "/rooms")
    {
        const CreateRoomResult room = CreateRoom();
        return HttpResponse{
            201,
            "Created",
            {},
            BuildJsonObject({
                {"roomCode", room.roomCode},
                {"sessionToken", room.sessionToken},
                {"udpServerPort", std::to_string(udpPort_)},
                {"expiresAt", FormatUtcIso8601(room.expiresAt)},
                {"status", StateToString(RoomState::WaitingForGuest)},
            })};
    }

    if (request.method == "POST" && request.path == "/rooms/join")
    {
        const auto body = ParseSimpleJsonObject(request.body);
        const auto roomCodeIt = body.find("roomCode");
        if (roomCodeIt == body.end() || roomCodeIt->second.empty())
        {
            return HttpResponse{400, "Bad Request", {}, BuildJsonObject({{"errorCode", "missing_room_code"}})};
        }

        const JoinRoomResult joined = JoinRoom(roomCodeIt->second);
        if (!joined.success)
        {
            const int statusCode = joined.errorCode == "room_not_found" ? 404 :
                joined.errorCode == "room_expired" ? 410 : 409;
            return HttpResponse{statusCode, "", {}, BuildJsonObject({{"errorCode", joined.errorCode}})};
        }

        return HttpResponse{
            200,
            "OK",
            {},
            BuildJsonObject({
                {"roomCode", roomCodeIt->second},
                {"sessionToken", joined.sessionToken},
                {"udpServerPort", std::to_string(udpPort_)},
                {"expiresAt", FormatUtcIso8601(joined.expiresAt)},
                {"status", StateToString(RoomState::WaitingForUdp)},
            })};
    }

    if (request.method == "GET")
    {
        const auto roomCode = ExtractRoomCodeFromPath(request.path);
        if (roomCode.has_value())
        {
            const auto tokenIt = request.query.find("sessionToken");
            if (tokenIt == request.query.end())
            {
                return HttpResponse{400, "Bad Request", {}, BuildJsonObject({{"errorCode", "missing_session_token"}})};
            }

            const StatusResult status = GetStatus(roomCode.value(), tokenIt->second);
            if (!status.found)
            {
                return HttpResponse{404, "Not Found", {}, BuildJsonObject({{"errorCode", status.errorCode}})};
            }

            std::vector<std::pair<std::string, std::string>> fields{
                {"status", status.status},
                {"role", status.role},
            };
            if (status.peerEndpoint.has_value())
            {
                fields.emplace_back("peerPublicIp", status.peerEndpoint->ip);
                fields.emplace_back("peerPublicPort", std::to_string(status.peerEndpoint->port));
            }

            return HttpResponse{200, "OK", {}, BuildJsonObject(fields)};
        }
    }

    return HttpResponse{404, "Not Found", {}, BuildJsonObject({{"errorCode", "unknown_route"}})};
}

void MatchmakingServer::HandleUdpPacket(const std::string& message, const sockaddr_in& remoteAddress)
{
    const std::vector<std::string> parts = SplitString(Trim(message), ' ');
    if (parts.empty())
    {
        return;
    }

    if (parts[0] == "REGISTER" && parts.size() >= 2)
    {
        RegisterUdp(parts[1], remoteAddress);
    }
    else if (parts[0] == "CONNECTED" && parts.size() >= 2)
    {
        MarkConnected(parts[1]);
    }
    else if (parts[0] == "RELAY")
    {
        const std::size_t firstSpace = message.find(' ');
        const std::size_t secondSpace = firstSpace == std::string::npos ? std::string::npos : message.find(' ', firstSpace + 1);
        if (firstSpace != std::string::npos && secondSpace != std::string::npos)
        {
            const std::string sessionToken = message.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            const std::string payload = message.substr(secondSpace + 1);
            RelayPayload(sessionToken, payload);
        }
    }
}

void MatchmakingServer::SendUdpMessage(const sockaddr_in& address, const std::string& message)
{
    sendto(
        udpSocket_,
        message.c_str(),
        static_cast<int>(message.size()),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
}

void MatchmakingServer::BeginRelay(Room& room)
{
    if (!room.guest.has_value() || !room.host.hasUdpRegistration || !room.guest->hasUdpRegistration)
    {
        room.state = RoomState::Failed;
        return;
    }

    sockaddr_in hostAddress{};
    sockaddr_in guestAddress{};
    if (!InitializeSocketAddress(room.host.publicEndpoint.ip, room.host.publicEndpoint.port, hostAddress) ||
        !InitializeSocketAddress(room.guest->publicEndpoint.ip, room.guest->publicEndpoint.port, guestAddress))
    {
        room.state = RoomState::Failed;
        return;
    }

    SendUdpMessage(hostAddress, "RELAY_READY guest");
    SendUdpMessage(guestAddress, "RELAY_READY host");
    room.state = RoomState::Relay;
}

CreateRoomResult MatchmakingServer::CreateRoom()
{
    std::scoped_lock lock(roomsMutex_);

    Room room{};
    room.code = GenerateUniqueRoomCodeLocked();
    room.host.token = GenerateToken(24);
    room.host.role = "host";
    room.state = RoomState::WaitingForGuest;
    room.createdAt = NowSystem();
    room.expiresAt = room.createdAt + kRoomLifetime;

    const CreateRoomResult result{room.code, room.host.token, room.expiresAt};
    roomsByCode_.emplace(room.code, room);
    return result;
}

JoinRoomResult MatchmakingServer::JoinRoom(const std::string& roomCode)
{
    std::scoped_lock lock(roomsMutex_);
    const auto roomIt = roomsByCode_.find(roomCode);
    if (roomIt == roomsByCode_.end())
    {
        return {false, {}, "room_not_found", {}};
    }

    Room& room = roomIt->second;
    if (NowSystem() >= room.expiresAt)
    {
        room.state = RoomState::Expired;
        return {false, {}, "room_expired", {}};
    }

    if (room.guest.has_value())
    {
        return {false, {}, "room_full", {}};
    }

    PeerSession guest{};
    guest.token = GenerateToken(24);
    guest.role = "guest";
    room.guest = guest;
    room.state = RoomState::WaitingForUdp;

    return {true, room.guest->token, {}, room.expiresAt};
}

StatusResult MatchmakingServer::GetStatus(const std::string& roomCode, const std::string& sessionToken)
{
    std::scoped_lock lock(roomsMutex_);
    const auto roomIt = roomsByCode_.find(roomCode);
    if (roomIt == roomsByCode_.end())
    {
        return {false, {}, {}, "room_not_found", std::nullopt};
    }

    Room& room = roomIt->second;
    if (NowSystem() >= room.expiresAt && room.state != RoomState::Connected)
    {
        room.state = RoomState::Expired;
    }

    PeerSession* self = nullptr;
    PeerSession* peer = nullptr;

    if (room.host.token == sessionToken)
    {
        self = &room.host;
        if (room.guest.has_value())
        {
            peer = &room.guest.value();
        }
    }
    else if (room.guest.has_value() && room.guest->token == sessionToken)
    {
        self = &room.guest.value();
        peer = &room.host;
    }
    else
    {
        return {false, {}, {}, "invalid_session_token", std::nullopt};
    }

    StatusResult result{true, StateToString(room.state), self->role, {}, std::nullopt};
    if (peer != nullptr && peer->hasUdpRegistration)
    {
        result.peerEndpoint = peer->publicEndpoint;
    }
    return result;
}

void MatchmakingServer::RegisterUdp(const std::string& sessionToken, const sockaddr_in& remoteAddress)
{
    std::scoped_lock lock(roomsMutex_);
    for (auto& [roomCode, room] : roomsByCode_)
    {
        PeerSession* self = nullptr;
        PeerSession* peer = nullptr;
        if (room.host.token == sessionToken)
        {
            self = &room.host;
            if (room.guest.has_value())
            {
                peer = &room.guest.value();
            }
        }
        else if (room.guest.has_value() && room.guest->token == sessionToken)
        {
            self = &room.guest.value();
            peer = &room.host;
        }

        if (self == nullptr)
        {
            continue;
        }

        self->publicEndpoint = EndpointFromSockaddr(remoteAddress);
        self->hasUdpRegistration = true;
        self->lastUdpSeen = NowSteady();
        SendUdpMessage(remoteAddress, "REGISTERED " + self->publicEndpoint.ip + " " + std::to_string(self->publicEndpoint.port));

        if (room.guest.has_value() && room.host.hasUdpRegistration && room.guest->hasUdpRegistration)
        {
            sockaddr_in hostAddress{};
            sockaddr_in guestAddress{};
            if (InitializeSocketAddress(room.host.publicEndpoint.ip, room.host.publicEndpoint.port, hostAddress) &&
                InitializeSocketAddress(room.guest->publicEndpoint.ip, room.guest->publicEndpoint.port, guestAddress))
            {
                SendUdpMessage(hostAddress, "PUNCH_START " + room.guest->publicEndpoint.ip + " " + std::to_string(room.guest->publicEndpoint.port));
                SendUdpMessage(guestAddress, "PUNCH_START " + room.host.publicEndpoint.ip + " " + std::to_string(room.host.publicEndpoint.port));
                room.state = RoomState::Punching;
                room.punchStartedAt = NowSteady();
            }
            else
            {
                room.state = RoomState::Failed;
            }
        }
        else if (peer != nullptr)
        {
            room.state = RoomState::WaitingForUdp;
        }

        return;
    }
}

void MatchmakingServer::MarkConnected(const std::string& sessionToken)
{
    std::scoped_lock lock(roomsMutex_);
    for (auto& [roomCode, room] : roomsByCode_)
    {
        const bool matchesHost = room.host.token == sessionToken;
        const bool matchesGuest = room.guest.has_value() && room.guest->token == sessionToken;
        if (matchesHost || matchesGuest)
        {
            room.state = RoomState::Connected;
            return;
        }
    }
}

void MatchmakingServer::RelayPayload(const std::string& sessionToken, const std::string& payload)
{
    std::scoped_lock lock(roomsMutex_);
    for (auto& [roomCode, room] : roomsByCode_)
    {
        if (!room.guest.has_value())
        {
            continue;
        }

        PeerSession* sender = nullptr;
        PeerSession* peer = nullptr;
        if (room.host.token == sessionToken)
        {
            sender = &room.host;
            peer = &room.guest.value();
        }
        else if (room.guest->token == sessionToken)
        {
            sender = &room.guest.value();
            peer = &room.host;
        }

        if (sender == nullptr || !peer->hasUdpRegistration)
        {
            continue;
        }

        sockaddr_in peerAddress{};
        if (!InitializeSocketAddress(peer->publicEndpoint.ip, peer->publicEndpoint.port, peerAddress))
        {
            room.state = RoomState::Failed;
            return;
        }

        SendUdpMessage(peerAddress, "RELAY_FROM " + sender->role + " " + payload);
        if (room.state == RoomState::Relay)
        {
            room.state = RoomState::Connected;
        }
        return;
    }
}

void MatchmakingServer::CleanupExpiredRooms()
{
    std::scoped_lock lock(roomsMutex_);
    const auto nowSystem = NowSystem();
    const auto nowSteady = NowSteady();

    for (auto roomIt = roomsByCode_.begin(); roomIt != roomsByCode_.end();)
    {
        Room& room = roomIt->second;

        if (room.state == RoomState::Punching &&
            room.punchStartedAt != std::chrono::steady_clock::time_point{} &&
            nowSteady - room.punchStartedAt > kPunchTimeout)
        {
            BeginRelay(room);
        }

        if (nowSystem >= room.expiresAt && room.state != RoomState::Connected)
        {
            room.state = RoomState::Expired;
        }

        const bool shouldRemove =
            room.state == RoomState::Failed ||
            room.state == RoomState::Expired ||
            nowSystem > room.expiresAt + std::chrono::minutes(1);
        if (shouldRemove)
        {
            roomIt = roomsByCode_.erase(roomIt);
        }
        else
        {
            ++roomIt;
        }
    }
}

std::string MatchmakingServer::GenerateUniqueRoomCodeLocked()
{
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, 9999);

    for (int attempt = 0; attempt < 10000; ++attempt)
    {
        std::ostringstream builder;
        builder << std::setw(4) << std::setfill('0') << distribution(generator);
        const std::string code = builder.str();
        if (!roomsByCode_.contains(code))
        {
            return code;
        }
    }

    throw std::runtime_error("Unable to allocate room code");
}

std::string MatchmakingServer::StateToString(RoomState state)
{
    switch (state)
    {
    case RoomState::WaitingForGuest:
        return "waiting_guest";
    case RoomState::WaitingForUdp:
        return "waiting_udp";
    case RoomState::Punching:
        return "punching";
    case RoomState::Relay:
        return "relay";
    case RoomState::Connected:
        return "connected";
    case RoomState::Failed:
        return "failed";
    case RoomState::Expired:
        return "expired";
    default:
        return "unknown";
    }
}

std::chrono::steady_clock::time_point MatchmakingServer::NowSteady()
{
    return std::chrono::steady_clock::now();
}

std::chrono::system_clock::time_point MatchmakingServer::NowSystem()
{
    return std::chrono::system_clock::now();
}
} // namespace windup
