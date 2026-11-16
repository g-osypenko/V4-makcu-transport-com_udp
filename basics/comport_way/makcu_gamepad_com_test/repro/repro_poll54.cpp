// repro_poll54.cpp - minimal reproduction for MAKCU V4.126: polling GET 0x54 (CONTROLLER_PHYSICAL) over the COM port
// at ~2000 requests/s makes the controller disappear from MAKCU's USB3 port, and the port stays dead.
//
// Single file, Win32 only, no other code from our project.
//
// What it does:
//   1. Opens the COM port (default COM5) at 4 000 000 baud, 8N1, no flow control, DTR/RTS off. Reads never wait
//      (ReadIntervalTimeout = MAXDWORD, other read timeouts 0).
//   2. Prints 0x02 DEVICE, km.version(), km.device() once.
//   3. Sends DE AD 01 00 54 (GET 0x54) at --rate requests/s, one request in flight (the next request is sent only
//      after the reply to the previous one or after a 100 ms timeout), and parses the 32-byte reply
//      (<IIHHhhhhIIHH: buttons, triggers, sticks, sequence u32 @20, usb_timestamp u32 @24, timing u16 @28,
//      cadence u16 @30). A 1-byte payload FF (DE AD 01 00 54 FF) is the "no controller" refusal.
//   4. Once per second prints: requests, 32-byte replies, FF refusals, timeouts, how far `sequence` advanced
//      (= controller reports per second as MAKCU sees them), and whether Windows still sees an XInput controller
//      (XInputGetState, slots 0-3) and how many XInput packets/s it gets.
//   5. Failure = `sequence` has not advanced for 1 s, or only FF refusals for 0.5 s. Then polling stops and, for
//      30 s, it asks 0x02 DEVICE and km.device() every 5 s, so you can see the port does not come back.
//      Exit code 3 on failure, 0 if --dur passed cleanly, 1 if the port did not open.
//
// Build (MinGW-w64): g++ -std=c++17 -O2 -Wall -Wextra repro_poll54.cpp -lwinmm -static -o repro_poll54.exe
// Run:               repro_poll54.exe [--port COM5] [--rate 2000] [--dur 300] [--log FILE]
//
// Our results (V4.126, GameSir XInput controller, see README.md): --rate 2000 failed after 4-108 s in 3 runs out of 3
// (in pad_soak.exe, which polls the same way). With --rate 0 (no COM traffic at all) the controller ran for 8+ minutes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>  // timeBeginPeriod (excluded by WIN32_LEAN_AND_MEAN)

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int64_t g_freq = 0;
int64_t nowTicks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double secSince(int64_t t0) { return static_cast<double>(nowTicks() - t0) / static_cast<double>(g_freq); }
int64_t ticksUs(double us) { return static_cast<int64_t>(us * static_cast<double>(g_freq) / 1e6); }

FILE* g_log = nullptr;
void out(const char* fmt, ...) {
    char line[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    std::fputs(line, stdout);
    std::fflush(stdout);
    if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}
std::string wallClock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}

// ---- COM port -------------------------------------------------------------------------------------------------
HANDLE g_h = INVALID_HANDLE_VALUE;
std::vector<uint8_t> g_rx;

bool openPort(const std::string& name) {
    const std::string path = "\\\\.\\" + name;
    g_h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_h == INVALID_HANDLE_VALUE) return false;
    SetupComm(g_h, 16384, 4096);
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;  // ReadFile returns at once with whatever is buffered
    t.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(g_h, &t);
    DCB d{};
    d.DCBlength = sizeof(d);
    GetCommState(g_h, &d);
    d.BaudRate = 4000000;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDtrControl = DTR_CONTROL_DISABLE;
    d.fRtsControl = RTS_CONTROL_DISABLE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    d.fAbortOnError = FALSE;
    if (!SetCommState(g_h, &d)) return false;
    PurgeComm(g_h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}
void pump() {
    uint8_t b[2048];
    DWORD n = 0;
    while (ReadFile(g_h, b, sizeof(b), &n, nullptr) && n > 0) g_rx.insert(g_rx.end(), b, b + n);
}
bool writeAll(const void* p, DWORD n) {
    DWORD done = 0;
    return WriteFile(g_h, p, n, &done, nullptr) && done == n;
}
// Next complete binary frame DE AD | LEN u16 | CMD | PAYLOAD from g_rx (bytes before DE AD are dropped).
bool nextFrame(uint8_t& op, std::vector<uint8_t>& pl) {
    size_t i = 0;
    for (; i + 5 <= g_rx.size(); ++i) {
        if (g_rx[i] != 0xDE || g_rx[i + 1] != 0xAD) continue;
        const size_t len = g_rx[i + 2] | (static_cast<size_t>(g_rx[i + 3]) << 8);
        if (len > 1024) continue;
        if (i + 5 + len > g_rx.size()) break;
        op = g_rx[i + 4];
        pl.assign(g_rx.begin() + static_cast<std::ptrdiff_t>(i + 5), g_rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + len));
        g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + len));
        return true;
    }
    g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<std::ptrdiff_t>(i));
    return false;
}
// Binary request without payload; returns true and the payload of the first reply with the same opcode.
bool ask(uint8_t op, std::vector<uint8_t>& pl, double timeoutMs) {
    const uint8_t req[5] = {0xDE, 0xAD, 0x00, 0x00, op};
    if (!writeAll(req, 5)) return false;
    const int64_t end = nowTicks() + ticksUs(timeoutMs * 1000.0);
    uint8_t got = 0;
    for (;;) {
        while (nextFrame(got, pl))
            if (got == op) return true;  // other frames (e.g. late replies) are skipped
        if (nowTicks() >= end) return false;
        pump();
        YieldProcessor();
    }
}
std::string textCmd(const char* cmd) {
    pump();
    g_rx.clear();
    std::string line = std::string(cmd) + "\r\n";
    writeAll(line.data(), static_cast<DWORD>(line.size()));
    Sleep(150);
    pump();
    std::string s;
    for (uint8_t c : g_rx) {
        if (c == '\r') s += "\\r";
        else if (c == '\n') s += "\\n";
        else if (c >= 32 && c < 127) s += static_cast<char>(c);
        else {
            char b[8];
            std::snprintf(b, sizeof(b), "\\x%02X", c);
            s += b;
        }
    }
    g_rx.clear();
    return s;
}
std::string device() {
    std::vector<uint8_t> pl;
    if (!ask(0x02, pl, 200)) return "no reply";
    char b[64];
    std::snprintf(b, sizeof(b), "0x%02X%s", pl.empty() ? 0 : pl[0],
                  pl.empty() ? "" : (pl[0] & 0x80) ? " (Xbox 360/XInput present)" : (pl[0] & 0xFC) ? " (controller present)" : " (no controller)");
    return b;
}

// ---- Windows side: XInput -----------------------------------------------------------------------------------
#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);
std::atomic<bool> g_stop{false}, g_xiOk{false};
std::atomic<uint64_t> g_xiPackets{0};
void xinputLoop() {
    HMODULE dll = LoadLibraryA("xinput1_4.dll");
    if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
    if (!dll) return;
    auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    DWORD last = 0;
    while (!g_stop) {
        bool ok = false;
        for (DWORD i = 0; i < 4; ++i) {
            XState s{};
            if (get(i, &s) == ERROR_SUCCESS) {
                ok = true;
                if (s.pkt != last) {
                    last = s.pkt;
                    ++g_xiPackets;
                }
                break;
            }
        }
        g_xiOk = ok;
        Sleep(0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    std::string port = "COM5", logPath;
    double rate = 2000.0, dur = 300.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--rate" && more) rate = std::atof(argv[++i]);
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("repro_poll54.exe [--port COM5] [--rate 2000] [--dur 300] [--log FILE]\n");
            return 2;
        }
    }
    if (!logPath.empty()) g_log = std::fopen(logPath.c_str(), "a");
    timeBeginPeriod(1);
    std::thread xi(xinputLoop);
    if (!openPort(port)) {
        out("[-] %s did not open: error %lu\n", port.c_str(), GetLastError());
        g_stop = true;
        xi.join();
        return 1;
    }
    Sleep(300);
    out("[i] %s %s @ 4000000, GET 0x54 at %.0f/s for %.0f s, one request in flight\n", wallClock().c_str(), port.c_str(), rate, dur);
    out("[i] 0x02 DEVICE: %s\n", device().c_str());
    out("[i] km.version(): %s\n", textCmd("km.version()").c_str());
    out("[i] km.device(): %s\n", textCmd("km.device()").c_str());
    out("[i] Windows XInput controller: %s\n", g_xiOk ? "present" : "NOT present");
    out("time         |   s | requests | 32-byte | FF | timeout | seq advance/s | Windows XInput, packets/s\n");

    const int64_t t0 = nowTicks();
    const int64_t period = rate > 0 ? ticksUs(1e6 / rate) : 0;
    int64_t next = t0, nextRep = t0 + ticksUs(1e6), lastAdvance = t0, ffSince = 0;
    uint64_t req = 0, ok = 0, ff = 0, tmo = 0, adv = 0, pReq = 0, pOk = 0, pFf = 0, pTmo = 0, pAdv = 0, pPk = 0;
    uint32_t lastSeq = 0;
    bool haveSeq = false, failed = false;
    std::vector<uint8_t> pl;
    while (secSince(t0) < dur) {
        const int64_t t = nowTicks();
        if (rate > 0 && t >= next) {
            next += period;
            if (next < t) next = t;  // late: no burst to catch up
            ++req;
            if (!ask(0x54, pl, 100.0)) ++tmo;
            else if (pl.size() == 32) {
                ++ok;
                ffSince = 0;
                const uint32_t seq = static_cast<uint32_t>(pl[20]) | static_cast<uint32_t>(pl[21]) << 8 |
                                     static_cast<uint32_t>(pl[22]) << 16 | static_cast<uint32_t>(pl[23]) << 24;
                if (!haveSeq) {
                    haveSeq = true;
                    lastSeq = seq;
                    lastAdvance = nowTicks();
                } else if (seq != lastSeq) {
                    adv += static_cast<uint32_t>(seq - lastSeq);
                    lastSeq = seq;
                    lastAdvance = nowTicks();
                }
            } else if (pl.size() == 1 && pl[0] == 0xFF) {
                ++ff;
                if (!ffSince) ffSince = nowTicks();
            }
            const bool seqStuck = haveSeq && nowTicks() - lastAdvance > ticksUs(1e6);
            const bool ffStuck = ffSince && nowTicks() - ffSince > ticksUs(5e5);
            if (seqStuck || ffStuck) {
                out("[!] %s FAILURE after %.1f s of polling: %s (requests %llu)\n", wallClock().c_str(), secSince(t0),
                    ffStuck ? "only FF refusals to 0x54 for 0.5 s" : "0x54 sequence has not advanced for 1 s",
                    static_cast<unsigned long long>(req));
                failed = true;
                break;
            }
        }
        if (nowTicks() >= nextRep) {
            nextRep += ticksUs(1e6);
            const uint64_t pk = g_xiPackets.load();
            out("%s | %3.0f | %8llu | %7llu | %2llu | %7llu | %13llu | %s, %llu\n", wallClock().c_str(), secSince(t0),
                static_cast<unsigned long long>(req - pReq), static_cast<unsigned long long>(ok - pOk),
                static_cast<unsigned long long>(ff - pFf), static_cast<unsigned long long>(tmo - pTmo),
                static_cast<unsigned long long>(adv - pAdv), g_xiOk ? "present" : "NOT present", static_cast<unsigned long long>(pk - pPk));
            pReq = req, pOk = ok, pFf = ff, pTmo = tmo, pAdv = adv, pPk = pk;
        }
        if (rate <= 0) Sleep(1);
    }
    if (failed) {
        out("[i] polling stopped; checking for 30 s whether the controller comes back by itself\n");
        for (int i = 0; i <= 6; ++i) {
            out("[i] %s +%2d s: 0x02 DEVICE %s | km.device() %s | Windows XInput %s\n", wallClock().c_str(), i * 5, device().c_str(),
                textCmd("km.device()").c_str(), g_xiOk ? "present" : "NOT present");
            if (i < 6) Sleep(5000);
        }
        out("[!] RESULT: controller lost after %llu requests. Replug the controller alone to see whether it re-enumerates;\n"
            "    in our runs only replugging all MAKCU cables brought it back.\n",
            static_cast<unsigned long long>(req));
    } else {
        out("[+] RESULT: %.0f s, %llu requests, no failure\n", dur, static_cast<unsigned long long>(req));
    }
    g_stop = true;
    xi.join();
    CloseHandle(g_h);
    timeEndPeriod(1);
    return failed ? 3 : 0;
}
