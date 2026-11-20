// pad_soak.cpp - "лестница нагрузки" для геймпада по COM: на какой нагрузке геймпад пропадает с платы.
//
//   pad_soak.exe [--port COM5] [--step S] [--steps a,b,c] [--from NAME] [--list]
//
// Зачем (2026-11-15, V4.126): геймпад GameSir простоял без команд ~5 ч 50 мин, а под эталоном пропал на втором
// прогоне (17:58:37, после пяти ступенек стика, перед первым tap()). Вопрос владельца: не слишком ли жёсткие прогоны.
// Эталон при сглаживании Off шлёт до ~3000 кадров/с: GET 0x54 2000/с + SET 0x40 1000/с. Лестница идёт от мягкой
// нагрузки к жёсткой, каждая ступень --step секунд (по умолчанию 120), и останавливается на первом обрыве.
//
// Ступени (имя: опрос 0x54 / команды стика / что делает):
//   poll1k       1000/с / нет     / только чтение
//   poll2k       2000/с / нет     / только чтение (как эталон без инжекта)
//   stick2-p1k   1000/с / 500/с   / круг R 12000, оборот за 1 с
//   stick1-p1k   1000/с / 1000/с  / круг
//   stick1-p2k   2000/с / 1000/с  / круг (как эталон сейчас)
//   taps         2000/с / 1000/с  / круг + tap(RT, 80 мс) каждые 250 мс
//   enter        2000/с / 1000/с  / вход (12000, -1) на 300 мс, отпускание, пауза 400 мс - как раздел 3 бенчмарка
//   hold         2000/с / нет     / удержание RT + A 3.2 с, пауза 0.5 с (кнопки без стика, повтор раз в 100 мс)
//   soft         1000/с / 1000/с  / всё вместе на мягком опросе: круг 2 с + tap, отпускание, удержание RT + A 1.5 с
//   poll250, poll500        250 и 500/с / нет / только чтение
//   stick8-p100, stick4-p100, stick2-p100, stick1-p100  100/с / 125, 250, 500 и 1000/с / круг
//   all-p100     100/с / 1000/с   / как soft, но опрос 100/с
//   enter2-p100, all2-p100  100/с / 500/с / как enter и soft на мягком темпе (all2-p100 замолчал на 51-й с)
//   taps2-p100   100/с / 500/с    / круг + tap(RT) каждые 250 мс, стик не отпускается (кнопки со стиком)
//   hold2-p100   100/с / нет      / удержание RT + A 3.2 с, пауза 0.5 с (кнопки без стика)
//   apply2-p100  100/с / 500/с    / круг + запись настроек 0x1D BEGIN / WRITE / APPLY каждые 2 с (буфер 4 <-> 8, Off)
// Без --steps ступени идут в порядке таблицы kSteps (с --from - начиная с указанной); с --steps - в заданном порядке.
//
// Свободные ступени (2026-11-20, поиск безопасного темпа на 4133 с упором на опрос): имя p<N> - опрос 0x54 N раз в с
// без инжекта, p<N>-c<M> - опрос N/с + круг стиком M команд/с (опрос 100..2000/с, команд до 1000/с).
// Пример: --steps p500-c250,p1000-c250,p2000-c250.
//
// --keep-going S (2026-11-20): на 4133 геймпад после отказа сам встаёт за ~1.2-2 с. С ключом лестница на отказе не
// останавливается: ждёт до S с новых отчётов, считает отказ и время восстановления и идёт дальше (нагрузка всё это
// время продолжается). Не вернулся за S с - стоп, код 3. Код 2 - отказы были, но все восстановились.
//
// ГЕЙМПАД НЕ ТРОГАТЬ. Окна нет: рядом запускать pad_probes\pad_probe_view.exe --rate 0 --notouch --dur S.
// Steam закрыть (раскладка рабочего стола превращает A в Enter). Код выхода: 0 - все ступени пройдены, 3 - обрыв.
//
// Сборка: build_example.bat
#include "makcu_gamepad_raw_transport.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Q = makcu::detail::Qpc;
using Id = makcu::MakcuControlId;

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
    const auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    int slot = -1;
    uint32_t lastPkt = 0;
    bool have = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
        XState s{};
        if (slot < 0) {
            for (DWORD i = 0; i < 4; ++i) {
                if (get(i, &s) == ERROR_SUCCESS) {
                    slot = static_cast<int>(i);
                    break;
                }
            }
            if (slot < 0) {
                g_xiOk = false;
                Sleep(100);
                continue;
            }
        } else if (get(static_cast<DWORD>(slot), &s) != ERROR_SUCCESS) {
            slot = -1;
            g_xiOk = false;
            continue;
        }
        g_xiOk = true;
        if (!have || s.pkt != lastPkt) {
            have = true;
            lastPkt = s.pkt;
            g_xiPackets.fetch_add(1, std::memory_order_relaxed);
        }
        const int64_t until = Q::now() + Q::fromUs(250.0);
        while (Q::now() < until) YieldProcessor();
    }
}

struct Step {
    const char* name;
    double pollUs;
    double cmdUs;  // 0 - стик не трогаем
    int pattern;   // 0 чтение, 1 круг, 2 круг + tap, 3 вход / отпускание, 4 удержание кнопок, 5 всё вместе, 6 круг + запись настроек
    const char* what;
};
const Step kSteps[] = {
    {"poll1k", 1000, 0, 0, "опрос 1000/с, без инжекта"},
    {"poll2k", 500, 0, 0, "опрос 2000/с, без инжекта"},
    {"stick2-p1k", 1000, 2000, 1, "опрос 1000/с, круг стиком 500 команд/с"},
    {"stick1-p1k", 1000, 1000, 1, "опрос 1000/с, круг стиком 1000 команд/с"},
    {"stick1-p2k", 500, 1000, 1, "опрос 2000/с, круг стиком 1000 команд/с (как эталон)"},
    {"taps", 500, 1000, 2, "опрос 2000/с, круг 1000 команд/с + tap(RT) каждые 250 мс"},
    {"enter", 500, 1000, 3, "опрос 2000/с, вход 300 мс / отпускание / пауза 400 мс"},
    {"hold", 500, 0, 4, "опрос 2000/с, удержание RT + A 3.2 с / пауза 0.5 с, без стика"},
    {"soft", 1000, 1000, 5, "опрос 1000/с: круг 2 с + tap, отпускание, удержание RT + A 1.5 с"},
    // Добавлено после первого запуска (обрыв на poll1k на 39-й секунде; голая проба 100/с - 300 с чисто):
    {"poll250", 4000, 0, 0, "опрос 250/с, без инжекта"},
    {"poll500", 2000, 0, 0, "опрос 500/с, без инжекта"},
    {"stick8-p100", 10000, 8000, 1, "опрос 100/с, круг стиком 125 команд/с"},
    {"stick4-p100", 10000, 4000, 1, "опрос 100/с, круг стиком 250 команд/с"},
    {"stick2-p100", 10000, 2000, 1, "опрос 100/с, круг стиком 500 команд/с"},
    {"enter2-p100", 10000, 2000, 3, "опрос 100/с, 500 команд/с: вход 300 мс / отпускание / пауза 400 мс"},
    // Добавлено 2026-11-15 вечером: all2-p100 замолчал на 51-й с, enter2-p100 (одни отпускания) - 180 с чисто.
    // Три ступени делят all2-p100 и опыт "Fixed + буфер" на части: кнопки со стиком, кнопки без стика, запись настроек.
    {"taps2-p100", 10000, 2000, 2, "опрос 100/с, круг 500 команд/с + tap(RT) каждые 250 мс, без отпусканий стика"},
    {"hold2-p100", 10000, 0, 4, "опрос 100/с, удержание RT + A 3.2 с / пауза 0.5 с, без стика"},
    {"apply2-p100", 10000, 2000, 6, "опрос 100/с, круг 500 команд/с + запись настроек (буфер 4 <-> 8, APPLY) каждые 2 с"},
    {"all2-p100", 10000, 2000, 5, "опрос 100/с, 500 команд/с: круг 2 с + tap, отпускание, удержание RT + A 1.5 с"},
    {"stick15-p100", 10000, 1500, 1, "опрос 100/с, круг стиком 667 команд/с"},
    {"stick1-p100", 10000, 1000, 1, "опрос 100/с, круг стиком 1000 команд/с"},
    {"all-p100", 10000, 1000, 5, "опрос 100/с: круг 2 с + tap, отпускание, удержание RT + A 1.5 с"},
    // Добавлено 2026-11-17 (прошивка 4133): рабочий темп владельца ~500 чтений + ~500 команд в секунду.
    {"stick2-p500", 2000, 2000, 1, "опрос 500/с, круг стиком 500 команд/с"},
    {"all2-p500", 2000, 2000, 5, "опрос 500/с, 500 команд/с: круг 2 с + tap, отпускание, удержание RT + A 1.5 с"},
};

struct RunStep {
    std::string name;
    double pollUs = 0, cmdUs = 0;
    int pattern = 0;
    std::string what;
    double onMs = 0, offMs = 0;  // pattern 7: эпизоды инжекта
};

// p<N>, p<N>-c<M> или p<N>-c<M>-e<ON>-<OFF>: опрос N/с, круг стиком M команд/с; с -e - эпизодами: ON мс инжект (круг),
// затем releaseRightStick() и OFF мс отпущено (как доводка прицела в игре). 2026-11-20.
bool parseFree(const std::string& name, RunStep& out) {
    int poll = 0, cmd = 0, on = 0, off = 0, used = 0;
    if (std::sscanf(name.c_str(), "p%d-c%d-e%d-%d%n", &poll, &cmd, &on, &off, &used) == 4 && used == static_cast<int>(name.size())) {
        if (on < 50 || off < 50 || cmd <= 0) return false;
    } else if (std::sscanf(name.c_str(), "p%d-c%d%n", &poll, &cmd, &used) == 2 && used == static_cast<int>(name.size())) {
    } else if (std::sscanf(name.c_str(), "p%d%n", &poll, &used) == 1 && used == static_cast<int>(name.size())) {
        cmd = 0;
    } else {
        return false;
    }
    if (poll < 100 || poll > 2000 || cmd < 0 || cmd > 1000 || (cmd > 0 && cmd < 10)) return false;
    out.name = name;
    out.pollUs = 1e6 / poll;
    out.cmdUs = cmd > 0 ? 1e6 / cmd : 0.0;
    out.pattern = on > 0 ? 7 : cmd > 0 ? 1 : 0;
    out.onMs = on;
    out.offMs = off;
    char b[128];
    if (on > 0) std::snprintf(b, sizeof(b), "опрос %d/с, эпизоды: %d мс круг %d команд/с, %d мс отпущено", poll, on, cmd, off);
    else if (cmd > 0) std::snprintf(b, sizeof(b), "опрос %d/с, круг стиком %d команд/с", poll, cmd);
    else std::snprintf(b, sizeof(b), "опрос %d/с, без инжекта", poll);
    out.what = b;
    return true;
}

// Состояние для окна pad_probe_view (2026-11-20): %TEMP%\makcu_pad_status.txt = OK / ABSENT / STALL. Во время инжекта
// Windows видит клон платы с инжектом, и зависание настоящего геймпада окну иначе не видно.
void writeSoakStatus(const char* s) {
    char dir[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, dir)) return;
    const std::string path = std::string(dir) + "makcu_pad_status.txt";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputs(s, f);
        std::fclose(f);
    }
}

std::string nowText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string port = "COM5", only, from;
    double stepSec = 120.0, keepGoing = 0.0;
    // --release-on-stall S (2026-11-20, просьба MAKCU снять отчёт Toolkit во время зависания): геймпад ЗАВИС (padStalled)
    // S с подряд -> записать RELEASED в файл состояния и выйти (код 5) БЕЗ close(): плате не уходит ни одной команды,
    // Windows закрывает порт сам. Иначе программа держит COM7 Toolkit и его отчёт без снимка устройства («lease busy»).
    double releaseOnStall = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = argv[++i];
        else if (a == "--step" && i + 1 < argc) stepSec = std::atof(argv[++i]);
        else if (a == "--steps" && i + 1 < argc) only = argv[++i];
        else if (a == "--from" && i + 1 < argc) from = argv[++i];
        else if (a == "--keep-going" && i + 1 < argc) keepGoing = std::atof(argv[++i]);
        else if (a == "--release-on-stall" && i + 1 < argc) releaseOnStall = std::atof(argv[++i]);
        else if (a == "--list") {
            for (const Step& s : kSteps) std::printf("%-11s %s\n", s.name, s.what);
            std::printf("p<N>[-c<M>] свободная ступень: опрос N/с (100..2000), круг стиком M команд/с (до 1000)\n");
            return 0;
        } else {
            std::printf("pad_soak.exe [--port COM5] [--step S] [--steps a,b,c] [--from NAME] [--keep-going S] [--release-on-stall S] [--list]\n");
            return 1;
        }
    }
    // Список ступеней: --steps в заданном порядке (имена таблицы или p<N>[-c<M>]), иначе вся таблица (с --from).
    std::vector<RunStep> plan;
    auto fromTable = [](const Step& s) { return RunStep{s.name, s.pollUs, s.cmdUs, s.pattern, s.what}; };
    if (!only.empty()) {
        size_t pos = 0;
        while (pos <= only.size()) {
            const size_t comma = only.find(',', pos);
            const std::string name = only.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? only.size() + 1 : comma + 1;
            if (name.empty()) continue;
            RunStep rs;
            bool ok = false;
            for (const Step& s : kSteps) {
                if (name == s.name) {
                    rs = fromTable(s);
                    ok = true;
                    break;
                }
            }
            if (!ok) ok = parseFree(name, rs);
            if (!ok) {
                std::printf("[-] неизвестная ступень %s (--list)\n", name.c_str());
                return 1;
            }
            plan.push_back(rs);
        }
    } else {
        bool started = from.empty();
        for (const Step& s : kSteps) {
            if (!started && from != s.name) continue;
            started = true;
            plan.push_back(fromTable(s));
        }
    }

    makcu::GamepadRawTransport tx;
    std::string error;
    if (!tx.open(port, error)) {
        std::printf("[-] %s\n", error.c_str());
        return 1;
    }
    std::printf("[+] %s %s\n", nowText().c_str(), tx.info().c_str());
    if (tx.controllerKind() == 0) {  // 2026-11-20: по радио геймпад мог уснуть до старта - ждём до 60 с, пока его разбудят
        std::printf("[i] плата не видит геймпад на USB3 - жду до 60 с (разбуди геймпад: качни левый стик)\n");
        const int64_t until = Q::now() + Q::fromUs(60e6);
        while (tx.controllerKind() == 0 && Q::now() < until) Sleep(100);
        if (tx.controllerKind() == 0) {
            std::printf("[-] плата не видит геймпад на USB3\n");
            return 1;
        }
        Sleep(1000);
        std::printf("[+] %s геймпад появился: %s\n", nowText().c_str(), tx.info().c_str());
    }
    std::thread xi(xinputLoop);
    Sleep(500);
    std::printf("[i] ступень %.0f с; правый стик, RT и A НЕ ТРОГАТЬ%s\n", stepSec,
        keepGoing > 0 ? "; на отказе лестница не останавливается (--keep-going)" : "");
    std::printf("время    | ступень     |   с | Windows пакетов/с | плата: запросов/с, новых отчётов/с, FF, без ответа, пропущено | команд стика/с, кнопок, отказов 0x40, починок\n");

    bool dropped = false;
    std::string droppedAt;
    double droppedSec = 0;
    int totalDrops = 0;
    struct StepResult {
        std::string name;
        double sec;
        int drops;
        double downMax;
    };
    std::vector<StepResult> results;
    for (const RunStep& st : plan) {
        std::printf("== %s %s: %s ==\n", nowText().c_str(), st.name.c_str(), st.what.c_str());
        int stepDrops = 0;
        double downMax = 0.0, downSince = -1.0;
        tx.setPhysicalPollUs(st.pollUs);
        tx.setCommandPeriodUs(st.cmdUs > 0 ? st.cmdUs : 1000.0);
        Sleep(300);
        tx.resetStats();
        const int64_t t0 = Q::now();
        uint64_t xiPrev = g_xiPackets.load();
        auto prev = tx.stats();
        double nextReport = 10.0, lastAlive = 0.0, nextStatus = 0.0, stalledSince = -1.0;
        uint64_t aliveSamples = prev.newSamples;
        uint64_t c0 = tx.commandCount();
        int phase = -1, taps = 0, applyAsked = 0, applyFailed = 0;
        const uint64_t writes0 = tx.stats().settingsWrites;
        double phaseT0 = 0.0;
        bool stickOn = false;
        for (;;) {
            const double t = Q::toUs(Q::now() - t0) / 1e6;
            if (t >= stepSec && downSince < 0) break;  // в отказе ступень дожидается возврата (или --keep-going)
            // --- нагрузка ---
            // Время по командам стика. СЧИТАТЬ ПОСЛЕ waitCommand() (2026-11-20): раньше cmdT брался в начале итерации, до
            // ожидания, и после пробуждения цель ставилась по старому счётчику - равной только что отправленной. Пока
            // транспорт слал команду каждый период, это давало дубль каждого значения (новых положений вдвое меньше
            // темпа); с повтором неизменной команды раз в 250 мс (ревизия 4) темп команд упал вдвое (200 -> 100/с).
            auto cmdTime = [&] { return static_cast<double>(tx.commandCount() - c0) * tx.commandPeriodUs() / 1e6; };
            auto circle = [&](double tt) {
                const double r = 12000.0 * std::min(1.0, tt / 0.25);
                tx.setRightStick(static_cast<int16_t>(std::lround(r * std::cos(6.2831853 * tt))), static_cast<int16_t>(std::lround(r * std::sin(6.2831853 * tt))));
            };
            switch (st.pattern) {
            case 0:
                Sleep(5);
                break;
            case 7: {  // эпизоды: 0 - круг onMs, 1 - отпущено offMs
                const double pt = t - phaseT0;
                if (phase < 0 || (phase == 1 && pt * 1000.0 >= st.offMs)) {
                    phase = 0;
                    phaseT0 = t;
                    c0 = tx.commandCount();
                    circle(0.0);
                } else if (phase == 0 && pt * 1000.0 >= st.onMs) {
                    phase = 1;
                    phaseT0 = t;
                    tx.releaseRightStick();
                } else if (phase == 0) {
                    tx.waitCommand(50);
                    circle(cmdTime());
                    break;
                }
                Sleep(1);
                break;
            }
            case 1:
            case 2:
            case 6:
                if (!stickOn) {
                    stickOn = true;
                    c0 = tx.commandCount();
                    circle(0.0);
                }
                tx.waitCommand(50);
                circle(cmdTime());
                if (st.pattern == 2 && t >= 0.25 * (taps + 1)) {
                    tx.tap(Id::RightTrigger, 80);
                    ++taps;
                }
                if (st.pattern == 6 && t >= 2.0 * (applyAsked + 1)) {  // интерполяция остаётся Off, меняется только буфер
                    tx.setSmoothingBuffer(applyAsked % 2 == 0 ? 4 : 8);
                    if (!tx.setSmoothing(0, 1000)) ++applyFailed;
                    ++applyAsked;
                }
                break;
            case 3: {  // 0: вход 300 мс, 1: отпущено 400 мс
                const double pt = t - phaseT0;
                if (phase < 0 || (phase == 1 && pt >= 0.4)) {
                    phase = 0;
                    phaseT0 = t;
                    tx.setRightStick(12000, -1);
                } else if (phase == 0 && pt >= 0.3) {
                    phase = 1;
                    phaseT0 = t;
                    tx.releaseRightStick();
                }
                Sleep(1);
                break;
            }
            case 4: {  // 0: держим 3.2 с, 1: пауза 0.5 с
                const double pt = t - phaseT0;
                if (phase < 0 || (phase == 1 && pt >= 0.5)) {
                    phase = 0;
                    phaseT0 = t;
                    tx.setTrigger(Id::RightTrigger, 1023);
                    tx.setButton(Id::South, true);
                } else if (phase == 0 && pt >= 3.2) {
                    phase = 1;
                    phaseT0 = t;
                    tx.setTrigger(Id::RightTrigger, 0);
                    tx.setButton(Id::South, false);
                }
                Sleep(1);
                break;
            }
            case 5: {  // 0: круг 2 с + tap, 1: отпущено 0.3 с, 2: удержание 1.5 с, 3: пауза 0.3 с
                const double pt = t - phaseT0;
                if (phase < 0 || (phase == 3 && pt >= 0.3)) {
                    phase = 0;
                    phaseT0 = t;
                    taps = 0;
                    c0 = tx.commandCount();
                    circle(0.0);
                } else if (phase == 0) {
                    if (pt >= 2.0) {
                        phase = 1;
                        phaseT0 = t;
                        tx.releaseRightStick();
                    } else {
                        tx.waitCommand(50);
                        circle(cmdTime());
                        if (pt >= 0.25 * (taps + 1)) {
                            tx.tap(Id::RightTrigger, 80);
                            ++taps;
                        }
                        break;
                    }
                } else if (phase == 1 && pt >= 0.3) {
                    phase = 2;
                    phaseT0 = t;
                    tx.setTrigger(Id::RightTrigger, 1023);
                    tx.setButton(Id::South, true);
                } else if (phase == 2 && pt >= 1.5) {
                    phase = 3;
                    phaseT0 = t;
                    tx.setTrigger(Id::RightTrigger, 0);
                    tx.setButton(Id::South, false);
                }
                Sleep(1);
                break;
            }
            }
            // --- жив ли геймпад на плате: новые отчёты 0x54 ---
            const auto s = tx.stats();
            if (s.newSamples != aliveSamples) {
                aliveSamples = s.newSamples;
                lastAlive = t;
            }
            if (t >= nextStatus) {
                nextStatus = t + 0.25;
                writeSoakStatus(tx.padStalled() ? "STALL" : tx.padAbsent() ? "ABSENT" : "OK");
            }
            if (releaseOnStall > 0) {
                if (!tx.padStalled()) stalledSince = -1.0;
                else if (stalledSince < 0) stalledSince = t;
                else if (t - stalledSince >= releaseOnStall) {
                    writeSoakStatus("RELEASED");
                    std::printf("[!] %s геймпад ЗАВИС %.0f с подряд (с %.1f с ступени, последний новый отчёт на %.1f с) - отключаюсь от платы без команд (--release-on-stall), код 5\n",
                        nowText().c_str(), t - stalledSince, stalledSince, lastAlive);
                    std::printf("    до зависания: %s\n", tx.statsLine().c_str());
                    std::fflush(stdout);
                    std::_Exit(5);
                }
            }
            const bool silent = t - lastAlive > 1.5 || tx.controllerKind() == 0;
            if (silent && downSince < 0) {
                std::printf("[!] %s ОБРЫВ на ступени %s: последний новый отчёт геймпада на %.1f с ступени; kinds %02X, Windows XInput: %s; транспорт: %s\n", nowText().c_str(),
                    st.name.c_str(), lastAlive, tx.controllerKind(), g_xiOk.load() ? "есть" : "нет",
                    tx.padAbsent() ? "плата сказала - геймпада нет (отказ платы)" : tx.padStalled() ? "геймпад ЗАВИС (снимки идут, sequence стоит)" : "ни отказа, ни зависания ещё не видит");
                ++stepDrops;
                ++totalDrops;
                if (keepGoing <= 0) {
                    dropped = true;
                    droppedAt = st.name;
                    droppedSec = lastAlive;
                    break;
                }
                downSince = lastAlive;
            } else if (!silent && downSince >= 0) {
                const double down = t - downSince;
                downMax = std::max(downMax, down);
                std::printf("[+] %s геймпад вернулся на %.1f с ступени: молчал %.1f с (отсчёт от последнего отчёта); kinds %02X, Windows XInput: %s\n", nowText().c_str(), t, down,
                    tx.controllerKind(), g_xiOk.load() ? "есть" : "нет");
                downSince = -1.0;
            } else if (silent && t - downSince > keepGoing) {
                dropped = true;
                droppedAt = st.name;
                droppedSec = downSince;
                std::printf("[!] %s геймпад НЕ ВЕРНУЛСЯ за %.0f с (молчит с %.1f с ступени) - лестница остановлена\n", nowText().c_str(), keepGoing, downSince);
                break;
            }
            if (t >= nextReport) {
                const uint64_t xiNow = g_xiPackets.load();
                std::printf("%s | %-11s | %3.0f | %17.0f | %6.0f %6.0f %4llu %4llu %4llu | %5.0f %4llu %4llu %4llu\n", nowText().c_str(), st.name.c_str(), t, (xiNow - xiPrev) / 10.0,
                    (s.polls - prev.polls) / 10.0, (s.newSamples - prev.newSamples) / 10.0, static_cast<unsigned long long>(s.rejects - prev.rejects),
                    static_cast<unsigned long long>(s.lost - prev.lost), static_cast<unsigned long long>(s.skipped - prev.skipped), (s.stickCommands - prev.stickCommands) / 10.0,
                    static_cast<unsigned long long>(s.padCommands - prev.padCommands), static_cast<unsigned long long>(s.stickRejects - prev.stickRejects),
                    static_cast<unsigned long long>(s.repairs - prev.repairs));
                xiPrev = xiNow;
                prev = s;
                nextReport += 10.0;
            }
        }
        const double stepT = Q::toUs(Q::now() - t0) / 1e6;
        tx.releaseButtons();
        tx.releaseRightStick(200);
        std::printf("   итог %s: %s\n", st.name.c_str(), tx.statsLine().c_str());
        std::printf("   отказов на ступени %s: %d за %.0f с, дольше всего молчал %.1f с\n", st.name.c_str(), stepDrops, stepT, downMax);
        results.push_back({st.name, stepT, stepDrops, downMax});
        if (st.pattern == 6) {
            const uint64_t writes = tx.stats().settingsWrites - writes0;
            std::printf("   записи настроек: запрошено %d, записано (BEGIN / WRITE / APPLY) %llu, не подтверждено %d\n", applyAsked, static_cast<unsigned long long>(writes), applyFailed);
            if (!dropped) {  // вернуть буфер 8 мс до следующей ступени (close() тоже вернёт исходное)
                tx.setSmoothingBuffer(8);
                tx.setSmoothing(0, 1000);
            }
        }
        if (dropped) break;
    }
    std::printf("== сводка: ступень | с | отказов | дольше всего молчал, с ==\n");
    for (const StepResult& r : results) std::printf("   %-14s | %4.0f | %3d | %.1f\n", r.name.c_str(), r.sec, r.drops, r.downMax);
    if (dropped) std::printf("[!] ИТОГ: геймпад пропал на ступени %s (%.1f с ступени) и не вернулся. Дальше лестница не идёт.\n", droppedAt.c_str(), droppedSec);
    else if (totalDrops > 0) std::printf("[~] ИТОГ %s: все ступени пройдены, отказов %d, все восстановились\n", nowText().c_str(), totalDrops);
    else std::printf("[+] ИТОГ %s: все ступени пройдены, геймпад на месте\n", nowText().c_str());
    g_stop = true;
    xi.join();
    tx.close();
    return dropped ? 3 : totalDrops > 0 ? 2 : 0;
}
