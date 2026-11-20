// mouse_interp.cpp - интерполяция инжекта мыши MAKCU (MAK_API 0x1F) и короткая сводка платы по COM.
// 2026-11-20: владелец просит гонять мышь с выключенной интерполяцией.
//
// Что такое 0x1F: слайдер Toolkit "Mouse injection / Interpolate" (settings byte 396). GET - пустой payload, ответ 1 байт;
// SET - 1 байт 0..100 или 255 = AUTO. Действует "на эту сессию": в NOR не пишется, после переподключения платы - как в
// сохранённых настройках (заводское AUTO). Относится к командам движения 0x18 / km.move; Raw 0x69 по контракту
// (raw-v4074.md) идёт ровно одним отчётом HID и интерполятор не проходит, только ждёт старый долг AUTO.
//
// Печатает: прошивку (0x04), kinds (0x02, бит 0x01 = мышь), km.device(), интерполяцию до и после.
// Порт читается без ожидания (CLAUDE.md §3 п. 8). Курсор не двигает.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra mouse_interp.cpp -static -o mouse_interp.exe
// Запуск: mouse_interp.exe [--port COM5] [--set 0..100|auto]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

HANDLE g_h = INVALID_HANDLE_VALUE;
std::vector<uint8_t> g_rx;

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
// Запрос с payload (может быть пустым), ждём ответ с тем же опкодом.
bool ask(uint8_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& pl, DWORD timeoutMs) {
    std::vector<uint8_t> req = {0xDE, 0xAD, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8), op};
    req.insert(req.end(), payload.begin(), payload.end());
    if (!writeAll(req.data(), static_cast<DWORD>(req.size()))) return false;
    const DWORD end = GetTickCount() + timeoutMs;
    uint8_t got = 0;
    for (;;) {
        while (nextFrame(got, pl))
            if (got == op) return true;
        if (static_cast<int32_t>(GetTickCount() - end) >= 0) return false;
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
        if (c == '\r' || c == '\n') continue;
        if (c >= 32 && c < 127) s += static_cast<char>(c);
    }
    g_rx.clear();
    return s;
}
std::string interpName(int v) { return v < 0 ? "нет ответа" : v == 255 ? "AUTO (255)" : std::to_string(v); }
int getInterp() {
    std::vector<uint8_t> pl;
    return ask(0x1F, {}, pl, 200) && !pl.empty() ? pl[0] : -1;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string port = "COM5";
    int want = -1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) {
            const std::string v = argv[++i];
            want = v == "auto" ? 255 : std::atoi(v.c_str());
            if (want != 255 && (want < 0 || want > 100)) {
                std::printf("[-] --set: 0..100 или auto\n");
                return 2;
            }
        } else {
            std::printf("mouse_interp.exe [--port COM5] [--set 0..100|auto]\n");
            return 2;
        }
    }
    if (!openPort(port)) {
        std::printf("[-] %s не открылся (занят Toolkit / другой программой или нет платы), ошибка %lu\n", port.c_str(), GetLastError());
        return 1;
    }
    std::vector<uint8_t> pl;
    if (ask(0x04, {}, pl, 300) && pl.size() >= 2) std::printf("[+] прошивка (0x04): %u\n", static_cast<unsigned>(pl[0] | pl[1] << 8));
    else std::printf("[-] прошивка (0x04): нет ответа\n");
    if (ask(0x02, {}, pl, 300) && !pl.empty())
        std::printf("[%c] kinds (0x02): %02X - мышь %s\n", (pl[0] & 1) ? '+' : '!', pl[0], (pl[0] & 1) ? "есть" : "НЕТ (ресивер не в USB3 платы или плата его потеряла)");
    else std::printf("[-] kinds (0x02): нет ответа\n");
    std::printf("[i] km.device(): %s\n", textCmd("km.device()").c_str());
    const int was = getInterp();
    std::printf("[i] интерполяция (0x1F): %s\n", interpName(was).c_str());
    int rc = 0;
    if (want >= 0) {
        std::vector<uint8_t> ack;
        (void)ask(0x1F, {static_cast<uint8_t>(want)}, ack, 200);
        const int now = getInterp();
        if (now == want) std::printf("[+] интерполяция: %s -> %s (на эту сессию, в NOR не пишется)\n", interpName(was).c_str(), interpName(now).c_str());
        else {
            std::printf("[-] плата не приняла %s, сейчас %s\n", interpName(want).c_str(), interpName(now).c_str());
            rc = 3;
        }
    }
    CloseHandle(g_h);
    return rc;
}
