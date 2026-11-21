// Стенд нагрузки для платы на прошивке medius (сторонняя прошивка, НЕ MAKCU).
//
// Ищет то же, что `track_test_com --stress` искал на MAKCU: пропадает ли мышь с платы под рабочим темпом.
// Окно, клик для старта, ClipCursor и Raw Input - из basics/track_bench.h (общий код стендов MAKCU не меняется:
// ниже маленькая прослойка с именами, которые он ждёт от заголовка транспорта).
//
//   medius_stress.exe [--port COM5] --stress S [--load inject,query,catch,busy | idle] [--now] [--hand] [--slog FILE]
//     inject  move_rel по кругу каждую 1 мс (обычный MOVE: через рендерер платы, как в работе)
//     query   QUERY(HEALTH) 1000/с отдельным потоком (как опрос позиции 1000/с у программы владельца)
//     catch   поток движения физической мыши (CATCH, все оси) - чтение руки без опроса
//     busy    клик левой каждые 2 с (удержание 40 мс)
//     --now   инжект через move_rel_now (мимо рендерера и riding)
//     --hand  надпись окна "водите мышью"; иначе "не трогайте"
//
// Каждую секунду: темп move и Raw Input, наибольшая пауза Raw Input, видит ли плата мышь, счётчики потерь платы
// (STATS) с начала прогона. Мышь пропала с платы (health.mouse_attached = 0) или Raw Input молчит при инжекте -
// нагрузка снимается, стенд ждёт до 60 с, вернётся ли мышь сама, и выходит с кодом 3.
//
// Сборка (MinGW + DLL библиотеки medius, собранной cargo для msvc; C ABI одинаковый):
//   cd medius && cargo build --release -p medius-capi
//   copy medius\target\release\medius_capi.dll basics\medius_way
//   g++ -std=c++17 -O2 -Wall -Wextra -I ..\..\medius\medius-capi\include medius_stress.cpp medius_capi.dll
//       -lwinmm -lgdi32 -static-libgcc -static-libstdc++ -o medius_stress.exe
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef interface
#undef interface  // windows.h (combaseapi) делает interface макросом, а в medius.h это имя поля
#endif
#include "medius.h"

// ---- прослойка для track_bench.h ---------------------------------------------------------
// track_bench.h написан под заголовки транспорта MAKCU и берёт оттуда часы QPC и имена кнопок и блокировок
// (в шаблонах режимов MAKCU, которые здесь не используются). Это только имена, к прошивке MAKCU отношения не имеют.
namespace makcu {
namespace detail {
struct Qpc {
    static int64_t freq() {
        static const int64_t f = [] {
            LARGE_INTEGER v;
            QueryPerformanceFrequency(&v);
            return static_cast<int64_t>(v.QuadPart);
        }();
        return f;
    }
    static int64_t now() {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }
    static int64_t fromUs(double us) { return static_cast<int64_t>(std::llround(us * static_cast<double>(freq()) / 1e6)); }
    static double toUs(int64_t ticks) { return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq()); }
};
}  // namespace detail
enum class LockTarget { X, Y, XPos, XNeg, YPos, YNeg };
enum class MouseButton { Left };
inline uint16_t lockBit(LockTarget t) { return static_cast<uint16_t>(1u << static_cast<int>(t)); }
}  // namespace makcu

#include "../track_bench.h"

namespace {

using namespace bench;

std::string g_port;        // пусто - первая плата medius
bool g_now = false;        // --now: move_rel_now
double g_stressS = 0.0;    // --stress S

const char* st(MediusStatus s) {
    switch (s) {
    case MEDIUS_STATUS_OK: return "OK";
    case MEDIUS_STATUS_ERR_IO: return "ERR_IO";
    case MEDIUS_STATUS_ERR_NOT_FOUND: return "ERR_NOT_FOUND";
    case MEDIUS_STATUS_ERR_NO_REPLY: return "ERR_NO_REPLY";
    case MEDIUS_STATUS_ERR_BAD_PROTO_VER: return "ERR_BAD_PROTO_VER";
    case MEDIUS_STATUS_ERR_QUERY_TIMEOUT: return "ERR_QUERY_TIMEOUT";
    case MEDIUS_STATUS_ERR_DISCONNECTED: return "ERR_DISCONNECTED";
    case MEDIUS_STATUS_ERR_PORT_BUSY: return "ERR_PORT_BUSY";
    default: return "ERR_?";
    }
}

// Монитор: HEALTH раз в 100 мс (или 1000/с при нагрузке query), STATS раз в секунду - отдельный поток, чтобы
// зависший запрос не останавливал такт инжекта.
struct Monitor {
    MediusDevice* dev = nullptr;
    bool fast = false;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> healthAsk{0}, healthOk{0}, healthFail{0};
    std::atomic<int> mouse{-1};       // -1 неизвестно, 0 нет, 1 есть
    std::atomic<int> link{-1};
    std::atomic<int64_t> mouseLostAt{0}, mouseBackAt{0};
    std::atomic<uint32_t> maxQueryUs{0};
    std::mutex m;
    MediusStats stats{};
    bool statsOk = false;
    std::string lastErr;
    std::thread th;

    void run() {
        int64_t nextStats = 0;
        int64_t due = Q::now();
        while (!stop.load()) {
            const int64_t now = Q::now();
            if (now >= nextStats) {
                nextStats = now + Q::fromUs(1e6);
                MediusStats s{};
                const MediusStatus r = medius_device_query_stats(dev, &s);
                std::lock_guard<std::mutex> g(m);
                statsOk = r == MEDIUS_STATUS_OK;
                if (statsOk) stats = s;
                else lastErr = st(r);
            }
            MediusHealth h{};
            const int64_t a = Q::now();
            ++healthAsk;
            const MediusStatus r = medius_device_query_health(dev, &h);
            const uint32_t us = static_cast<uint32_t>(Q::toUs(Q::now() - a));
            if (us > maxQueryUs.load()) maxQueryUs = us;
            if (r == MEDIUS_STATUS_OK) {
                ++healthOk;
                const int was = mouse.exchange(h.mouse_attached ? 1 : 0);
                link = h.link_up ? 1 : 0;
                if (was == 1 && !h.mouse_attached) mouseLostAt = Q::now();
                if (was == 0 && h.mouse_attached) mouseBackAt = Q::now();
            } else {
                ++healthFail;
                std::lock_guard<std::mutex> g(m);
                lastErr = st(r);
            }
            due += Q::fromUs(fast ? 1000.0 : 100000.0);
            const int64_t n2 = Q::now();
            if (due < n2) due = n2;  // отстали (долгий ответ) - не догонять пачкой
            while (Q::now() < due && !stop.load()) {
                if (Q::toUs(due - Q::now()) > 2000.0) Sleep(1);
                else YieldProcessor();
            }
        }
    }
};

// Поток физического движения (CATCH, все оси).
struct Catch {
    MediusEventStream* s = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> events{0};
    std::atomic<int64_t> sumX{0}, sumY{0};
    std::thread th;
    void run() {
        MediusCatchEvent e{};
        while (!stop.load()) {
            if (!medius_event_stream_recv_timeout(s, 100, &e)) continue;
            if (e.kind != MEDIUS_CATCH_EVENT_KIND_MOTION) continue;
            ++events;
            sumX += e.data.motion.dx;
            sumY += e.data.motion.dy;
        }
    }
};

int runMediusStress(double seconds) {
    const bool inj = stressHas("inject"), query = stressHas("query"), cat = stressHas("catch"), busy = stressHas("busy");
    MediusDevice* dev = nullptr;
    const MediusStatus os = g_port.empty() ? medius_device_find(&dev) : medius_device_open(g_port.c_str(), &dev);
    if (os != MEDIUS_STATUS_OK || !dev) {
        std::printf("[-] плата medius не открылась: %s (дашборд в браузере держит порт? Disconnect)\n", st(os));
        return 1;
    }
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char path[96];
    if (g_stressLog.empty()) {
        std::snprintf(path, sizeof(path), "medius_stress_%04u%02u%02u_%02u%02u%02u.log", lt.wYear, lt.wMonth, lt.wDay,
            lt.wHour, lt.wMinute, lt.wSecond);
        g_stressLog = path;
    }
    FILE* lf = std::fopen(g_stressLog.c_str(), "a");
    auto out = [&](bool console, const char* fmt, auto... args) {
        char line[768];
        std::snprintf(line, sizeof(line), fmt, args...);
        if (console) {
            std::fputs(line, stdout);
            std::fflush(stdout);
        }
        if (lf) {
            std::fputs(line, lf);
            std::fflush(lf);
        }
    };
    auto clock = [] {
        SYSTEMTIME t;
        GetLocalTime(&t);
        static char s[16];
        std::snprintf(s, sizeof(s), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
        return std::string(s);
    };

    // Что за плата и настройки - в журнал.
    MediusVersion v{};
    MediusRenderStatus rs{};
    MediusSpreadStatus sp{};
    MediusEmitPaceStatus ep{};
    MediusHealth h0{};
    medius_device_query_version(dev, &v);
    medius_device_query_render(dev, &rs);
    medius_device_query_spread(dev, &sp);
    medius_device_query_emit_pace(dev, &ep);
    medius_device_query_health(dev, &h0);
    out(true, "[+] medius fw %u.%u.%u proto %u, %s, библиотека %s\n", v.fw_major, v.fw_minor, v.fw_patch, v.proto_ver, v.name,
        medius_version_string());
    out(true, "[+] render mode %d full %u ready %u | spread %u %% | emit mode %d %u Гц | мышь на плате %s, связь чипов %s\n",
        static_cast<int>(rs.mode), rs.full, rs.ready, sp.percent, static_cast<int>(ep.mode), ep.resolved_hz,
        h0.mouse_attached ? "да" : "НЕТ", h0.link_up ? "да" : "НЕТ");
    out(true, "[+] --stress %.0f с, старт %s, нагрузка: %s%s%s%s%s, рука: %s, журнал %s\n", seconds, clock().c_str(),
        inj ? (g_now ? "инжект-1кГц(move_rel_now) " : "инжект-1кГц(move_rel) ") : "", query ? "HEALTH-1000/с " : "",
        cat ? "поток-физики " : "", busy ? "клики " : "", (inj || query || cat || busy) ? "" : "простой",
        g_hand ? "водит" : "не трогать", g_stressLog.c_str());

    Monitor mon;
    mon.dev = medius_device_clone(dev);
    mon.fast = query;
    Catch ca;
    if (cat) {
        MediusCatchFilter f = medius_catch_filter_watch_axes();
        const MediusStatus cs = medius_device_catch_events(dev, &f, 1, &ca.s);
        if (cs != MEDIUS_STATUS_OK) out(true, "[-] CATCH не включился: %s\n", st(cs));
    }
    MediusStats s0{};
    if (medius_device_query_stats(dev, &s0) != MEDIUS_STATUS_OK) out(true, "[-] STATS в начале не прочитан\n");
    MediusCountersSnapshot c0{};
    medius_device_counters(dev, &c0);
    mon.th = std::thread([&] { mon.run(); });
    if (ca.s) ca.th = std::thread([&] { ca.run(); });

    static const wchar_t* const kHintInj = L"прогон на стабильность: курсор ходит кругами; Esc - досрочно";
    static const wchar_t* const kHintIdle = L"прогон на стабильность; Esc - досрочно";
    say(g_hand ? L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ" : L"НЕ ТРОГАЙТЕ МЫШЬ", inj ? kHintInj : kHintIdle);
    sleepMs(1500.0);
    out(true, "время    |    с | move/с | Raw/с | max пауза Raw, мс | такт/move макс., мс | мышь | HEALTH отв./с (макс. мкс) | CATCH соб./с | "
              "потери tx/link/host/relay | склеено | сбросы USB/конф. | рестарты host | session | окно в фокусе\n");

    const MediusUsage left = medius_usage_button(MEDIUS_BUTTON_LEFT);
    const uint64_t raw0 = g_rawTotal.load();
    const int64_t sx0 = g_rawSumX.load(), sy0 = g_rawSumY.load();
    const int64_t t0 = Q::now(), sec = Q::fromUs(1e6);
    int64_t due = t0, nextRep = t0 + sec, lostAt = 0;
    int64_t sentX = 0, sentY = 0;
    int64_t lastTick = 0, maxTick = 0, maxMove = 0;  // наибольший промежуток между тактами и самый долгий move за секунду
    uint64_t moves = 0, movesPrev = 0, moveFail = 0, rawPrev = raw0, hOkPrev = 0, catPrev = 0;
    double phase = 0.0, fx = 0.0, fy = 0.0;
    int rep = 0, gaps15 = 0, rawSilentRun = 0;
    bool back = false, held = false;
    std::string gapList;
    const char* lostWhy = "";
    g_rawMaxGap.store(0);
    for (uint64_t tick = 0; !g_stop.load(); ++tick) {
        spinUntil(due);
        due += Q::fromUs(1000.0);
        const int64_t now = Q::now();
        if (lastTick && now - lastTick > maxTick) maxTick = now - lastTick;
        lastTick = now;
        if (!lostAt && now - t0 >= Q::fromUs(seconds * 1e6)) break;
        if (busy && !lostAt) {
            if (tick % 2000 == 1000) {
                medius_device_press(dev, left);
                held = true;
            }
            if (tick % 2000 == 1040) {
                medius_device_soft_release(dev, left);
                held = false;
            }
        }
        if (inj && !lostAt) {
            const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
            fx += -r * w * std::sin(phase);
            fy += r * w * std::cos(phase);
            phase += w;
            const int64_t ix = std::llround(fx), iy = std::llround(fy);
            fx -= static_cast<double>(ix);
            fy -= static_cast<double>(iy);
            if (ix || iy) {
                const int64_t mA = Q::now();
                const MediusStatus ms = g_now ? medius_device_move_rel_now(dev, static_cast<int16_t>(ix), static_cast<int16_t>(iy))
                                              : medius_device_move_rel(dev, static_cast<int16_t>(ix), static_cast<int16_t>(iy));
                if (Q::now() - mA > maxMove) maxMove = Q::now() - mA;
                if (ms == MEDIUS_STATUS_OK) {
                    ++moves;
                    sentX += ix;
                    sentY += iy;
                } else {
                    ++moveFail;
                }
            }
        }
        if (now < nextRep) continue;
        nextRep += sec;
        ++rep;
        const uint64_t raw = g_rawTotal.load();
        const double gapMs = Q::toUs(g_rawMaxGap.exchange(0)) / 1000.0;
        const double el = Q::toUs(now - t0) / 1e6;
        const uint64_t mv = moves - movesPrev, rw = raw - rawPrev;
        const uint64_t hOk = mon.healthOk.load(), ce = ca.events.load();
        MediusStats s{};
        bool sOk;
        {
            std::lock_guard<std::mutex> g(mon.m);
            s = mon.stats;
            sOk = mon.statsOk;
        }
        const int mouse = mon.mouse.load();
        if (gapMs >= 15.0 && rw > 0 && !lostAt && rep > 1) {  // в 1-й секунде пауза - 1.5 с перед стартом
            ++gaps15;
            char b[48];
            std::snprintf(b, sizeof(b), "%s%.0fс:%.1fмс", gapList.empty() ? "" : " ", el, gapMs);
            if (gapList.size() < 600) gapList += b;
        }
        out(rep % 10 == 0 || lostAt || gapMs >= 15.0 || mouse != 1,
            "%s | %4.0f | %6llu | %5llu | %17.1f | %8.1f/%-10.1f | %4s | %8llu (%6u) | %12llu | %5u/%u/%u/%u | %7u | %u/%u | %13u | %7u | %s\n",
            clock().c_str(), el, static_cast<unsigned long long>(mv), static_cast<unsigned long long>(rw), gapMs,
            Q::toUs(maxTick) / 1000.0, Q::toUs(maxMove) / 1000.0,
            mouse == 1 ? "да" : mouse == 0 ? "НЕТ" : "?", static_cast<unsigned long long>(hOk - hOkPrev), mon.maxQueryUs.exchange(0),
            static_cast<unsigned long long>(ce - catPrev), sOk ? s.tx_drops - s0.tx_drops : 0u,
            sOk ? s.link_rx_drops - s0.link_rx_drops : 0u, sOk ? s.host_rx_drops - s0.host_rx_drops : 0u,
            sOk ? s.relay_drops - s0.relay_drops : 0u, sOk ? static_cast<unsigned>(s.tx_merges - s0.tx_merges) : 0u,
            sOk ? static_cast<unsigned>(s.reset_count - s0.reset_count) : 0u, sOk ? static_cast<unsigned>(s.config_count - s0.config_count) : 0u,
            sOk ? static_cast<unsigned>(s.host_restarts - s0.host_restarts) : 0u, sOk ? static_cast<unsigned>(s.session - s0.session) : 0u,
            foreground() ? "да" : "НЕТ (Raw Input прорежен)");
        movesPrev = moves;
        maxTick = maxMove = 0;
        rawPrev = raw;
        hOkPrev = hOk;
        catPrev = ce;
        // Инжект шёл всю секунду, а Raw Input молчит: мышь в ПК умерла.
        rawSilentRun = (inj && !lostAt && mv > 500 && rw == 0) ? rawSilentRun + 1 : 0;
        if (!lostAt && (mouse == 0 || rawSilentRun >= 2)) {
            lostAt = now;
            lostWhy = mouse == 0 ? "плата не видит мышь (health.mouse_attached = 0)" : "Raw Input молчит 2 с при инжекте";
            if (held) medius_device_soft_release(dev, left);
            out(true, "[!] МЫШЬ ПРОПАЛА %s, на %.1f с прогона: %s. Нагрузка снята, жду до 60 с, вернётся ли сама\n",
                clock().c_str(), el, lostWhy);
        } else if (lostAt && mouse == 1 && rw > 0) {
            out(true, "[+] мышь вернулась сама через %.1f с\n", Q::toUs(now - lostAt) / 1e6);
            back = true;
            break;
        } else if (lostAt && now - lostAt > Q::fromUs(60e6)) {
            out(true, "[-] за 60 с мышь не вернулась (мышь на плате: %s)\n", mouse == 1 ? "да" : mouse == 0 ? "нет" : "?");
            break;
        }
    }
    if (held) medius_device_soft_release(dev, left);
    sleepMs(300.0);  // хвост инжекта (рендерер, spread) доходит до Windows
    const double total = Q::toUs(Q::now() - t0) / 1e6;
    const int64_t wx = g_rawSumX.load() - sx0, wy = g_rawSumY.load() - sy0;
    mon.stop = true;
    if (mon.th.joinable()) mon.th.join();
    ca.stop = true;
    if (ca.th.joinable()) ca.th.join();
    MediusStats s1{};
    const bool s1ok = medius_device_query_stats(dev, &s1) == MEDIUS_STATUS_OK;
    MediusCountersSnapshot c1{};
    medius_device_counters(dev, &c1);
    out(true, "итог %s: %.0f с, %s, move %llu (ошибок %llu), отправлено %+lld,%+lld, Windows %+lld,%+lld%s, Raw Input %llu\n",
        clock().c_str(), total, lostAt ? (back ? "МЫШЬ ПРОПАДАЛА, вернулась сама" : "МЫШЬ ПРОПАЛА, не вернулась") : "мышь не пропадала",
        static_cast<unsigned long long>(moves), static_cast<unsigned long long>(moveFail), static_cast<long long>(sentX),
        static_cast<long long>(sentY), static_cast<long long>(wx), static_cast<long long>(wy),
        g_hand ? " (рука водила: путь не сравнивается)" : (wx == sentX && wy == sentY ? " (совпал)" : " <-- путь не совпал"),
        static_cast<unsigned long long>(g_rawTotal.load() - raw0));
    out(true, "паузы Raw Input >= 15 мс: %d%s%s\n", gaps15, gapList.empty() ? "" : " - ", gapList.c_str());
    if (s1ok) {
        out(true, "STATS за прогон: inject_emits %u, tx_drops %u, tx_merges %u, tx_maxdepth %u, tx_wedges %u, link_rx_drops %u, "
                  "host_rx_drops %u, relay_drops %u, reset_count %u, config_count %u, host_restarts %u, session %u\n",
            s1.inject_emits - s0.inject_emits, static_cast<unsigned>(s1.tx_drops - s0.tx_drops), static_cast<unsigned>(s1.tx_merges - s0.tx_merges),
            s1.tx_maxdepth, static_cast<unsigned>(s1.tx_wedges - s0.tx_wedges), s1.link_rx_drops - s0.link_rx_drops,
            s1.host_rx_drops - s0.host_rx_drops, s1.relay_drops - s0.relay_drops, static_cast<unsigned>(s1.reset_count - s0.reset_count),
            static_cast<unsigned>(s1.config_count - s0.config_count), static_cast<unsigned>(s1.host_restarts - s0.host_restarts),
            static_cast<unsigned>(s1.session - s0.session));
    } else {
        out(true, "STATS в конце не прочитан\n");
    }
    out(true, "библиотека: кадров %llu/%llu, CRC-отбросов %llu, переподключений %llu, рестартов чипа %llu; HEALTH запросов %llu, "
              "ответов %llu, ошибок %llu%s%s; CATCH событий %llu (физика %+lld,%+lld), потеряно в очереди %llu\n",
        static_cast<unsigned long long>(c1.frames_tx - c0.frames_tx), static_cast<unsigned long long>(c1.frames_rx - c0.frames_rx),
        static_cast<unsigned long long>(c1.crc_drops - c0.crc_drops), static_cast<unsigned long long>(c1.reconnects - c0.reconnects),
        static_cast<unsigned long long>(c1.restarts - c0.restarts), static_cast<unsigned long long>(mon.healthAsk.load()),
        static_cast<unsigned long long>(mon.healthOk.load()), static_cast<unsigned long long>(mon.healthFail.load()),
        mon.lastErr.empty() ? "" : ", последняя ", mon.lastErr.c_str(), static_cast<unsigned long long>(ca.events.load()),
        static_cast<long long>(ca.sumX.load()), static_cast<long long>(ca.sumY.load()),
        static_cast<unsigned long long>(ca.s ? medius_event_stream_dropped(ca.s) : 0));
    if (ca.s) {
        medius_event_stream_close(ca.s);
        medius_event_stream_free(ca.s);
    }
    medius_device_reset(dev);  // назад в passthrough
    medius_device_free(mon.dev);
    medius_device_free(dev);
    say(L"ГОТОВО");
    if (lf) std::fclose(lf);
    return lostAt ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (std::strcmp(argv[i], "--port") == 0 && more) g_port = argv[++i];
        else if (std::strcmp(argv[i], "--stress") == 0 && more) g_stressS = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--load") == 0 && more) g_stressLoad = argv[++i];
        else if (std::strcmp(argv[i], "--slog") == 0 && more) g_stressLog = argv[++i];
        else if (std::strcmp(argv[i], "--hand") == 0) g_hand = true;
        else if (std::strcmp(argv[i], "--now") == 0) g_now = true;
        else {
            std::printf("medius_stress.exe [--port COM5] --stress S [--load inject,query,catch,busy | idle] [--now] [--hand] [--slog FILE]\n");
            return 2;
        }
    }
    if (g_stressS <= 0.0) {
        std::printf("нужен --stress S\n");
        return 2;
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::thread ui(uiThread);
    while (!g_ui.ready.load()) Sleep(5);
    say(L"КЛИКНИТЕ В ЭТО ОКНО", L"тест начнётся по клику");
    int rc = 1;
    if (!waitClick(300.0)) {
        std::printf("[-] клика в окно теста не было, тест не запускался\n");
    } else {
        rc = runMediusStress(g_stressS);
    }
    g_cleanDone = true;
    Sleep(800);
    g_stop = true;
    ui.join();
    timeEndPeriod(1);
    return rc;
}
