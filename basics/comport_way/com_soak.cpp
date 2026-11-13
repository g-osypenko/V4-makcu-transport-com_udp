// com_soak.cpp - сколько выдержит плата MAKCU по COM (USB2, CH343) под потоком одной команды только для чтения.
// Пара к udp_way/probes/udp_soak.cpp: курсор не двигается, окна нет. Поиск причины зависания
// (HANDOFF_UDP_TRACKING_HANG.md): по UDP плату вешает темп у предела (~530/с); здесь - есть ли предел у COM.
//
// Замкнутый цикл: запрос, ответ (или тайм-аут 20 мс), не раньше чем через 1/rate с - следующий.
// Строка раз в секунду в журнал (fflush), раз в 10 с в консоль. Ответов нет 2 с - обрыв: проба
// записывает, есть ли порт в системе, и до 60 с раз в 1 с переоткрывает порт и спрашивает DEVICE (0x02).
// Код выхода 3 - обрыв.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra com_soak.cpp -lwinmm -static -o com_soak.exe
// Запуск: com_soak.exe [--port COM5] [--op 0x63] [--rate 2000] [--dur 300] [--log FILE]
//   --op: 0x02 DEVICE, 0x04 FIRMWARE, 0x55 физ. кнопки, 0x56 снимок, 0x63 позиция, 0x6A метаданные Raw
//   --rate 0: без паузы, следующий запрос сразу по ответу
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

HANDLE g_h = INVALID_HANDLE_VALUE;
std::vector<uint8_t> g_rx;

bool portOpen(const std::string& name) {
    const std::string path = "\\\\.\\" + name;
    g_h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_h == INVALID_HANDLE_VALUE) return false;
    SetupComm(g_h, 4096, 4096);
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;  // чтение возвращается сразу
    t.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(g_h, &t);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(g_h, &dcb);
    dcb.BaudRate = 4000000;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;  // иначе можно перезапустить ESP32
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(g_h, &dcb)) {
        CloseHandle(g_h);
        g_h = INVALID_HANDLE_VALUE;
        return false;
    }
    PurgeComm(g_h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    g_rx.clear();
    return true;
}

void portClose() {
    if (g_h != INVALID_HANDLE_VALUE) CloseHandle(g_h);
    g_h = INVALID_HANDLE_VALUE;
}

// Есть ли порт в системе (HKLM\HARDWARE\DEVICEMAP\SERIALCOMM), независимо от того, открыт ли он.
bool portListed(const std::string& name) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool found = false;
    for (DWORD i = 0;; ++i) {
        char vn[256], data[64];
        DWORD vl = sizeof(vn), dl = sizeof(data), type = 0;
        if (RegEnumValueA(k, i, vn, &vl, nullptr, &type, reinterpret_cast<BYTE*>(data), &dl) != ERROR_SUCCESS) break;
        if (type == REG_SZ && name == data) found = true;
    }
    RegCloseKey(k);
    return found;
}

bool nextFrame(uint8_t& op) {
    size_t i = 0;
    for (; i + 5 <= g_rx.size(); ++i) {
        if (g_rx[i] != 0xDE || g_rx[i + 1] != 0xAD) continue;
        const size_t plen = static_cast<size_t>(g_rx[i + 2]) | static_cast<size_t>(g_rx[i + 3]) << 8;
        if (plen > 512) continue;
        if (i + 5 + plen > g_rx.size()) break;
        op = g_rx[i + 4];
        g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
        return true;
    }
    g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<std::ptrdiff_t>(i));
    return false;
}

// Запрос и ответ с тем же опкодом (по COM nonce нет, команда одна в полёте). События 0x53 пропускаются.
bool ask(uint8_t op, const uint8_t* pl, uint16_t len, double timeoutUs, double* rttMs) {
    if (g_h == INVALID_HANDLE_VALUE) return false;
    uint8_t d[64] = {0xDE, 0xAD, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), op};
    if (len) std::memcpy(d + 5, pl, len);
    const int64_t t0 = now();
    DWORD done = 0;
    if (!WriteFile(g_h, d, 5u + len, &done, nullptr) || done != 5u + len) return false;
    const int64_t end = t0 + us(timeoutUs);
    for (;;) {
        uint8_t got = 0;
        while (nextFrame(got)) {
            if (got != op) continue;
            if (rttMs) *rttMs = toMs(now() - t0);
            return true;
        }
        uint8_t buf[512];
        DWORD n = 0;
        if (ReadFile(g_h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            g_rx.insert(g_rx.end(), buf, buf + n);
            continue;
        }
        if (now() >= end) return false;
        YieldProcessor();
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string port = "COM5", logPath;
    int op = 0x63;
    double rate = 2000.0, dur = 300.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--op" && more) op = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        else if (a == "--rate" && more) rate = std::atof(argv[++i]);
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("com_soak.exe [--port COM5] [--op 0x63] [--rate 2000] [--dur 300] [--log FILE]\n");
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
    if (logPath.empty()) {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        char p[64];
        std::snprintf(p, sizeof(p), "com_soak_%04u%02u%02u_%02u%02u%02u.log", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute,
            lt.wSecond);
        logPath = p;
    }
    if (!portOpen(port)) {
        std::printf("[-] %s не открылся: ошибка %lu (порт занят Toolkit/SDK или USB2 не подключён)\n", port.c_str(), GetLastError());
        return 1;
    }
    g_log = std::fopen(logPath.c_str(), "a");
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    if (!ask(0x02, nullptr, 0, 150000.0, nullptr)) {
        out(true, "[-] %s: плата не ответила на DEVICE (0x02)\n", port.c_str());
        return 1;
    }
    out(true, "[+] com_soak %s, старт %s, опкод 0x%02X, темп %s, %.0f с, журнал %s\n", port.c_str(), clockStr().c_str(), op,
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
            out(rep % 10 == 0, "%s | %4.0f | %8llu | %7llu | %8llu | %10.3f | %11.3f\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
                static_cast<unsigned long long>(req), static_cast<unsigned long long>(ok), static_cast<unsigned long long>(lost),
                ok ? rttSum / static_cast<double>(ok) : 0.0, rttMax);
            totReq += req;
            totOk += ok;
            req = ok = lost = 0;
            rttSum = rttMax = 0.0;
        }
        if (now() - lastOk > us(2e6)) {
            deadAt = now();
            out(true, "[!] ОБРЫВ %s, на %.1f с: 2 с ни одного ответа. Последний ответ на %.1f с. %s в системе: %s\n", clockStr().c_str(),
                toMs(deadAt - t0) / 1000.0, toMs(lastOk - t0) / 1000.0, port.c_str(), portListed(port) ? "есть" : "НЕТ");
            break;
        }
    }
    if (deadAt) {
        rc = 3;
        bool back = false;
        bool listed = true;
        while (now() - deadAt < us(60e6)) {
            Sleep(1000);
            const bool l = portListed(port);
            if (l != listed) {
                out(true, "[i] %s, +%.0f с: %s %s\n", clockStr().c_str(), toMs(now() - deadAt) / 1000.0, port.c_str(),
                    l ? "снова в системе" : "пропал из системы");
                listed = l;
            }
            portClose();
            if (l && portOpen(port) && ask(0x02, nullptr, 0, 150000.0, nullptr)) {
                out(true, "[+] плата ответила на DEVICE через %.1f с после обрыва (порт переоткрыт)\n", toMs(now() - deadAt) / 1000.0);
                back = true;
                break;
            }
        }
        if (!back) out(true, "[-] за 60 с плата по %s не ответила\n", port.c_str());
    }
    out(true, "итог %s: %.0f с, запросов %llu, ответов %llu%s\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
        static_cast<unsigned long long>(totReq + req), static_cast<unsigned long long>(totOk + ok), deadAt ? ", ОБРЫВ" : ", обрывов нет");
    timeEndPeriod(1);
    if (g_log) std::fclose(g_log);
    portClose();
    return rc;
}
