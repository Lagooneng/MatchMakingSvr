#pragma once

#include "../Common/Protocol.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace windup
{
enum class RoomState
{
    WaitingForGuest,
    WaitingForUdp,
    Punching,
    Relay,
    Connected,
    Failed,
    Expired
};

struct PeerSession
{
    std::string token;
    std::string role;
    Endpoint publicEndpoint;
    bool hasUdpRegistration = false;
    std::chrono::steady_clock::time_point lastUdpSeen{};
};

struct Room
{
    std::string code;
    PeerSession host;
    std::optional<PeerSession> guest;
    RoomState state = RoomState::WaitingForGuest;
    std::chrono::system_clock::time_point createdAt{};
    std::chrono::system_clock::time_point expiresAt{};
    std::chrono::steady_clock::time_point punchStartedAt{};
};

struct CreateRoomResult
{
    std::string roomCode;
    std::string sessionToken;
    std::chrono::system_clock::time_point expiresAt{};
};

struct JoinRoomResult
{
    bool success = false;
    std::string sessionToken;
    std::string errorCode;
    std::chrono::system_clock::time_point expiresAt{};
};

struct StatusResult
{
    bool found = false;
    std::string status;
    std::string role;
    std::string errorCode;
    std::optional<Endpoint> peerEndpoint;
};

class MatchmakingServer
{
public:
    MatchmakingServer(std::uint16_t httpPort, std::uint16_t udpPort);
    ~MatchmakingServer();

    void Run();
    void Stop();

private:
    void SetupSockets();
    void RunHttpLoop();
    void RunUdpLoop();
    void RunMaintenanceLoop();
    void HandleHttpClient(SOCKET clientSocket);
    HttpResponse HandleHttpRequest(const HttpRequest& request);
    void HandleUdpPacket(const std::string& message, const sockaddr_in& remoteAddress);
    void SendUdpMessage(const sockaddr_in& address, const std::string& message);
    void BeginRelay(Room& room);

    CreateRoomResult CreateRoom();
    JoinRoomResult JoinRoom(const std::string& roomCode);
    StatusResult GetStatus(const std::string& roomCode, const std::string& sessionToken);
    void RegisterUdp(const std::string& sessionToken, const sockaddr_in& remoteAddress);
    void MarkConnected(const std::string& sessionToken);
    void RelayPayload(const std::string& sessionToken, const std::string& payload);
    void CleanupExpiredRooms();

    std::string GenerateUniqueRoomCodeLocked();
    static std::string StateToString(RoomState state);
    static std::chrono::steady_clock::time_point NowSteady();
    static std::chrono::system_clock::time_point NowSystem();

    std::uint16_t httpPort_;
    std::uint16_t udpPort_;
    SOCKET httpSocket_ = INVALID_SOCKET;
    SOCKET udpSocket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread httpThread_;
    std::thread udpThread_;
    std::thread maintenanceThread_;
    std::mutex roomsMutex_;
    std::unordered_map<std::string, Room> roomsByCode_;
};
} // namespace windup
