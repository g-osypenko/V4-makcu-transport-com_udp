// pad_probe_view.cpp - наблюдение за геймпадом В ОКНЕ: держится ли он и что показывают стики. ТОЛЬКО ЧТЕНИЕ.
// То же, что pad_probe_hold.cpp, но с окном pad_probe_ui.h: стики двигаются вживую, крупная надпись говорит,
// на месте ли геймпад, сколько прошло и сколько было обрывов. Стартует сразу, геймпадом не управляет.
//
// --rate 0 (по умолчанию): COM не открывается, плате не уходит ни одной команды; в окне живут только стики Windows.
// --rate N: дополнительно GET 0x54 N раз в секунду - в окне живут и стики платы, считаются отчёты геймпада в секунду.
//
// Надписи окна (в чат писать ровно так):
//   зелёная  "Геймпад на месте M:SS. Обрывов: N. Стики можно крутить"
//   оранжевая (--notouch) "НЕ ТРОГАЙ геймпад: правый стик двигает программа. На месте M:SS, обрывов: N"
//            --notouch ставят, когда рядом запущен example_pad.exe или pad_bench_com.exe (у них окна нет): окно
//            с --rate 0 COM не занимает и показывает в круге "Windows, правый" то, что они инжектят.
//   красная  "ГЕЙМПАД ПРОПАЛ в ЧЧ:ММ:СС. Обрывов: N. Жду, когда вернётся"
//   красная  "ГЕЙМПАД МОЛЧИТ с ЧЧ:ММ:СС: подключён, но отчётов нет. Переподключи кабель геймпада"  (пакетов нет 2 с)
//   --notouch: при отказе "ГЕЙМПАД МОЛЧИТ с ЧЧ:ММ:СС. Ничего не трогай: жди сообщения в чате" и
//              "ГЕЙМПАД ПРОПАЛ в ЧЧ:ММ:СС. Ничего не трогай: жди сообщения в чате" (сначала снимок платы, потом руки)
//   красная  "Геймпада в Windows нет. Подключи геймпад"           (с самого старта не появлялся)
//   --watch (рядом pad_drop_watch.ps1): зелёная "Крути ПРАВЫЙ стик кругами без перерыва. Донгл и провода НЕ трогай. На месте M:SS, обрывов: N";
//            после отказа 40 с красная "ГЕЙМПАД ПРОПАЛ|МОЛЧИТ в ЧЧ:ММ:СС. НИЧЕГО НЕ ТРОГАЙ ещё N с: опрашиваю плату",
//            затем "ГЕЙМПАД ПРОПАЛ|МОЛЧИТ в ЧЧ:ММ:СС. Плата опрошена. Ничего не перевтыкай, жди сообщения в чате"
//   чёрная   "ГОТОВО. Окно закроется само"
// Под надписью: "осталось N с". Нижние строки: состояние платы и Windows, пакетов/с, время последнего события.
// Esc или крестик - выход раньше срока.
//
// Журнал (--log FILE, дописывается): строка на каждое событие и раз в 10 с.
// Код выхода: 0 - геймпад был на месте весь прогон, 3 - пропадал, 4 - так и не появился, 1 - порт не открылся.
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_view.cpp -lwinmm -lgdi32 -static -o pad_probe_view.exe
// Запуск: pad_probe_view.exe [--port COM5] [--rate 0|N] [--dur 600] [--log FILE]
#include "pad_probe_ui.h"

using namespace padprobe;

namespace {

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
    std::snprintf(s, sizeof(s), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return s;
}

std::atomic<uint64_t> g_polls{0}, g_ok{0}, g_ff{0}, g_lost{0}, g_seqChanges{0};

// Опрос 0x54 с заданным темпом: значения стиков платы для окна и счёт отчётов геймпада.
void boardLoop(Port* p, double rate) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const int64_t period = us(1e6 / rate);
    int64_t due = now();
    bool have = false;
    uint32_t lastSeq = 0;
    while (!g_stop) {
        while (now() < due) {
            if (due - now() > us(1500.0)) Sleep(1);
            else YieldProcessor();
        }
        due = std::max(due + period, now() - period);
        g_polls.fetch_add(1, std::memory_order_relaxed);
        auto r = p->ask(kOpCtlPhysical, nullptr, 0, 20000.0);
        if (!r) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            g_boardOk = false;
            continue;
        }
        if (r->size() != 32) {
            g_ff.fetch_add(1, std::memory_order_relaxed);
            g_boardOk = false;
            continue;
        }
        g_ok.fetch_add(1, std::memory_order_relaxed);
        const PadPhysical v = parsePhysical(r->data());
        g_boardOk = true;
        g_bRx = v.s.rx;
        g_bRy = v.s.ry;
        g_bLx = v.s.lx;
        g_bLy = v.s.ly;
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
    std::string port = "COM5", logPath;
    double dur = 600.0, rate = 0.0;
    bool noTouch = false;  // рядом идёт программа, которая инжектит стик (example_pad, pad_bench_com): надпись "НЕ ТРОГАЙ"
    // --keepalive: то же, но геймпад по радио может уснуть без касаний (инжект он не чувствует), поэтому владелец раз в
    // 20 с качает ЛЕВЫЙ стик (ступени pad_soak его не используют). 2026-11-16.
    bool keepAlive = false;
    bool press = false;    // --press: проверка кнопок рукой (рядом example_pad --buttons): надпись "Нажимай ЗАДНИЕ кнопки..."
    bool watch = false;    // --watch: рядом pad_drop_watch.ps1 ловит отказ и опрашивает плату по COM ~40 с после него
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--rate" && more) rate = std::atof(argv[++i]);
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--log" && more) logPath = argv[++i];
        else if (a == "--notouch") noTouch = true;
        else if (a == "--keepalive") noTouch = keepAlive = true;
        else if (a == "--press") press = true;
        else if (a == "--watch") watch = true;
        else {
            std::printf("pad_probe_view.exe [--port COM5] [--rate 0|N] [--dur 600] [--log FILE] [--notouch | --keepalive | --press | --watch]\n");
            return 2;
        }
    }
    const bool useBoard = rate > 0.0;
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
    g_boardUsed = useBoard;
    timeBeginPeriod(1);
    g_t0 = now();
    if (useBoard) out("[i] %s старт: окно, опрос 0x54 %.0f/с, %.0f с\n", clockStr().c_str(), rate, dur);
    else out("[i] %s старт: окно, COM закрыт, плате не уходит ни одной команды, %.0f с\n", clockStr().c_str(), dur);
    openWindow(L"MAKCU: наблюдение за геймпадом (только чтение)");
    std::thread xi(xinputLoop);
    std::thread board;
    if (useBoard) board = std::thread(boardLoop, &p, rate);

    static wchar_t hint[2][160];
    int which = 0;
    const int64_t t0 = now(), endT = t0 + us(dur * 1e6);
    int64_t nextUi = t0, nextRep = t0 + us(10e6), upSince = 0;
    int lastXi = -1, lastBoard = -1, drops = 0;
    bool everXi = false;
    uint64_t pPk = 0, pSeq = 0, uiPk = 0, uiSeq = 0;
    int64_t uiT = t0;
    double pkRate = 0.0, seqRate = 0.0;
    // Надписи окна - широкие строки: %hs в swprintf читает узкую строку в кодировке ANSI, а исходник в UTF-8
    // (2026-11-14 "последнее событие" печаталось кракозябрами).
    auto wclock = [] {
        const std::string s = clockStr();
        return std::wstring(s.begin(), s.end());  // только цифры и двоеточия
    };
    std::wstring lastEvent = L"нет", lostAt, silentAt;
    int64_t lostT = t0, silentT = t0;  // когда пропал / замолчал (для отсчёта --watch)
    // "Молчит": Windows считает геймпад подключённым, но номер пакета XInput не растёт дольше 2 с. У GameSir номер
    // растёт ~1000 раз/с и в покое, поэтому тишина = геймпад завис (так было 2026-11-14 22:13 при инжекте с Off:
    // плата отвечала kinds 82 и отдавала застывший снимок 0x54, стики не двигались, помогло переподключение кабеля).
    uint64_t silPk = 0;
    int64_t silT = t0;
    bool silent = false;
    int silences = 0;
    while (!g_stop && now() < endT) {
        pumpMessages();
        const int64_t t = now();
        const double sec = toMs(t - t0) / 1000.0;
        const int xiOk = g_xiOk.load() ? 1 : 0;
        if (xiOk != lastXi && (xiOk || sec > 0.7)) {  // первые 0.7 с поток XInput ещё ищет слот: это не событие
            if (xiOk) {
                out("[+] %s (%.1f с) Windows: геймпад появился\n", clockStr().c_str(), sec);
                lastEvent = L"появился в " + wclock();
                everXi = true;
                upSince = t;
                silT = t;
            } else {
                out("[!] %s (%.1f с) Windows: геймпада в XInput НЕТ\n", clockStr().c_str(), sec);
                if (everXi) {
                    ++drops;
                    lostAt = wclock();
                    lostT = t;
                    lastEvent = L"ПРОПАЛ в " + lostAt;
                }
                silent = false;
            }
            lastXi = xiOk;
        }
        {
            const uint64_t pk = g_xiPackets.load();
            if (pk != silPk) {
                silPk = pk;
                silT = t;
                if (silent) {
                    silent = false;
                    upSince = t;
                    out("[+] %s (%.1f с) Windows: пакеты снова идут\n", clockStr().c_str(), sec);
                    lastEvent = L"ожил в " + wclock();
                }
            } else if (xiOk == 1 && !silent && toMs(t - silT) > 2000.0) {
                silent = true;
                ++silences;
                silentAt = wclock();
                silentT = t;
                out("[!] %s (%.1f с) Windows: геймпад подключён, но МОЛЧИТ (пакетов нет 2 с)\n", clockStr().c_str(), sec);
                lastEvent = L"ЗАМОЛЧАЛ в " + silentAt;
            }
        }
        if (useBoard) {
            const int b = g_boardOk.load() ? 1 : 0;
            if (b != lastBoard && sec > 0.7) {
                out("[%c] %s (%.1f с) плата: 0x54 %s\n", b ? '+' : '!', clockStr().c_str(), sec, b ? "отдаёт снимки" : "не отдаёт снимки (отказ FF или молчит)");
                lastBoard = b;
            }
        }
        if (t >= nextUi) {
            nextUi = t + us(250e3);
            if (toMs(t - uiT) >= 1000.0) {
                const uint64_t pk = g_xiPackets.load(), sq = g_seqChanges.load();
                const double span = toMs(t - uiT) / 1000.0;
                pkRate = (pk - uiPk) / span;
                seqRate = (sq - uiSeq) / span;
                uiPk = pk;
                uiSeq = sq;
                uiT = t;
            }
            which ^= 1;
            COLORREF color = RGB(200, 0, 0);
            // --watch: после отказа 40 с "НИЧЕГО НЕ ТРОГАЙ" с отсчётом (pad_drop_watch.ps1 в это время делает три снимка платы:
            // сразу, через 10 и через 30 с, каждый ~2.5 с), потом "плата опрошена, жди сообщения в чате".
            const bool badNow = (xiOk == 1 && silent) || (xiOk != 1 && everXi);
            if (watch && badNow) {
                const bool sil = xiOk == 1 && silent;
                const int left = 40 - static_cast<int>(toMs(t - (sil ? silentT : lostT)) / 1000.0);
                if (left > 0)
                    swprintf(hint[which], 160, L"ГЕЙМПАД %ls в %ls. НИЧЕГО НЕ ТРОГАЙ ещё %d с: опрашиваю плату", sil ? L"МОЛЧИТ" : L"ПРОПАЛ",
                             (sil ? silentAt : lostAt).c_str(), left);
                else
                    swprintf(hint[which], 160, L"ГЕЙМПАД %ls в %ls. Плата опрошена. Ничего не перевтыкай, жди сообщения в чате",
                             sil ? L"МОЛЧИТ" : L"ПРОПАЛ", (sil ? silentAt : lostAt).c_str());
            } else if (xiOk == 1 && silent) {
                if (noTouch) swprintf(hint[which], 160, L"ГЕЙМПАД МОЛЧИТ с %ls. Ничего не трогай: жди сообщения в чате", silentAt.c_str());
                else swprintf(hint[which], 160, L"ГЕЙМПАД МОЛЧИТ с %ls: подключён, но отчётов нет. Переподключи кабель геймпада", silentAt.c_str());
            } else if (xiOk == 1) {
                const int up = static_cast<int>(toMs(t - upSince) / 1000.0);
                if (watch) swprintf(hint[which], 160, L"Крути ПРАВЫЙ стик кругами без перерыва. Донгл и провода НЕ трогай. На месте %d:%02d, обрывов: %d", up / 60, up % 60, drops);
                else if (press) swprintf(hint[which], 160, L"Нажимай ЗАДНИЕ кнопки геймпада по очереди, каждую по 3 раза. Потом A и правый триггер");
                else if (keepAlive) swprintf(hint[which], 160, L"Правый стик, RT и A НЕ трогай: их двигает программа. Раз в 20 с качни ЛЕВЫЙ стик. На месте %d:%02d, обрывов: %d", up / 60, up % 60, drops);
                else if (noTouch) swprintf(hint[which], 160, L"НЕ ТРОГАЙ геймпад: стик и кнопки двигает программа. На месте %d:%02d, обрывов: %d", up / 60, up % 60, drops);
                else swprintf(hint[which], 160, L"Геймпад на месте %d:%02d. Обрывов: %d. Стики можно крутить", up / 60, up % 60, drops);
                color = noTouch ? RGB(200, 90, 0) : RGB(0, 130, 0);
            } else if (everXi) {
                if (noTouch) swprintf(hint[which], 160, L"ГЕЙМПАД ПРОПАЛ в %ls. Ничего не трогай: жди сообщения в чате", lostAt.c_str());
                else swprintf(hint[which], 160, L"ГЕЙМПАД ПРОПАЛ в %ls. Обрывов: %d. Жду, когда вернётся", lostAt.c_str(), drops);
            } else {
                swprintf(hint[which], 160, L"Геймпада в Windows нет. Подключи геймпад");
            }
            say(hint[which], color);
            g_countEnd = endT;
            // Кнопки и триггеры, которые видит Windows: нажатые сейчас или за последние 250 мс, и счётчик нажатий.
            wchar_t btn[128];
            {
                static const wchar_t* const kNames[16] = {L"ВВЕРХ", L"ВНИЗ", L"ВЛЕВО", L"ВПРАВО", L"START", L"BACK", L"L3", L"R3", L"LB", L"RB", L"", L"", L"A", L"B", L"X", L"Y"};
                const int seen = g_xButtonsSeen.exchange(0) | g_xButtons.load();
                const int lt = std::max(g_xLtMax.exchange(0), g_xLt.load()), rt = std::max(g_xRtMax.exchange(0), g_xRt.load());
                std::wstring names;
                for (int b = 0; b < 16; ++b) {
                    if ((seen >> b) & 1) names += std::wstring(kNames[b]) + L" ";
                }
                if (lt) names += L"LT " + std::to_wstring(lt) + L" ";
                if (rt) names += L"RT " + std::to_wstring(rt) + L" ";
                swprintf(btn, 128, L"кнопки в Windows: %ls(нажатий %d)", names.empty() ? L"нет " : names.c_str(), g_xPresses.load());
            }
            if (useBoard)
                swprintf(g_status, 256, L"Windows: %.0f пакетов/с   |   плата: %.0f отчётов/с, FF %llu, без ответа %llu   |   %ls   |   событие: %ls", pkRate, seqRate,
                    static_cast<unsigned long long>(g_ff.load()), static_cast<unsigned long long>(g_lost.load()), btn, lastEvent.c_str());
            else swprintf(g_status, 256, L"Windows: %.0f пакетов/с   |   %ls   |   событие: %ls", pkRate, btn, lastEvent.c_str());
        }
        if (t >= nextRep) {
            nextRep += us(10e6);
            const uint64_t pk = g_xiPackets.load(), sq = g_seqChanges.load();
            out("%s | %4.0f с | Windows %s, %5.0f пакетов/с | плата: отчётов/с %5.0f, FF %llu, молчит %llu | обрывов %d\n", clockStr().c_str(), sec,
                xiOk == 1 ? "подключён" : "НЕТ", (pk - pPk) / 10.0, (sq - pSeq) / 10.0, static_cast<unsigned long long>(g_ff.load()),
                static_cast<unsigned long long>(g_lost.load()), drops);
            pPk = pk;
            pSeq = sq;
        }
        Sleep(5);
    }
    const bool aborted = g_stop.load();
    const double total = toMs(now() - t0) / 1000.0;
    if (!aborted) {
        say(L"ГОТОВО. Окно закроется само");
        waitUi(1.5);
    }
    g_stop = true;
    xi.join();
    if (board.joinable()) board.join();
    out("итог %s: %.0f с%s, обрывов %d, замолкал %d раз; геймпад %s\n", clockStr().c_str(), total, aborted ? " (остановлено Esc)" : "", drops, silences,
        drops ? "ПРОПАДАЛ" : silences ? "МОЛЧАЛ (подключён, но без отчётов)" : everXi ? "был на месте весь прогон" : "так и не появился в Windows");
    timeEndPeriod(1);
    if (useBoard) p.close();
    if (g_log) std::fclose(g_log);
    return drops || silences ? 3 : everXi ? 0 : 4;
}
