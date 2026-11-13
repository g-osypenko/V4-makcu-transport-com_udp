// com_ref_soak.cpp - долгая нагрузка драйвера CH343 через сам эталон COM, без окна и без движения курсора.
// Зачем: синий экран 0xD1 в CH343S64.SYS (HANDOFF_UDP_TRACKING_HANG.md) воспроизводился стендом с эталоном, а голый
// синхронный опрос com_soak.exe его не давал. Отличие - ввод-вывод эталона: overlapped-хэндл, кольцо до 32 WriteFile
// в полёте, поток чтения с висящим ReadFile. Эта проба гоняет именно его, но без инжекта и кликов: можно оставить
// без человека.
//
// Нагрузка: setTracking(true) (опрос 0x63 ~2 кГц) + setSensitivity(--sens, по умолчанию 50: маска 0x16 переключается
// окнами от 2 мс, ~1000 команд/с, после каждой снимок 0x56). ВНИМАНИЕ: пока идёт проба, физическая мышь идёт с этой
// долей движения (50 % - вдвое медленнее). Ctrl+C - досрочно, close() вернёт 100 % и km.screen.
// Строка раз в секунду в журнал (fflush), раз в 60 с в консоль. Код выхода 3 - обрыв связи.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra -I perfect-transport-comport com_ref_soak.cpp -lsetupapi -lwinmm -static -o com_ref_soak.exe
// Запуск: com_ref_soak.exe [--port COM5] [--dur 3600] [--sens 50] [--log FILE]
#include "makcu_raw_transport.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::atomic<bool> g_stop{false};
BOOL WINAPI onCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

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
    char line[1024];
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

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    std::string port, logPath;
    double dur = 3600.0;
    int sens = 50;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--sens" && more) sens = std::atoi(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("com_ref_soak.exe [--port COM5] [--dur 3600] [--sens 50] [--log FILE]\n");
            return 2;
        }
    }
    if (logPath.empty()) logPath = "com_ref_soak.log";
    g_log = std::fopen(logPath.c_str(), "a");
    makcu::RawTransport tx;
    std::string err;
    if (!tx.open(port, err)) {
        out(true, "[-] %s\n", err.c_str());
        return 1;
    }
    out(true, "[+] эталон: %s\n", tx.info().c_str());
    if (!tx.setTracking(true)) out(true, "[-] setTracking(true) не прошёл\n");
    tx.setSensitivity(sens);
    out(true, "[+] com_ref_soak, старт %s, %.0f с, слежение вкл, чувствительность %d %%, журнал %s\n", clockStr().c_str(), dur, sens,
        logPath.c_str());
    out(true, "время    |    с | WriteFile/с | в полёте max | 0x63 отв./с | маска/с | снимков/с | ошибок | связь | обрывов\n");
    using Q = makcu::detail::Qpc;
    const auto s0 = tx.stats();
    auto prev = s0;
    const int64_t t0 = Q::now();
    int rep = 0, rc = 0;
    while (!g_stop.load() && Q::now() - t0 < Q::fromUs(dur * 1e6)) {
        Sleep(1000);
        ++rep;
        const auto s = tx.stats();
        out(rep % 60 == 0 || s.linkLost != prev.linkLost, "%s | %4d | %11llu | %12u | %11llu | %7llu | %9llu | %6llu | %5s | %llu\n",
            clockStr().c_str(), rep, static_cast<unsigned long long>(s.writes - prev.writes), s.maxInflight,
            static_cast<unsigned long long>(s.posReplies - prev.posReplies), static_cast<unsigned long long>(s.sensToggles - prev.sensToggles),
            static_cast<unsigned long long>(s.snapshots - prev.snapshots), static_cast<unsigned long long>(s.errors - s0.errors),
            s.connected ? "да" : "НЕТ", static_cast<unsigned long long>(s.linkLost - s0.linkLost));
        if (s.linkLost != s0.linkLost) rc = 3;
        prev = s;
    }
    const auto s = tx.stats();
    out(true, "%s\n", tx.statsLine().c_str());
    out(true, "итог %s: %d с, WriteFile %llu, опросов с ответом %llu, переключений маски %llu, обрывов %llu\n", clockStr().c_str(), rep,
        static_cast<unsigned long long>(s.writes - s0.writes), static_cast<unsigned long long>(s.posReplies - s0.posReplies),
        static_cast<unsigned long long>(s.sensToggles - s0.sensToggles), static_cast<unsigned long long>(s.linkLost - s0.linkLost));
    tx.setSensitivity(100);
    tx.setTracking(false);
    tx.close();
    if (g_log) std::fclose(g_log);
    return rc;
}
