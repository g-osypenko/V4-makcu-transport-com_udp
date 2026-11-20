// pad_bench_com.cpp - бенчмарк эталона геймпада по COM: makcu_gamepad_raw_transport.h + makcu_gamepad_renderer.h.
//
//   pad_bench_com.exe [--port COM5] [--live PREFIX [--from S] [--len S]] [--out PREFIX] [--no-latency]
//
// Что меряет:
//   1. скорость PadTextureRenderer::step() (без железа): нс на такт;
//   2. чтение физического геймпада: опросов 0x54 в секунду, новых отчётов, пропущенных, RTT (2 с);
//   3. задержку SET 0x40 -> Windows (XInput): ступенька 12000 и отпускание нулями, 5 повторов;
//   4. текстуру (нужен --live): из записи живой руки pad_probe_record (PREFIX_board.csv, PREFIX_xinput.csv) берётся
//      участок [from, from + len), его сглаживание (гаусс, СКО 20 мс) - "чистая математическая кривая". Она подаётся
//      плате тремя способами: как есть, с одним опережением (рендерер без текстуры) и через полный рендерер.
//      По тому, что получил Windows, считается PadTexture19 и расстояние W1 до живой руки.
//
// Во время пунктов 3 и 4 ГЕЙМПАД НЕ ТРОГАТЬ: правый стик двигает программа. Окна нет.
//
// Сборка: build_example.bat
#include "makcu_gamepad_raw_transport.h"
#include "makcu_gamepad_renderer.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Q = makcu::detail::Qpc;
using makcu::PadTexture19;

#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);

struct XiRow {
    int64_t t;
    int16_t rx, ry;
    uint16_t buttons;
    uint8_t lt, rt;
};
std::vector<XiRow> g_xi;
std::atomic<bool> g_stop{false}, g_log{false}, g_xiOk{false};

void xinputLoop() {
    HMODULE dll = LoadLibraryA("xinput1_4.dll");
    if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
    if (!dll) return;
    const auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
            continue;
        }
        const int64_t t = Q::now();
        g_xiOk = true;
        if (!have || s.pkt != lastPkt) {
            have = true;
            lastPkt = s.pkt;
            if (g_log.load(std::memory_order_relaxed)) g_xi.push_back({t, s.rx, s.ry, s.buttons, s.lt, s.rt});
        }
        const int64_t until = t + Q::fromUs(120.0);
        while (Q::now() < until) YieldProcessor();
    }
}

void waitUs(double us) {
    const int64_t until = Q::now() + Q::fromUs(us);
    while (Q::now() < until) {
        if (Q::toUs(until - Q::now()) > 2000.0) Sleep(1);
        else YieldProcessor();
    }
}

// Значение ряда (t, v) на момент at: последний отсчёт не позже at.
template <class F>
double zoh(const std::vector<XiRow>& rows, int64_t at, F field) {
    size_t lo = 0, hi = rows.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (rows[mid].t <= at) lo = mid + 1;
        else hi = mid;
    }
    return lo ? field(rows[lo - 1]) : field(rows.front());
}

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
        for (size_t c = 0; c < idx.size(); ++c) {
            cols[c].push_back(static_cast<size_t>(idx[c]) < cells.size() ? std::atof(cells[static_cast<size_t>(idx[c])].c_str()) : 0.0);
        }
    }
    std::fclose(f);
    return !cols[0].empty();
}

// Ряд (t_us, v) на сетку 1 мс от t0 длиной n.
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

struct Variant {
    std::string name;
    std::vector<XiRow> rows;  // что получил Windows
    int64_t t0 = 0;           // когда задан отсчёт цели 0
    double lagMs = 0.0, errMean = 0.0, errP99 = 0.0;
    std::vector<PadTexture19::Row> feat;
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string port, live, outPrefix;
    double from = 17.5, len = 7.0;
    double periodUs = 0.0;  // 0 - период транспорта по умолчанию (2000: сглаживание платы Auto). При Off ставить 1000
    double kappa = -1.0;    // < 0 - опережение из пресета (1.0, под Auto). При Off ставить 0
    bool latency = true;
    bool off = false;       // --off: пресет рендерера под сглаживание платы Off
    double blendIn = -1.0, blendOut = -1.0;  // --blend: плавный вход и выход транспорта; < 0 - по настройкам платы
    int smoothing = -1;     // --set-smoothing: поставить интерполяцию платы кодом (0 Off, 1 Fixed, 2 Auto)
    bool keep = false;      // --keep: не возвращать прежнюю интерполяцию при выходе
    bool buttons = false;   // --buttons: раздел 5, инжект кнопок и триггеров
    bool holdOnly = false;  // --hold: раздел 5 без серии tap()
    double pollUs = 0.0;    // --poll US: период опроса 0x54 (setPhysicalPollUs); 0 - по умолчанию транспорта (с ревизии 3: max(2000, интервал геймпада), до неё 500)
    int bufferMs = -1;      // --buffer MS: буфер сглаживания платы (setSmoothingBuffer), вместе с --set-smoothing
    bool mid2 = false;      // --mid2: рендерер "АЦП раз в команду, середину ставит плата" (опыт Fixed + буфер 2 мс)
    bool ramp = false;      // --ramp: раздел 6, пила стиком и приращения пакетов Windows (как плата делит шаг команды)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--live" && more) live = argv[++i];
        else if (a == "--from" && more) from = std::atof(argv[++i]);
        else if (a == "--len" && more) len = std::atof(argv[++i]);
        else if (a == "--out" && more) outPrefix = argv[++i];
        else if (a == "--no-latency") latency = false;
        else if (a == "--period" && more) periodUs = std::atof(argv[++i]);
        else if (a == "--kappa" && more) kappa = std::atof(argv[++i]);
        else if (a == "--blend" && more) {
            // плавный вход и выход транспорта, мс: "16,24"; "0,0" - выключить (скачки, как отдаёт плата при Off)
            if (std::sscanf(argv[++i], "%lf,%lf", &blendIn, &blendOut) != 2) blendIn = blendOut = -1.0;
        } else if (a == "--set-smoothing" && more) {
            const std::string m = argv[++i];
            smoothing = m == "off" ? 0 : m == "fixed" ? 1 : m == "auto" ? 2 : -1;
        } else if (a == "--keep") keep = true;
        else if (a == "--buttons") buttons = true;
        else if (a == "--hold") buttons = holdOnly = true;  // раздел 5 без tap(): только удержание и проверка со стиком
        else if (a == "--poll" && more) pollUs = std::atof(argv[++i]);
        else if (a == "--buffer" && more) bufferMs = std::atoi(argv[++i]);
        else if (a == "--mid2") mid2 = true;
        else if (a == "--ramp") ramp = true;
        else if (a == "--off") {
            // сглаживание платы Off: период команд 1 мс, без опережения, рендерер - presetOff (модель АЦП геймпада)
            off = true;
            if (periodUs <= 0) periodUs = 1000.0;
            if (kappa < 0) kappa = 0.0;
        } else {
            std::printf("pad_bench_com.exe [--port COM5] [--live PREFIX [--from S] [--len S]] [--out PREFIX] [--no-latency] [--off] [--period US] [--kappa K]\n"
                        "                  [--blend IN_MS,OUT_MS] [--set-smoothing off|fixed|auto [--buffer MS] [--keep]] [--buttons | --hold]\n"
                        "                  [--poll US] [--ramp] [--mid2]\n");
            return 2;
        }
    }

    // ---- 1. скорость рендерера ----
    std::printf("== 1. PadTextureRenderer::step(), без железа ==\n");
    for (auto preset : {makcu::PadTextureRenderer::Preset::XInput16bit, makcu::PadTextureRenderer::Preset::Sony8bit}) {
        makcu::PadTextureRenderer r;
        r.setProfile(makcu::PadTextureRenderer::preset(preset));
        r.beginEvent(1, 0, -1);
        const int kSteps = 2000000;
        int64_t sum = 0;
        double worst = 0.0;
        const int64_t t0 = Q::now();
        for (int b = 0; b < 200; ++b) {
            const int64_t a = Q::now();
            for (int k = 0; k < kSteps / 200; ++k) {
                const double t = (b * (kSteps / 200) + k) * 0.002;
                int16_t x, y;
                r.step(12000.0 * std::cos(6.28 * t), 12000.0 * std::sin(6.28 * 1.3 * t), x, y);
                sum += x + y;
            }
            worst = std::max(worst, Q::toUs(Q::now() - a) * 1000.0 / (kSteps / 200));
        }
        const double ns = Q::toUs(Q::now() - t0) * 1000.0 / kSteps;
        std::printf("   пресет %s: %.0f нс на такт в среднем, худшая пачка из 10000 тактов %.0f нс (контрольная сумма %lld)\n",
            preset == makcu::PadTextureRenderer::Preset::XInput16bit ? "XInput 16 бит" : "Sony 8 бит (сетка 257, шум, люфт)", ns, worst, static_cast<long long>(sum));
    }
    std::printf("   размер состояния рендерера: %zu байт, профиля: %zu байт\n", sizeof(makcu::PadTextureRenderer), sizeof(makcu::PadRendererProfile));

    makcu::GamepadRawTransport tx;
    if (smoothing >= 0) {
        tx.setSmoothing(static_cast<uint8_t>(smoothing));  // до open(): применится при открытии
        tx.keepSmoothingOnClose(keep);
    }
    if (blendIn >= 0 && blendOut >= 0) tx.setBlendMs(blendIn, blendOut);
    if (bufferMs > 0) {
        tx.setSmoothingBuffer(bufferMs);
        tx.keepSmoothingOnClose(keep);
    }
    if (pollUs > 0) tx.setPhysicalPollUs(pollUs);
    std::string error;
    if (!tx.open(port, error)) {
        std::printf("[-] %s\n", error.c_str());
        return 1;
    }
    std::printf("[+] %s\n", tx.info().c_str());
    std::printf("[i] плавный вход %.0f мс, выход %.0f мс; записей настроек %llu%s\n", tx.blendEnterMs(), tx.blendExitMs(),
        static_cast<unsigned long long>(tx.stats().settingsWrites), smoothing >= 0 ? (keep ? "; интерполяция останется (--keep)" : "; при выходе интерполяция вернётся") : "");
    if (tx.controllerKind() == 0) {
        std::printf("[-] плата не видит геймпад на USB3\n");
        return 1;
    }
    if (periodUs > 0) tx.setCommandPeriodUs(periodUs);
    periodUs = tx.commandPeriodUs();
    std::printf("[i] период команд %.0f мкс, опережение рендерера %s\n", periodUs, kappa < 0 ? "из пресета (1.0)" : std::to_string(kappa).c_str());
    std::thread xi(xinputLoop);
    g_xi.reserve(400000);

    // ---- 2. чтение физики ----
    std::printf("== 2. Чтение физического геймпада (0x54), 2 с без инжекта ==\n");
    Sleep(300);
    tx.resetStats();
    Sleep(2000);
    {
        const auto s = tx.stats();
        std::printf("   запросов %.0f/с, новых отчётов %.0f/с, пропущено отчётов %llu, отказов FF %llu, без ответа %llu, такт опоздал до %.0f мкс\n", s.polls / 2.0,
            s.newSamples / 2.0, static_cast<unsigned long long>(s.skipped), static_cast<unsigned long long>(s.rejects), static_cast<unsigned long long>(s.lost),
            s.tickLateMaxUs);
    }
    if (!g_xiOk) std::printf("[!] Windows не видит геймпад в XInput: пункты 3 и 4 останутся без данных\n");

    // ---- 3. задержка ----
    if (latency && g_xiOk) {
        std::printf("== 3. Задержка SET 0x40 -> Windows: ступенька (12000, -1) и отпускание, 5 повторов ==\n");
        double first = 0, half = 0, full = 0, relFirst = 0, relFull = 0;
        int n = 0;
        for (int rep = 0; rep < 5; ++rep) {
            waitUs(300000.0 + rep * 317.0);
            g_xi.clear();
            g_log = true;
            waitUs(20000.0);
            const int64_t t0 = Q::now();
            tx.setRightStick(12000, -1);
            waitUs(300000.0);
            const int64_t t1 = Q::now();
            tx.releaseRightStick();
            waitUs(100000.0);
            g_log = false;
            Sleep(5);
            // Покой стика - не обязательно 0: 2026-11-14 вечером GameSir в покое давал rx -638. Точка отсчёта - последний
            // пакет до ступеньки; "изменилось" и "вернулось в покой" - с допуском kTol.
            const int kTol = 16;
            int rest = 0;
            for (const auto& r : g_xi) {
                if (r.t < t0) rest = r.rx;
            }
            double f = -1, h = -1, fu = -1, rf = -1, rfu = -1;
            int jumpIn = 0, jumpOut = 0, prevRx = rest;  // наибольшее изменение между соседними пакетами: вход и выход
            for (const auto& r : g_xi) {
                const double ms = Q::toUs(r.t - t0) / 1000.0, rms = Q::toUs(r.t - t1) / 1000.0;
                if (ms >= 0) (rms >= 0 ? jumpOut : jumpIn) = std::max(rms >= 0 ? jumpOut : jumpIn, std::abs(r.rx - prevRx));
                prevRx = r.rx;
                if (ms >= 0 && f < 0 && std::abs(r.rx - rest) > kTol) f = ms;
                if (ms >= 0 && h < 0 && r.rx >= rest + (12000 - rest) / 2) h = ms;
                if (ms >= 0 && fu < 0 && r.rx == 12000) fu = ms;
                if (rms >= 0 && rf < 0 && r.rx != 12000) rf = rms;
                if (rms >= 0 && rf >= 0 && rfu < 0 && std::abs(r.rx - rest) <= kTol) rfu = rms;
            }
            if (rep == 0) std::printf("   покой правого стика по X в Windows: %d\n", rest);
            std::printf("   повтор %d: первое изменение %.2f мс, 50 %% %.2f мс, 100 %% %.2f мс | отпускание: первое изменение %.2f мс, покой %.2f мс | "
                        "наибольший шаг между пакетами: вход %d, выход %d\n",
                rep + 1, f, h, fu, rf, rfu, jumpIn, jumpOut);
            if (f >= 0 && fu >= 0 && rfu >= 0) {
                first += f;
                half += h;
                full += fu;
                relFirst += rf;
                relFull += rfu;
                ++n;
            }
        }
        if (n) {
            std::printf("   среднее по %d: первое изменение %.2f мс, 50 %% %.2f мс, 100 %% %.2f мс | отпускание: первое изменение %.2f мс, покой %.2f мс\n", n, first / n,
                half / n, full / n, relFirst / n, relFull / n);
            std::printf("   (время отсчитывается от вызова setRightStick(); команда уходит на ближайшем такте транспорта, до 1 мс позже)\n");
        }
    }

    // ---- 5. кнопки и триггеры (печатается до раздела 4, если задан --buttons) ----
    if (buttons && g_xiOk) {
        std::printf("== 5. Кнопки и триггеры: tap() -> Windows (XInput), по 3 повтора. ГЕЙМПАД НЕ ТРОГАТЬ ==\n");
        using Id = makcu::MakcuControlId;
        struct Probe {
            const char* name;
            Id id;
            int holdMs;
            uint16_t value;  // глубина триггера
            WORD mask;       // бит XInput для кнопки
            int trig;        // 0 - кнопка, 1 - LT, 2 - RT
        };
        const Probe probes[] = {{"RT 1023", Id::RightTrigger, 100, 1023, 0, 2}, {"LT 1023", Id::LeftTrigger, 100, 1023, 0, 1}, {"RT 512", Id::RightTrigger, 100, 512, 0, 2},
            {"RT 256", Id::RightTrigger, 100, 256, 0, 2}, {"RT 4", Id::RightTrigger, 100, 4, 0, 2}, {"A (SOUTH)", Id::South, 60, 0, 0x1000, 0},
            {"B (EAST)", Id::East, 60, 0, 0x2000, 0}, {"RB", Id::RightShoulder, 60, 0, 0x0200, 0}, {"R3", Id::RightStickButton, 20, 0, 0x0080, 0},
            {"DPAD_UP", Id::DpadUp, 20, 0, 0x0001, 0}};
        auto level = [](const XiRow& r, const Probe& p) { return p.trig == 2 ? static_cast<int>(r.rt) : p.trig == 1 ? static_cast<int>(r.lt) : ((r.buttons & p.mask) ? 1 : 0); };
        if (!holdOnly) {
        for (const Probe& p : probes) {
            for (int rep = 0; rep < 3; ++rep) {
                waitUs(120000.0 + rep * 317.0);
                g_xi.clear();
                g_log = true;
                waitUs(20000.0);
                const int64_t t0 = Q::now();
                tx.tap(p.id, p.holdMs, p.value);
                waitUs((p.holdMs + 120) * 1000.0);
                g_log = false;
                Sleep(5);
                double on = -1, offT = -1;
                int peak = 0, steps = 0, prev = 0;  // steps - сколько разных ненулевых значений на подъёме (ступенька или плавный ход)
                for (const auto& r : g_xi) {
                    const double ms = Q::toUs(r.t - t0) / 1000.0;
                    if (ms < 0) continue;
                    const int v = level(r, p);
                    if (v > 0 && on < 0) on = ms;
                    if (on >= 0 && offT < 0 && v == 0) offT = ms;
                    if (offT < 0 && v != prev && v > 0) ++steps;
                    prev = v;
                    peak = std::max(peak, v);
                }
                std::printf("   %-10s повтор %d: нажатие через %.2f мс, отпускание через %.2f мс (держали %d мс, в Windows %.2f мс), значение %d, ступеней подъёма %d\n", p.name,
                    rep + 1, on, offT, p.holdMs, (on >= 0 && offT >= 0) ? offT - on : -1.0, peak, steps);
            }
        }
        }  // конец блока tap() (при --hold пропускается)
        // Удержание 3.2 с: только RT, только A, RT и A вместе. Проверка сторожа молчания платы (1 с) и "уступки"
        // (2 с у стика). 2026-11-15 при первом прогоне (RT и A вместе) геймпад пропал с платы примерно на 2-й секунде
        // удержания и сам вернулся через ~30 с; поэтому после каждого варианта проверяется, на месте ли геймпад, и
        // печатается, когда пришёл последний пакет Windows.
        bool padGone = false;
        for (int variant = 0; variant < 3 && !padGone; ++variant) {
            const bool useRt = variant != 1, useA = variant != 0;
            waitUs(500000.0);
            g_xi.clear();
            g_log = true;
            waitUs(20000.0);
            const int64_t t0 = Q::now();
            if (useRt) tx.setTrigger(Id::RightTrigger, 1023);
            if (useA) tx.setButton(Id::South, true);
            waitUs(3200000.0);
            const int64_t t1 = Q::now();
            tx.setTrigger(Id::RightTrigger, 0);
            tx.setButton(Id::South, false);
            waitUs(150000.0);
            g_log = false;
            Sleep(5);
            int rtMin = 255, aDrops = 0;
            double rtLost = -1, aLost = -1, rtOff = -1, aOff = -1, lastPkt = -1;
            for (const auto& r : g_xi) {
                const double ms = Q::toUs(r.t - t0) / 1000.0, rel = Q::toUs(r.t - t1) / 1000.0;
                lastPkt = ms;
                if (ms > 30.0 && rel < 0) {
                    rtMin = std::min<int>(rtMin, r.rt);
                    if (r.rt < 255 && rtLost < 0) rtLost = ms;
                    if (!(r.buttons & 0x1000)) {
                        ++aDrops;
                        if (aLost < 0) aLost = ms;
                    }
                }
                if (rel >= 0 && rtOff < 0 && r.rt == 0) rtOff = rel;
                if (rel >= 0 && aOff < 0 && !(r.buttons & 0x1000)) aOff = rel;
            }
            const makcu::PadPhysical ph = tx.physical();
            padGone = !ph.live || tx.controllerKind() == 0 || lastPkt < 3300.0;
            std::printf("   удержание 3.2 с, %s:", variant == 0 ? "только RT" : variant == 1 ? "только A" : "RT и A вместе");
            if (useRt) std::printf(" RT наименьшее значение %d%s, после отпускания 0 через %.2f мс;", rtMin,
                rtLost < 0 ? " (держался всё время)" : (" (упал на " + std::to_string(rtLost) + " мс)").c_str(), rtOff);
            if (useA) std::printf(" A отпускалась в %d пакетах%s, после отпускания 0 через %.2f мс;", aDrops,
                aLost < 0 ? " (держалась всё время)" : (" (первый раз на " + std::to_string(aLost) + " мс)").c_str(), aOff);
            std::printf(" последний пакет Windows на %.0f мс; геймпад на плате: %s\n", lastPkt, padGone ? "ПРОПАЛ" : "на месте");
        }
        if (padGone) std::printf("[!] геймпад пропал во время удержания: проверка со стиком пропущена\n");
        // Вместе со стиком: круг 1.6 с и tap(RT, 80 мс) каждые 250 мс. Кнопки едут в командах стика.
        if (!padGone) {
            tx.resetStats();
            g_xi.clear();
            g_log = true;
            const makcu::PadPhysical ph = tx.physical();
            tx.setRightStick(ph.rx, ph.ry);
            const uint64_t c0 = tx.commandCount();
            const double stepUs = tx.commandPeriodUs();
            int taps = 0;
            for (;;) {
                if (!tx.waitCommand(50)) break;
                const double t = static_cast<double>(tx.commandCount() - c0 + 1) * stepUs / 1e6;
                if (t >= 1.6) break;
                if (t >= 0.1 + 0.25 * taps && taps < 6) {
                    tx.tap(Id::RightTrigger, 80);
                    ++taps;
                }
                const double r = 12000.0 * std::min(1.0, t / 0.25);
                tx.setRightStick(static_cast<int16_t>(std::lround(r * std::cos(6.2831853 * t))), static_cast<int16_t>(std::lround(r * std::sin(6.2831853 * t))));
            }
            tx.releaseRightStick(200);
            waitUs(150000.0);
            g_log = false;
            Sleep(5);
            int seen = 0, prev = 0;
            double durSum = 0, onT = 0;
            for (const auto& r : g_xi) {
                if (r.rt > 0 && prev == 0) {
                    ++seen;
                    onT = Q::toUs(r.t) / 1000.0;
                }
                if (r.rt == 0 && prev > 0) durSum += Q::toUs(r.t) / 1000.0 - onT;
                prev = r.rt;
            }
            const auto s = tx.stats();
            std::printf("   со стиком (круг 1.6 с): tap(RT, 80 мс) задано %d, в Windows нажатий %d, средняя длительность %.1f мс; интервал команд стика %.0f/%.0f мкс\n", taps,
                seen, seen ? durSum / seen : 0.0, s.cmdIntervalAvgUs, s.cmdIntervalMaxUs);
        }
        std::printf("    %s\n", tx.statsLine().c_str());
    }

    // ---- 6. пила: как плата делит шаг команды между пакетами Windows (печатается до раздела 4, если задан --ramp) ----
    // Пила по X между -8000 и 8000 со скоростью 100 единиц/мс, 4 хода по 160 мс; команда несёт значение пилы на момент
    // отправки. В Windows пакеты идут раз в 1 мс. Если плата держит значение до следующей команды (Off), при периоде
    // 2 мс приращения пакетов чередуются "200, 0"; если ставит середину - "100, 100".
    if (ramp && g_xiOk) {
        std::printf("== 6. Пила 100 единиц/мс, период команд %.0f мкс: приращения X между пакетами Windows. ГЕЙМПАД НЕ ТРОГАТЬ ==\n", periodUs);
        waitUs(300000.0);
        g_xi.clear();
        g_log = true;
        const double stepMs = periodUs / 1000.0;
        tx.setRightStick(-8000, -1);
        tx.waitCommand(50);
        waitUs(60000.0);  // дойти до начала пилы
        const uint64_t c0 = tx.commandCount();
        const int64_t t0 = Q::now();
        for (;;) {
            if (!tx.waitCommand(50)) break;
            const double ms = static_cast<double>(tx.commandCount() - c0) * stepMs;
            if (ms >= 640.0) break;
            const double ph = std::fmod(ms, 320.0);
            const double x = ph < 160.0 ? -8000.0 + 100.0 * ph : 8000.0 - 100.0 * (ph - 160.0);
            tx.setRightStick(static_cast<int16_t>(std::lround(x)), -1);
        }
        const int64_t t1 = Q::now();
        tx.releaseRightStick(200);
        g_log = false;
        Sleep(5);
        // приращения на ходах вверх (20..140 мс каждого периода 320 мс): гистограмма и первые 40 подряд
        std::vector<std::pair<int, int>> hist;  // значение приращения, сколько раз
        std::vector<int> firstDeltas;
        std::vector<double> firstDt;
        int prevRx = 0;
        int64_t prevT = 0;
        bool havePrev = false;
        size_t packets = 0;
        for (const auto& r : g_xi) {
            if (r.t < t0 || r.t >= t1) continue;
            ++packets;
            const double ms = Q::toUs(r.t - t0) / 1000.0;
            const double ph = std::fmod(ms, 320.0);
            if (havePrev && ph >= 20.0 && ph < 140.0) {
                const int d = r.rx - prevRx;
                auto it = std::find_if(hist.begin(), hist.end(), [&](const auto& h) { return h.first == d; });
                if (it == hist.end()) hist.push_back({d, 1});
                else ++it->second;
                if (firstDeltas.size() < 40) {
                    firstDeltas.push_back(d);
                    firstDt.push_back(Q::toUs(r.t - prevT) / 1000.0);
                }
            }
            prevRx = r.rx;
            prevT = r.t;
            havePrev = true;
        }
        std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("   пакетов Windows за пилу: %zu за %.0f мс\n   первые приращения на ходе вверх:", packets, Q::toUs(t1 - t0) / 1000.0);
        for (int d : firstDeltas) std::printf(" %d", d);
        std::printf("\n   интервалы этих пакетов, мс:");
        for (double d : firstDt) std::printf(" %.1f", d);
        std::printf("\n   самые частые приращения на ходах вверх (значение x раз):");
        for (size_t i = 0; i < hist.size() && i < 12; ++i) std::printf(" %d x%d", hist[i].first, hist[i].second);
        std::printf("\n    %s\n", tx.statsLine().c_str());
        tx.resetStats();
    }

    // ---- 4. текстура ----
    if (!live.empty() && g_xiOk) {
        std::printf("== 4. Текстура: живая рука против инжекта (%s, с %.1f с, %.1f с) ==\n", live.c_str(), from, len);
        std::vector<std::vector<double>> b, x;
        if (!loadColumns(live + "_board.csv", {"t_us", "rx", "ry"}, b) || !loadColumns(live + "_xinput.csv", {"t_us", "rx", "ry"}, x)) {
            std::printf("[-] не прочитаны %s_board.csv / %s_xinput.csv\n", live.c_str(), live.c_str());
        } else {
            const size_t n = static_cast<size_t>(len * 1000.0);
            const double t0b = b[0][0] + from * 1e6, t0x = x[0][0] + from * 1e6;
            const std::vector<double> bx = onGrid(b[0], b[1], t0b, n), by = onGrid(b[0], b[2], t0b, n);
            const std::vector<double> sx = gauss(bx, 20.0), sy = gauss(by, 20.0);
            // Живая рука: пакеты XInput участка как есть, по строке на пакет - так же, как ниже берётся инжект и как
            // считает analyze_replay.py (сверка C++ с Python: pad_probes/pad_texture_check).
            std::vector<int16_t> liveX, liveY;
            for (size_t i = 0; i < x[0].size(); ++i) {
                if (x[0][i] >= t0x && x[0][i] < t0x + len * 1e6) {
                    liveX.push_back(static_cast<int16_t>(x[1][i]));
                    liveY.push_back(static_cast<int16_t>(x[2][i]));
                }
            }
            const auto liveFeat = PadTexture19::windows(liveX.data(), liveY.data(), liveX.size());

            makcu::PadRendererProfile full =
                (off || mid2) ? makcu::PadTextureRenderer::presetOff(tx.controllerKind()) : makcu::PadTextureRenderer::presetFor(tx.controllerKind());
            if (mid2) {
                // Опыт "Fixed + буфер 2 мс": команда раз в 2 мс несёт выборку АЦП, промежуточный отчёт (середину) должна
                // поставить плата. Поэтому АЦП берёт выборку на каждом такте рендерера и середину сам не выдаёт.
                full.regime.adc_period_ticks = 1;
                full.regime.adc_midpoint = false;
                full.regime.adc_stale_start = 0.0f;  // пачки "без середины" без середины не имеют смысла
            }
            makcu::PadRendererProfile leadOnly = full;
            leadOnly.regime.raw_lsb = 0.0f;  // "кривая + опережение" - без модели АЦП геймпада, она только в варианте "рендерер"
            leadOnly.tremor_rms = leadOnly.tremor_rms_per_speed = leadOnly.tremor_rms_max = 0.0f;
            leadOnly.resonance_rms = leadOnly.resonance_rms_per_speed = leadOnly.resonance_rms_max = 0.0f;
            leadOnly.cross_axis = 0.0f;
            if (kappa >= 0) full.kappa_lead = leadOnly.kappa_lead = static_cast<float>(kappa);
            full.tick_hz = leadOnly.tick_hz = static_cast<float>(1e6 / periodUs);  // такт рендерера = период команд
            leadOnly.finalize();
            full.finalize();
            // Четвёртый вариант - запись руки как есть (снимки 0x54 живого стика на сетке 1 мс, без сглаживания и без
            // рендерера): показывает, что транспорт и плата доносят до Windows, если подать им настоящую текстуру.
            std::vector<Variant> vars(4);
            vars[0].name = "кривая";
            vars[1].name = "кривая+опереж.";
            vars[2].name = "рендерер";
            vars[3].name = "запись руки";
            const double stepMs = periodUs / 1000.0;  // сколько отсчётов цели (сетка 1 мс) на одну команду
            const size_t cmds = static_cast<size_t>(static_cast<double>(n) / stepMs);
            auto at = [&](size_t k) { return std::min(n - 1, static_cast<size_t>(std::llround(static_cast<double>(k) * stepMs))); };
            for (size_t v = 0; v < vars.size(); ++v) {
                makcu::PadTextureRenderer r;
                r.setProfile(v == 2 ? full : leadOnly);
                waitUs(400000.0);
                const makcu::PadPhysical ph = tx.physical();
                r.beginEvent(0xC0FFEEull + v, ph.rx, ph.ry);
                g_xi.clear();
                g_log = true;
                // Первая команда - текущее положение; дальше значение на каждую отправленную команду.
                // Номер отсчёта цели берётся из счётчика команд транспорта (commandCount()), а не из числа оборотов
                // этого цикла: если поток бенчмарка опоздал и транспорт успел отправить несколько команд с прежним
                // значением, цикл пропускает столько же отсчётов (рендерер при этом шагает по каждому, чтобы его
                // резонаторы жили в своём времени). Прогон 2026-11-14 без этого: поток отставал на 20-30 мс, цель
                // съезжала по времени и ошибка слежения росла с ~30 до тысяч единиц.
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                tx.setRightStick(ph.rx, ph.ry);
                bool ok = tx.waitCommand(50);
                const uint64_t c0 = tx.commandCount();
                vars[v].t0 = Q::now();
                size_t done = 0, missed = 0, worstMiss = 0;  // done - сколько отсчётов цели уже пройдено
                while (ok && done < cmds) {
                    const size_t want = std::min<size_t>(cmds, static_cast<size_t>(tx.commandCount() - c0) + 1);
                    if (want > done + 1) {
                        missed += want - done - 1;
                        worstMiss = std::max(worstMiss, want - done - 1);
                    }
                    int16_t ox = 0, oy = 0;
                    for (; done < want; ++done) {
                        const size_t i = at(done);
                        const double tgx = sx[i], tgy = sy[i];
                        if (v == 0) {
                            ox = static_cast<int16_t>(std::lround(tgx));
                            oy = static_cast<int16_t>(std::lround(tgy));
                        } else if (v == 3) {
                            ox = static_cast<int16_t>(std::lround(bx[i]));
                            oy = static_cast<int16_t>(std::lround(by[i]));
                        } else {
                            r.step(tgx, tgy, ox, oy);
                        }
                    }
                    tx.setRightStick(ox, oy);
                    ok = tx.waitCommand(50);
                }
                const int64_t tEnd = Q::now();
                tx.releaseRightStick(200);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                g_log = false;
                Sleep(5);
                if (!ok) std::printf("[-] %s: транспорт не отправил команду за 50 мс\n", vars[v].name.c_str());
                std::printf("   %-16s цикл бенчмарка пропустил команд: %zu (подряд до %zu), длительность %.1f мс на %zu команд\n", vars[v].name.c_str(), missed, worstMiss,
                    Q::toUs(tEnd - vars[v].t0) / 1000.0, cmds);
                for (const auto& row : g_xi) {
                    if (row.t >= vars[v].t0 && row.t < tEnd) vars[v].rows.push_back(row);
                }
                // слежение: сдвиг с наименьшей средней ошибкой против цели (в координатах платы: Y = -Yw - 1)
                double best = 1e18;
                std::vector<double> errs;
                for (int lag = 0; lag <= 80; ++lag) {
                    double acc = 0.0;
                    size_t cnt = 0;
                    std::vector<double> e;
                    for (double at = 100000.0; at < len * 1e6 - 30000.0; at += 250.0) {
                        const double src = at - lag * 250.0;
                        if (src < 0) continue;
                        const size_t si = static_cast<size_t>(src / 1000.0);
                        if (si >= n) break;
                        const int64_t tq = vars[v].t0 + Q::fromUs(at);
                        const double wx = zoh(vars[v].rows, tq, [](const XiRow& q) { return static_cast<double>(q.rx); });
                        const double wy = -zoh(vars[v].rows, tq, [](const XiRow& q) { return static_cast<double>(q.ry); }) - 1.0;
                        const double d = std::hypot(wx - sx[si], wy - sy[si]);
                        acc += d;
                        e.push_back(d);
                        ++cnt;
                    }
                    if (cnt && acc / cnt < best) {
                        best = acc / cnt;
                        vars[v].lagMs = lag * 0.25;
                        errs.swap(e);
                    }
                }
                vars[v].errMean = best;
                if (!errs.empty()) {
                    std::sort(errs.begin(), errs.end());
                    vars[v].errP99 = errs[errs.size() * 99 / 100];
                }
                std::vector<int16_t> wx(vars[v].rows.size()), wy(vars[v].rows.size());
                for (size_t i = 0; i < wx.size(); ++i) {
                    wx[i] = vars[v].rows[i].rx;
                    wy[i] = vars[v].rows[i].ry;
                }
                vars[v].feat = PadTexture19::windows(wx.data(), wy.data(), wx.size());
                const auto s = tx.stats();
                std::printf("   %-16s пакетов Windows %zu за %.1f с; отставание от цели %.2f мс, ошибка средняя %.1f, p99 %.0f; интервал команд %.0f/%.0f мкс\n",
                    vars[v].name.c_str(), vars[v].rows.size(), len, vars[v].lagMs, vars[v].errMean, vars[v].errP99, s.cmdIntervalAvgUs, s.cmdIntervalMaxUs);
                tx.resetStats();
                if (!outPrefix.empty()) {
                    const std::string path = outPrefix + "_" + std::to_string(v) + ".csv";
                    if (FILE* f = std::fopen(path.c_str(), "w")) {
                        std::fprintf(f, "t_us,rx,ry\n");
                        for (const auto& row : vars[v].rows) std::fprintf(f, "%.1f,%d,%d\n", Q::toUs(row.t - vars[v].t0), row.rx, row.ry);
                        std::fclose(f);
                    }
                }
            }
            auto median = [](const std::vector<PadTexture19::Row>& rows, int c) {
                std::vector<double> v;
                for (const auto& r : rows) v.push_back(r[static_cast<size_t>(c)]);
                if (v.empty()) return 0.0;
                std::sort(v.begin(), v.end());
                return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
            };
            std::printf("   PadTexture19, медиана по окнам 512 отчётов (окон: рука %zu", liveFeat.size());
            for (const auto& v : vars) std::printf(", %s %zu", v.name.c_str(), v.feat.size());
            std::printf(")\n   %-26s %10s", "описатель", "рука");
            for (const auto& v : vars) std::printf(" %15s", v.name.c_str());
            std::printf("  |  W1 до руки:");
            for (const auto& v : vars) std::printf(" %15s", v.name.c_str());
            std::printf("\n");
            std::vector<PadTexture19::Row> w(vars.size());
            std::vector<double> tot(vars.size(), 0.0);
            for (size_t v = 0; v < vars.size(); ++v) w[v] = PadTexture19::w1(liveFeat, vars[v].feat, &tot[v]);
            for (int c = 0; c < PadTexture19::kCount; ++c) {
                std::printf("   %-26s %10.3f", PadTexture19::name(c), median(liveFeat, c));
                for (const auto& v : vars) std::printf(" %15.3f", median(v.feat, c));
                std::printf("  |             ");
                for (size_t v = 0; v < vars.size(); ++v) std::printf(" %15.3f", w[v][static_cast<size_t>(c)]);
                std::printf("\n");
            }
            std::printf("   %-26s %10s", "СУММА W1", "");
            for (size_t v = 0; v < vars.size(); ++v) std::printf(" %15s", "");
            std::printf("  |             ");
            for (size_t v = 0; v < vars.size(); ++v) std::printf(" %15.3f", tot[v]);
            std::printf("\n");
        }
    }
    g_stop = true;
    xi.join();
    const bool released = tx.releaseRightStick(200);
    std::printf("[%c] стик отпущен, GET 0x40 подтверждён: %s\n", released ? '+' : '-', released ? "да" : "НЕТ");
    std::printf("    %s\n", tx.statsLine().c_str());
    tx.close();
    if (tx.smoothingRestoreFailed()) std::printf("[!] сглаживание платы НЕ возвращено (геймпада не было 4 с): на плате осталось заданное\n");
    return 0;
}
