// pad_render_sim.cpp - подбор рендерера под сглаживание платы Off БЕЗ ЖЕЛЕЗА.
//
// При Off плата - чистая задержка (HANDOFF_GAMEPAD_COM.md, 9б): Windows получает ровно то, что выдал рендерер.
// Поэтому текстуру можно считать прямо по выходу PadTextureRenderer::step() и сравнивать с записью живой руки.
//
//   pad_render_sim.exe PREFIX FROM_S LEN_S [--seeds N] [--auto] [--dump FILE] [имя=значение ...]
//
// PREFIX_board.csv - запись pad_probe_record (снимки 0x54 живого стика: t_us, rx, ry; строка на отчёт, 1 мс).
// Участок [FROM, FROM + LEN): его сглаживание (гаусс, СКО 20 мс) - цель; сам участок - "живая рука".
// Варианты: "кривая" (цель, округлённая до целых) и "рендерер" (presetOff + правки имя=значение, имена как в файле
// профиля: tremor_rms, raw_lsb, adc_hysteresis_lsb, ...). Рендерер считается для N зёрен (по умолчанию 5), окна всех
// зёрен складываются в один набор. --auto: пресет под Auto (presetFor) вместо presetOff - для сравнения.
// Печать: медианы PadTexture19, W1 до руки по описателям и сумма; отставание и ошибка слежения рендерера за целью.
// --dump FILE: выход рендерера первого зерна (t_ms, цель, выход) в CSV.
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra -I ..\perfect-transport-gamepad-com pad_render_sim.cpp -static -o pad_render_sim.exe
#include "makcu_gamepad_renderer.h"

#include <cstdlib>

using makcu::PadTexture19;

static bool loadRange(const std::string& path, double from, double len, std::vector<double>& x, std::vector<double>& y) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    if (!std::fgets(line, sizeof(line), f)) {
        std::fclose(f);
        return false;
    }
    int it = -1, ix = -1, iy = -1, col = 0;
    for (char* tok = std::strtok(line, ",\r\n"); tok; tok = std::strtok(nullptr, ",\r\n"), ++col) {
        const std::string n = tok;
        if (n == "t_us") it = col;
        else if (n == "rx") ix = col;
        else if (n == "ry") iy = col;
    }
    if (it < 0 || ix < 0 || iy < 0) {
        std::fclose(f);
        return false;
    }
    double t0 = 0.0;
    bool first = true;
    while (std::fgets(line, sizeof(line), f)) {
        double t = 0.0, vx = 0.0, vy = 0.0;
        col = 0;
        for (char* tok = std::strtok(line, ",\r\n"); tok; tok = std::strtok(nullptr, ",\r\n"), ++col) {
            if (col == it) t = std::atof(tok);
            else if (col == ix) vx = std::atof(tok);
            else if (col == iy) vy = std::atof(tok);
        }
        if (first) {
            t0 = t;
            first = false;
        }
        const double s = (t - t0) / 1e6;
        if (s >= from && s < from + len) {
            x.push_back(vx);
            y.push_back(vy);
        }
    }
    std::fclose(f);
    return !x.empty();
}

static std::vector<double> gauss(const std::vector<double>& v, double sigma) {
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

static double median(const std::vector<PadTexture19::Row>& rows, int c) {
    std::vector<double> v;
    for (const auto& r : rows) v.push_back(r[static_cast<size_t>(c)]);
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("pad_render_sim.exe PREFIX FROM_S LEN_S [--seeds N] [--auto] [--dump FILE] [имя=значение ...]\n");
        return 2;
    }
    const std::string prefix = argv[1];
    const double from = std::atof(argv[2]), len = std::atof(argv[3]);
    int seeds = 5;
    bool autoMode = false;
    std::string dump;
    makcu::PadRendererProfile prof = makcu::PadTextureRenderer::presetOff(0x80);
    std::vector<std::pair<std::string, double>> sets;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seeds" && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else if (a == "--auto") autoMode = true;
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a.find('=') != std::string::npos) sets.emplace_back(a.substr(0, a.find('=')), std::atof(a.substr(a.find('=') + 1).c_str()));
        else {
            std::printf("непонятный аргумент %s\n", a.c_str());
            return 2;
        }
    }
    if (autoMode) prof = makcu::PadTextureRenderer::presetFor(0x80);
    for (const auto& s : sets) {
        if (!makcu::PadTextureRenderer::setField(prof, s.first, s.second)) {
            std::printf("нет поля профиля %s\n", s.first.c_str());
            return 2;
        }
    }
    prof.finalize();
    std::vector<double> bx, by;
    if (!loadRange(prefix + "_board.csv", from, len, bx, by)) {
        std::printf("не прочитан %s_board.csv\n", prefix.c_str());
        return 1;
    }
    const size_t n = bx.size();
    const std::vector<double> sx = gauss(bx, 20.0), sy = gauss(by, 20.0);
    std::vector<int16_t> lx(n), ly(n), cx(n), cy(n);
    for (size_t i = 0; i < n; ++i) {
        lx[i] = static_cast<int16_t>(bx[i]);
        ly[i] = static_cast<int16_t>(by[i]);
        cx[i] = static_cast<int16_t>(std::lround(sx[i]));
        cy[i] = static_cast<int16_t>(std::lround(sy[i]));
    }
    const auto live = PadTexture19::windows(lx.data(), ly.data(), n);
    const auto curve = PadTexture19::windows(cx.data(), cy.data(), n);
    std::vector<PadTexture19::Row> rend;
    double lagBest = 0.0, errBest = 0.0;
    for (int s = 0; s < seeds; ++s) {
        makcu::PadTextureRenderer r;
        r.setProfile(prof);
        r.beginEvent(0xC0FFEEull + static_cast<uint64_t>(s) * 7919ull, static_cast<int16_t>(std::lround(sx[0])), static_cast<int16_t>(std::lround(sy[0])));
        std::vector<int16_t> ox(n), oy(n);
        for (size_t i = 0; i < n; ++i) r.step(sx[i], sy[i], ox[i], oy[i]);
        const auto w = PadTexture19::windows(ox.data(), oy.data(), n);
        rend.insert(rend.end(), w.begin(), w.end());
        if (s == 0) {
            double best = 1e18;
            for (int lag = 0; lag <= 20; ++lag) {
                double acc = 0.0;
                size_t cnt = 0;
                for (size_t i = 100 + static_cast<size_t>(lag); i < n; ++i, ++cnt) acc += std::hypot(ox[i] - sx[i - static_cast<size_t>(lag)], oy[i] - sy[i - static_cast<size_t>(lag)]);
                if (cnt && acc / static_cast<double>(cnt) < best) {
                    best = acc / static_cast<double>(cnt);
                    lagBest = lag;
                }
            }
            errBest = best;
            if (!dump.empty()) {
                if (FILE* f = std::fopen(dump.c_str(), "w")) {
                    std::fprintf(f, "t_ms,tx,ty,ox,oy,lx,ly\n");
                    for (size_t i = 0; i < n; ++i) std::fprintf(f, "%zu,%.1f,%.1f,%d,%d,%d,%d\n", i, sx[i], sy[i], ox[i], oy[i], lx[i], ly[i]);
                    std::fclose(f);
                }
            }
        }
    }
    double totC = 0.0, totR = 0.0;
    const auto wc = PadTexture19::w1(live, curve, &totC), wr = PadTexture19::w1(live, rend, &totR);
    std::printf("участок %.1f..%.1f с, отчётов %zu; окон: рука %zu, кривая %zu, рендерер %zu (%d зёрен)%s\n", from, from + len, n, live.size(), curve.size(), rend.size(),
        seeds, autoMode ? "; ПРЕСЕТ ПОД AUTO" : "");
    for (const auto& s : sets) std::printf("   %s=%g\n", s.first.c_str(), s.second);
    std::printf("%-26s %9s %9s %9s | %9s %9s\n", "описатель", "рука", "кривая", "рендерер", "W1 крив.", "W1 ренд.");
    for (int c = 0; c < PadTexture19::kCount; ++c) {
        std::printf("%-26s %9.3f %9.3f %9.3f | %9.3f %9.3f\n", PadTexture19::name(c), median(live, c), median(curve, c), median(rend, c), wc[static_cast<size_t>(c)],
            wr[static_cast<size_t>(c)]);
    }
    std::printf("%-26s %9s %9s %9s | %9.3f %9.3f\n", "СУММА W1", "", "", "", totC, totR);
    std::printf("слежение рендерера за целью (зерно 0): отставание %.0f мс, ошибка средняя %.1f\n", lagBest, errBest);
    // строка для скриптов (латиница: PowerShell 5.1 читает .ps1 без BOM как ANSI и не находит кириллицу)
    std::printf("SUM curve=%.3f render=%.3f hold=%.3f lag1=%.3f lagms=%.0f err=%.1f\n", totC, totR, median(rend, 0), median(rend, 18), lagBest, errBest);
    return 0;
}
