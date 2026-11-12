// Поиск платы MAKCU в локальной сети (UDP). Отдельно от транспорта: один заголовок, только
// Winsock 2, собирается вместе с makcu_udp_batch_transport.h в любом порядке.
//
// Адрес платы у каждого свой: в режиме Device его выдаёт DHCP роутера. Обнаружения по сети
// нет ни в MAK_API, ни в SDK mak-suite (там host обязателен), поэтому поиск сделан обходом:
//   1. запомненный адрес (%LOCALAPPDATA%\MAKCU\udp_board.txt): если там та же плата, обхода нет;
//   2. GET DEVICE 0x02 на порт команд каждого адреса подсетей ПК. Платой считается только тот,
//      кто вернул кадр обёртки RAW с нашим nonce и опкодом. Запрос только читает;
//   3. у ответивших - FIRMWARE 0x04 и MAC из статуса 0x3D (формат из бандла Toolkit).
//
// Как пользоваться (перед open(), один раз):
//   std::string host = "auto", err;
//   if (!makcu::resolveUdpHost(host, port, err) || !tx.open(host, port, err)) { /* err */ }
//
// host в resolveUdpHost:
//   "auto"              найти плату; если плат несколько, выбрать запомненную по MAC, иначе ошибка
//   MAC ("6c-1f-...")   найти именно эту плату: MAC, в отличие от IP, не меняется
//   всё остальное       IP или имя, возвращается как есть, сеть не трогается
//
// Ограничения:
//   - обходится одна /24 вокруг адреса ПК, даже если подсеть шире: дальше обход слишком долгий.
//     Плата в другом месте широкой подсети находится только по явному адресу;
//   - 169.254.x.x пропускается: это адрес без DHCP, /16 не обойти;
//   - порт команд, изменённый в настройках платы, надо передать явно;
//   - поиск работает до open(). Если DHCP сменит адрес на ходу, транспорт его не узнает и
//     будет ждать старый: нужно закрыть транспорт и снова пройти resolveUdpHost + open();
//   - брандмауэр поиску не мешает (ответ идёт с того же адреса и порта, куда ушёл запрос), но
//     событиям кнопок 0x53 нужно разрешающее правило: setup_firewall.bat.
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace makcu {

struct UdpBoard {
    std::string ip;
    uint16_t port = 8080;
    uint8_t kinds = 0;      // ответ DEVICE 0x02, бит 0 - маршрут мыши
    uint32_t firmware = 0;  // ответ FIRMWARE 0x04, 0 - не ответила
    std::string mac;        // "6c-1f-f7-c5-c6-68", пусто - 0x3D не ответила
};

namespace udpfind {

constexpr uint16_t kDefaultPort = 8080;
constexpr uint8_t kOpDevice = 0x02;
constexpr uint8_t kOpFirmware = 0x04;
constexpr uint8_t kOpNetwork = 0x3D;        // GET без данных: статус сети, 62 байта
constexpr size_t kNetworkStatusSize = 62;
constexpr size_t kNetworkMacOffset = 53;
constexpr int kSweepPasses = 3;             // первый проход может уйти на разрешение ARP
constexpr int kSweepWaitMs = 200;
constexpr int kQueryTries = 3;
constexpr int kQueryWaitMs = 150;
constexpr uint32_t kWidestMask = 0xFFFFFF00u;  // шире /24 не обходим

struct Range {
    uint32_t first;  // адреса в порядке хоста, включительно
    uint32_t last;
};

struct Reply {
    uint32_t ip = 0;  // как в sockaddr_in
    uint64_t seq = 0;
    uint8_t op = 0;
    std::vector<uint8_t> data;
};

inline std::string ipText(uint32_t ipNet) {
    in_addr a{};
    a.s_addr = ipNet;
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

class Socket {
public:
    Socket() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        started_ = true;
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return;
        // ICMP "порт недоступен" от чужих адресов иначе ломает recvfrom() ошибкой WSAECONNRESET.
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (::bind(sock_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) return;
        u_long nonblocking = 1;
        ioctlsocket(sock_, FIONBIO, &nonblocking);
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        seq_ = (static_cast<uint64_t>(t.QuadPart) << 20) ^ static_cast<uint64_t>(GetCurrentProcessId());
        ok_ = true;
    }
    ~Socket() {
        if (sock_ != INVALID_SOCKET) closesocket(sock_);
        if (started_) WSACleanup();
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool ok() const { return ok_; }

    // Выделить count подряд идущих nonce, вернуть первый.
    uint64_t reserve(uint64_t count) {
        const uint64_t first = seq_;
        seq_ += count;
        return first;
    }

    // Подсети ПК. Адрес самого ПК не исключается: ответить с него может только то, что
    // говорит на протоколе платы (на этом держится проверка поиска без второй платы).
    std::vector<Range> localRanges() const {
        std::vector<Range> ranges;
        INTERFACE_INFO list[64];
        DWORD bytes = 0;
        if (WSAIoctl(sock_, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list), &bytes, nullptr, nullptr) != 0) {
            return ranges;
        }
        const size_t count = bytes / sizeof(INTERFACE_INFO);
        for (size_t i = 0; i < count; ++i) {
            const INTERFACE_INFO& in = list[i];
            if (!(in.iiFlags & IFF_UP) || (in.iiFlags & IFF_LOOPBACK)) continue;
            const uint32_t ip = ntohl(in.iiAddress.AddressIn.sin_addr.s_addr);
            uint32_t mask = ntohl(in.iiNetmask.AddressIn.sin_addr.s_addr);
            if (ip == 0 || (ip >> 16) == 0xA9FEu) continue;  // 169.254.x.x
            if (mask < kWidestMask) mask = kWidestMask;
            const Range r{(ip & mask) + 1, (ip | ~mask) - 1};
            if (r.last < r.first) continue;  // /31, /32: обходить нечего
            bool seen = false;
            for (const Range& have : ranges) seen = seen || (have.first == r.first && have.last == r.last);
            if (!seen) ranges.push_back(r);
        }
        return ranges;
    }

    // Датаграмма = 0x55 | nonce u64 | DE AD | LEN u16 | op. Здесь только GET без данных.
    bool send(uint32_t ipNet, uint16_t port, uint64_t seq, uint8_t op) const {
        uint8_t d[14]{};
        d[0] = 0x55;
        for (int i = 0; i < 8; ++i) d[1 + i] = static_cast<uint8_t>(seq >> (8 * i));
        d[9] = 0xDE;
        d[10] = 0xAD;
        d[13] = op;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = ipNet;
        return ::sendto(sock_, reinterpret_cast<const char*>(d), sizeof(d), 0, reinterpret_cast<const sockaddr*>(&to),
                   sizeof(to)) == static_cast<int>(sizeof(d));
    }

    // Один кадр обёртки RAW. false - до deadline (GetTickCount64) больше ничего не пришло.
    bool receive(ULONGLONG deadline, Reply& r) const {
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            const ULONGLONG leftMs = deadline - now;
            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock_, &set);
            timeval tv{};
            tv.tv_sec = static_cast<long>(leftMs / 1000);
            tv.tv_usec = static_cast<long>((leftMs % 1000) * 1000);
            if (::select(0, &set, nullptr, nullptr, &tv) <= 0) return false;
            uint8_t d[1500];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = ::recvfrom(sock_, reinterpret_cast<char*>(d), sizeof(d), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 14 || d[0] != 0x55 || d[9] != 0xDE || d[10] != 0xAD) continue;  // ошибка сети или чужой кадр
            const size_t plen = static_cast<size_t>(d[11]) | static_cast<size_t>(d[12]) << 8;
            if (14 + plen > static_cast<size_t>(n)) continue;
            r.ip = from.sin_addr.s_addr;
            r.seq = 0;
            for (int i = 0; i < 8; ++i) r.seq |= static_cast<uint64_t>(d[1 + i]) << (8 * i);
            r.op = d[13];
            r.data.assign(d + 14, d + 14 + plen);
            return true;
        }
    }

    // GET к одному адресу. Команды к плате строго по одной: она выбрасывает датаграмму,
    // пришедшую во время передачи на второй чип.
    std::optional<std::vector<uint8_t>> query(uint32_t ipNet, uint16_t port, uint8_t op) {
        for (int attempt = 0; attempt < kQueryTries; ++attempt) {
            const uint64_t seq = reserve(1);
            if (!send(ipNet, port, seq, op)) continue;
            const ULONGLONG deadline = GetTickCount64() + kQueryWaitMs;
            Reply r;
            while (receive(deadline, r)) {
                if (r.ip == ipNet && r.seq == seq && r.op == op) return r.data;
            }
        }
        return std::nullopt;
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    uint64_t seq_ = 0;
    bool started_ = false;
    bool ok_ = false;
};

inline UdpBoard describe(Socket& s, uint32_t ipNet, uint16_t port, uint8_t kinds) {
    UdpBoard b;
    b.ip = ipText(ipNet);
    b.port = port;
    b.kinds = kinds;
    if (auto fw = s.query(ipNet, port, kOpFirmware); fw && fw->size() == 4) {
        const uint8_t* p = fw->data();
        b.firmware = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                     static_cast<uint32_t>(p[3]) << 24;
    }
    if (auto st = s.query(ipNet, port, kOpNetwork); st && st->size() == kNetworkStatusSize) {
        const uint8_t* m = st->data() + kNetworkMacOffset;
        char text[18]{};
        std::snprintf(text, sizeof(text), "%02x-%02x-%02x-%02x-%02x-%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        b.mac = text;
    }
    return b;
}

// "6C:1F:F7:C5:C6:68" и "6c-1f-f7-c5-c6-68" -> нижний регистр через дефис.
inline bool parseMac(const std::string& s, std::string& mac) {
    if (s.size() != 17) return false;
    std::string out = s;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (i % 3 == 2) {
            if (c != '-' && c != ':') return false;
            out[i] = '-';
        } else {
            if (!std::isxdigit(c)) return false;
            out[i] = static_cast<char>(std::tolower(c));
        }
    }
    mac = out;
    return true;
}

// Запомненная плата: необязательное ускорение и выбор среди нескольких плат. Не вышло
// прочитать или записать файл - просто будет обход.
inline std::string lastBoardPath() {
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string path(dir);
    path += "\\MAKCU";
    CreateDirectoryA(path.c_str(), nullptr);
    return path + "\\udp_board.txt";
}

inline void loadLastBoard(std::string& ip, std::string& mac) {
    const std::string path = lastBoardPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char ipBuf[64]{}, macBuf[32]{};
    if (std::fscanf(f, "%63s %31s", ipBuf, macBuf) == 2) {
        ip = ipBuf;
        if (std::strcmp(macBuf, "-") != 0) mac = macBuf;
    }
    std::fclose(f);
}

inline void saveLastBoard(const UdpBoard& b) {
    const std::string path = lastBoardPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s %s\n", b.ip.c_str(), b.mac.empty() ? "-" : b.mac.c_str());
    std::fclose(f);
}

}  // namespace udpfind

// Спросить один адрес (только IP). nullopt - там не плата или она молчит.
inline std::optional<UdpBoard> probeUdpBoard(const std::string& ip, uint16_t port = udpfind::kDefaultPort) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return std::nullopt;
    udpfind::Socket s;
    if (!s.ok()) return std::nullopt;
    auto kinds = s.query(a.s_addr, port, udpfind::kOpDevice);
    if (!kinds || kinds->size() != 1) return std::nullopt;
    return udpfind::describe(s, a.s_addr, port, (*kinds)[0]);
}

// Все платы в подсетях ПК. scanned (если задан) - что обошли, для сообщений об ошибке.
inline std::vector<UdpBoard> findUdpBoards(uint16_t port = udpfind::kDefaultPort, std::string* scanned = nullptr) {
    std::vector<UdpBoard> boards;
    udpfind::Socket s;
    if (!s.ok()) return boards;
    std::vector<uint32_t> targets;
    for (const udpfind::Range& r : s.localRanges()) {
        for (uint32_t host = r.first; host <= r.last; ++host) targets.push_back(htonl(host));
        if (scanned) {
            if (!scanned->empty()) *scanned += ", ";
            *scanned += udpfind::ipText(htonl(r.first)) + " - " + udpfind::ipText(htonl(r.last));
        }
    }
    std::vector<int> kinds(targets.size(), -1);
    const uint64_t base = s.reserve(targets.size());
    for (int pass = 0; pass < udpfind::kSweepPasses; ++pass) {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (kinds[i] < 0) s.send(targets[i], port, base + i, udpfind::kOpDevice);
        }
        const ULONGLONG deadline = GetTickCount64() + udpfind::kSweepWaitMs;
        udpfind::Reply r;
        while (s.receive(deadline, r)) {
            const uint64_t i = r.seq - base;
            if (i < targets.size() && targets[i] == r.ip && r.op == udpfind::kOpDevice && r.data.size() == 1) {
                kinds[i] = r.data[0];
            }
        }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        if (kinds[i] >= 0) boards.push_back(udpfind::describe(s, targets[i], port, static_cast<uint8_t>(kinds[i])));
    }
    return boards;
}

// Превратить host в адрес для open(), см. шапку. false + причина в err.
inline bool resolveUdpHost(std::string& host, uint16_t port, std::string& err) {
    std::string wantMac;
    const bool byMac = udpfind::parseMac(host, wantMac);
    if (host != "auto" && !byMac) return true;

    std::string lastIp, lastMac;
    udpfind::loadLastBoard(lastIp, lastMac);
    if (!byMac) wantMac = lastMac;
    if (!lastIp.empty()) {
        if (auto b = probeUdpBoard(lastIp, port); b && (wantMac.empty() || b->mac == wantMac)) {
            host = b->ip;
            return true;
        }
    }

    std::string scanned;
    const std::vector<UdpBoard> boards = findUdpBoards(port, &scanned);
    const UdpBoard* pick = nullptr;
    for (const UdpBoard& b : boards) {
        if (!wantMac.empty() && b.mac == wantMac) pick = &b;
    }
    if (!pick && !byMac && boards.size() == 1) pick = &boards[0];
    if (!pick) {
        std::string list;
        for (const UdpBoard& b : boards) {
            if (!list.empty()) list += ", ";
            list += b.ip + " (" + (b.mac.empty() ? "MAC неизвестен" : b.mac) + ")";
        }
        if (boards.empty()) {
            err = "плата не найдена на порту " + std::to_string(port) + ", обойдено: " +
                  (scanned.empty() ? "нет подсетей" : scanned) +
                  ". Плата в этой сети (после подключения ~15 с)? Другой порт или сеть - задайте адрес: --host IP";
        } else if (byMac) {
            err = "платы " + wantMac + " нет в сети, отвечают: " + list;
        } else {
            err = "найдено несколько плат: " + list + ". Укажите нужную: --host IP или --host MAC";
        }
        return false;
    }
    udpfind::saveLastBoard(*pick);
    host = pick->ip;
    return true;
}

}  // namespace makcu
