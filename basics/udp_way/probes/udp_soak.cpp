// udp_soak.cpp - сколько выдержит плата MAKCU по UDP под потоком одной команды только для чтения.
// Курсор не двигается, окна нет: нужна только живая плата. Проба для поиска причины зависания USB3
// (HANDOFF_UDP_TRACKING_HANG.md): отделить "число команд в секунду" от "какая команда".
//
// Замкнутый цикл: запрос, ответ (или тайм-аут 20 мс), не раньше чем через 1/rate с - следующий.
// Строка раз в секунду в журнал (fflush), раз в 10 с в консоль. Ответов нет 2 с - обрыв: проба
// пишет время и до 30 с раз в 0.5 с спрашивает DEVICE (0x02), вернётся ли плата сама. Код выхода 3.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra udp_soak.cpp -lws2_32 -lwinmm -static -o udp_soak.exe
// Запуск: udp_soak.exe [--host 192.168.50.175] [--op 0x63] [--rate 500] [--dur 300] [--log FILE]
//   --op: 0x02 DEVICE, 0x04 FIRMWARE, 0x55 физ. кнопки, 0x56 снимок, 0x63 позиция, 0x6A метаданные Raw
//   --rate 0: без паузы, следующий запрос сразу по ответу
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

int64_t qpcFreq() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}
const int64_t kFreq = qpcFreq();
int64_t now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
int64_t us(double v) { return static_cast<int64_t>(v * static_cast<double>(kFreq) / 1e6); }
double toMs(int64_t t) { return static_cast<double>(t) * 1000.0 / static_cast<double>(kFreq); }

std::string clockStr() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[16];
    std::snprintf(s, sizeof(s), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return s;
}

SOCKET g_s = INVALID_SOCKET;
sockaddr_in g_to{};
uint64_t g_seq = 0;
FILE* g_log = nullptr;

template <class... A>
void out(bool console, const char* fmt, A... a) {
    char line[512];
    std::snprintf(line, sizeof(line), fmt, a...);
    if (console) {
        std::fputs(line, stdout);
        std::fflush(stdout);
    }
    if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}

// Запрос и ответ с тем же nonce и опкодом. События 0x53 и чужие ответы пропускаются.
bool ask(uint8_t op, const uint8_t* pl, uint16_t len, double timeoutUs, double* rttMs) {
    uint8_t buf[2048];
    for (;;) {  // выбросить опоздавшие ответы
        if (recv(g_s, reinterpret_cast<char*>(buf), sizeof(buf), 0) <= 0) break;
    }
    const uint64_t seq = ++g_seq;
    uint8_t d[64];
    d[0] = 0x55;
    std::memcpy(d + 1, &seq, 8);
    d[9] = 0xDE;
    d[10] = 0xAD;
    d[11] = static_cast<uint8_t>(len);
    d[12] = static_cast<uint8_t>(len >> 8);
    d[13] = op;
    if (len) std::memcpy(d + 14, pl, len);
    const int64_t t0 = now();
    sendto(g_s, reinterpret_cast<const char*>(d), 14 + len, 0, reinterpret_cast<const sockaddr*>(&g_to), sizeof(g_to));
    const int64_t end = t0 + us(timeoutUs);
    for (;;) {
        const int n = recv(g_s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) {
            if (now() >= end) return false;
            YieldProcessor();
            continue;
        }
        if (n < 14 || buf[0] != 0x55 || buf[9] != 0xDE || buf[10] != 0xAD || buf[13] == 0x53) continue;
        uint64_t got = 0;
        std::memcpy(&got, buf + 1, 8);
        if (got != seq || buf[13] != op) continue;
        if (rttMs) *rttMs = toMs(now() - t0);
        return true;
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string host = "192.168.50.175", logPath;
    int op = 0x63;
    double rate = 500.0, dur = 300.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--host" && more) host = argv[++i];
        else if (a == "--op" && more) op = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        else if (a == "--rate" && more) rate = std::atof(argv[++i]);
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("udp_soak.exe [--host IP] [--op 0x63] [--rate 500] [--dur 300] [--log FILE]\n");
            return 2;
        }
    }
    uint8_t pl[8] = {};
    uint16_t len = 0;
    if (op == 0x6A) len = 5;  // id=0: метаданные, ничего не освобождает
    else if (op != 0x02 && op != 0x04 && op != 0x55 && op != 0x56 && op != 0x63) {
        std::printf("[-] опкод 0x%02X не из списка только для чтения\n", op);
        return 2;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(g_s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    DWORD off = FALSE, ret = 0;
    WSAIoctl(g_s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    u_long nb = 1;
    ioctlsocket(g_s, FIONBIO, &nb);
    g_to.sin_family = AF_INET;
    g_to.sin_port = htons(8080);
    if (inet_pton(AF_INET, host.c_str(), &g_to.sin_addr) != 1) {
        std::printf("[-] плохой адрес %s\n", host.c_str());
        return 2;
    }
    g_seq = static_cast<uint64_t>(now()) << 8;
    if (logPath.empty()) {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        char p[64];
        std::snprintf(p, sizeof(p), "udp_soak_%04u%02u%02u_%02u%02u%02u.log", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute,
            lt.wSecond);
        logPath = p;
    }
    g_log = std::fopen(logPath.c_str(), "a");
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    if (!ask(0x02, nullptr, 0, 150000.0, nullptr)) {
        out(true, "[-] %s:8080 не ответил на DEVICE (0x02)\n", host.c_str());
        return 1;
    }
    out(true, "[+] udp_soak %s, старт %s, опкод 0x%02X, темп %s, %.0f с, журнал %s\n", host.c_str(), clockStr().c_str(), op,
        rate > 0 ? (std::to_string(static_cast<int>(rate)) + "/с").c_str() : "без паузы", dur, logPath.c_str());
    out(true, "время    |    с | запросов | ответов | потеряно | RTT ср, мс | RTT max, мс\n");
    const int64_t t0 = now(), sec = us(1e6), period = rate > 0 ? us(1e6 / rate) : 0;
    int64_t due = t0, nextRep = t0 + sec, lastOk = t0, deadAt = 0;
    uint64_t req = 0, ok = 0, lost = 0, totReq = 0, totOk = 0;
    double rttSum = 0.0, rttMax = 0.0;
    int rep = 0, rc = 0;
    while (now() - t0 < us(dur * 1e6)) {
        while (now() < due) YieldProcessor();
        due = period ? std::max(due + period, now() - period) : now();
        double rtt = 0.0;
        ++req;
        if (ask(static_cast<uint8_t>(op), pl, len, 20000.0, &rtt)) {
            ++ok;
            rttSum += rtt;
            rttMax = std::max(rttMax, rtt);
            lastOk = now();
        } else {
            ++lost;
        }
        if (now() >= nextRep) {
            nextRep += sec;
            ++rep;
            out(rep % 10 == 0, "%s | %4.0f | %8llu | %7llu | %8llu | %10.2f | %11.2f\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
                static_cast<unsigned long long>(req), static_cast<unsigned long long>(ok), static_cast<unsigned long long>(lost),
                ok ? rttSum / static_cast<double>(ok) : 0.0, rttMax);
            totReq += req;
            totOk += ok;
            req = ok = lost = 0;
            rttSum = rttMax = 0.0;
        }
        if (now() - lastOk > us(2e6)) {
            deadAt = now();
            out(true, "[!] ОБРЫВ %s, на %.1f с: 2 с ни одного ответа. Последний ответ на %.1f с\n", clockStr().c_str(),
                toMs(deadAt - t0) / 1000.0, toMs(lastOk - t0) / 1000.0);
            break;
        }
    }
    if (deadAt) {
        rc = 3;
        bool back = false;
        while (now() - deadAt < us(30e6)) {
            Sleep(500);
            if (ask(0x02, nullptr, 0, 100000.0, nullptr)) {
                out(true, "[+] плата ответила на DEVICE через %.1f с после обрыва\n", toMs(now() - deadAt) / 1000.0);
                back = true;
                break;
            }
        }
        if (!back) out(true, "[-] за 30 с плата не ответила: нужна перезагрузка (переподключение) платы\n");
    }
    out(true, "итог %s: %.0f с, запросов %llu, ответов %llu%s\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
        static_cast<unsigned long long>(totReq + req), static_cast<unsigned long long>(totOk + ok), deadAt ? ", ОБРЫВ" : ", обрывов нет");
    timeEndPeriod(1);
    if (g_log) std::fclose(g_log);
    closesocket(g_s);
    WSACleanup();
    return rc;
}
