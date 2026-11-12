// Показать платы MAKCU в локальной сети (makcu_udp_discover.h).
//
//   find_makcu.exe [--port 8080]
//
// Курсор не двигается, на плату уходят только запросы чтения, рука не нужна. Запомненный адрес
// здесь не используется и не меняется: обход всегда полный. Код возврата: 0 - плата найдена.
#include "makcu_udp_discover.h"

#include <cstdlib>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    uint16_t port = makcu::udpfind::kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else {
            std::printf("find_makcu.exe [--port 8080]\n");
            return 2;
        }
    }
    std::string scanned;
    const ULONGLONG t0 = GetTickCount64();
    const std::vector<makcu::UdpBoard> boards = makcu::findUdpBoards(port, &scanned);
    const ULONGLONG ms = GetTickCount64() - t0;
    std::printf("обойдено: %s (%llu мс)\n", scanned.empty() ? "нет подсетей" : scanned.c_str(), static_cast<unsigned long long>(ms));
    for (const makcu::UdpBoard& b : boards) {
        std::printf("[+] %s:%u | MAC %s | прошивка %u | маршрут мыши %s\n", b.ip.c_str(), b.port,
            b.mac.empty() ? "неизвестен" : b.mac.c_str(), b.firmware, (b.kinds & 1) ? "да" : "нет");
    }
    if (boards.empty()) std::printf("[-] плат нет: плата в этой сети (после подключения ~15 с)? порт другой - задайте --port\n");
    return boards.empty() ? 1 : 0;
}
