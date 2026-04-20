#include "MatchmakingServer.h"

#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
std::atomic<bool> gShouldStop{false};

void HandleSignal(int)
{
    gShouldStop.store(true);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        const windup::WinSockRuntime winSock;
        const std::uint16_t httpPort = argc >= 2 ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 8080;
        const std::uint16_t udpPort = argc >= 3 ? static_cast<std::uint16_t>(std::stoi(argv[2])) : 9000;

        windup::MatchmakingServer server(httpPort, udpPort);
        server.Run();

        std::cout << "MatchMakingSvr listening on HTTP " << httpPort << ", UDP " << udpPort << '\n';
        std::cout << "Press Ctrl+C to stop the server.\n";

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        while (!gShouldStop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        server.Stop();
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Server failed: " << exception.what() << '\n';
        return 1;
    }
}
