// pad_texture_check.cpp - сверка PadTexture19 на C++ (makcu_gamepad_renderer.h) с эталоном на Python (pad_texture.py).
// Железо не нужно, порт не открывается.
//
//   pad_texture_check.exe FILE.csv FROM_S LEN_S FROM2_S LEN2_S
//
// FILE.csv - запись pad_probe_record (*_xinput.csv или *_board.csv): столбцы t_us, rx, ry. Берутся строки двух участков
// [FROM, FROM + LEN) и [FROM2, FROM2 + LEN2) от первой строки файла, как есть (по строке на пакет, без пересчёта на сетку).
// Печать: медианы 22 описателей по окнам 512 для обоих участков и W1 между ними, по 9 знаков.
// Тот же вывод даёт pad_texture_check.py; сравнение - Compare-Object по строкам.
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra -I ..\perfect-transport-gamepad-com pad_texture_check.cpp -static -o pad_texture_check.exe
#include "makcu_gamepad_renderer.h"

#include <cstdlib>

using makcu::PadTexture19;

static bool loadRange(const char* path, double from, double len, std::vector<int16_t>& x, std::vector<int16_t>& y) {
    FILE* f = std::fopen(path, "r");
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
            x.push_back(static_cast<int16_t>(vx));
            y.push_back(static_cast<int16_t>(vy));
        }
    }
    std::fclose(f);
    return true;
}

static double median(const std::vector<PadTexture19::Row>& rows, int c) {
    std::vector<double> v;
    for (const auto& r : rows) v.push_back(r[static_cast<size_t>(c)]);
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::printf("pad_texture_check.exe FILE.csv FROM_S LEN_S FROM2_S LEN2_S\n");
        return 2;
    }
    std::vector<int16_t> ax, ay, bx, by;
    if (!loadRange(argv[1], std::atof(argv[2]), std::atof(argv[3]), ax, ay) || !loadRange(argv[1], std::atof(argv[4]), std::atof(argv[5]), bx, by)) {
        std::printf("не прочитан %s\n", argv[1]);
        return 1;
    }
    const auto fa = PadTexture19::windows(ax.data(), ay.data(), ax.size());
    const auto fb = PadTexture19::windows(bx.data(), by.data(), bx.size());
    double total = 0.0;
    const auto w = PadTexture19::w1(fa, fb, &total);
    std::printf("rows %zu %zu windows %zu %zu\n", ax.size(), bx.size(), fa.size(), fb.size());
    for (int c = 0; c < PadTexture19::kCount; ++c) {
        std::printf("%s %.9f %.9f %.9f\n", PadTexture19::name(c), median(fa, c), median(fb, c), w[static_cast<size_t>(c)]);
    }
    std::printf("total %.9f\n", total);
    return 0;
}
