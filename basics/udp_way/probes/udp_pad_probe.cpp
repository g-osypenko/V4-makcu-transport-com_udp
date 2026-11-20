// udp_pad_probe.cpp - геймпад через MAKCU по UDP: чтение и поток физики 0x73 / 0x74 -> 0x75. НИЧЕГО НЕ ИНЖЕКТИТ.
//
//   udp_pad_probe.exe [--host auto|IP|MAC] [--port 8080] [--stream MS] [--poll RATE] [--dur S] [--csv FILE]
//
// Что делает:
//   1. DEVICE 0x02, FIRMWARE 0x04, GET 0x54 (снимок физики геймпада), настройки (INFO + первые 12 байт образа:
//      интерполяция и буфер), состояние потоков 0x73 (оси) и 0x74 (кнопки) - запрос с пустым телом;
//   2. --stream MS: SET 0x73 и 0x74 = [2 (двоичный), MS u16] с проверкой, затем dur секунд слушает события 0x75
//      (kind 1 оси id 10-15: LT RT LX LY RX RY, kind 2 кнопки; value u16 LE; id 255 - снимок недоступен).
//      Параллельно XInput (Windows) и раз в 250 мс GET 0x54 (sequence: сколько отчётов дал геймпад). В конце -
//      потоки выключаются (mode 0) и это проверяется. Итог: событий в секунду по каждой оси и кнопкам, интервалы
//      событий оси RX, отставание события RX от того же значения в XInput;
//   3. --poll RATE (без --stream): GET 0x54 RATE раз в секунду (не чаще 250/с: на 4122 USB3 платы по UDP виснет при
//      темпе команд ~500/с), dur секунд - для сравнения с потоком.
// Темп: одна команда в полёте, команды не ближе 4 мс (правило UDP-эталона мыши, ревизия 6).
// События платы приходят на наш порт С ДРУГОГО порта платы, поэтому сокет без connect() и нужно правило брандмауэра
// на входящий UDP от платы (setup_firewall.bat). Нет событий при включённом потоке - первым делом брандмауэр.
//
// Сборка (из basics\udp_way\probes):
//   g++ -std=c++17 -O2 -Wall -Wextra -I ..\perfect-transport-udp udp_pad_probe.cpp -lws2_32 -liphlpapi -lwinmm -static -o udp_pad_probe.exe
#include "udp_pad_common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace {

using namespace udppad;

const char* axisName(uint8_t id) {
    static const char* const n[6] = {"LT", "RT", "LX", "LY", "RX", "RY"};
    return id >= 10 && id <= 15 ? n[id - 10] : "?";
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string host = "auto", csv;
    uint16_t port = 8080;
    int streamMs = 0;
    double pollRate = 0, dur = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--host" && more) host = argv[++i];
        else if (a == "--port" && more) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--stream" && more) streamMs = std::clamp(std::atoi(argv[++i]), 1, 1000);
        else if (a == "--poll" && more) pollRate = std::min(250.0, std::atof(argv[++i]));
        else if (a == "--dur" && more) dur = std::atof(argv[++i]);
        else if (a == "--csv" && more) csv = argv[++i];
        else {
            std::printf("udp_pad_probe.exe [--host auto|IP|MAC] [--port 8080] [--stream MS] [--poll RATE] [--dur S] [--csv FILE]\n");
            return 2;
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);
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
    std::printf("== 1. Плата %s:%u ==\n", host.c_str(), port);
    auto dev = b.ask(0x02, {});
    auto fw = b.ask(0x04, {});
    if (!dev) {
        std::printf("[-] DEVICE 0x02: нет ответа. Плата в сети? (после подключения ~15 с)\n");
        return 1;
    }
    std::printf("    0x02 DEVICE: %s(kinds; 80 = Xbox 360/XInput)\n", hex(*dev).c_str());
    if (fw && fw->size() >= 2) std::printf("    0x04 FIRMWARE: %s-> %u\n", hex(*fw).c_str(), le16(fw->data()));
    auto r54 = b.ask(0x54, {});
    Snap s0 = parse54(r54);
    if (s0.ok) {
        std::printf("    GET 0x54: LT %u RT %u | левый %d, %d | правый %d, %d | sequence %u, timing %04X, cadence %u uf\n", s0.lt, s0.rt, s0.lx, s0.ly, s0.rx, s0.ry, s0.seq,
            s0.timing, s0.cadence);
        Sleep(200);
        Snap s1 = parse54(b.ask(0x54, {}));
        if (s1.ok) std::printf("    за ~0.2 с sequence +%u (отчётов геймпада ~%.0f/с)\n", s1.seq - s0.seq, (s1.seq - s0.seq) / 0.2);
    } else {
        std::printf("    GET 0x54: %s(геймпада нет?)\n", r54 ? hex(*r54).c_str() : "нет ответа ");
    }
    {
        auto info = b.ask(0x3E, {0x1D, 0x00}, 300000.0);
        if (info && info->size() == 14 && (*info)[0] == 0x1D) {
            const uint32_t rev = le32(info->data() + 8);
            std::vector<uint8_t> rd = {0x1D, 0x01, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24), 0, 0, 12};
            auto img = b.ask(0x3E, rd, 300000.0);
            if (img && img->size() == 9 + 12) {
                static const char* const names[3] = {"Off", "Fixed", "Auto"};
                const uint32_t interp = le32(img->data() + 9 + 4);
                std::printf("    настройки: разделы %02X, интерполяция %u (%s), буфер %u мс\n", (*info)[4], interp, interp < 3 ? names[interp] : "?", le32(img->data() + 9 + 8));
            } else {
                std::printf("    настройки: INFO %s, READ %s\n", hex(*info).c_str(), img ? hex(*img).c_str() : "нет ответа");
            }
        } else {
            std::printf("    настройки INFO: %s\n", info ? hex(*info).c_str() : "нет ответа");
        }
    }
    auto q73 = b.ask(0x73, {}), q74 = b.ask(0x74, {});
    auto streamText = [](const std::optional<std::vector<uint8_t>>& q) {
        if (!q) return std::string("нет ответа");
        if (q->size() == 1 && (*q)[0] == 0xFF) return std::string("FF - команды нет в прошивке");
        if (q->size() == 3) return "mode " + std::to_string((*q)[0]) + ", период " + std::to_string(le16(q->data() + 1)) + " мс";
        return hex(*q);
    };
    std::printf("    поток 0x73 (оси): %s\n    поток 0x74 (кнопки): %s\n", streamText(q73).c_str(), streamText(q74).c_str());
    b.drain(20000.0);
    const size_t evBefore = b.events.size();
    if (evBefore) std::printf("[i] событий 0x75 до включения: %zu (поток уже был включён)\n", evBefore);

    std::thread xi(xinputLoop);
    Sleep(300);
    std::printf("    Windows XInput: %s\n", g_xiOk ? "геймпад есть" : "геймпада НЕТ");

    if (streamMs > 0) {
        std::printf("== 2. Поток физики: SET 0x73 / 0x74 = [2, %d мс], %.0f с. Стики и кнопки можно крутить ==\n", streamMs, dur);
        bool ok = true;
        for (uint8_t op : {static_cast<uint8_t>(0x73), static_cast<uint8_t>(0x74)}) {
            auto set = b.ask(op, {2, static_cast<uint8_t>(streamMs), static_cast<uint8_t>(streamMs >> 8)}, 30000.0);
            auto q = b.ask(op, {});
            std::printf("    SET 0x%02X: ответ %s; запрос после: %s\n", op, set ? hex(*set).c_str() : "нет", streamText(q).c_str());
            if (!q || q->size() != 3 || (*q)[0] != 2) ok = false;
        }
        if (!ok) std::printf("[!] поток не встал в двоичный режим - всё равно слушаю\n");
        b.events.clear();
        {
            std::lock_guard<std::mutex> lock(g_xiMutex);
            g_xi.clear();
        }
        g_xiLog = true;
        const uint64_t xi0 = g_xiPackets.load();
        const double t0 = nowUs();
        Snap first = parse54(b.ask(0x54, {})), last = first;
        uint64_t snaps = 0, snapLost = 0;
        double nextSnap = t0 + 250000.0, nextPrint = t0 + 2e6;
        size_t evPrinted = 0;
        uint64_t xiPrinted = xi0;
        while (nowUs() < t0 + dur * 1e6) {
            if (nowUs() >= nextSnap) {
                nextSnap += 250000.0;
                Snap s = parse54(b.ask(0x54, {}, 50000.0));
                if (s.ok) {
                    last = s;
                    ++snaps;
                } else {
                    ++snapLost;
                }
            } else {
                b.drain(std::min(5000.0, nextSnap - nowUs()));
            }
            if (nowUs() >= nextPrint) {
                const uint64_t xn = g_xiPackets.load();
                std::printf("    %4.0f с: событий 0x75 %5zu за 2 с, XInput пакетов %5llu, sequence %u\n", (nowUs() - t0) / 1e6, b.events.size() - evPrinted,
                    static_cast<unsigned long long>(xn - xiPrinted), last.seq);
                evPrinted = b.events.size();
                xiPrinted = xn;
                nextPrint += 2e6;
            }
        }
        const double span = (nowUs() - t0) / 1e6;
        g_xiLog = false;
        // выключить
        for (uint8_t op : {static_cast<uint8_t>(0x73), static_cast<uint8_t>(0x74)}) {
            b.ask(op, {0, static_cast<uint8_t>(streamMs), static_cast<uint8_t>(streamMs >> 8)}, 30000.0);
            std::printf("    выключение 0x%02X: запрос после: %s\n", op, streamText(b.ask(op, {})).c_str());
        }
        b.drain(50000.0);
        // итог
        std::printf("== 3. Итог потока за %.1f с ==\n", span);
        std::map<int, uint64_t> byKey;
        uint64_t unavailable = 0, buttonsEv = 0;
        std::vector<double> rxGaps;
        double lastRx = -1;
        for (const auto& e : b.events) {
            if (e.t > t0 + span * 1e6) continue;
            if (e.id == 255) {
                ++unavailable;
                continue;
            }
            if (e.kind == 2) ++buttonsEv;
            ++byKey[e.kind * 256 + e.id];
            if (e.kind == 1 && e.id == 14) {
                if (lastRx > 0) rxGaps.push_back((e.t - lastRx) / 1000.0);
                lastRx = e.t;
            }
        }
        std::printf("    событий всего %zu (%.0f/с), недоступно (id 255) %llu, кнопок %llu; чужих датаграмм %llu, запоздалых ответов %llu\n", b.events.size(),
            b.events.size() / span, static_cast<unsigned long long>(unavailable), static_cast<unsigned long long>(buttonsEv), static_cast<unsigned long long>(b.foreign),
            static_cast<unsigned long long>(b.lateReplies));
        for (const auto& [k, n] : byKey) {
            if (k / 256 == 1) std::printf("      ось %-2s (id %d): %llu (%.0f/с)\n", axisName(static_cast<uint8_t>(k % 256)), k % 256, static_cast<unsigned long long>(n), n / span);
            else std::printf("      кнопка id %d: %llu\n", k % 256, static_cast<unsigned long long>(n));
        }
        const uint64_t xiN = g_xiPackets.load() - xi0;
        std::printf("    XInput (Windows): %llu пакетов (%.0f/с); снимков 0x54 %llu (потеряно %llu): sequence %u -> %u = %.0f отчётов геймпада/с\n",
            static_cast<unsigned long long>(xiN), xiN / span, static_cast<unsigned long long>(snaps), static_cast<unsigned long long>(snapLost), first.seq, last.seq,
            first.ok && last.ok ? (last.seq - first.seq) / span : 0.0);
        if (!rxGaps.empty()) {
            std::printf("    интервал событий RX, мс: медиана %.2f, p10 %.2f, p90 %.2f, p99 %.2f, max %.1f (%zu интервалов)\n", pct(rxGaps, 0.5), pct(rxGaps, 0.1), pct(rxGaps, 0.9),
                pct(rxGaps, 0.99), pct(rxGaps, 1.0), rxGaps.size());
        }
        // Отставание: событие RX со значением v против ближайшего по времени пакета XInput с rx == v (окно +-50 мс).
        // > 0 - событие пришло позже, чем Windows увидел то же значение.
        std::vector<double> lag;
        size_t matched = 0, rxEvents = 0;
        {
            std::lock_guard<std::mutex> lock(g_xiMutex);
            for (const auto& e : b.events) {
                if (e.kind != 1 || e.id != 14) continue;
                ++rxEvents;
                const int16_t v = static_cast<int16_t>(e.value);
                double best = 1e18;
                for (const auto& r : g_xi) {
                    if (r.t < e.t - 50000.0) continue;
                    if (r.t > e.t + 50000.0) break;
                    if (r.rx == v && std::fabs(e.t - r.t) < std::fabs(best)) best = e.t - r.t;
                }
                if (best < 1e17) {
                    ++matched;
                    lag.push_back(best / 1000.0);
                }
            }
        }
        if (!lag.empty()) {
            std::printf("    событие RX позже Windows (то же значение, +-50 мс): медиана %.2f мс, p10 %.2f, p90 %.2f; совпало %zu из %zu событий RX\n", pct(lag, 0.5), pct(lag, 0.1),
                pct(lag, 0.9), matched, rxEvents);
        } else if (rxEvents) {
            std::printf("    событий RX %zu, но совпадений с XInput нет (значения в Windows другие?)\n", rxEvents);
        } else {
            std::printf("    событий RX нет: правый стик не двигали, или события режет брандмауэр (setup_firewall.bat), или поток не работает\n");
        }
        if (!csv.empty()) {
            if (FILE* f = std::fopen(csv.c_str(), "w")) {
                std::fprintf(f, "t_us,kind,id,value\n");
                for (const auto& e : b.events) std::fprintf(f, "%.1f,%u,%u,%d\n", e.t - t0, e.kind, e.id, e.kind == 1 && e.id >= 12 ? static_cast<int16_t>(e.value) : e.value);
                std::fclose(f);
                std::lock_guard<std::mutex> lock(g_xiMutex);
                const std::string xp = csv + ".xinput.csv";
                if (FILE* g = std::fopen(xp.c_str(), "w")) {
                    std::fprintf(g, "t_us,rx,ry\n");
                    for (const auto& r : g_xi) std::fprintf(g, "%.1f,%d,%d\n", r.t - t0, r.rx, r.ry);
                    std::fclose(g);
                }
            }
        }
    } else if (pollRate > 0) {
        std::printf("== 2. Опрос GET 0x54 %.0f/с, %.0f с. Стики можно крутить ==\n", pollRate, dur);
        const uint64_t xi0 = g_xiPackets.load();
        const double t0 = nowUs(), step = 1e6 / pollRate;
        uint64_t sent = 0, ok = 0, ff = 0, lost = 0, fresh = 0;
        uint32_t prevSeq = 0;
        bool have = false;
        std::vector<double> rtt;
        double next = t0;
        while (nowUs() < t0 + dur * 1e6) {
            while (nowUs() < next) {
                if (next - nowUs() > 1500) Sleep(1);
                else YieldProcessor();
            }
            next += step;
            const double a = nowUs();
            auto r = b.ask(0x54, {}, 30000.0);
            ++sent;
            if (!r) {
                ++lost;
                continue;
            }
            rtt.push_back((nowUs() - a) / 1000.0);
            if (r->size() == 1 && (*r)[0] == 0xFF) {
                ++ff;
                continue;
            }
            const Snap s = parse54(r);
            if (!s.ok) continue;
            ++ok;
            if (have && s.seq != prevSeq) ++fresh;
            prevSeq = s.seq;
            have = true;
        }
        const double span = (nowUs() - t0) / 1e6;
        const uint64_t xiN = g_xiPackets.load() - xi0;
        std::printf("    запросов %llu (%.0f/с), ответов %llu, FF %llu, без ответа %llu, новых снимков %llu; RTT медиана %.2f мс, p99 %.2f; XInput %.0f пакетов/с\n",
            static_cast<unsigned long long>(sent), sent / span, static_cast<unsigned long long>(ok), static_cast<unsigned long long>(ff), static_cast<unsigned long long>(lost),
            static_cast<unsigned long long>(fresh), pct(rtt, 0.5), pct(rtt, 0.99), xiN / span);
    }
    g_stop = true;
    xi.join();
    timeEndPeriod(1);
    return 0;
}
