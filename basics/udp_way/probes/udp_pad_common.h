// udp_pad_common.h - общее для проб геймпада по UDP (udp_pad_probe.cpp, udp_pad_inject.cpp). Не эталон.
//
// Board: свой UDP-сокет без connect() (события платы приходят с другого порта), обёртка RAW: 0x55 + nonce u64 + кадр
// DE AD LEN CMD PAYLOAD. ask() - команда с ответом, не ближе minGapUs к прошлой (по умолчанию 4 мс: предел темпа команд
// UDP, на 4122 при ~500/с USB3 платы виснет, HANDOFF_UDP_TRACKING_HANG.md). post() - команда без ответа (SET 0x40),
// пауза задаётся вызывающим. События 0x75 (поток физики) копятся в events.
// XInput: поток читает геймпад Windows раз в ~120 мкс, при g_xiLog пишет каждый новый пакет.
#pragma once
#include "makcu_udp_discover.h"

#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace udppad {

inline double nowUs() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
}
// Ждать до t. Sleep(1) - только если до срока больше 20 мс: Windows 11 у фонового процесса без видимого окна не держит
// timeBeginPeriod(1), и Sleep(1) растягивается до ~15.6 мс (2026-11-19: на периоде 4 мс проба через ~8 с начала опаздывать,
// пропускать такты и запросы снимка - ложные «плата не отвечает»). Плюс noPowerThrottling() в main().
inline void spinUntil(double t) {
    while (nowUs() < t) {
        if (t - nowUs() > 20000.0) Sleep(1);
        else YieldProcessor();
    }
}
// Снять с процесса энергосбережение Windows (EcoQoS и игнорирование разрешения таймера). Ошибки молча пропускаются.
inline void noPowerThrottling() {
    struct Throttling {
        ULONG Version, ControlMask, StateMask;
    } st{1, 0x1 | 0x4, 0};  // PROCESS_POWER_THROTTLING_EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, состояние - выключено
    using SetInfo = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    if (HMODULE k = GetModuleHandleA("kernel32.dll")) {
        if (auto f = reinterpret_cast<SetInfo>(reinterpret_cast<void*>(GetProcAddress(k, "SetProcessInformation")))) f(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof(st));
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24); }

// ---- XInput ----
#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);
struct XRow {
    double t;
    int16_t rx, ry;
};
inline std::vector<XRow> g_xi;
inline std::mutex g_xiMutex;
inline std::atomic<bool> g_stop{false}, g_xiLog{false}, g_xiOk{false};
inline std::atomic<uint64_t> g_xiPackets{0};

inline void xinputLoop() {
    HMODULE dll = LoadLibraryA("xinput1_4.dll");
    if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
    if (!dll) return;
    const auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int slot = -1;
    DWORD last = 0;
    while (!g_stop) {
        XState s{};
        if (slot < 0) {
            for (DWORD i = 0; i < 4 && slot < 0; ++i) {
                if (get(i, &s) == ERROR_SUCCESS) slot = static_cast<int>(i);
            }
            if (slot < 0) {
                g_xiOk = false;
                Sleep(50);
                continue;
            }
        } else if (get(static_cast<DWORD>(slot), &s) != ERROR_SUCCESS) {
            slot = -1;
            continue;
        }
        g_xiOk = true;
        if (s.pkt != last) {
            last = s.pkt;
            g_xiPackets.fetch_add(1);
            if (g_xiLog) {
                std::lock_guard<std::mutex> lock(g_xiMutex);
                g_xi.push_back({nowUs(), s.rx, s.ry});
            }
        }
        const double until = nowUs() + 120.0;
        while (nowUs() < until) YieldProcessor();
    }
}

// ---- UDP ----
struct Board {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in to{};
    uint64_t seq = 0x5AD0000000000000ull;
    double lastSendUs = -1e9;
    double minGapUs = 4000.0;
    struct Ev {
        double t;
        uint8_t kind, id;
        uint16_t value;
    };
    std::vector<Ev> events;
    uint64_t foreign = 0, lateReplies = 0, posted = 0, stray = 0, replies54 = 0;
    std::vector<uint8_t> last54;  // последний ответ на GET 0x54, отправленный через post()
    double last54T = 0;

    bool open(const std::string& host, uint16_t port, std::string& err) {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            err = "socket()";
            return false;
        }
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
        int buf = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        if (bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
            err = "bind()";
            return false;
        }
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) {
            err = "адрес " + host;
            return false;
        }
        return true;
    }
    bool take(const uint8_t* d, int n, uint64_t want, uint8_t op, std::vector<uint8_t>& reply) {
        if (n < 14 || d[0] != 0x55 || d[9] != 0xDE || d[10] != 0xAD) {
            ++foreign;
            return false;
        }
        uint64_t sq = 0;
        for (int i = 0; i < 8; ++i) sq |= static_cast<uint64_t>(d[1 + i]) << (8 * i);
        const size_t plen = le16(d + 11);
        if (14 + plen > static_cast<size_t>(n)) {
            ++foreign;
            return false;
        }
        const uint8_t* p = d + 14;
        if (d[13] == 0x75) {  // событие несёт nonce какой-то прошлой команды - разбирать по опкоду
            if (plen == 4) events.push_back({nowUs(), p[0], p[1], le16(p + 2)});
            else ++foreign;
            return false;
        }
        if (want && sq == want && d[13] == op) {
            reply.assign(p, p + plen);
            return true;
        }
        if (!want && d[13] == 0x54) {  // снимок, отправленный post() без ожидания (долгий прогон)
            last54.assign(p, p + plen);
            last54T = nowUs();
            ++replies54;
            return false;
        }
        if (want) ++lateReplies;
        else ++stray;  // ответ, которого никто не ждал (например, на SET 0x40)
        return false;
    }
    // Вычитать всё, что пришло, ждать до waitUs (0 - только то, что уже лежит).
    void drain(double waitUs) {
        std::vector<uint8_t> dummy;
        const double until = nowUs() + waitUs;
        uint8_t d[2048];
        for (;;) {
            const double left = std::max(0.0, until - nowUs());
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            timeval tv{0, static_cast<long>(left)};
            if (select(0, &rs, nullptr, nullptr, &tv) <= 0) {
                if (nowUs() >= until) return;
                continue;
            }
            const int n = recvfrom(s, reinterpret_cast<char*>(d), sizeof(d), 0, nullptr, nullptr);
            if (n > 0) take(d, n, 0, 0, dummy);
        }
    }
    void sendRaw(uint8_t op, const uint8_t* pl, size_t len, uint64_t sq) {
        uint8_t pkt[512];
        pkt[0] = 0x55;
        for (int i = 0; i < 8; ++i) pkt[1 + i] = static_cast<uint8_t>(sq >> (8 * i));
        pkt[9] = 0xDE;
        pkt[10] = 0xAD;
        pkt[11] = static_cast<uint8_t>(len);
        pkt[12] = static_cast<uint8_t>(len >> 8);
        pkt[13] = op;
        std::memcpy(pkt + 14, pl, len);
        lastSendUs = nowUs();
        sendto(s, reinterpret_cast<const char*>(pkt), static_cast<int>(14 + len), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
    // Команда без ответа, сразу (темп держит вызывающий).
    void post(uint8_t op, const uint8_t* pl, size_t len) {
        sendRaw(op, pl, len, ++seq);
        ++posted;
    }
    // Команда с ответом; не ближе minGapUs к прошлой. nullopt - нет ответа за timeoutUs.
    std::optional<std::vector<uint8_t>> ask(uint8_t op, const std::vector<uint8_t>& pl, double timeoutUs = 100000.0) {
        const double gap = lastSendUs + minGapUs - nowUs();
        if (gap > 0) drain(gap);
        const uint64_t sq = ++seq;
        sendRaw(op, pl.data(), pl.size(), sq);
        std::vector<uint8_t> reply;
        const double until = lastSendUs + timeoutUs;
        uint8_t d[2048];
        for (;;) {
            const double left = until - nowUs();
            if (left <= 0) return std::nullopt;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            timeval tv{0, static_cast<long>(left)};
            if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
            const int n = recvfrom(s, reinterpret_cast<char*>(d), sizeof(d), 0, nullptr, nullptr);
            if (n > 0 && take(d, n, sq, op, reply)) return reply;
        }
    }

    // ---- настройки 0x3E / 0x1D (DEVICE_SETTINGS.md), как в COM-эталоне геймпада: APPLY без SAVE ----
    bool readImage(std::array<uint8_t, 400>& img, uint32_t& rev, uint8_t& sections) {
        auto info = ask(0x3E, {0x1D, 0x00}, 300000.0);
        if (!(info && info->size() == 14 && (*info)[0] == 0x1D && (*info)[1] == 0x00 && (*info)[2] == 0 && le16(info->data() + 12) == 400)) return false;
        rev = le32(info->data() + 8);
        sections = (*info)[4];
        for (unsigned off = 0; off < 400; off += 96) {
            const unsigned len = std::min(96u, 400u - off);
            auto r = ask(0x3E, {0x1D, 0x01, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24),
                                   static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8), static_cast<uint8_t>(len)},
                300000.0);
            if (!(r && r->size() == 9 + len && (*r)[0] == 0x1D && (*r)[1] == 0x01 && (*r)[2] == 0 && le32(r->data() + 3) == rev)) return false;
            std::copy(r->begin() + 9, r->end(), img.begin() + off);
        }
        return true;
    }
    // interp / bufferMs < 0 - не трогать. true - применено (или уже такое).
    bool writeSmoothing(int interp, int bufferMs) {
        std::array<uint8_t, 400> img{};
        uint32_t rev = 0;
        uint8_t sections = 0;
        if (!readImage(img, rev, sections) || !(sections & 1u)) return false;
        const bool same = (interp < 0 || le32(&img[4]) == static_cast<uint32_t>(interp)) && (bufferMs < 0 || le32(&img[8]) == static_cast<uint32_t>(bufferMs));
        if (same) return true;
        if (interp >= 0) {
            img[4] = static_cast<uint8_t>(interp);
            img[5] = img[6] = img[7] = 0;
        }
        if (bufferMs >= 0) {
            img[8] = static_cast<uint8_t>(bufferMs);
            img[9] = img[10] = img[11] = 0;
        }
        auto b = ask(0x3E, {0x1D, 0x02, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24), 0x01}, 300000.0);
        if (!(b && b->size() == 7 && (*b)[0] == 0x1D && (*b)[1] == 0x02 && (*b)[2] == 0)) return false;
        const uint8_t tk[4] = {(*b)[3], (*b)[4], (*b)[5], (*b)[6]};
        bool ok = true;
        for (unsigned off = 0; off < 400 && ok; off += 96) {
            const unsigned len = std::min(96u, 400u - off);
            std::vector<uint8_t> wr = {0x1D, 0x03, tk[0], tk[1], tk[2], tk[3], static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8)};
            wr.insert(wr.end(), img.begin() + off, img.begin() + off + len);
            auto r = ask(0x3E, wr, 300000.0);
            ok = r && r->size() == 5 && (*r)[0] == 0x1D && (*r)[1] == 0x03 && (*r)[2] == 0 && le16(r->data() + 3) == off + len;
        }
        if (ok) {
            auto r = ask(0x3E, {0x1D, 0x04, tk[0], tk[1], tk[2], tk[3]}, 300000.0);
            ok = r && r->size() == 3 && (*r)[0] == 0x1D && (*r)[1] == 0x04 && (*r)[2] == 0;
        }
        if (!ok) ask(0x3E, {0x1D, 0x06, tk[0], tk[1], tk[2], tk[3]}, 100000.0);
        return ok;
    }
    // Интерполяция и буфер на плате; false - не прочитано.
    bool readSmoothing(int& interp, int& bufferMs, uint8_t* sectionsOut = nullptr) {
        std::array<uint8_t, 400> img{};
        uint32_t rev = 0;
        uint8_t sections = 0;
        if (!readImage(img, rev, sections)) return false;
        interp = static_cast<int>(le32(&img[4]));
        bufferMs = static_cast<int>(le32(&img[8]));
        if (sectionsOut) *sectionsOut = sections;
        return true;
    }
};

inline std::string hex(const std::vector<uint8_t>& v) {
    std::string out;
    char b[4];
    for (uint8_t x : v) {
        std::snprintf(b, sizeof(b), "%02X ", x);
        out += b;
    }
    return out.empty() ? "(пусто)" : out;
}

struct Snap {
    bool ok = false;
    uint32_t seq = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint16_t lt = 0, rt = 0, timing = 0, cadence = 0;
};
inline Snap parse54(const std::optional<std::vector<uint8_t>>& r) {
    Snap s;
    if (!r || r->size() != 32) return s;
    const uint8_t* p = r->data();
    s.ok = true;
    s.lt = le16(p + 8);
    s.rt = le16(p + 10);
    s.lx = static_cast<int16_t>(le16(p + 12));
    s.ly = static_cast<int16_t>(le16(p + 14));
    s.rx = static_cast<int16_t>(le16(p + 16));
    s.ry = static_cast<int16_t>(le16(p + 18));
    s.seq = le32(p + 20);
    s.timing = le16(p + 28);
    s.cadence = le16(p + 30);
    return s;
}

inline double pct(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

}  // namespace udppad
