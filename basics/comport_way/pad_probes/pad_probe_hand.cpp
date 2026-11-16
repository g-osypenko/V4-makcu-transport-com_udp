// pad_probe_hand.cpp - проба 5 (фаза 1): инжект правого стика, пока рука двигает стик.
//
// ПИШЕТ В ПЛАТУ ТОЛЬКО SET 0x40 (правый стик). Настройки 0x3E читает до и после (INFO, READ).
// Порт читается без ожидания (как все пробы): тайм-ауты чтения драйвера CH343 не используются.
//
// Такт 1 мс: GET 0x54 (физика), на чётных тактах - команда, если шаг её требует. Параллельно поток XInput.
// Журналы: PREFIX_tx.csv (команды), PREFIX_board.csv (0x54), PREFIX_xinput.csv (Windows), столбец step - имя шага.
//
// Шаги и надписи в окне (их же писать в чат):
//   старт   "Нажми кнопку A на геймпаде, чтобы начать"
//   rotate  "Вращай ПРАВЫЙ стик по кругу и не останавливайся"            13 с
//             0-3 с ничего; 3-10 с постоянный инжект (12000, -1); на 10-й секунде отпускание нулями; 10-13 с ничего
//   right   "Отклони ПРАВЫЙ стик ВПРАВО до упора и держи"                 9 с
//             0-4 с ничего; 4-6 с инжект (-12000, -1); на 6-й секунде отпускание; 6-9 с ничего
//   echo    "Води ПРАВЫМ стиком плавно в разные стороны"                 12 с
//             0-3 с ничего; 3-7.5 с команда = физика (0x54) как есть; 7.5-12 с команда = физика с опережением
//             u = d + (d - d_пред); в конце отпускание
//   конец   "ГОТОВО. Отпусти стик, окно закроется само"
// Человек отзывается на надпись через ~2.5 с: разбор берёт участки с запасом.
//
// Набор 2 (--set 2): от чего зависит уход выхода за рукой через 2 с инжекта. Надписи:
//   rotcircle "Вращай ПРАВЫЙ стик по кругу и не останавливайся"          12 с: 3-11 с инжект - круг радиусом 12000, оборот за 2 с
//   rotdither "Продолжай вращать ПРАВЫЙ стик по кругу"                    12 с: 2-10 с инжект 12000/12001 через команду
//   holdright "Отклони ПРАВЫЙ стик ВПРАВО до упора и держи неподвижно"   11 с: 4-10 с инжект (-12000, -1)
//   wiggle    "Покачивай ПРАВЫЙ стик чуть-чуть около центра"              12 с: 3-11 с инжект (12000, -1)
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_hand.cpp -lwinmm -lgdi32 -static -o pad_probe_hand.exe
// Запуск: pad_probe_hand.exe [--port COM5] [--out PREFIX] [--set 1|2]
#include "pad_probe_ui.h"

using namespace padprobe;

namespace {

enum class Mode { None, Const, Echo, EchoLead, Circle, Dither };  // Circle: rx - радиус, ry - период, мс; Dither: цель +0/+1 через команду
struct Span {
    int fromMs, toMs;
    Mode mode;
    int rx, ry;  // для Const
};
struct Step {
    const char* tag;
    const wchar_t* text;
    int durMs;
    std::vector<Span> spans;
};

struct TxRow {
    int64_t t;
    int step, rx, ry, release, mode;
};
struct BoardRow {
    int64_t t;
    int step;
    PadPhysical v;
};
std::vector<TxRow> g_tx;
std::vector<BoardRow> g_board;
std::vector<std::string> g_notes;
std::atomic<int> g_stepNow{-1};
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
std::string readProcessing(Port& p) {
    const uint8_t q[2] = {0x1D, 0x00};
    auto info = p.ask(kOpConnection, q, sizeof(q), 300000.0);
    if (!info || info->size() != 14 || (*info)[2] != 0) return "INFO: нет ответа";
    const uint32_t rev = le32(&(*info)[8]);
    uint8_t rd[9] = {0x1D, 0x01, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24),
        0, 0, 32};
    auto r = p.ask(kOpConnection, rd, sizeof(rd), 300000.0);
    if (!r || r->size() != 41 || (*r)[2] != 0) return "READ: нет ответа";
    const uint8_t* img = &(*r)[9];
    char s[200];
    std::snprintf(s, sizeof(s), "ревизия %u, интерполяция %u (0 Off, 1 Fixed, 2 Auto), буфер %u мс, правый стик: флаги %06X, байт strength %u (15 = не задана)", rev,
        le32(img + 4), le32(img + 8), le32(img + 20), img[25]);
    return s;
}

void runScript(Port* port, const std::vector<Step>* steps) {
    Port& p = *port;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint32_t lastSeq = 0;
    bool have = false;
    PadPhysical phys;
    auto tickWait = [](int64_t due) {
        while (now() < due) {
            if (due - now() > us(1600.0)) Sleep(1);
            else YieldProcessor();
        }
    };
    for (size_t i = 0; i < steps->size() && !g_stop; ++i) {
        const Step& s = (*steps)[i];
        const int step = static_cast<int>(i);
        g_stepNow = step;
        g_tag = step;
        say(s.text, RGB(0, 120, 0), s.durMs / 1000.0);
        const int64_t start = now();
        Mode prevMode = Mode::None;
        int prevD[2] = {0, 0};
        bool havePrevD = false;
        for (int k = 0; k < s.durMs && !g_stop; ++k) {
            tickWait(start + us(k * 1000.0));
            if (pollBoardOnce(p, &phys)) {
                if (!have || phys.seq != lastSeq) {
                    have = true;
                    lastSeq = phys.seq;
                    g_board.push_back({now() - g_t0, step, phys});
                }
            }
            Mode mode = Mode::None;
            int cx = 0, cy = 0;
            for (const auto& sp : s.spans) {
                if (k >= sp.fromMs && k < sp.toMs) {
                    mode = sp.mode;
                    cx = sp.rx;
                    cy = sp.ry;
                }
            }
            if (mode == Mode::None && prevMode != Mode::None) {
                // отпускание: обе координаты 0
                sendState(p, 0, 0);
                g_tx.push_back({now() - g_t0, step, 0, 0, 1, 0});
                g_txOn = false;
                havePrevD = false;
                if (auto st = readState(p)) {
                    char note[160];
                    std::snprintf(note, sizeof(note), "%s: после нулей на %d мс GET 0x40 = %d, %d", s.tag, k, st->rx, st->ry);
                    g_notes.emplace_back(note);
                }
            }
            if (mode != Mode::None && (k % 2) == 0) {
                if (mode == Mode::Echo || mode == Mode::EchoLead) {
                    int d[2] = {phys.s.rx, phys.s.ry};
                    cx = d[0];
                    cy = d[1];
                    if (mode == Mode::EchoLead && havePrevD) {
                        cx = d[0] + (d[0] - prevD[0]);
                        cy = d[1] + (d[1] - prevD[1]);
                    }
                    prevD[0] = d[0];
                    prevD[1] = d[1];
                    havePrevD = true;
                }
                if (mode == Mode::Circle) {
                    const double a = 2.0 * 3.14159265358979323846 * k / static_cast<double>(cy);
                    const int r = cx;
                    cx = static_cast<int>(std::lround(r * std::cos(a)));
                    cy = static_cast<int>(std::lround(r * std::sin(a)));
                } else if (mode == Mode::Dither) {
                    cx += (k / 2) % 2;
                }
                cx = std::clamp(cx, -32768, 32767);
                cy = std::clamp(cy, -32768, 32767);
                if (cx == 0 && cy == 0) cy = -1;  // (0, 0) - это отпускание, а не центр
                sendState(p, cx, cy);
                g_tx.push_back({now() - g_t0, step, cx, cy, 0, static_cast<int>(mode)});
                g_txRx = cx;
                g_txRy = cy;
                g_txOn = true;
            }
            prevMode = mode;
        }
        if (prevMode != Mode::None) {
            sendState(p, 0, 0);
            g_tx.push_back({now() - g_t0, step, 0, 0, 1, 0});
            g_txOn = false;
        }
    }
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
    std::string port = "COM5", prefix = "pad_hand";
    int stepSet = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--out" && more) prefix = argv[++i];
        else if (a == "--set" && more) stepSet = std::atoi(argv[++i]);
        else {
            std::printf("pad_probe_hand.exe [--port COM5] [--out PREFIX] [--set 1|2]\n");
            return 2;
        }
    }
    const std::vector<Step> set1 = {
        {"rotate", L"Вращай ПРАВЫЙ стик по кругу и не останавливайся", 13000, {{3000, 10000, Mode::Const, 12000, -1}}},
        {"right", L"Отклони ПРАВЫЙ стик ВПРАВО до упора и держи", 9000, {{4000, 6000, Mode::Const, -12000, -1}}},
        {"echo", L"Води ПРАВЫМ стиком плавно в разные стороны", 12000, {{3000, 7500, Mode::Echo, 0, 0}, {7500, 12000, Mode::EchoLead, 0, 0}}},
    };
    // Набор 2: от чего зависит уход выхода за рукой через 2 с инжекта (замер 2026-11-14, набор 1, шаг rotate).
    const std::vector<Step> set2 = {
        {"rotcircle", L"Вращай ПРАВЫЙ стик по кругу и не останавливайся", 12000, {{3000, 11000, Mode::Circle, 12000, 2000}}},
        {"rotdither", L"Продолжай вращать ПРАВЫЙ стик по кругу", 12000, {{2000, 10000, Mode::Dither, 12000, -1}}},
        {"holdright", L"Отклони ПРАВЫЙ стик ВПРАВО до упора и держи неподвижно", 11000, {{4000, 10000, Mode::Const, -12000, -1}}},
        {"wiggle", L"Покачивай ПРАВЫЙ стик чуть-чуть около центра", 12000, {{3000, 11000, Mode::Const, 12000, -1}}},
    };
    const std::vector<Step>& steps = stepSet == 2 ? set2 : set1;
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
    timeBeginPeriod(1);
    g_tx.reserve(40000);
    g_board.reserve(80000);
    g_xi.reserve(200000);
    g_t0 = now();
    HWND hwnd = openWindow(L"MAKCU: инжект правого стика при движущейся руке");
    std::thread xi(xinputLoop);
    if (waitForA(p)) {
        std::thread worker(runScript, &p, &steps);
        while (!g_done) {
            pumpMessages();
            const int st = g_stepNow.load();
            if (st >= 0 && st < static_cast<int>(steps.size())) swprintf(g_status, 256, L"шаг %d из %zu: %hs", st + 1, steps.size(), steps[static_cast<size_t>(st)].tag);
            Sleep(5);
        }
        worker.join();
        if (!g_stop) {
            say(L"ГОТОВО. Отпусти стик, окно закроется само");
            waitUi(1.5);
        }
    } else {
        std::printf("[-] старт не дан (Esc или 300 с без кнопки A)\n");
    }
    g_stop = true;
    xi.join();
    DestroyWindow(hwnd);
    timeEndPeriod(1);

    auto tag = [&](int i) { return i >= 0 && i < static_cast<int>(steps.size()) ? steps[static_cast<size_t>(i)].tag : "?"; };
    if (FILE* f = std::fopen((prefix + "_tx.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,step,rx,ry,release,mode\n");
        for (const auto& r : g_tx) std::fprintf(f, "%.1f,%s,%d,%d,%d,%d\n", toUs(r.t), tag(r.step), r.rx, r.ry, r.release, r.mode);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((prefix + "_board.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,step,seq,usb_time,timing,cadence,low,high,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_board) {
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), tag(r.step), r.v.seq, r.v.usbTime, r.v.timing, r.v.cadence, r.v.s.low, r.v.s.high,
                r.v.s.lt, r.v.s.rt, r.v.s.lx, r.v.s.ly, r.v.s.rx, r.v.s.ry);
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen((prefix + "_xinput.csv").c_str(), "w")) {
        std::fprintf(f, "t_us,step,pkt,buttons,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_xi) {
            if (r.tag < 0 || r.tag >= static_cast<int>(steps.size())) continue;
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), tag(r.tag), r.pkt, r.buttons, r.lt, r.rt, r.lx, r.ly, r.rx, r.ry);
        }
        std::fclose(f);
    }
    std::printf("[+] команд %zu, снимков платы %zu, пакетов Windows %zu -> %s_{tx,board,xinput}.csv\n", g_tx.size(), g_board.size(), g_xi.size(), prefix.c_str());
    for (const auto& n : g_notes) std::printf("    %s\n", n.c_str());
    std::printf("[i] настройки после прогона: %s\n", readProcessing(p).c_str());
    if (auto st = readState(p)) std::printf("[i] GET 0x40 после прогона: правый стик %d, %d\n", st->rx, st->ry);
    p.close();
    return 0;
}
