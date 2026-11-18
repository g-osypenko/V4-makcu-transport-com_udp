// mouse_drop_watch.cpp - наблюдение за мышью за платой MAKCU: пропадает ли она из Windows, заикается ли, видит ли её плата.
// 2026-11-18, по жалобе владельца: на 4134 (и, возможно, 4133) мышь Logitech G502 X LIGHTSPEED через плату заикается и
// постоянно отключается/подключается; ресивер прямо в ПК - всё чисто.
//
// Курсор НЕ двигает, команд движения и блокировок не шлёт. Что делает:
//   * Raw Input (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY) по всем мышам. Мышь "наша" - имя содержит --match (VID_046D&PID_C547).
//     Раз в секунду: отчётов, путь |dx|+|dy|, наибольшая пауза между отчётами посреди движения.
//     Заикание: пауза >= --gap мс (30) между двумя отчётами с движением, перед которой за 50 мс было >= 20 отчётов с движением
//     (рука шла плотно), и пауза < 1 с. Считается только когда окно в фокусе: без фокуса Windows прореживает Raw Input.
//   * Присутствие мыши в Windows: GetRawInputDeviceList раз в 100 мс + WM_INPUT_DEVICE_CHANGE. Переход - строка в журнал
//     и два сигнала (нужно действие владельца: при Toolkit на COM7 - Create report).
//   * Плата (если --port не none): раз в 0.5 с GET 0x02 DEVICE (kinds, бит 0x01 = мышь). Переход - строка, km.device().
//     Порт читается без ожидания (правило CLAUDE.md §3 п. 8).
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra mouse_drop_watch.cpp -lwinmm -lgdi32 -static -o mouse_drop_watch.exe
// Запуск: mouse_drop_watch.exe [--port COM5|COM7|none] [--dur 600] [--gap 30] [--match VID_046D&PID_C547]
//                              [--mode circles|use] [--log FILE]
//   --port COM7 - через виртуальный порт MAKCU Toolkit (Toolkit открыт и держит COM5; так Toolkit видит отказ).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
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
double msBetween(int64_t a, int64_t b) { return static_cast<double>(b - a) * 1000.0 / static_cast<double>(g_freq); }
int64_t ticksMs(double ms) { return static_cast<int64_t>(ms * static_cast<double>(g_freq) / 1000.0); }

std::mutex g_logMu;
FILE* g_log = nullptr;
std::string wallClock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}
void out(const char* fmt, ...) {
    char line[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> lk(g_logMu);
    std::fputs(line, stdout);
    std::fflush(stdout);
    if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}
void beep2Async() {
    std::thread([] {
        Beep(1000, 180);
        Sleep(120);
        Beep(1000, 180);
    }).detach();
}

// ---- параметры ----------------------------------------------------------------------------------------------
std::string g_port = "none", g_match = "VID_046D&PID_C547", g_mode = "use";
double g_dur = 600, g_gapMs = 30;

// ---- COM: GET 0x02 раз в 0.5 с -------------------------------------------------------------------------------
HANDLE g_h = INVALID_HANDLE_VALUE;
std::vector<uint8_t> g_rx;
std::atomic<int> g_kinds{-2};  // -2 COM не опрашивается, -1 нет ответа, иначе байт kinds
std::atomic<int> g_boardLost{0};
std::atomic<bool> g_stop{false};

bool openPort(const std::string& name) {
    const std::string path = "\\\\.\\" + name;
    g_h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_h == INVALID_HANDLE_VALUE) return false;
    SetupComm(g_h, 16384, 4096);
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;  // чтение без ожидания
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
bool ask(uint8_t op, std::vector<uint8_t>& pl, double timeoutMs) {
    const uint8_t req[5] = {0xDE, 0xAD, 0x00, 0x00, op};
    if (!writeAll(req, 5)) return false;
    const int64_t end = nowTicks() + ticksMs(timeoutMs);
    uint8_t got = 0;
    for (;;) {
        while (nextFrame(got, pl))
            if (got == op) return true;
        if (nowTicks() >= end) return false;
        pump();
        Sleep(1);
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
void comLoop() {
    std::vector<uint8_t> pl;
    if (ask(0x04, pl, 200) && pl.size() >= 2)
        out("[i] %s board 0x04 firmware %u\n", wallClock().c_str(), static_cast<unsigned>(pl[0] | pl[1] << 8));
    out("[i] %s km.device() %s\n", wallClock().c_str(), textCmd("km.device()").c_str());
    int prev = -3;
    while (!g_stop) {
        int k = -1;
        if (ask(0x02, pl, 200) && !pl.empty()) k = pl[0];
        g_kinds = k;
        if (k != prev) {
            const bool hadMouse = prev >= 0 && (prev & 0x01);
            const bool hasMouse = k >= 0 && (k & 0x01);
            out("[%c] %s board 0x02 kinds %s%02X (%s)\n", hasMouse ? '+' : '!', wallClock().c_str(), k < 0 ? "no reply " : "",
                k < 0 ? 0 : k, hasMouse ? "mouse present" : "NO mouse");
            if (prev != -3) out("[i] %s km.device() %s\n", wallClock().c_str(), textCmd("km.device()").c_str());
            if (hadMouse && !hasMouse) {
                ++g_boardLost;
                beep2Async();
            }
            prev = k;
        }
        for (int i = 0; i < 50 && !g_stop; ++i) Sleep(10);
    }
}

// ---- Raw Input ----------------------------------------------------------------------------------------------
std::map<HANDLE, std::string> g_names;
std::string devName(HANDLE h) {
    auto it = g_names.find(h);
    if (it != g_names.end()) return it->second;
    UINT n = 0;
    GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, nullptr, &n);
    std::string s(n, '\0');
    if (n && GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, &s[0], &n) > 0) s.resize(std::strlen(s.c_str()));
    else s = "?";
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    g_names[h] = s;
    return s;
}
bool isOurs(const std::string& name) {
    std::string m = g_match;
    std::transform(m.begin(), m.end(), m.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return name.find(m) != std::string::npos;
}
bool ourMousePresent() {
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> v(n + 8);
    n = static_cast<UINT>(v.size());
    const UINT got = GetRawInputDeviceList(v.data(), &n, sizeof(RAWINPUTDEVICELIST));
    if (got == static_cast<UINT>(-1)) return true;  // не смогли прочитать - не объявляем пропажу
    for (UINT i = 0; i < got; ++i)
        if (v[i].dwType == RIM_TYPEMOUSE && isOurs(devName(v[i].hDevice))) return true;
    return false;
}

// Счётчики (поток окна)
uint64_t g_rep = 0, g_path = 0, g_otherRep = 0;
double g_maxGapSec = 0;
int g_stutters = 0, g_winLost = 0;
double g_lastStutterMs = 0;
std::string g_lastStutterAt, g_lastLostAt, g_lastBoardLostAt;
int64_t g_lastT = 0;
bool g_lastMoved = false;
int64_t g_ring[32];
int g_ringN = 0, g_ringPos = 0;
bool g_present = true, g_fg = false;
int64_t g_t0 = 0;

void onMouse(HANDLE h, LONG dx, LONG dy) {
    const int64_t t = nowTicks();
    if (!isOurs(devName(h))) {
        ++g_otherRep;
        return;
    }
    ++g_rep;
    const bool moved = dx != 0 || dy != 0;
    g_path += static_cast<uint64_t>(std::labs(dx) + std::labs(dy));
    if (g_lastT && moved && g_lastMoved) {
        const double gap = msBetween(g_lastT, t);
        if (g_fg) g_maxGapSec = std::max(g_maxGapSec, gap);
        bool dense = false;
        if (g_ringN >= 20) {
            const int64_t t20 = g_ring[(g_ringPos - 20 + 32) % 32];
            dense = msBetween(t20, g_lastT) <= 50.0;
        }
        if (g_fg && dense && gap >= g_gapMs && gap < 1000.0) {
            ++g_stutters;
            g_lastStutterMs = gap;
            g_lastStutterAt = wallClock();
            out("[!] %s stutter: %.1f ms without reports in the middle of movement (total %d)\n", g_lastStutterAt.c_str(), gap, g_stutters);
        }
    }
    if (moved) {
        g_ring[g_ringPos] = t;
        g_ringPos = (g_ringPos + 1) % 32;
        g_ringN = std::min(g_ringN + 1, 32);
    }
    g_lastT = t;
    g_lastMoved = moved;
}

std::wstring g_lines[7];
HFONT g_big = nullptr, g_small = nullptr;

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void updateText() {
    const double el = msBetween(g_t0, nowTicks()) / 1000.0;
    const int left = std::max(0, static_cast<int>(g_dur - el));
    wchar_t b[512];
    if (g_mode == "circles")
        g_lines[0] = L"Кликни в это окно и води мышью кругами без остановки. Провода не трогай.";
    else
        g_lines[0] = L"Пользуйся мышью как обычно (можно игру). Провода не трогай. Заикается или пропала - продолжай.";
    swprintf(b, 512, L"Мышь %hs в Windows: %ls | отчётов за с: %llu | путь за с: %llu", g_match.c_str(),
             g_present ? L"ЕСТЬ" : L"НЕТ", static_cast<unsigned long long>(g_rep), static_cast<unsigned long long>(g_path));
    g_lines[1] = b;
    swprintf(b, 512, L"Заиканий (пауза от %.0f мс посреди движения): %d%ls%ls", g_gapMs, g_stutters,
             g_stutters ? L", последнее в " : L"", g_stutters ? widen(g_lastStutterAt).c_str() : L"");
    g_lines[2] = b;
    swprintf(b, 512, L"Пропаданий из Windows: %d%ls%ls", g_winLost, g_winLost ? L", последнее в " : L"",
             g_winLost ? widen(g_lastLostAt).c_str() : L"");
    g_lines[3] = b;
    const int k = g_kinds.load();
    if (k == -2) swprintf(b, 512, L"Плата: COM не опрашивается");
    else if (k == -1) swprintf(b, 512, L"Плата (%hs): нет ответа | мышь пропадала на плате: %d", g_port.c_str(), g_boardLost.load());
    else
        swprintf(b, 512, L"Плата (%hs): kinds %02X, мышь %ls | мышь пропадала на плате: %d", g_port.c_str(), k,
                 (k & 1) ? L"есть" : L"НЕТ", g_boardLost.load());
    g_lines[4] = b;
    swprintf(b, 512, L"Окно в фокусе: %ls (без фокуса Windows прореживает Raw Input, заикания не считаются)", g_fg ? L"да" : L"НЕТ");
    g_lines[5] = b;
    swprintf(b, 512, L"До конца %d:%02d. Окно закроется само.", left / 60, left % 60);
    g_lines[6] = b;
}

LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_INPUT: {
            RAWINPUT ri;
            UINT sz = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE)
                onMouse(ri.header.hDevice, ri.data.mouse.lLastX, ri.data.mouse.lLastY);
            return DefWindowProcW(w, m, wp, lp);
        }
        case WM_INPUT_DEVICE_CHANGE: {
            const HANDLE h = reinterpret_cast<HANDLE>(lp);
            const std::string n = devName(h);
            out("[i] %s WM_INPUT_DEVICE_CHANGE %s %s\n", wallClock().c_str(), wp == GIDC_ARRIVAL ? "ARRIVAL" : "REMOVAL", n.c_str());
            return 0;
        }
        case WM_TIMER: {
            if (wp == 1) {  // 100 мс: присутствие
                const bool p = ourMousePresent();
                if (p != g_present) {
                    g_present = p;
                    if (!p) {
                        ++g_winLost;
                        g_lastLostAt = wallClock();
                        beep2Async();
                    }
                    out("[%c] %s Windows: mouse %s %s\n", p ? '+' : '!', wallClock().c_str(), g_match.c_str(), p ? "APPEARED" : "GONE");
                }
                g_fg = GetForegroundWindow() == w;
            } else if (wp == 2) {  // 1 с: строка журнала
                const double el = msBetween(g_t0, nowTicks()) / 1000.0;
                const int k = g_kinds.load();
                out("%s | %4.0f | fg %d | win %d | rep %4llu | path %6llu | maxgap %6.1f | stutters %3d | winlost %d | kinds %s%02X | boardlost %d | other %llu\n",
                    wallClock().c_str(), el, g_fg ? 1 : 0, g_present ? 1 : 0, static_cast<unsigned long long>(g_rep),
                    static_cast<unsigned long long>(g_path), g_maxGapSec, g_stutters, g_winLost, k < 0 ? (k == -2 ? "-" : "?") : "",
                    k < 0 ? 0 : k, g_boardLost.load(), static_cast<unsigned long long>(g_otherRep));
                updateText();
                InvalidateRect(w, nullptr, TRUE);
                g_rep = g_path = g_otherRep = 0;
                g_maxGapSec = 0;
                if (el >= g_dur) DestroyWindow(w);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            RECT rc;
            GetClientRect(w, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            int y = 14;
            for (int i = 0; i < 7; ++i) {
                SelectObject(dc, i == 0 ? g_big : g_small);
                COLORREF c = RGB(220, 220, 220);
                if (i == 0) c = RGB(255, 230, 80);
                if ((i == 1 && !g_present) || (i == 2 && g_stutters) || (i == 3 && g_winLost) || (i == 4 && g_boardLost.load()))
                    c = RGB(255, 90, 90);
                SetTextColor(dc, c);
                TextOutW(dc, 16, y, g_lines[i].c_str(), static_cast<int>(g_lines[i].size()));
                y += i == 0 ? 44 : 30;
            }
            EndPaint(w, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    std::string logPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) g_port = argv[++i];
        else if (a == "--dur" && more) g_dur = std::atof(argv[++i]);
        else if (a == "--gap" && more) g_gapMs = std::atof(argv[++i]);
        else if (a == "--match" && more) g_match = argv[++i];
        else if (a == "--mode" && more) g_mode = argv[++i];
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("mouse_drop_watch.exe [--port COM5|COM7|none] [--dur 600] [--gap 30] [--match VID_046D&PID_C547] [--mode circles|use] [--log FILE]\n");
            return 2;
        }
    }
    if (!logPath.empty()) g_log = std::fopen(logPath.c_str(), "a");
    timeBeginPeriod(1);
    out("[i] %s mouse_drop_watch: port %s, dur %.0f s, gap %.0f ms, match %s, mode %s\n", wallClock().c_str(), g_port.c_str(), g_dur,
        g_gapMs, g_match.c_str(), g_mode.c_str());
    std::thread com;
    if (g_port != "none") {
        if (!openPort(g_port)) {
            out("[-] %s did not open: error %lu (Toolkit holds COM5? use --port COM7 with Toolkit open)\n", g_port.c_str(), GetLastError());
            return 1;
        }
        com = std::thread(comLoop);
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MouseDropWatch";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_big = CreateFontW(30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_small = CreateFontW(22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"MAKCU: мышь - пропадания и заикания", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60,
                             1250, 300, nullptr, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget = w;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) out("[-] RegisterRawInputDevices failed: %lu\n", GetLastError());
    g_t0 = nowTicks();
    g_present = ourMousePresent();
    out("[i] %s Windows: mouse %s %s at start\n", wallClock().c_str(), g_match.c_str(), g_present ? "present" : "NOT present");
    out("time         |    s | fg | win | rep/s | path/s | maxgap ms | stutters | winlost | kinds | boardlost | other mice rep/s\n");
    updateText();
    SetTimer(w, 1, 100, nullptr);
    SetTimer(w, 2, 1000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_stop = true;
    if (com.joinable()) com.join();
    out("[=] %s RESULT: stutters %d, gone from Windows %d, gone on board %d\n", wallClock().c_str(), g_stutters, g_winLost, g_boardLost.load());
    if (g_h != INVALID_HANDLE_VALUE) CloseHandle(g_h);
    timeEndPeriod(1);
    return (g_winLost || g_boardLost.load()) ? 3 : 0;
}
