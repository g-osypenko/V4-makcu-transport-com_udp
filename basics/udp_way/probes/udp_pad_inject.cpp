// udp_pad_inject.cpp - инжект правого стика геймпада через MAKCU по UDP (SET 0x40). Проба, не эталон.
//
//   udp_pad_inject.exe [--host auto|IP|MAC] [--period US] [--smoothing off|fixed|auto] [--buffer MS] [--keep]
//                      [--live PREFIX] [--from S] [--len S] [--out PREFIX] [--no-latency] [--no-circle] [--no-texture]
//
// Сценарий (ГЕЙМПАД НЕ ТРОГАТЬ: правый стик двигает программа; можно держать в руке и качать ЛЕВЫЙ стик):
//   0. чтение: 0x02, 0x04, 0x54, настройки; --smoothing: интерполяция (и --buffer) кодом, APPLY без SAVE, в конце
//      возвращается прежнее (до 4 с ждёт геймпад: без него раздела настроек контроллера нет);
//   1. задержка: ступенька (12000, -1) 300 мс и отпускание нулями, 5 повторов (как раздел 3 pad_bench_com);
//   2. круг 3 с радиусом 12000: пакетов Windows в секунду, отставание и ошибка слежения;
//   3. текстура (как раздел 4 pad_bench_com, та же запись руки): кривая (гаусс 20 мс), рендерер
//      (PadTextureRenderer::presetForTransport под режим и период), запись руки (снимки 0x54 как есть) -> PadTexture19,
//      W1 до живой руки. Числа сравнимы с COM (HANDOFF_GAMEPAD_COM.md §9л).
//   После каждой фазы - проверка, что плата отвечает (0x02) и геймпад шлёт отчёты (sequence 0x54 растёт). Плата
//   молчит - ждать до 30 с, потом выход с кодом 3 (USB3 платы завис: лечит переподключение платы).
// Темп: SET 0x40 без ответа ровно раз в --period мкс (250 / 500 / 1000 команд в секунду = 4000 / 2000 / 1000).
// ВНИМАНИЕ: на 4122 USB3 платы по UDP виснет при темпе команд ~500/с (любой опкод); вместе с ним пропадают мышь и сеть.
//
// Сборка (из basics\udp_way\probes):
//   g++ -std=c++17 -O2 -Wall -Wextra -I ..\perfect-transport-udp -I ..\..\comport_way\perfect-transport-gamepad-com udp_pad_inject.cpp
//       -lws2_32 -liphlpapi -lwinmm -static -o udp_pad_inject.exe
#include "udp_pad_common.h"
#include "makcu_gamepad_renderer.h"

#include <cstdlib>
#include <cstring>

namespace {

using namespace udppad;
using makcu::PadTexture19;

uint64_t g_cmds = 0, g_late = 0;
double g_cmdGapMax = 0, g_lastCmd = 0;
bool g_skipStick = false;
bool g_abortTicks = false;  // f() в runTicks: прервать серию тактов (долгий прогон, отказ дольше 60 с)  // такт занят другой командой: команду стика в нём не слать (одна датаграмма на такт)

void sendStick(Board& b, int16_t rx, int16_t ry) {
    uint8_t pl[20] = {};
    pl[16] = static_cast<uint8_t>(rx);
    pl[17] = static_cast<uint8_t>(static_cast<uint16_t>(rx) >> 8);
    pl[18] = static_cast<uint8_t>(ry);
    pl[19] = static_cast<uint8_t>(static_cast<uint16_t>(ry) >> 8);
    const double t = nowUs();
    if (g_lastCmd > 0) g_cmdGapMax = std::max(g_cmdGapMax, t - g_lastCmd);
    g_lastCmd = t;
    b.post(0x40, pl, sizeof(pl));
    ++g_cmds;
}

// Такт инжекта: вызывает f(k) на каждом такте k = 0..n-1 от t0 с шагом period; f возвращает (rx, ry). Если поток опоздал
// больше чем на такт, пропущенные такты не шлются (k идёт по времени). Между тактами вычитывает сокет.
template <class F>
void runTicks(Board& b, double t0, double period, size_t n, F f) {
    g_lastCmd = 0;  // промежуток считается только внутри серии, паузы между сериями не в счёт
    for (size_t k = 0; k < n;) {
        spinUntil(t0 + static_cast<double>(k) * period);
        const size_t now = static_cast<size_t>((nowUs() - t0) / period);
        if (now > k) {
            g_late += now - k;
            k = std::min(n - 1, now);
        }
        int16_t x = 0, y = 0;
        g_skipStick = false;
        f(k, x, y);
        if (g_abortTicks) {  // f() попросила остановить серию
            g_abortTicks = false;
            return;
        }
        if (!g_skipStick) sendStick(b, x, y);  // f() могла занять такт другой командой (GET 0x54 в долгом прогоне)
        b.drain(0);
        ++k;
    }
}
void release(Board& b, double period) {
    const double t0 = nowUs();
    runTicks(b, t0, period, std::max<size_t>(3, static_cast<size_t>(6000.0 / period)), [](size_t, int16_t& x, int16_t& y) { x = y = 0; });
}

// ---- запись руки ----
bool loadColumns(const std::string& path, const std::vector<std::string>& names, std::vector<std::vector<double>>& cols) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    if (!std::fgets(line, sizeof(line), f)) {
        std::fclose(f);
        return false;
    }
    std::vector<std::string> head;
    for (char* tok = std::strtok(line, ",\r\n"); tok; tok = std::strtok(nullptr, ",\r\n")) head.emplace_back(tok);
    std::vector<int> idx;
    for (const auto& n : names) {
        int at = -1;
        for (size_t i = 0; i < head.size(); ++i) {
            if (head[i] == n) at = static_cast<int>(i);
        }
        if (at < 0) {
            std::fclose(f);
            return false;
        }
        idx.push_back(at);
    }
    cols.assign(names.size(), {});
    while (std::fgets(line, sizeof(line), f)) {
        std::vector<std::string> cells;
        for (char* tok = std::strtok(line, ",\r\n"); tok; tok = std::strtok(nullptr, ",\r\n")) cells.emplace_back(tok);
        for (size_t c = 0; c < idx.size(); ++c) cols[c].push_back(static_cast<size_t>(idx[c]) < cells.size() ? std::atof(cells[static_cast<size_t>(idx[c])].c_str()) : 0.0);
    }
    std::fclose(f);
    return !cols[0].empty();
}
std::vector<double> onGrid(const std::vector<double>& t, const std::vector<double>& v, double t0Us, size_t n) {
    std::vector<double> out(n);
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const double at = t0Us + static_cast<double>(i) * 1000.0;
        while (j + 1 < t.size() && t[j + 1] <= at) ++j;
        out[i] = v[j];
    }
    return out;
}
std::vector<double> gauss(const std::vector<double>& v, double sigma) {
    const int r = static_cast<int>(sigma * 4);
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double sum = 0.0;
    for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * (i / sigma) * (i / sigma));
    std::vector<double> out(v.size());
    const int n = static_cast<int>(v.size());
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int j = -r; j <= r; ++j) acc += k[static_cast<size_t>(j + r)] * v[static_cast<size_t>(std::clamp(i + j, 0, n - 1))];
        out[static_cast<size_t>(i)] = acc / sum;
    }
    return out;
}
double zoh(const std::vector<XRow>& rows, double at, bool y) {
    size_t lo = 0, hi = rows.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (rows[mid].t <= at) lo = mid + 1;
        else hi = mid;
    }
    const XRow& r = lo ? rows[lo - 1] : rows.front();
    return y ? -static_cast<double>(r.ry) - 1.0 : static_cast<double>(r.rx);  // в координатах платы: Y = -Yw - 1
}
// Слежение: сдвиг (0..20 мс, шаг 0.25) с наименьшей средней ошибкой против цели tx(t_ms), ty(t_ms).
template <class TX, class TY>
void track(const std::vector<XRow>& rows, double t0, double lenMs, TX tx, TY ty, double& lagMs, double& errMean, double& errP99) {
    double best = 1e18;
    std::vector<double> bestE;
    for (int lag = 0; lag <= 80; ++lag) {
        double acc = 0;
        std::vector<double> e;
        for (double at = 100.0; at < lenMs - 30.0; at += 0.25) {
            const double src = at - lag * 0.25;
            if (src < 0) continue;
            const double d = std::hypot(zoh(rows, t0 + at * 1000.0, false) - tx(src), zoh(rows, t0 + at * 1000.0, true) - ty(src));
            acc += d;
            e.push_back(d);
        }
        if (!e.empty() && acc / static_cast<double>(e.size()) < best) {
            best = acc / static_cast<double>(e.size());
            lagMs = lag * 0.25;
            bestE.swap(e);
        }
    }
    errMean = best;
    errP99 = pct(bestE, 0.99);
}

std::vector<XRow> takeRows(double from, double to) {
    std::lock_guard<std::mutex> lock(g_xiMutex);
    std::vector<XRow> out;
    for (const auto& r : g_xi) {
        if (r.t >= from && r.t < to) out.push_back(r);
    }
    return out;
}
void startLog() {
    std::lock_guard<std::mutex> lock(g_xiMutex);
    g_xi.clear();
    g_xiLog = true;
}

// Плата жива: 0x02 отвечает, sequence 0x54 растёт. Ждёт до 30 с. Пишет строку.
bool healthy(Board& b, const char* where) {
    const double t0 = nowUs();
    bool said = false;
    while (nowUs() - t0 < 30e6) {
        auto d = b.ask(0x02, {}, 100000.0);
        Snap a = parse54(b.ask(0x54, {}, 100000.0));
        Sleep(100);
        Snap c = parse54(b.ask(0x54, {}, 100000.0));
        if (d && a.ok && c.ok && c.seq != a.seq) {
            std::printf("   [%s] плата отвечает, геймпад шлёт ~%.0f отчётов/с (sequence +%u за ~0.1 с)%s\n", where, (c.seq - a.seq) / 0.104, c.seq - a.seq,
                said ? (" - ВЕРНУЛСЯ через " + std::to_string(static_cast<int>((nowUs() - t0) / 1e6)) + " с").c_str() : "");
            return true;
        }
        if (!said) {
            std::printf("   [%s] !! %s - жду до 30 с\n", where, !d ? "ПЛАТА НЕ ОТВЕЧАЕТ по UDP" : !a.ok ? "0x54 без снимка (геймпада нет)" : "геймпад МОЛЧИТ (sequence стоит)");
            said = true;
        }
        Sleep(900);
    }
    std::printf("   [%s] !! не вернулось за 30 с\n", where);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string host = "auto", live = "..\\..\\comport_way\\pad_probes\\data\\rec2", outPrefix;
    uint16_t port = 8080;
    double period = 4000, from = 17.5, len = 7.0;
    int smoothing = -1, bufferMs = -1;
    bool keep = false, doLatency = true, doCircle = true, doTexture = true;
    double soakS = 0;   // --soak S: долгий прогон вместо фаз 1-3
    int streamMs = 0;   // --stream MS: на время долгого прогона включить поток физики 0x73 / 0x74 (0 - не включать)
    std::string logPath;  // --log FILE: строки долгого прогона ещё и в файл (дописывается, сбрасывается каждую строку)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--host" && more) host = argv[++i];
        else if (a == "--port" && more) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--period" && more) period = std::clamp(std::atof(argv[++i]), 1000.0, 20000.0);
        else if (a == "--smoothing" && more) {
            const std::string m = argv[++i];
            smoothing = m == "off" ? 0 : m == "fixed" ? 1 : m == "auto" ? 2 : -1;
        } else if (a == "--buffer" && more) bufferMs = std::clamp(std::atoi(argv[++i]), 1, 64);
        else if (a == "--keep") keep = true;
        else if (a == "--live" && more) live = argv[++i];
        else if (a == "--from" && more) from = std::atof(argv[++i]);
        else if (a == "--len" && more) len = std::atof(argv[++i]);
        else if (a == "--out" && more) outPrefix = argv[++i];
        else if (a == "--no-latency") doLatency = false;
        else if (a == "--no-circle") doCircle = false;
        else if (a == "--no-texture") doTexture = false;
        else if (a == "--soak" && more) soakS = std::atof(argv[++i]);
        else if (a == "--stream" && more) streamMs = std::clamp(std::atoi(argv[++i]), 0, 1000);
        else if (a == "--log" && more) logPath = argv[++i];
        else {
            std::printf("udp_pad_inject.exe [--host auto|IP|MAC] [--period US] [--smoothing off|fixed|auto] [--buffer MS] [--keep]\n"
                        "                   [--live PREFIX] [--from S] [--len S] [--out PREFIX] [--no-latency] [--no-circle] [--no-texture]\n"
                        "                   [--soak S [--stream MS] [--log FILE]]\n");
            return 2;
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);
    noPowerThrottling();
    std::string err;
    if (!makcu::resolveUdpHost(host, port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    Board b;
    if (!b.open(host, port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    auto dev = b.ask(0x02, {});
    auto fw = b.ask(0x04, {});
    if (!dev || dev->empty()) {
        std::printf("[-] плата %s:%u не отвечает\n", host.c_str(), port);
        return 1;
    }
    const uint8_t kind = static_cast<uint8_t>((*dev)[0] & 0xFC);
    std::printf("[+] плата %s:%u, прошивка %u, kinds %02X; период команд %.0f мкс (%.0f команд/с)\n", host.c_str(), port, fw && fw->size() >= 2 ? le16(fw->data()) : 0,
        (*dev)[0], period, 1e6 / period);
    if (!kind) {
        std::printf("[-] геймпада на плате нет (kinds & FC = 0)\n");
        return 1;
    }
    static const char* const names[3] = {"Off", "Fixed", "Auto"};
    int wasInterp = -1, wasBuffer = -1, interp = -1, buf = -1;
    if (!b.readSmoothing(wasInterp, wasBuffer)) std::printf("[!] настройки не прочитаны\n");
    bool changed = false;
    if (smoothing >= 0 || bufferMs > 0) {
        if (!b.writeSmoothing(smoothing, bufferMs)) {
            std::printf("[-] запись сглаживания не удалась\n");
            return 1;
        }
        changed = true;
    }
    b.readSmoothing(interp, buf);
    std::printf("[i] сглаживание: %s, буфер %d мс%s\n", interp >= 0 && interp < 3 ? names[interp] : "?", buf,
        changed ? (std::string(" (поставлено кодом, было ") + (wasInterp >= 0 && wasInterp < 3 ? names[wasInterp] : "?") + " / " + std::to_string(wasBuffer) + " мс)").c_str() : "");
    const bool boardAuto = interp == 2;

    std::thread xi(xinputLoop);
    Sleep(300);
    if (!g_xiOk) std::printf("[!] Windows не видит геймпад в XInput\n");
    int code = 0;
    const auto phaseStats = [&](const char* name) {
        std::printf("   %s: команд %llu, пропущено тактов (поток опоздал) %llu, наибольший промежуток %.2f мс; ответов без запроса %llu\n", name,
            static_cast<unsigned long long>(g_cmds), static_cast<unsigned long long>(g_late), g_cmdGapMax / 1000.0, static_cast<unsigned long long>(b.stray));
        g_cmds = g_late = 0;
        g_cmdGapMax = g_lastCmd = 0;
        b.stray = 0;
    };
    if (!healthy(b, "старт")) code = 3;
    if (soakS > 0) doLatency = doCircle = doTexture = false;

    // ---- 4. долгий прогон (--soak) ----
    // Рабочая связка: команда стика каждый такт (рендерер ведёт к кругу 0.5 об/с с дышащим радиусом 7200..12000),
    // раз в 10 с отпускание на 300 мс (handoff руке); поток физики 0x73 / 0x74 (--stream), раз в 1 с GET 0x54 без ожидания
    // (sequence: отчёты геймпада). Строка в секунду: команд, пакетов Windows, событий потока, отчётов геймпада, ответов.
    // Отказ: нет ответа 0x54 3 с (плата), sequence стоит (геймпад молчит) или в XInput нет пакетов. Отказ дольше 60 с -
    // выход с кодом 3. Темп не меняется и во время отказа.
    if (code == 0 && soakS > 0) {
        FILE* lf = logPath.empty() ? nullptr : std::fopen(logPath.c_str(), "a");
        auto out = [&](const char* fmt, auto... args) {
            std::printf(fmt, args...);
            if (lf) {
                std::fprintf(lf, fmt, args...);
                std::fflush(lf);
            }
        };
        out("== 4. Долгий прогон %.0f с: %.0f команд/с, сглаживание %s / %d мс, поток %s ==\n", soakS, 1e6 / period, interp >= 0 && interp < 3 ? names[interp] : "?", buf,
            streamMs > 0 ? (std::to_string(streamMs) + " мс").c_str() : "выключен");
        if (streamMs > 0) {
            for (uint8_t op : {static_cast<uint8_t>(0x73), static_cast<uint8_t>(0x74)}) {
                b.ask(op, {2, static_cast<uint8_t>(streamMs), static_cast<uint8_t>(streamMs >> 8)}, 30000.0);
                auto q = b.ask(op, {});
                out("   поток 0x%02X: %s\n", op, q && q->size() == 3 ? ("mode " + std::to_string((*q)[0]) + ", " + std::to_string(le16(q->data() + 1)) + " мс").c_str() : "НЕ ВКЛЮЧИЛСЯ");
            }
        }
        const makcu::PadRendererProfile prof = makcu::PadTextureRenderer::presetForTransport(kind, !boardAuto, period, 1000.0);
        makcu::PadTextureRenderer r;
        r.setProfile(prof);
        r.beginEvent(0x50A4ull, 0, -1);
        const size_t n = static_cast<size_t>(soakS * 1e6 / period);
        uint64_t drops = 0, releases = 0, lastXi = g_xiPackets.load(), last54n = b.replies54, cmdsAtSec = 0;
        uint32_t lastSeq = 0;
        bool haveSeq = false, bad = false;
        double badSince = 0, lastReplyT = nowUs();
        int missed = 0;            // подряд секунд без ответа на GET 0x54
        bool boardSilent = false;  // плата молчит 2 с подряд: инжект приостановлен
        std::string badWhy;
        b.events.clear();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const double t0 = nowUs();
        double nextGetT = t0 + 0.5e6, nextSecT = t0 + 1e6;
        bool abort = false;
        runTicks(b, t0, period, n, [&](size_t k, int16_t& ox, int16_t& oy) {
            if (abort) return;
            // Плата не отвечает: команды стика не слать (только раз в секунду GET 0x54 - ожила ли).
            if (boardSilent) g_skipStick = true;
            const double ms = static_cast<double>(k) * period / 1000.0;
            const double inCycle = std::fmod(ms, 10000.0);
            if (inCycle >= 9700.0) {  // отпускание 300 мс
                ox = oy = 0;
                if (inCycle - period / 1000.0 < 9700.0) ++releases;
                if (inCycle + period / 1000.0 >= 10000.0) r.beginEvent(0x50A4ull + releases, 0, -1);
            } else {
                const double rad = 9600.0 + 2400.0 * std::sin(6.2831853 * ms / 7000.0);
                const double ang = 6.2831853 * ms / 2000.0;
                r.step(rad * std::cos(ang), rad * std::sin(ang), ox, oy);
            }
            // Снимок и строка - по часам, а не по номеру такта: если поток опоздал и такты пропущены, секунда не теряется.
            const double tn = nowUs();
            if (tn >= nextGetT) {
                nextGetT += 1e6;
                const uint8_t none = 0;
                b.post(0x54, &none, 0);
                g_skipStick = true;  // этот такт - снимок вместо команды стика (стик держит прошлое значение)
            }
            if (tn >= nextSecT) {
                nextSecT += 1e6;
                const uint64_t xi = g_xiPackets.load(), xiD = xi - lastXi;
                lastXi = xi;
                const uint64_t rep = b.replies54 - last54n;
                last54n = b.replies54;
                if (rep) lastReplyT = nowUs();
                missed = rep ? 0 : missed + 1;
                if (missed >= 2 && !boardSilent) out("   !! %7.1f с: плата 2 с не отвечает по UDP - команды стика приостановлены\n", ms / 1000.0);
                if (missed == 0 && boardSilent) out("   ++ %7.1f с: плата снова отвечает - инжект продолжен\n", ms / 1000.0);
                boardSilent = missed >= 2;
                const Snap s = parse54(std::optional<std::vector<uint8_t>>(b.last54));
                const bool seqKnown = haveSeq && s.ok;  // первый снимок сравнивать не с чем
                const uint32_t seqD = seqKnown ? s.seq - lastSeq : 0;
                if (s.ok) {
                    lastSeq = s.seq;
                    haveSeq = true;
                }
                const size_t evN = b.events.size();
                size_t evRight = 0;  // события правого стика (RX / RY): его двигает только программа
                int16_t lastEvRx = 0;
                for (const auto& e : b.events) {
                    if (e.kind == 1 && (e.id == 14 || e.id == 15)) ++evRight;
                    if (e.kind == 1 && e.id == 14) lastEvRx = static_cast<int16_t>(e.value);
                }
                b.events.clear();
                std::string why;
                if (boardSilent) why = "ПЛАТА НЕ ОТВЕЧАЕТ по UDP";
                else if (rep && (!s.ok || (seqKnown && seqD == 0))) why = s.ok ? "ГЕЙМПАД МОЛЧИТ" : "0x54 без снимка (геймпада нет)";
                else if (xiD == 0) why = "в Windows нет пакетов";
                if (!why.empty() && !bad) {
                    bad = true;
                    badSince = nowUs();
                    badWhy = why;
                    ++drops;
                    out("   !! %7.1f с: ОТКАЗ %llu: %s\n", ms / 1000.0, static_cast<unsigned long long>(drops), why.c_str());
                } else if (why.empty() && bad) {
                    bad = false;
                    out("   ++ %7.1f с: вернулось через %.1f с (%s)\n", ms / 1000.0, (nowUs() - badSince) / 1e6, badWhy.c_str());
                }
                if (bad && nowUs() - badSince > 60e6) {
                    out("   !! отказ дольше 60 с - прогон остановлен\n");
                    abort = true;
                    g_abortTicks = true;
                    code = 3;
                }
                out("   %6.0f с | команд %4llu | Windows %4llu пак/с | поток %4zu соб/с (RX/RY %4zu, посл. RX %6d, снимок RX %6d) | геймпад %4u отч/с | 0x54 ответов %llu | опоздал тактов %llu | отказов %llu\n",
                    ms / 1000.0, static_cast<unsigned long long>(g_cmds - cmdsAtSec), static_cast<unsigned long long>(xiD), evN, evRight, lastEvRx, s.ok ? s.rx : 0, seqD,
                    static_cast<unsigned long long>(rep),
                    static_cast<unsigned long long>(g_late), static_cast<unsigned long long>(drops));
                cmdsAtSec = g_cmds;
            }
        });
        release(b, period);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        if (streamMs > 0) {
            for (uint8_t op : {static_cast<uint8_t>(0x73), static_cast<uint8_t>(0x74)}) {
                b.ask(op, {0, static_cast<uint8_t>(streamMs), static_cast<uint8_t>(streamMs >> 8)}, 30000.0);
                auto q = b.ask(op, {});
                out("   поток 0x%02X выключен: %s\n", op, q && q->size() == 3 ? ("mode " + std::to_string((*q)[0])).c_str() : "нет ответа");
            }
        }
        out("   ИТОГ: %.0f с, отказов %llu, отпусканий %llu, команд %llu, опоздавших тактов %llu\n", (nowUs() - t0) / 1e6, static_cast<unsigned long long>(drops),
            static_cast<unsigned long long>(releases), static_cast<unsigned long long>(g_cmds), static_cast<unsigned long long>(g_late));
        if (lf) std::fclose(lf);
    }

    // ---- 1. задержка ----
    if (code == 0 && doLatency) {
        std::printf("== 1. Ступенька (12000, -1) и отпускание, 5 повторов ==\n");
        double sf = 0, sh = 0, sfu = 0, srf = 0, srfu = 0;
        int n = 0;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (int rep = 0; rep < 5; ++rep) {
            Sleep(300 + rep);
            startLog();
            Sleep(20);
            const double t0 = nowUs();
            runTicks(b, t0, period, static_cast<size_t>(300000.0 / period), [](size_t, int16_t& x, int16_t& y) {
                x = 12000;
                y = -1;
            });
            const double t1 = nowUs();
            release(b, period);
            Sleep(100);
            g_xiLog = false;
            const auto rows = takeRows(t0 - 20000.0, t1 + 100000.0);
            const int kTol = 16;
            int rest = 0;
            for (const auto& r : rows) {
                if (r.t < t0) rest = r.rx;
            }
            double f = -1, h = -1, fu = -1, rf = -1, rfu = -1;
            for (const auto& r : rows) {
                const double ms = (r.t - t0) / 1000.0, rms = (r.t - t1) / 1000.0;
                if (ms >= 0 && f < 0 && std::abs(r.rx - rest) > kTol) f = ms;
                if (ms >= 0 && h < 0 && r.rx >= rest + (12000 - rest) / 2) h = ms;
                if (ms >= 0 && fu < 0 && r.rx == 12000) fu = ms;
                if (rms >= 0 && rf < 0 && r.rx != 12000) rf = rms;
                if (rms >= 0 && rf >= 0 && rfu < 0 && std::abs(r.rx - rest) <= kTol) rfu = rms;
            }
            std::printf("   повтор %d: первое изменение %.2f мс, 50 %% %.2f, 100 %% %.2f | отпускание: первое изменение %.2f, покой %.2f мс\n", rep + 1, f, h, fu, rf, rfu);
            if (f >= 0 && fu >= 0 && rfu >= 0) {
                sf += f;
                sh += h;
                sfu += fu;
                srf += rf;
                srfu += rfu;
                ++n;
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        if (n)
            std::printf("   среднее по %d: первое изменение %.2f мс, 50 %% %.2f, 100 %% %.2f | отпускание: первое изменение %.2f, покой %.2f мс\n", n, sf / n, sh / n, sfu / n, srf / n,
                srfu / n);
        else std::printf("   !! ни одного полного повтора (команды не дошли?)\n");
        phaseStats("ступеньки");
        if (!healthy(b, "после ступенек")) code = 3;
    }

    // ---- 2. круг ----
    if (code == 0 && doCircle) {
        std::printf("== 2. Круг 3 с, радиус 12000, 1 оборот/с ==\n");
        const auto cx = [](double ms) { return 12000.0 * std::min(1.0, ms / 250.0) * std::cos(6.2831853 * ms / 1000.0); };
        const auto cy = [](double ms) { return 12000.0 * std::min(1.0, ms / 250.0) * std::sin(6.2831853 * ms / 1000.0); };
        Sleep(300);
        startLog();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const double t0 = nowUs();
        const size_t n = static_cast<size_t>(3e6 / period);
        runTicks(b, t0, period, n, [&](size_t k, int16_t& x, int16_t& y) {
            const double ms = static_cast<double>(k) * period / 1000.0;
            x = static_cast<int16_t>(std::lround(cx(ms)));
            y = static_cast<int16_t>(std::lround(cy(ms)));
        });
        const double t1 = nowUs();
        release(b, period);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        Sleep(50);
        g_xiLog = false;
        const auto rows = takeRows(t0, t1);
        size_t changes = 0;
        for (size_t i = 1; i < rows.size(); ++i) changes += rows[i].rx != rows[i - 1].rx || rows[i].ry != rows[i - 1].ry;
        double lag = 0, em = 0, e99 = 0;
        if (rows.size() > 100) track(rows, t0, (t1 - t0) / 1000.0, cx, cy, lag, em, e99);
        std::printf("   пакетов Windows %zu за %.2f с (%.0f/с), из них с новым значением стика %zu (%.0f/с); отставание %.2f мс, ошибка средняя %.1f, p99 %.0f\n", rows.size(),
            (t1 - t0) / 1e6, rows.size() / ((t1 - t0) / 1e6), changes, changes / ((t1 - t0) / 1e6), lag, em, e99);
        phaseStats("круг");
        if (!healthy(b, "после круга")) code = 3;
    }

    // ---- 3. текстура ----
    if (code == 0 && doTexture) {
        std::printf("== 3. Текстура: живая рука против инжекта (%s, с %.1f с, %.1f с) ==\n", live.c_str(), from, len);
        std::vector<std::vector<double>> bc, xc;
        if (!loadColumns(live + "_board.csv", {"t_us", "rx", "ry"}, bc) || !loadColumns(live + "_xinput.csv", {"t_us", "rx", "ry"}, xc)) {
            std::printf("[-] не прочитаны %s_board.csv / %s_xinput.csv\n", live.c_str(), live.c_str());
        } else {
            const size_t n = static_cast<size_t>(len * 1000.0);
            const double t0b = bc[0][0] + from * 1e6, t0x = xc[0][0] + from * 1e6;
            const std::vector<double> bx = onGrid(bc[0], bc[1], t0b, n), by = onGrid(bc[0], bc[2], t0b, n);
            const std::vector<double> sx = gauss(bx, 20.0), sy = gauss(by, 20.0);
            std::vector<int16_t> liveX, liveY;
            for (size_t i = 0; i < xc[0].size(); ++i) {
                if (xc[0][i] >= t0x && xc[0][i] < t0x + len * 1e6) {
                    liveX.push_back(static_cast<int16_t>(xc[1][i]));
                    liveY.push_back(static_cast<int16_t>(xc[2][i]));
                }
            }
            const auto liveFeat = PadTexture19::windows(liveX.data(), liveY.data(), liveX.size());
            const makcu::PadRendererProfile prof = makcu::PadTextureRenderer::presetForTransport(kind, !boardAuto, period, 1000.0);
            std::printf("   рендерер: presetForTransport(%s, такт %.0f мкс): tick_hz %.0f, kappa_lead %.2f, АЦП раз в %u тактов, середина %s\n",
                boardAuto ? "под Auto" : "под Off", period, prof.tick_hz, prof.kappa_lead, prof.regime.adc_period_ticks, prof.regime.adc_midpoint ? "да" : "нет");
            const char* const vnames[3] = {"кривая", "рендерер", "запись руки"};
            std::vector<PadTexture19::Row> feats[3];
            double tot[3] = {0, 0, 0};
            const double stepMs = period / 1000.0;
            const size_t cmds = static_cast<size_t>(static_cast<double>(n) / stepMs);
            auto at = [&](size_t k) { return std::min(n - 1, static_cast<size_t>(std::llround(static_cast<double>(k) * stepMs))); };
            for (int v = 0; v < 3 && code == 0; ++v) {
                makcu::PadTextureRenderer r;
                r.setProfile(prof);
                Sleep(400);
                const Snap ph = parse54(b.ask(0x54, {}, 50000.0));
                r.beginEvent(0xC0FFEEull + static_cast<uint64_t>(v), ph.ok ? ph.rx : 0, ph.ok ? ph.ry : -1);
                startLog();
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                const double t0 = nowUs();
                size_t done = 0;
                runTicks(b, t0, period, cmds, [&](size_t k, int16_t& ox, int16_t& oy) {
                    for (; done <= k; ++done) {
                        const size_t i = at(done);
                        if (v == 0) {
                            ox = static_cast<int16_t>(std::lround(sx[i]));
                            oy = static_cast<int16_t>(std::lround(sy[i]));
                        } else if (v == 2) {
                            ox = static_cast<int16_t>(std::lround(bx[i]));
                            oy = static_cast<int16_t>(std::lround(by[i]));
                        } else {
                            r.step(sx[i], sy[i], ox, oy);
                        }
                    }
                });
                const double t1 = nowUs();
                release(b, period);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                Sleep(20);
                g_xiLog = false;
                const auto rows = takeRows(t0, t1);
                double lag = 0, em = 0, e99 = 0;
                if (rows.size() > 100) {
                    track(rows, t0, (t1 - t0) / 1000.0, [&](double ms) { return sx[std::min(n - 1, static_cast<size_t>(ms))]; },
                        [&](double ms) { return sy[std::min(n - 1, static_cast<size_t>(ms))]; }, lag, em, e99);
                }
                std::vector<int16_t> wx(rows.size()), wy(rows.size());
                for (size_t i = 0; i < rows.size(); ++i) {
                    wx[i] = rows[i].rx;
                    wy[i] = rows[i].ry;
                }
                feats[v] = PadTexture19::windows(wx.data(), wy.data(), wx.size());
                PadTexture19::w1(liveFeat, feats[v], &tot[v]);
                std::printf("   %-12s пакетов Windows %zu за %.1f с; отставание от цели %.2f мс, ошибка средняя %.1f, p99 %.0f; W1 %.3f\n", vnames[v], rows.size(), (t1 - t0) / 1e6, lag, em,
                    e99, tot[v]);
                phaseStats(vnames[v]);
                if (!outPrefix.empty()) {
                    if (FILE* f = std::fopen((outPrefix + "_" + std::to_string(v) + ".csv").c_str(), "w")) {
                        std::fprintf(f, "t_us,rx,ry\n");
                        for (const auto& row : rows) std::fprintf(f, "%.1f,%d,%d\n", row.t - t0, row.rx, row.ry);
                        std::fclose(f);
                    }
                }
                if (!healthy(b, vnames[v])) code = 3;
            }
            std::printf("   СУММА W1 до живой руки: кривая %.3f, рендерер %.3f, запись руки %.3f\n", tot[0], tot[1], tot[2]);
        }
    }

    g_stop = true;
    xi.join();
    if (changed && !keep) {
        bool ok = false;
        const double until = nowUs() + 4e6;
        while (!(ok = b.writeSmoothing(wasInterp, wasBuffer)) && nowUs() < until) Sleep(200);
        int i2 = -1, b2 = -1;
        b.readSmoothing(i2, b2);
        std::printf("[%c] сглаживание возвращено: %s, буфер %d мс%s\n", ok ? '+' : '!', i2 >= 0 && i2 < 3 ? names[i2] : "?", b2, ok ? "" : " - НЕ ВЕРНУЛОСЬ (геймпада нет?)");
    }
    auto st = b.ask(0x40, {});
    std::printf("[i] GET 0x40 в конце: %s\n", st ? hex(*st).c_str() : "нет ответа");
    timeEndPeriod(1);
    return code;
}
