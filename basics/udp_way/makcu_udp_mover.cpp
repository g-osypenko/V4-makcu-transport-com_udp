// MAKCU mover (UDP): тот же стенд, что basics\comport_way, но связь с платой
// только по сети. Общие заголовки (motion_engine.h, substepper.h, abc_renderer.h,
// hook_monitor.h, rawinput_monitor.h, rate_probe.h, mover_common.h) берутся из
// ..\comport_way. Подробности и цифры - README.md рядом.
//
// Сборка: build.bat   (MinGW g++, без SDK: только WinAPI + ws2_32)
#include "makcu_udp_transport.h"
#include "motion_engine.h"
#include "mover_common.h"
#include "abc_renderer.h"
#include "hook_monitor.h"
#include "rawinput_monitor.h"
#include "rate_probe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace mover;

namespace {

// rawmove - 0x69 RAW_MOVE + квитанции 0x6A (один HID-отчёт на запрос, мимо AUTO);
// move    - MAK_API MOVE 0x18 (интерполятор прошивки);
// movenow - MOVE_NOW 0x67 (на 4094 тоже интерполятор).
enum class TxKind { RawMove, Move, MoveNow };

const char* txName(TxKind k) {
    switch (k) {
    case TxKind::RawMove: return "rawmove";
    case TxKind::Move: return "move";
    case TxKind::MoveNow: return "movenow";
    }
    return "?";
}

std::optional<TxKind> parseTx(const std::string& v) {
    for (TxKind k : {TxKind::RawMove, TxKind::Move, TxKind::MoveNow}) {
        if (v == txName(k)) return k;
    }
    return std::nullopt;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

struct Options {
    std::string host = "192.168.50.175";
    uint16_t port = 8080;
    RawUdpTransport::Wire wire = RawUdpTransport::Wire::Raw;
    double gapUs = RawUdpTransport::kDefaultGapUs;
    double burst = RawUdpTransport::kDefaultBurst;
    double hz = 1000.0;
    TxKind tx = TxKind::RawMove;
    std::vector<TxKind> compare;  // --compare: форматы для сравнения
    std::vector<double> rates;    // --rates: частоты команд для --sweep/--compare
    int interp = -1;              // --interp: 0..100 или 255 (AUTO), live через 0x1F; -1 - не трогать
    bool patternSet = false;
    Pattern pattern = Pattern::Circle;
    double speed = 4.0;
    double size = 150.0;
    double durationS = 10.0;
    bool hybridWait = false;
    int core = -1;
    bool keepAccel = false;
    bool abc = false;
    std::string abcModel;
    std::string abcProfile;
    uint64_t abcSeed = 0;
    bool substep = false;
    bool runOnce = false;
    bool check = false;
    bool netProbe = false;
    bool sweep = false;
    double startDelayS = 0.0;
};

std::atomic<bool> g_exit{false};
LiveStats g_live;
PointerAccelGuard g_accel;
HookMonitor g_hook;
AbcRenderer g_abc;
bool g_abcReady = false;
uint64_t g_runCounter = 0;
RawInputMonitor* g_rawmon = nullptr;

std::string exeDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Модель и профиль ABCurves относительно exe (basics\udp_way -> ..\..\ABCurves,
// профиль - ..\comport_way\profiles).
void initAbcurves(const Options& o) {
    const std::string dir = exeDir();
    const std::string model = !o.abcModel.empty() ? o.abcModel
                                                  : dir + "\\..\\..\\ABCurves\\models\\renderer_global_h80.bin";
    const std::string profilePath =
        !o.abcProfile.empty() ? o.abcProfile : dir + "\\..\\comport_way\\profiles\\human_start_256.csv";
    std::string err;
    std::vector<Report16> profile;
    if (!g_abc.loadModel(model, err) || !AbcRenderer::readProfileCsv(profilePath, profile, err) ||
        !g_abc.prepareProfile(profile, err)) {
        std::printf("[!] ABCurves Renderer недоступен: %s\n", err.c_str());
        return;
    }
    g_abcReady = true;
    std::printf("[+] ABCurves Renderer готов: модель %s, профиль %s\n", model.c_str(), profilePath.c_str());
}

int64_t receivedPathAbs() {
    if (!g_rawmon) return 0;
    const auto dev = g_rawmon->primary();
    return dev ? dev->sumAbs : 0;
}

bool pointerAccelOn() {
    int params[3]{};
    return SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0) && params[2] != 0;
}

BOOL WINAPI consoleHandler(DWORD event) {
    g_exit = true;
    g_live.stopRequest = true;
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        // Дать прогону остановиться: runMotion() вызывает tx.finish(), который
        // подтверждает raw-квитанции (иначе они висят на плате до её перезагрузки).
        for (int i = 0; i < 200 && g_live.running.load(); ++i) Sleep(10);
        g_accel.restore();
    }
    return TRUE;
}

void printUsage() {
    std::printf(
        "makcu_udp_mover.exe [опции]\n"
        "  --host IP            адрес MAKCU (по умолчанию 192.168.50.175)\n"
        "  --udp-port N         UDP-порт (8080)\n"
        "  --wire raw|host      обёртка: raw = 0x55 + nonce 8 байт + кадр (по умолчанию, работает на 4094);\n"
        "                       host = голый кадр (на 4094 плата не отвечает)\n"
        "  --gap-us N           минимальный интервал между датаграммами, мкс (по умолчанию 1800: плата\n"
        "                       принимает ~583 датаграммы/с, чаще - теряет)\n"
        "  --burst B            ёмкость ведра темпа, датаграмм (по умолчанию 1 = строго не чаще раза в 2 мс\n"
        "                       на сетке тиков 1 мс, без потерь; 2 = изредка 1 мс: +11%% темпа, ~2%% потерь)\n"
        "  --tx rawmove|move|movenow  формат (по умолчанию rawmove = 0x69 + квитанции 0x6A; move = 0x18,\n"
        "                       movenow = 0x67 - через интерполятор прошивки)\n"
        "  --interp auto|N      интерполяция 0x1F live (255 = AUTO), при выходе вернётся. На raw не влияет\n"
        "  --hz N               частота тиков, Гц (1000)\n"
        "  --compare A,B,..     сравнить форматы на частотах --rates (мышь не трогать)\n"
        "  --rates N,N,..       частоты для --sweep/--compare (по умолчанию 125,250,500,1000)\n"
        "  --pattern circle|eight|line|ramp|wobble|flicks|steps\n"
        "  --speed V --size S --duration S   траектория (4.0 пикс/мс, 150, 10 с)\n"
        "  --abc, --abc-model PATH, --abc-profile PATH, --abc-seed N   прогон через ABCurves Renderer\n"
        "  --substep            прогон через накопитель (шаг 1 мс) + таблица скорость -> частота\n"
        "  --run                один прогон и выход (без меню)\n"
        "  --sweep              перебор частот --rates и выход\n"
        "  --check              связь + Raw API + RTT/потери на запросах 0x6A(id=0), курсор стоит\n"
        "  --netprobe           замер приёма платы: потери/RTT по интервалу датаграмм, курсор стоит\n"
        "  --wait spin|hybrid, --core N, --delay S, --keep-accel   как у COM-стенда\n");
}

bool parseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("Не хватает значения для %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") o.host = next("--host");
        else if (a == "--udp-port") o.port = static_cast<uint16_t>(std::atoi(next("--udp-port")));
        else if (a == "--wire") {
            const std::string v = next("--wire");
            if (v == "raw") o.wire = RawUdpTransport::Wire::Raw;
            else if (v == "host") o.wire = RawUdpTransport::Wire::Host;
            else return false;
        } else if (a == "--gap-us") o.gapUs = std::atof(next("--gap-us"));
        else if (a == "--burst") o.burst = std::atof(next("--burst"));
        else if (a == "--hz") o.hz = std::atof(next("--hz"));
        else if (a == "--tx") {
            const auto k = parseTx(next("--tx"));
            if (!k) return false;
            o.tx = *k;
        } else if (a == "--compare") {
            for (const auto& v : splitList(next("--compare"))) {
                const auto k = parseTx(v);
                if (!k) return false;
                o.compare.push_back(*k);
            }
        } else if (a == "--rates") {
            for (const auto& v : splitList(next("--rates"))) {
                const double hz = std::atof(v.c_str());
                if (hz < 10.0 || hz > 8000.0) return false;
                o.rates.push_back(hz);
            }
        } else if (a == "--interp") {
            const std::string v = next("--interp");
            o.interp = v == "auto" ? 255 : std::atoi(v.c_str());
            if (o.interp != 255 && (o.interp < 0 || o.interp > 100)) return false;
        } else if (a == "--pattern") {
            o.patternSet = true;
            const std::string v = next("--pattern");
            if (v == "circle") o.pattern = Pattern::Circle;
            else if (v == "eight") o.pattern = Pattern::Eight;
            else if (v == "line") o.pattern = Pattern::Line;
            else if (v == "ramp") o.pattern = Pattern::Ramp;
            else if (v == "wobble") o.pattern = Pattern::Wobble;
            else if (v == "flicks") o.pattern = Pattern::Flicks;
            else if (v == "steps") o.pattern = Pattern::Steps;
            else return false;
        } else if (a == "--speed") o.speed = std::atof(next("--speed"));
        else if (a == "--size") o.size = std::atof(next("--size"));
        else if (a == "--duration") o.durationS = std::atof(next("--duration"));
        else if (a == "--wait") o.hybridWait = std::string(next("--wait")) == "hybrid";
        else if (a == "--core") o.core = std::atoi(next("--core"));
        else if (a == "--keep-accel") o.keepAccel = true;
        else if (a == "--abc") o.abc = true;
        else if (a == "--substep") o.substep = true;
        else if (a == "--abc-model") o.abcModel = next("--abc-model");
        else if (a == "--abc-profile") o.abcProfile = next("--abc-profile");
        else if (a == "--abc-seed") o.abcSeed = std::strtoull(next("--abc-seed"), nullptr, 10);
        else if (a == "--run") o.runOnce = true;
        else if (a == "--check") o.check = true;
        else if (a == "--netprobe") o.netProbe = true;
        else if (a == "--sweep") o.sweep = true;
        else if (a == "--delay") o.startDelayS = std::atof(next("--delay"));
        else if (a == "--help" || a == "-h") {
            printUsage();
            std::exit(0);
        } else {
            std::printf("Неизвестная опция: %s\n", a.c_str());
            return false;
        }
    }
    return o.hz >= 10.0 && o.hz <= 8000.0 && o.speed > 0.0 && o.size > 1.0 && o.durationS > 0.0 && o.port != 0 &&
           o.gapUs >= 0.0 && o.burst >= 1.0;
}

// ---------------------------------------------------------------------------
// Связь с MAKCU по UDP: проверка при старте, формат, Raw, интерполяция.
// ---------------------------------------------------------------------------
class MakcuUdpLink {
public:
    bool init(const Options& opt) {
        std::string error;
        std::printf("[*] UDP %s:%u, обёртка %s...\n", opt.host.c_str(), opt.port,
            opt.wire == RawUdpTransport::Wire::Raw ? "RAW (0x55 + nonce)" : "HOST (голый кадр)");
        if (!tx_.open(opt.host, opt.port, opt.wire, error)) {
            std::printf("[-] %s\n", error.c_str());
            return false;
        }
        tx_.setGapUs(opt.gapUs);
        tx_.setBurst(opt.burst);
        std::printf("[+] MAKCU %s: прошивка %u, DEVICE kinds 0x%02X%s | темп датаграмм: в среднем раз в %.0f мкс,"
                    " ведро %.1f\n",
            tx_.endpoint().c_str(), tx_.firmware(), tx_.deviceKinds(), (tx_.deviceKinds() & 1) ? " (mouse OK)" : "",
            tx_.gapUs(), tx_.burst());
        if (!(tx_.deviceKinds() & 1)) {
            std::printf("[-] Маршрут мыши не активен (DEVICE не содержит mouse).\n");
            return false;
        }
        if (auto km = tx_.kmQuery("m.device()")) reportUsbPeriod(*km);
        if (opt.interp >= 0) {
            interpOriginal_ = tx_.getInterp();
            if (!interpOriginal_) {
                std::printf("[!] Интерполяция 0x1F не прочитана - не меняю\n");
            } else if (tx_.setInterp(static_cast<uint8_t>(opt.interp))) {
                interpChanged_ = *interpOriginal_ != opt.interp;
                std::printf("[+] Интерполяция 0x1F: %s -> %s (live, при выходе вернётся)\n",
                    interpName(*interpOriginal_).c_str(), interpName(static_cast<uint8_t>(opt.interp)).c_str());
            } else {
                std::printf("[!] Плата не приняла интерполяцию %s\n", interpName(static_cast<uint8_t>(opt.interp)).c_str());
            }
        } else if (auto v = tx_.getInterp()) {
            std::printf("[+] Интерполяция 0x1F = %s (для 0x18/0x67; на raw не влияет)\n", interpName(*v).c_str());
        }
        return true;
    }

    // Формат для прогона. Переход на rawmove - каждый раз заново prepareRaw()
    // (водяной знак id, дочистка, окно), чтобы не смешивать с очередью AUTO.
    Transport* acquire(TxKind kind) {
        if (!tx_.isOpen()) return nullptr;
        using F = RawUdpTransport::Format;
        const F want = kind == TxKind::RawMove ? F::RawMove : kind == TxKind::Move ? F::Mak : F::MoveNow;
        if (want == F::RawMove && (tx_.format() != F::RawMove || !tx_.healthy())) {
            tx_.setFormat(F::RawMove);
            std::string info;
            if (!tx_.prepareRaw(info)) {
                std::printf("[!] Raw movement 0x69/0x6A недоступен (%s) - использую MOVE 0x18\n", info.c_str());
                tx_.setFormat(F::Mak);
            } else {
                std::printf("[+] Raw movement 0x69/0x6A: %s\n", info.c_str());
            }
        } else if (want != F::RawMove) {
            if (tx_.format() == F::RawMove) tx_.finishRaw(400);
            tx_.setFormat(want);
        }
        return &tx_;
    }

    [[nodiscard]] RawUdpTransport& udp() { return tx_; }

    // Интерполяция по кругу (F11): 0 -> AUTO -> 25 -> 50 -> 100 -> 0.
    void cycleInterp() {
        static constexpr uint8_t kSteps[] = {0, 255, 25, 50, 100};
        if (tx_.format() == RawUdpTransport::Format::RawMove) tx_.finishRaw(400);
        const auto cur = tx_.getInterp();
        if (!cur) {
            std::printf("[!] Интерполяция 0x1F не прочитана\n");
            return;
        }
        if (!interpOriginal_) interpOriginal_ = cur;
        uint8_t next = kSteps[0];
        for (size_t i = 0; i < std::size(kSteps); ++i) {
            if (kSteps[i] == *cur) next = kSteps[(i + 1) % std::size(kSteps)];
        }
        if (tx_.setInterp(next)) {
            interpChanged_ = next != *interpOriginal_;
            std::printf("[+] Интерполяция 0x1F = %s (live; на raw не влияет, при выходе вернётся %s)\n",
                interpName(next).c_str(), interpName(*interpOriginal_).c_str());
        } else {
            std::printf("[!] Плата не приняла интерполяцию %s\n", interpName(next).c_str());
        }
    }

    std::string interpNow() {
        const auto v = tx_.getInterp();
        return v ? interpName(*v) : std::string("?");
    }

    std::string mouseUf() {
        const auto km = tx_.kmQuery("m.device()");
        if (!km) return "?";
        const auto m = km->find("M:");
        if (m == std::string::npos) return *km;
        const auto end = km->find(';', m);
        return km->substr(m + 2, end == std::string::npos ? std::string::npos : end - m - 2);
    }

    void shutdown() {
        if (tx_.isOpen() && tx_.format() == RawUdpTransport::Format::RawMove) tx_.finishRaw(400);
        if (interpChanged_ && interpOriginal_ && tx_.isOpen()) {
            if (tx_.setInterp(*interpOriginal_)) {
                std::printf("[+] Интерполяция 0x1F возвращена в %s\n", interpName(*interpOriginal_).c_str());
            } else {
                std::printf("[!] Интерполяцию вернуть не удалось (вернётся после переподключения платы)\n");
            }
        }
        tx_.close();
    }

    static std::string interpName(uint8_t v) { return v == 255 ? std::string("AUTO") : std::to_string(v); }

private:
    void reportUsbPeriod(const std::string& km) {
        std::printf("[+] km.device(): %s\n", km.c_str());
        const auto m = km.find("M:");
        if (m == std::string::npos) return;
        const int uf = std::atoi(km.c_str() + m + 2);
        if (uf <= 0) return;
        std::printf("[+] USB-интервал мыши MAKCU: %d мкфр = %.3f мс -> потолок %d Гц\n", uf, uf * 0.125, 8000 / uf);
    }

    RawUdpTransport tx_;
    std::optional<uint8_t> interpOriginal_;
    bool interpChanged_ = false;
};

// ---------------------------------------------------------------------------
// Пресеты: глобальные F-клавиши работают, пока в фокусе тестер поллинга.
// ---------------------------------------------------------------------------
struct Preset {
    int vk;
    const char* key;
    const char* title;
    double hz;
    std::optional<Pattern> pattern;
    std::optional<double> durationS;
    bool sweep = false;
    bool abc = false;
    bool substep = false;
};

const Preset kPresets[] = {
    {VK_F1, "F1", " 250 Гц | 30 с", 250, {}, 30.0},
    {VK_F2, "F2", "1000 Гц | 30 с (основной режим)", 1000, {}, 30.0},
    {VK_F3, "F3", "ABCurves | рывки к целям | 30 с", 1000, Pattern::Flicks, 30.0, false, true},
    {VK_F4, "F4", "накопитель | те же рывки | 30 с", 1000, Pattern::Flicks, 30.0, false, false, true},
    {VK_F5, "F5", "ABCurves | круг | 30 с", 1000, Pattern::Circle, 30.0, false, true},
    {VK_F6, "F6", "ПЕРЕБОР частот --rates: HID + путь, мышь не трогать", 0, {}, {}, true},
    {VK_F8, "F8", "1000 Гц | линия 60 с (тест с рукой)", 1000, Pattern::Line, 60.0},
    {VK_F10, "F10", "накопитель | лестница скоростей 0.05..8 отсч/мс по 2 с | 24 с", 1000, Pattern::Steps, 24.0,
        false, false, true},
};

class KeyEdge {
public:
    bool pressed(int vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !state_[vk & 0xFF];
        state_[vk & 0xFF] = down;
        return edge;
    }
    void sync() {
        for (int vk = 0; vk < 256; ++vk) state_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

private:
    bool state_[256]{};
};

// ---------------------------------------------------------------------------
// Прогон + живой вывод + итог (как у COM-стенда).
// ---------------------------------------------------------------------------
struct HidWindowStats {
    double avgHz = 0.0;
    double medianHz = 0.0;
    double worstHz = 0.0;
    double pathRatio = 0.0;
};

void printSummary(const RunConfig& cfg, const Transport& tx, RawInputMonitor& monitor, const HidWindowStats& hid) {
    const LiveStats& l = g_live;
    const double elapsed = std::max(1e-6, l.elapsedS.load());
    const uint64_t ticks = l.ticks.load();
    const uint64_t writes = tx.stats.writes.load();

    std::printf("\n================ ИТОГ: %s ================\n", cfg.label.c_str());
    std::printf("Траектория: %s, %.2f пикс/мс, размер %.0f | %.0f Гц | %.2f с\n", patternName(cfg.trajectory.pattern),
        cfg.trajectory.speed, cfg.trajectory.size, cfg.hz, elapsed);
    std::printf("Транспорт %-10s команд движения %llu (%.0f/с) | вызов ср %.1f / max %.1f мкс | отложено %llu | ошибок %llu\n",
        tx.name(), static_cast<unsigned long long>(writes), writes / elapsed,
        writes ? tx.stats.sumCallUs.load() / writes : 0.0, tx.stats.maxCallUs.load(),
        static_cast<unsigned long long>(l.deferred.load()), static_cast<unsigned long long>(tx.stats.errors.load()));
    std::printf("Тайминг: тиков %llu | пропущено %llu | фризов %llu | опоздание max %.0f мкс\n",
        static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(l.skippedTicks.load()),
        static_cast<unsigned long long>(l.stalls.load()), l.runLateMaxUs.load());
    {
        uint64_t total = 0;
        for (const auto& b : l.lateHist) total += b.load();
        std::printf("  опоздание тика:");
        for (size_t i = 0; i < l.lateHist.size(); ++i) {
            const uint64_t v = l.lateHist[i].load();
            if (v) std::printf("  %s: %.2f%%", LiveStats::kLateNames[i], 100.0 * v / std::max<uint64_t>(1, total));
        }
        std::printf("\n");
    }
    if (const std::string extra = tx.extraStats(); !extra.empty()) std::printf("%s\n", extra.c_str());

    std::printf("Повышенная точность указателя сейчас: %s\n",
        pointerAccelOn() ? "ВКЛЮЧЕНА (курсор будет ускоряться при крупных дельтах!)" : "выключена");
    if (cfg.substep && !cfg.renderer) {
        std::printf("Накопитель: ненулевых отчётов %.0f/с\n", l.rendererReports.load() / elapsed);
    }
    if (cfg.renderer) {
        std::printf("ABCurves Renderer: ненулевых отчётов %.0f/с (seed %llu)%s\n", l.rendererReports.load() / elapsed,
            static_cast<unsigned long long>(cfg.rendererSeed),
            l.rendererError.load() ? " | ОШИБКА рендерера, прогон прерван" : "");
    }
    std::printf("HID (LL-хук): в среднем %.0f Гц | окна 1 с: медиана %.0f, худшее %.0f Гц | отчётов на команду %.2f\n",
        hid.avgHz, hid.medianHz, hid.worstHz, writes ? static_cast<double>(g_hook.count()) / writes : 0.0);
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    std::printf("  интервалы:");
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i]) std::printf("  %s: %.1f%%", HookMonitor::kBucketNames[i], 100.0 * h[i] / std::max<uint64_t>(1, total));
    }
    std::printf("\n");

    if (const auto dev = monitor.primary()) {
        const int64_t sx = l.sentX.load();
        const int64_t sy = l.sentY.load();
        std::printf("Путь (%s): отправлено (%lld, %lld), пришло в Windows (%lld, %lld) -> %s\n", dev->name.c_str(),
            static_cast<long long>(sx), static_cast<long long>(sy), static_cast<long long>(dev->sumX),
            static_cast<long long>(dev->sumY),
            (sx == dev->sumX && sy == dev->sumY) ? "СОВПАДАЕТ"
                                                 : "расхождение (если трогали мышь - это её путь, так и должно быть)");
    }
}

HidWindowStats runAndWatch(const RunConfig& cfg, Transport& tx, int stopVk, bool printLive) {
    g_live.reset();
    tx.resetStats();
    g_hook.reset();

    std::thread worker([&] { runMotion(cfg, tx, g_live); });

    KeyEdge keys;
    keys.sync();
    const int64_t waitStart = Clock::now();
    while (!g_live.running.load() && Clock::toUs(Clock::now() - waitStart) < 500000) Sleep(1);
    int64_t lastPrint = Clock::now();
    uint64_t lastMoves = 0;
    uint64_t lastHid = g_hook.count();
    int64_t lastSentAbs = 0;
    int64_t lastRecvAbs = receivedPathAbs();
    std::vector<double> windows;
    uint64_t hidAfterFirst = 0;
    double tAfterFirst = 0.0;
    int64_t sentAfterFirst = 0;
    int64_t recvAfterFirst = 0;
    while (g_live.running.load()) {
        Sleep(10);
        if (keys.pressed(VK_ESCAPE) || (stopVk && keys.pressed(stopVk)) || g_exit.load()) g_live.stopRequest = true;
        const int64_t now = Clock::now();
        const double dtUs = Clock::toUs(now - lastPrint);
        if (dtUs >= 1000000.0 && g_live.running.load()) {
            const uint64_t moves = g_live.moves.load();
            const uint64_t hid = g_hook.count();
            const double hidHz = (hid - lastHid) * 1e6 / dtUs;
            if (windows.empty()) {
                hidAfterFirst = hid;
                tAfterFirst = Clock::toUs(now) / 1e6;
                sentAfterFirst = g_live.sentAbs.load();
                recvAfterFirst = receivedPathAbs();
            }
            windows.push_back(hidHz);
            const int64_t sentAbs = g_live.sentAbs.load();
            const int64_t recvAbs = receivedPathAbs();
            if (printLive) {
                const double sentD = static_cast<double>(sentAbs - lastSentAbs);
                const double ratio = sentD > 0 ? static_cast<double>(recvAbs - lastRecvAbs) / sentD : 0.0;
                std::printf("  [%5.1f с] HID %5.0f Гц | команд %5.0f/с | путь x%.2f | отложено %llu | опозд.max %5.0f мкс\n",
                    g_live.elapsedS.load(), hidHz, (moves - lastMoves) * 1e6 / dtUs, ratio,
                    static_cast<unsigned long long>(g_live.deferred.load()), g_live.windowLateMaxUs.exchange(0.0));
                std::fflush(stdout);
            }
            lastSentAbs = sentAbs;
            lastRecvAbs = recvAbs;
            lastMoves = moves;
            lastHid = hid;
            lastPrint = now;
        }
    }
    const double tEnd = Clock::toUs(Clock::now()) / 1e6;
    const uint64_t hidEnd = g_hook.count();
    worker.join();

    HidWindowStats s;
    Sleep(15);
    {
        const double sentD = static_cast<double>(g_live.sentAbs.load() - sentAfterFirst);
        s.pathRatio = sentD > 0 ? static_cast<double>(receivedPathAbs() - recvAfterFirst) / sentD : 0.0;
    }
    if (windows.size() >= 2) {
        s.avgHz = (hidEnd - hidAfterFirst) / std::max(1e-3, tEnd - tAfterFirst);
        std::vector<double> w(windows.begin() + 1, windows.end());
        std::sort(w.begin(), w.end());
        s.worstHz = w.front();
        s.medianHz = w[w.size() / 2];
    } else if (!windows.empty()) {
        s.avgHz = s.medianHz = s.worstHz = windows.front();
    }
    return s;
}

void runOnce(const RunConfig& base, Transport& tx, RawInputMonitor& monitor, int stopVk) {
    RunConfig cfg = base;
    TickLog log;
    const bool probe = cfg.renderer || cfg.substep;
    if (probe) {
        log.reserve(static_cast<size_t>(cfg.durationS * 1000.0) + 2000);
        cfg.log = &log;
        g_hook.startLog();
    }
    monitor.reset();
    std::printf("\n>>> СТАРТ: %s | %s | %s | %.1f с  (стоп: %s / ESC)\n", cfg.label.c_str(), tx.name(),
        patternName(cfg.trajectory.pattern), cfg.durationS, stopVk ? "та же F-клавиша" : "ESC");
    const HidWindowStats hid = runAndWatch(cfg, tx, stopVk, true);
    Sleep(50);
    printSummary(cfg, tx, monitor, hid);
    if (probe) {
        const RateSeries series = seriesFromRun(log, g_hook.stopLog());
        printRateTable(series, cfg.renderer ? "ABCurves Renderer" : "Накопитель");
        const std::string csv = exeDir() + (cfg.renderer ? "\\rate_abc.csv" : "\\rate_substep.csv");
        if (writeRateCsv(series, csv)) std::printf("  по миллисекундам: %s\n", csv.c_str());
    }
}

RunConfig makeConfig(const Options& o, double hz, Pattern pattern, double durationS, std::string label) {
    RunConfig cfg;
    cfg.label = std::move(label);
    cfg.hz = hz;
    cfg.trajectory.pattern = pattern;
    cfg.trajectory.speed = o.speed;
    cfg.trajectory.size = o.size;
    cfg.durationS = durationS;
    cfg.hybridWait = o.hybridWait;
    cfg.core = o.core;
    const uint64_t seed = o.abcSeed != 0 ? o.abcSeed : GetTickCount64() + (++g_runCounter) * 7919u;
    if (pattern == Pattern::Flicks) cfg.trajectory.flicks = makeFlicks(seed, durationS * 1000.0, o.size * 2.0);
    cfg.rendererSeed = seed;
    return cfg;
}

RunConfig withSubstep(RunConfig cfg) {
    cfg.substep = true;
    cfg.hz = 1000.0;
    return cfg;
}

RunConfig withRenderer(RunConfig cfg) {
    cfg.renderer = &g_abc;
    cfg.hz = 1000.0;
    return cfg;
}

// Калибровка замера: без инжекта, человек водит реальной мышью.
void measureHand(int stopVk) {
    std::printf("\n>>> ЗАМЕР РУКИ: 15 с, инжекта нет - водите реальной мышью в середине экрана с РАЗНОЙ\n"
                "    скоростью: очень медленно, медленно, средне, быстро (по несколько секунд)\n");
    int pointerSpeed = 0;
    if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &pointerSpeed, 0) && pointerSpeed != 10) {
        std::printf("[!] Скорость указателя Windows %d/20 (не 10): скорость руки в таблице будет не в отсчётах\n",
            pointerSpeed);
    }
    g_hook.reset();
    g_hook.startLog();
    KeyEdge keys;
    keys.sync();
    uint64_t last = 0;
    int64_t t = Clock::now();
    for (int sec = 0; sec < 15 && !g_exit.load(); ++sec) {
        const int64_t until = t + Clock::fromUs(1e6);
        bool stop = false;
        while (Clock::now() < until) {
            Sleep(10);
            if (keys.pressed(VK_ESCAPE) || keys.pressed(stopVk)) stop = true;
        }
        const int64_t now = Clock::now();
        const uint64_t c = g_hook.count();
        std::printf("  [%d с] HID %5.0f Гц\n", sec + 1, (c - last) * 1e6 / Clock::toUs(now - t));
        last = c;
        t = now;
        if (stop) break;
    }
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    std::printf("  интервалы:");
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i]) std::printf("  %s: %.1f%%", HookMonitor::kBucketNames[i], 100.0 * h[i] / std::max<uint64_t>(1, total));
    }
    std::printf("\n");
    const RateSeries series = seriesFromHand(g_hook.stopLog());
    printRateTable(series, "Рука");
    const std::string csv = exeDir() + "\\rate_hand.csv";
    if (writeRateCsv(series, csv)) std::printf("  по миллисекундам: %s\n", csv.c_str());
}

struct RateRow {
    HidWindowStats hid;
    double reportsPerCmd = 0.0;
    double cmdPerS = 0.0;
    double oneFramePct = 0.0;
    double twoFramePct = 0.0;
    double longPct = 0.0;
};

RateRow measureRate(const Options& o, Transport& tx, double hz, int stopVk) {
    const Pattern pat = o.pattern == Pattern::Wobble ? Pattern::Wobble : Pattern::Ramp;
    const RunConfig cfg = makeConfig(o, hz, pat, 4.0, "sweep");
    RateRow r;
    r.hid = runAndWatch(cfg, tx, stopVk, false);
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    const double tot = static_cast<double>(std::max<uint64_t>(1, total));
    r.oneFramePct = 100.0 * h[1] / tot;
    r.twoFramePct = 100.0 * h[3] / tot;
    r.longPct = 100.0 * (h[4] + h[5]) / tot;
    const uint64_t writes = tx.stats.writes.load();
    r.reportsPerCmd = writes ? static_cast<double>(g_hook.count()) / static_cast<double>(writes) : 0.0;
    r.cmdPerS = writes / std::max(1e-3, g_live.elapsedS.load());
    return r;
}

std::vector<double> sweepRates(const Options& o) {
    return o.rates.empty() ? std::vector<double>{125, 250, 500, 1000} : o.rates;
}

void printRateHeader() {
    std::printf("  тиков/с | формат  | интерп | команд/с | HID ср | HID худш | отч/команду | путь | 1 кадр %% | 2 кадра %% | >2.5 мс %%\n");
}

void printRateRow(double hz, const char* fmt, const std::string& interp, const RateRow& r) {
    std::printf("  %7.0f | %-7s | %6s | %8.0f | %6.0f | %8.0f | %11.2f | %4.2f | %8.1f | %9.1f | %9.1f\n", hz, fmt,
        interp.c_str(), r.cmdPerS, r.hid.avgHz, r.hid.worstHz, r.reportsPerCmd, r.hid.pathRatio, r.oneFramePct,
        r.twoFramePct, r.longPct);
    std::fflush(stdout);
}

void runSweep(const Options& o, MakcuUdpLink& link, TxKind kind, int stopVk) {
    std::printf("\n>>> ПЕРЕБОР: ramp %.1f пикс/мс, по 4 с на частоту. Мышь НЕ трогать.\n", o.speed);
    const std::string interp = link.interpNow();
    printRateHeader();
    g_live.stopRequest = false;
    for (double hz : sweepRates(o)) {
        if (g_live.stopRequest.load() || g_exit.load()) break;
        Transport* tx = link.acquire(kind);
        if (!tx) return;
        Sleep(500);
        const RateRow r = measureRate(o, *tx, hz, stopVk);
        if (g_live.stopRequest.load()) break;
        printRateRow(hz, txName(kind), interp, r);
        if (const std::string extra = tx->extraStats(); !extra.empty()) std::printf("           %s\n", extra.c_str());
    }
    std::printf("  путь 1.00 = доставлено всё движение; отч/команду > 1 - прошивка режет команду (интерполятор).\n");
}

void runCompare(const Options& o, MakcuUdpLink& link) {
    std::printf("\n>>> СРАВНЕНИЕ ФОРМАТОВ: ramp %.1f пикс/мс, по 4 с на ячейку. Мышь НЕ трогать.\n", o.speed);
    const std::string interp = link.interpNow();
    printRateHeader();
    g_live.stopRequest = false;
    for (double hz : sweepRates(o)) {
        for (TxKind kind : o.compare) {
            if (g_live.stopRequest.load() || g_exit.load()) return;
            Transport* tx = link.acquire(kind);
            if (!tx) return;
            Sleep(500);
            const RateRow r = measureRate(o, *tx, hz, 0);
            if (g_live.stopRequest.load()) return;
            printRateRow(hz, txName(kind), interp, r);
            if (const std::string extra = tx->extraStats(); !extra.empty()) std::printf("           %s\n", extra.c_str());
        }
    }
}

// Приём платы: потери и RTT по интервалу датаграмм (только 0x6A(id=0), курсор стоит).
void runNetProbe(RawUdpTransport& udp, bool quick) {
    std::printf("\n>>> ПРИЁМ ПЛАТЫ: запросы 0x6A(id=0) с заданным интервалом мимо регулятора темпа (курсор стоит)\n");
    std::printf("  интервал, мкс | отправлено | ответов | потеряно %% | ответов/с | RTT p50 | p99 | max, мс\n");
    const std::vector<double> gaps = quick ? std::vector<double>{3000, 2000, 1800, 1500, 1000}
                                           : std::vector<double>{5000, 3000, 2500, 2000, 1800, 1750, 1600, 1500, 1250,
                                                 1000, 500};
    const uint32_t count = quick ? 300 : 1000;
    for (double g : gaps) {
        if (g_exit.load()) break;
        const auto r = udp.probeIntake(g, count);
        std::printf("  %13.0f | %10u | %7u | %10.1f | %9.0f | %7.2f | %4.2f | %4.2f\n", g, r.sent, r.answered,
            r.sent ? 100.0 * (r.sent - r.answered) / r.sent : 0.0, r.answered / std::max(1e-3, r.seconds), r.rttP50Ms,
            r.rttP99Ms, r.rttMaxMs);
        std::fflush(stdout);
        Sleep(100);
    }
    std::printf("  Потери при интервале < ~1.75 мс - предел приёма платы (~583 датаграммы/с), не сети.\n");
}

void printMenu(Pattern pattern, const Options& o, TxKind kind, const std::string& interp) {
    std::printf("\n=================================================================================\n");
    std::printf(" Горячие клавиши (глобальные - держите в фокусе тестер поллинга):\n");
    for (const auto& p : kPresets) std::printf("   [%s] %s\n", p.key, p.title);
    std::printf("   [F7] формат: сейчас %s -> rawmove / move (0x18) / movenow (0x67)\n", txName(kind));
    std::printf("   [F9] траектория (сейчас: %s)   скорость %.2f пикс/мс, размер %.0f, %.0f с\n", patternName(pattern),
        o.speed, o.size, o.durationS);
    std::printf("   [F11] интерполяция 0x1F: сейчас %s -> 0 / AUTO / 25 / 50 / 100 (live, при выходе вернётся)\n",
        interp.c_str());
    std::printf("   [F12] замер руки 15 с без инжекта      [ESC] / та же F-клавиша - стоп      [END] - выход\n");
    std::printf("=================================================================================\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    Options opt;
    if (!parseOptions(argc, argv, opt)) {
        printUsage();
        return 2;
    }

    tuneProcessForTiming();
    std::printf("=== MAKCU mover (UDP) | Raw 0x69/0x6A по сети, без SDK ===\n");

    MakcuUdpLink link;
    if (!link.init(opt)) {
        link.shutdown();
        restoreProcessTiming();
        return 1;
    }

    if (opt.check || opt.netProbe) {
        if (link.acquire(TxKind::RawMove)) {
            runNetProbe(link.udp(), opt.check);
            std::printf("%s\n", link.udp().extraStats().c_str());
        }
        link.shutdown();
        restoreProcessTiming();
        return 0;
    }

    initAbcurves(opt);
    if (!opt.keepAccel) {
        if (g_accel.disable()) {
            std::printf(g_accel.wasEnabled()
                    ? "[+] 'Повышенная точность указателя' ВЫКЛЮЧЕНА на время работы (вернётся при выходе)\n"
                    : "[+] 'Повышенная точность указателя' уже выключена\n");
        } else {
            std::printf("[!] Не удалось прочитать/изменить акселерацию указателя\n");
        }
    }
    if (!g_hook.start()) std::printf("[!] LL-хук мыши не установился - HID-частоту программа не покажет\n");
    RawInputMonitor monitor;
    g_rawmon = &monitor;
    if (!monitor.start()) std::printf("[!] Raw Input монитор не запустился - будет только статистика отправки\n");

    TxKind kind = opt.tx;
    if (!link.acquire(kind)) {
        g_hook.stop();
        monitor.stop();
        g_accel.restore();
        link.shutdown();
        restoreProcessTiming();
        return 1;
    }
    if (opt.startDelayS > 0.0) {
        std::printf("[*] Ожидание %.0f с перед стартом (мышь не трогать)...\n", opt.startDelayS);
        std::fflush(stdout);
        Sleep(static_cast<DWORD>(opt.startDelayS * 1000.0));
    }

    if (!opt.compare.empty()) {
        runCompare(opt, link);
    } else if (opt.sweep) {
        runSweep(opt, link, kind, 0);
    } else if (opt.runOnce) {
        if (Transport* tx = link.acquire(kind)) {
            char label[96];
            std::snprintf(label, sizeof(label), "%.0f Гц | %s", opt.hz, txName(kind));
            Sleep(300);
            RunConfig cfg = makeConfig(opt, opt.hz, opt.pattern, opt.durationS, label);
            if (opt.abc) {
                if (!g_abcReady) {
                    std::printf("[-] --abc: ABCurves Renderer не загружен\n");
                } else {
                    cfg = withRenderer(cfg);
                    cfg.label += " | ABCurves";
                    runOnce(cfg, *tx, monitor, 0);
                }
            } else if (opt.substep) {
                cfg = withSubstep(cfg);
                cfg.label += " | накопитель";
                runOnce(cfg, *tx, monitor, 0);
            } else {
                runOnce(cfg, *tx, monitor, 0);
            }
        }
    } else {
        Pattern pattern = opt.pattern;
        printMenu(pattern, opt, kind, link.interpNow());
        KeyEdge keys;
        keys.sync();
        while (!g_exit.load()) {
            Sleep(15);
            if (keys.pressed(VK_END)) break;
            if (keys.pressed(VK_F7)) {
                kind = kind == TxKind::RawMove ? TxKind::Move : kind == TxKind::Move ? TxKind::MoveNow : TxKind::RawMove;
                (void)link.acquire(kind);
                std::printf("[*] Формат: %s\n", txName(kind));
            }
            if (keys.pressed(VK_F11)) link.cycleInterp();
            if (keys.pressed(VK_F12)) {
                measureHand(VK_F12);
                printMenu(pattern, opt, kind, link.interpNow());
                keys.sync();
            }
            if (keys.pressed(VK_F9)) {
                pattern = pattern == Pattern::Circle ? Pattern::Eight
                        : pattern == Pattern::Eight  ? Pattern::Line
                        : pattern == Pattern::Line   ? Pattern::Flicks
                                                     : Pattern::Circle;
                std::printf("[*] Траектория: %s\n", patternName(pattern));
            }
            for (const auto& p : kPresets) {
                if (!keys.pressed(p.vk)) continue;
                Transport* tx = link.acquire(kind);
                if (!tx) {
                    std::printf("[-] Транспорт недоступен\n");
                    break;
                }
                if (p.sweep) {
                    runSweep(opt, link, kind, p.vk);
                } else if (p.abc && !g_abcReady) {
                    std::printf("[-] ABCurves Renderer не загружен - пресет недоступен\n");
                    break;
                } else {
                    const std::string label = std::string(p.key) + " " + p.title + " | " + tx->name();
                    RunConfig cfg = makeConfig(opt, p.hz, p.pattern.value_or(pattern), p.durationS.value_or(opt.durationS),
                        label);
                    if (p.abc) cfg = withRenderer(cfg);
                    if (p.substep) cfg = withSubstep(cfg);
                    runOnce(cfg, *tx, monitor, p.vk);
                    if (!tx->healthy()) std::printf("[-] Транспорт сообщил об ошибке (плата недоступна?)\n");
                }
                printMenu(pattern, opt, kind, link.interpNow());
                keys.sync();
                break;
            }
        }
    }

    std::printf("\n[*] Завершение...\n");
    g_hook.stop();
    monitor.stop();
    g_accel.restore();
    link.shutdown();
    restoreProcessTiming();
    return 0;
}
