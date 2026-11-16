// pad_probe_inject.cpp - проба 4 (фаза 1): как плата передаёт инжект правого стика (SET 0x40) в Windows при
// interpolation = auto и как работает отпускание нулями (zero-state handoff).
//
// ПИШЕТ В ПЛАТУ ТОЛЬКО SET 0x40 CONTROLLER_STATE (20 байт, все поля кроме правого стика равны 0). Настройки 0x3E
// только читает (INFO и READ) до и после прогона: интерполяция должна остаться 2 (Auto).
//
// Такт 1 мс: SET 0x40 (если отрезок на этом такте что-то шлёт), затем GET 0x54 (физика во время инжекта).
// Параллельно поток XInput пишет, что получил Windows. После каждого отрезка: SET 0x40 с нулями (отпускание),
// проверка GET 0x40 и пауза. Журналы: PREFIX_tx.csv (что и когда ушло), PREFIX_board.csv (0x54),
// PREFIX_xinput.csv (Windows).
//
// На плате ось Y направлена ВНИЗ: Windows Y = -Y-1. Пара (0, 0) в 0x40 - это не центр, а "отпустить стик".
// Центр под управлением программы - (0, -1): в Windows это ровно (0, 0).
//
// Надписи в окне (их же писать в чат):
//   "Нажми кнопку A на геймпаде, чтобы начать"
//   "Положи геймпад на стол и убери руки"                 (3 с)
//   "НЕ ТРОГАЙ геймпад: идёт инжект правого стика"        (весь прогон)
//   "ГОТОВО. Окно закроется само"
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_inject.cpp -lwinmm -lgdi32 -static -o pad_probe_inject.exe
// Запуск: pad_probe_inject.exe [--port COM5] [--out PREFIX] [--only ИМЯ[,ИМЯ...]] [--script ФАЙЛ] [--nostart]
//   --script ФАЙЛ: отрезки из файла вместо встроенных. Строка "seg ИМЯ ДЛИТЕЛЬНОСТЬ_МС ПАУЗА_МС", за ней строки
//   "такт rx ry" (координаты платы, такт в мс от начала отрезка; такты без строки ничего не шлют).
//   Сценарии делает make_inject_script.py.
#include "pad_probe_ui.h"

#include <functional>
#include <memory>

using namespace padprobe;

namespace {

struct Cmd {
    bool send = false;
    int rx = 0, ry = 0;
};
struct Segment {
    std::string name;
    int durMs;
    int gapMs;
    std::function<Cmd(int)> fn;  // такт (мс от начала отрезка) -> команда
};

struct TxRow {
    int64_t t;
    int seg, k, rx, ry, release;
};
struct BoardRow {
    int64_t t;
    int seg;
    PadPhysical v;
};
std::vector<TxRow> g_tx;
std::vector<BoardRow> g_board;
std::vector<std::string> g_notes;
std::atomic<int> g_segNow{-1};
std::atomic<bool> g_done{false};

bool sendState(Port& p, int rx, int ry) {
    uint8_t pl[20] = {};
    const int16_t x = static_cast<int16_t>(std::clamp(rx, -32768, 32767)), y = static_cast<int16_t>(std::clamp(ry, -32768, 32767));
    pl[16] = static_cast<uint8_t>(x);
    pl[17] = static_cast<uint8_t>(static_cast<uint16_t>(x) >> 8);
    pl[18] = static_cast<uint8_t>(y);
    pl[19] = static_cast<uint8_t>(static_cast<uint16_t>(y) >> 8);
    return p.send(kOpCtlState, pl, sizeof(pl));
}

std::optional<PadState> readState(Port& p) {
    auto r = p.ask(kOpCtlState, nullptr, 0, 30000.0);
    if (!r || r->size() != 20) return std::nullopt;
    return parseState(r->data());
}

// Байты 0..19 образа настроек: флаги, интерполяция, буфер, профиль.
std::string readProcessing(Port& p) {
    const uint8_t q[2] = {0x1D, 0x00};
    auto info = p.ask(kOpConnection, q, sizeof(q), 300000.0);
    if (!info || info->size() != 14 || (*info)[2] != 0) return "INFO: нет ответа";
    const uint32_t rev = le32(&(*info)[8]);
    uint8_t rd[9] = {0x1D, 0x01, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24),
        0, 0, 20};
    auto r = p.ask(kOpConnection, rd, sizeof(rd), 300000.0);
    if (!r || r->size() != 29 || (*r)[2] != 0) return "READ: нет ответа";
    const uint8_t* img = &(*r)[9];
    char s[160];
    std::snprintf(s, sizeof(s), "ревизия %u, интерполяция %u (0 Off, 1 Fixed, 2 Auto), буфер %u мс, флаги %08X", rev, le32(img + 4), le32(img + 8), le32(img));
    return s;
}

void pollPhysical(Port& p, int seg, uint32_t& lastSeq, bool& have) {
    PadPhysical v;
    if (!pollBoardOnce(p, &v)) return;
    if (!have || v.seq != lastSeq) {
        have = true;
        lastSeq = v.seq;
        g_board.push_back({now() - g_t0, seg, v});
    }
}

void runScript(Port* port, const std::vector<Segment>* segs) {
    Port& p = *port;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint32_t lastSeq = 0;
    bool have = false;
    auto tickWait = [](int64_t due) {
        while (now() < due) {
            if (due - now() > us(1600.0)) Sleep(1);
            else YieldProcessor();
        }
    };
    for (size_t i = 0; i < segs->size() && !g_stop; ++i) {
        const Segment& s = (*segs)[i];
        const int seg = static_cast<int>(i);
        g_segNow = seg;
        g_tag = seg;
        const int64_t start = now();
        std::optional<PadState> back;
        Cmd last;
        for (int k = 0; k < s.durMs && !g_stop; ++k) {
            tickWait(start + us(k * 1000.0));
            const Cmd c = s.fn(k);
            if (c.send) {
                sendState(p, c.rx, c.ry);
                g_tx.push_back({now() - g_t0, seg, k, c.rx, c.ry, 0});
                g_txRx = c.rx;
                g_txRy = c.ry;
                g_txOn = true;
                last = c;
            }
            pollPhysical(p, seg, lastSeq, have);
            if (k == s.durMs / 2) back = readState(p);  // что плата считает заданным состоянием в середине отрезка
        }
        // отпускание: обе координаты правого стика 0
        const int64_t relStart = now();
        sendState(p, 0, 0);
        g_tx.push_back({now() - g_t0, seg, s.durMs, 0, 0, 1});
        g_txOn = false;
        std::optional<PadState> after;
        for (int k = 0; k < s.gapMs; ++k) {
            tickWait(relStart + us((k + 1) * 1000.0));
            pollPhysical(p, seg, lastSeq, have);
            if (k == 20) after = readState(p);
        }
        char note[256];
        std::snprintf(note, sizeof(note), "%-10s последняя команда (%d, %d); GET 0x40 в середине: %s; через 20 мс после нулей: %s", s.name.c_str(), last.rx, last.ry,
            back ? (std::to_string(back->rx) + ", " + std::to_string(back->ry)).c_str() : "нет ответа",
            after ? (std::to_string(after->rx) + ", " + std::to_string(after->ry)).c_str() : "нет ответа");
        g_notes.emplace_back(note);
    }
    // Чистый выход: нули и проверка
    g_tag = -1;
    for (int attempt = 0; attempt < 5; ++attempt) {
        sendState(p, 0, 0);
        Sleep(5);
        auto st = readState(p);
        if (st && st->rx == 0 && st->ry == 0 && st->lx == 0 && st->ly == 0 && st->low == 0 && st->high == 0 && st->lt == 0 && st->rt == 0) {
            g_notes.emplace_back("выход: GET 0x40 = все нули (попытка " + std::to_string(attempt + 1) + ")");
            break;
        }
        if (attempt == 4) g_notes.emplace_back("[-] выход: GET 0x40 НЕ нули после 5 попыток");
    }
    g_done = true;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();
    std::string port = "COM5", prefix = "pad_inject", only, script;
    bool noStart = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--out" && more) prefix = argv[++i];
        else if (a == "--only" && more) only = argv[++i];
        else if (a == "--script" && more) script = argv[++i];
        else if (a == "--nostart") noStart = true;
        else {
            std::printf("pad_probe_inject.exe [--port COM5] [--out PREFIX] [--only ИМЯ[,ИМЯ...]] [--nostart]\n");
            return 2;
        }
    }
    const double kPi = 3.14159265358979323846;
    std::vector<Segment> all;
    auto hold = [](int rx, int ry) { return [rx, ry](int) { return Cmd{true, rx, ry}; }; };
    all.push_back({"latch", 600, 700, [](int k) { return Cmd{k == 0, 12000, -1}; }});
    all.push_back({"hold12k", 600, 700, hold(12000, -1)});
    all.push_back({"hold2k", 400, 500, hold(2000, -1)});
    all.push_back({"hold30k", 400, 700, hold(30000, -1)});
    all.push_back({"holdUp", 500, 600, hold(0, -12000)});
    all.push_back({"flip", 900, 600, [](int k) { return Cmd{true, (k / 300) % 2 ? -12000 : 12000, -1}; }});
    all.push_back({"center", 700, 500, [](int k) { return Cmd{true, k < 300 ? 12000 : 0, -1}; }});
    all.push_back({"ramp", 2000, 600, [](int k) { return Cmd{true, k < 1000 ? 30 * k : 30000 - 30 * (k - 1000), -1}; }});
    all.push_back({"stairs", 600, 500, [](int k) { return Cmd{true, k < 300 ? 10000 + k / 10 : 10030 + 16 * ((k - 300) / 10), -1}; }});
    for (int f : {1, 2, 5, 10, 20, 40, 80, 160}) {
        const int dur = f <= 2 ? 2000 : f == 5 ? 1500 : 1000;
        all.push_back({"sine" + std::to_string(f), dur, 400,
            [f, kPi](int k) { return Cmd{true, static_cast<int>(std::lround(12000.0 * std::sin(2.0 * kPi * f * k / 1000.0))), -1}; }});
    }
    all.push_back({"circle", 2000, 600, [kPi](int k) {
                       const double a = 2.0 * kPi * 2.0 * k / 1000.0;
                       int x = static_cast<int>(std::lround(20000.0 * std::cos(a))), y = static_cast<int>(std::lround(20000.0 * std::sin(a)));
                       if (x == 0 && y == 0) y = -1;
                       return Cmd{true, x, y};
                   }});
    all.push_back({"noise", 1000, 600, [](int) {
                       static uint32_t s = 12345;
                       auto rnd = [] {
                           s = s * 1664525u + 1013904223u;
                           return static_cast<int>((s >> 8) % 401) - 200;
                       };
                       return Cmd{true, 15000 + rnd(), -5000 + rnd()};
                   }});
    all.push_back({"sparse4", 1000, 600, [](int k) { return Cmd{k % 4 == 0, 2000 + 20 * k, -1}; }});
    all.push_back({"sparse8", 1000, 600, [](int k) { return Cmd{k % 8 == 0, 2000 + 20 * k, -1}; }});
    all.push_back({"sparse50", 600, 600, [](int k) { return Cmd{k % 50 == 0, 3000 + 2000 * (k / 50), -1}; }});
    std::vector<Segment> segs;
    if (!script.empty()) {
        // Сценарий из файла: строка "seg ИМЯ ДЛИТЕЛЬНОСТЬ_МС ПАУЗА_МС", за ней строки "такт rx ry" (такт в мс от начала отрезка).
        FILE* f = std::fopen(script.c_str(), "r");
        if (!f) {
            std::printf("[-] сценарий %s не открылся\n", script.c_str());
            return 2;
        }
        char line[256];
        std::shared_ptr<std::vector<Cmd>> table;
        while (std::fgets(line, sizeof(line), f)) {
            char name[64];
            int a = 0, b = 0, c = 0;
            if (std::sscanf(line, "seg %63s %d %d", name, &a, &b) == 3) {
                table = std::make_shared<std::vector<Cmd>>(static_cast<size_t>(std::max(a, 1)));
                auto t = table;
                segs.push_back({name, a, b, [t](int k) { return (*t)[static_cast<size_t>(k)]; }});
            } else if (table && std::sscanf(line, "%d %d %d", &a, &b, &c) == 3 && a >= 0 && a < static_cast<int>(table->size())) {
                if (b == 0 && c == 0) c = -1;  // (0, 0) - это отпускание, а не центр
                (*table)[static_cast<size_t>(a)] = Cmd{true, b, c};
            }
        }
        std::fclose(f);
        if (segs.empty()) {
            std::printf("[-] в сценарии нет отрезков\n");
            return 2;
        }
    } else if (only.empty()) segs = all;
    else {
        const std::string list = "," + only + ",";
        for (const auto& s : all) {
            if (list.find("," + s.name + ",") != std::string::npos) segs.push_back(s);
        }
        if (segs.empty()) {
            std::printf("[-] отрезков с такими именами нет\n");
            return 2;
        }
    }

    Port p;
    if (!p.open(port)) {
        std::printf("[-] %s не открылся: ошибка %lu (порт занят Toolkit/SDK или USB2 не подключён)\n", port.c_str(), GetLastError());
        return 1;
    }
    uint8_t kinds = 0;
    if (auto r = p.ask(kOpDevice, nullptr, 0, 300000.0)) kinds = r->empty() ? 0 : (*r)[0];
    else {
        std::printf("[-] плата не ответила на DEVICE (0x02)\n");
        return 1;
    }
    std::printf("[i] kinds %02X = %s\n", kinds, kindsText(kinds).c_str());
    if (!(kinds & 0xFC)) {
        std::printf("[-] плата не видит геймпад на USB3\n");
        return 1;
    }
    std::printf("[i] настройки до прогона: %s\n", readProcessing(p).c_str());
    if (auto st = readState(p)) std::printf("[i] GET 0x40 до прогона: правый стик %d, %d; левый %d, %d; кнопки %08X\n", st->rx, st->ry, st->lx, st->ly, st->low);
    timeBeginPeriod(1);
    g_tx.reserve(60000);
    g_board.reserve(120000);
    g_xi.reserve(200000);
    g_t0 = now();
    HWND hwnd = openWindow(L"MAKCU: проба инжекта правого стика");
    std::thread xi(xinputLoop);
    bool go = noStart || waitForA(p);
    if (go) {
        say(L"Положи геймпад на стол и убери руки", RGB(200, 0, 0), 3.0);
        go = waitUi(3.0);
    }
    if (go) {
        double total = 0;
        for (const auto& s : segs) total += (s.durMs + s.gapMs) / 1000.0;
        say(L"НЕ ТРОГАЙ геймпад: идёт инжект правого стика", RGB(200, 0, 0), total);
        std::thread worker(runScript, &p, &segs);
        while (!g_done) {
            pumpMessages();
            const int seg = g_segNow.load();
            if (seg >= 0 && seg < static_cast<int>(segs.size()))
                swprintf(g_status, 256, L"отрезок %d из %zu: %hs", seg + 1, segs.size(), segs[static_cast<size_t>(seg)].name.c_str());
            Sleep(5);
        }
        worker.join();
        if (!g_stop) {
            say(L"ГОТОВО. Окно закроется само");
            waitUi(1.5);
        }
    } else {
        std::printf("[-] старт не дан (Esc или 300 с без кнопки A)\n");
    }
    g_stop = true;
    xi.join();
    DestroyWindow(hwnd);
    timeEndPeriod(1);

    if (FILE* f = std::fopen((prefix + "_tx.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,seg,k,rx,ry,release\n");
        for (const auto& r : g_tx) std::fprintf(f, "%.1f,%s,%d,%d,%d,%d\n", toUs(r.t), segs[static_cast<size_t>(r.seg)].name.c_str(), r.k, r.rx, r.ry, r.release);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((prefix + "_board.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,seg,seq,usb_time,timing,cadence,low,high,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_board) {
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), segs[static_cast<size_t>(r.seg)].name.c_str(), r.v.seq, r.v.usbTime,
                r.v.timing, r.v.cadence, r.v.s.low, r.v.s.high, r.v.s.lt, r.v.s.rt, r.v.s.lx, r.v.s.ly, r.v.s.rx, r.v.s.ry);
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen((prefix + "_xinput.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,seg,pkt,buttons,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_xi) {
            if (r.tag < 0 || r.tag >= static_cast<int>(segs.size())) continue;
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), segs[static_cast<size_t>(r.tag)].name.c_str(), r.pkt, r.buttons, r.lt, r.rt, r.lx, r.ly,
                r.rx, r.ry);
        }
        std::fclose(f);
    }
    std::printf("[+] команд %zu, снимков платы %zu, пакетов Windows %zu -> %s_{tx,board,xinput}.csv\n", g_tx.size(), g_board.size(), g_xi.size(), prefix.c_str());
    for (const auto& n : g_notes) std::printf("    %s\n", n.c_str());
    std::printf("[i] настройки после прогона: %s\n", readProcessing(p).c_str());
    if (auto st = readState(p)) std::printf("[i] GET 0x40 после прогона: правый стик %d, %d; левый %d, %d; кнопки %08X\n", st->rx, st->ry, st->lx, st->ly, st->low);
    p.close();
    return 0;
}
