// pad_probe_hold.cpp - проба 2 (фаза 1): держится ли геймпад на плате и зависит ли это от темпа опроса 0x54.
// ТОЛЬКО ЧТЕНИЕ. Окна нет, геймпадом не управляет.
//
// Windows: раз в 20 мс XInputGetState по слотам 0..3 - подключён ли геймпад и растёт ли номер пакета.
// Плата (если --rate не 0): GET 0x54 с заданным темпом, счёт снимков, отказов FF и молчания.
// При --rate 0 порт COM не открывается вообще: это контроль "геймпад живёт без единой команды плате".
// Строка раз в 5 с и строка на каждое событие (геймпад появился/пропал в Windows, плата начала/перестала
// отдавать снимки). Код выхода: 0 - геймпад был на месте весь прогон, 3 - пропадал, 1 - порт не открылся.
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_hold.cpp -lwinmm -static -o pad_probe_hold.exe
// Запуск: pad_probe_hold.exe [--port COM5] [--rate 0|N|max] [--dur 180] [--log FILE]
#include "pad_probe_common.h"

#include <atomic>
#include <thread>

using namespace padprobe;

namespace {

#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);

FILE* g_log = nullptr;
template <class... A>
void out(const char* fmt, A... a) {
    char line[512];
    std::snprintf(line, sizeof(line), fmt, a...);
    std::fputs(line, stdout);
    if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}
std::string clockStr() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[20];
    std::snprintf(s, sizeof(s), "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return s;
}

std::atomic<bool> g_stop{false};
std::atomic<int> g_xiSlot{-1};
std::atomic<uint64_t> g_xiPackets{0};
std::atomic<uint64_t> g_polls{0}, g_ok{0}, g_ff{0}, g_lost{0}, g_seqChanges{0};
std::atomic<int> g_boardState{0};  // 0 неизвестно, 1 снимки, 2 отказ FF, 3 молчит
std::atomic<double> g_rttMax{0.0};

void xinputLoop() {
    HMODULE dll = LoadLibraryA("xinput1_4.dll");
    if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
    if (!dll) return;
    const auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    uint32_t lastPkt = 0;
    while (!g_stop) {
        int slot = -1;
        XState s{};
        for (DWORD i = 0; i < 4; ++i) {
            if (get(i, &s) == ERROR_SUCCESS) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot >= 0 && s.pkt != lastPkt) {
            g_xiPackets.fetch_add(s.pkt - lastPkt, std::memory_order_relaxed);
            lastPkt = s.pkt;
        }
        g_xiSlot = slot;
        Sleep(20);
    }
}

void boardLoop(Port* p, double rate) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const int64_t period = rate > 0 ? us(1e6 / rate) : 0;
    int64_t due = now();
    bool have = false;
    uint32_t lastSeq = 0;
    while (!g_stop) {
        if (period) {
            while (now() < due) {
                if (due - now() > us(1500.0)) Sleep(1);
                else YieldProcessor();
            }
            due = std::max(due + period, now() - period);
        }
        double ms = 0.0;
        g_polls.fetch_add(1, std::memory_order_relaxed);
        auto r = p->ask(kOpCtlPhysical, nullptr, 0, 20000.0, &ms);
        if (!r) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            g_boardState = 3;
            continue;
        }
        if (ms > g_rttMax.load(std::memory_order_relaxed)) g_rttMax = ms;
        if (r->size() != 32) {
            g_ff.fetch_add(1, std::memory_order_relaxed);
            g_boardState = 2;
            if (!period) Sleep(1);
            continue;
        }
        g_ok.fetch_add(1, std::memory_order_relaxed);
        g_boardState = 1;
        const PadPhysical v = parsePhysical(r->data());
        if (!have || v.seq != lastSeq) {
            have = true;
            lastSeq = v.seq;
            g_seqChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();
    std::string port = "COM5", logPath, rateArg = "0";
    double dur = 180.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--rate" && more) rateArg = argv[++i];
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("pad_probe_hold.exe [--port COM5] [--rate 0|N|max] [--dur 180] [--log FILE]\n");
            return 2;
        }
    }
    const bool useBoard = rateArg != "0";
    const double rate = rateArg == "max" ? 0.0 : std::atof(rateArg.c_str());
    if (!logPath.empty()) g_log = std::fopen(logPath.c_str(), "a");
    Port p;
    if (useBoard) {
        if (!p.open(port)) {
            out("[-] %s не открылся: ошибка %lu\n", port.c_str(), GetLastError());
            return 1;
        }
        uint8_t kinds = 0;
        if (auto r = p.ask(kOpDevice, nullptr, 0, 300000.0)) kinds = r->empty() ? 0 : (*r)[0];
        out("[i] %s плата: kinds %02X = %s\n", clockStr().c_str(), kinds, kindsText(kinds).c_str());
    }
    timeBeginPeriod(1);
    out("[i] %s старт: %s, %.0f с\n", clockStr().c_str(),
        !useBoard ? "COM закрыт, плате не уходит ни одной команды" : rateArg == "max" ? "опрос 0x54 без паузы" : ("опрос 0x54 " + rateArg + "/с").c_str(), dur);
    out("время        |   с | Windows XInput | пакетов/с | опросов | снимков | новых seq/с | FF | молчит | RTT max, мс\n");
    std::thread xi(xinputLoop);
    std::thread board;
    if (useBoard) board = std::thread(boardLoop, &p, rate);
    Sleep(300);
    const int64_t t0 = now();
    int64_t nextRep = t0 + us(5e6);
    int lastSlot = -2, lastBoard = -1, drops = 0;
    bool everXi = false, everBoard = false;
    uint64_t pPk = 0, pPolls = 0, pOk = 0, pSeq = 0, pFf = 0, pLost = 0;
    while (toMs(now() - t0) < dur * 1000.0) {
        const int slot = g_xiSlot.load();
        if (slot != lastSlot) {
            if (slot >= 0) {
                out("[+] %s (%.1f с) Windows: геймпад в слоте XInput %d\n", clockStr().c_str(), toMs(now() - t0) / 1000.0, slot);
                everXi = true;
            } else {
                out("[!] %s (%.1f с) Windows: геймпада в XInput НЕТ\n", clockStr().c_str(), toMs(now() - t0) / 1000.0);
                if (everXi) ++drops;
            }
            lastSlot = slot;
        }
        const int bs = g_boardState.load();
        if (useBoard && bs != lastBoard && bs != 0) {
            if (bs == 1) {
                out("[+] %s (%.1f с) плата: 0x54 отдаёт снимки\n", clockStr().c_str(), toMs(now() - t0) / 1000.0);
                everBoard = true;
            } else {
                out("[!] %s (%.1f с) плата: 0x54 %s (опросов до этого %llu)\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
                    bs == 2 ? "отвечает отказом FF" : "молчит", static_cast<unsigned long long>(g_polls.load()));
                if (everBoard) ++drops;
            }
            lastBoard = bs;
        }
        if (now() >= nextRep) {
            nextRep += us(5e6);
            const uint64_t pk = g_xiPackets.load(), polls = g_polls.load(), ok = g_ok.load(), sq = g_seqChanges.load(), ff = g_ff.load(),
                           lost = g_lost.load();
            out("%s | %3.0f | %-14s | %9.0f | %7llu | %7llu | %11.0f | %2llu | %6llu | %.3f\n", clockStr().c_str(), toMs(now() - t0) / 1000.0,
                slot >= 0 ? "подключён" : "НЕТ", (pk - pPk) / 5.0, static_cast<unsigned long long>(polls - pPolls),
                static_cast<unsigned long long>(ok - pOk), (sq - pSeq) / 5.0, static_cast<unsigned long long>(ff - pFf),
                static_cast<unsigned long long>(lost - pLost), g_rttMax.exchange(0.0));
            pPk = pk;
            pPolls = polls;
            pOk = ok;
            pSeq = sq;
            pFf = ff;
            pLost = lost;
        }
        Sleep(10);
    }
    g_stop = true;
    xi.join();
    if (board.joinable()) board.join();
    out("итог %s: %.0f с, опросов %llu, снимков %llu, FF %llu, молчит %llu; геймпад %s\n", clockStr().c_str(), dur,
        static_cast<unsigned long long>(g_polls.load()), static_cast<unsigned long long>(g_ok.load()), static_cast<unsigned long long>(g_ff.load()),
        static_cast<unsigned long long>(g_lost.load()), drops ? "ПРОПАДАЛ" : everXi ? "был на месте весь прогон" : "так и не появился в Windows");
    timeEndPeriod(1);
    if (useBoard) p.close();
    if (g_log) std::fclose(g_log);
    return drops ? 3 : everXi ? 0 : 4;
}
