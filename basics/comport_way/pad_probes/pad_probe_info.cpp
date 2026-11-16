// pad_probe_info.cpp - проба 1 (фаза 1): какой геймпад видит плата MAKCU и какие у неё настройки обработки.
// ТОЛЬКО ЧТЕНИЕ: ни одной команды SET, в 0x3E только INFO (операция 0) и READ (операция 1).
// Геймпадом не управляет, окна нет. Во время пробы геймпад не трогать (лежит на столе).
//
// Что спрашивает:
//   0x02 DEVICE, 0x04 FIRMWARE_VERSION
//   текстом: km.version(), km.device(), km.controller(), km.controller_state(), km.controller_physical()
//   GET 0x40 CONTROLLER_STATE, GET 0x41 CONTROLLER_STREAM, GET 0x52 INPUT_STREAM kind=3
//   GET 0x54 CONTROLLER_PHYSICAL (один раз и замкнутым циклом --poll секунд)
//   0x3E [1D 00] INFO, [1D 01 revision offset length] READ - образ настроек 400 байт
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_info.cpp -lwinmm -static -o pad_probe_info.exe
// Запуск: pad_probe_info.exe [--port COM5] [--poll 2] [--image FILE]
#include "pad_probe_common.h"

#include <map>

using namespace padprobe;

namespace {

void printState(const char* title, const PadState& s) {
    std::printf("    %s: кнопки low %08X high %08X | LT %u RT %u | левый стик %d, %d | правый стик %d, %d\n", title, s.low, s.high, s.lt,
        s.rt, s.lx, s.ly, s.rx, s.ry);
}

const char* interpName(uint32_t v) {
    switch (v) {
    case 0: return "Off";
    case 1: return "Fixed";
    case 2: return "Auto";
    }
    return "?";
}

// Раскладка образа - mak-suite/cpp/makxd-cpp/src/device_settings.cpp (settingsDecode).
void decodeImage(const std::vector<uint8_t>& p) {
    const uint32_t flags = le32(&p[0]);
    std::printf("    [0..3]   флаги %08X: кривая %u, разброс времени (timing variance) %u %%, старые силы %u / %u / %u\n", flags, flags & 1,
        (flags >> 22) & 127, (flags >> 1) & 127, (flags >> 8) & 127, (flags >> 15) & 127);
    std::printf("    [4..7]   интерполяция %u (%s)\n", le32(&p[4]), interpName(le32(&p[4])));
    std::printf("    [8..11]  буфер %u мс\n", le32(&p[8]));
    std::printf("    [12..19] выбранный профиль %u, профилей %u\n", le32(&p[12]), le32(&p[16]));
    static const char* const chan[4] = {"правый стик", "левый стик", "левый триггер", "правый триггер"};
    for (unsigned i = 0; i < 4; ++i) {
        const size_t at = 20 + 88 * i;
        const uint32_t f = le32(&p[at]);
        if (!f) {
            std::printf("    behaviors[%u] (%s): пусто\n", i, chan[i]);
            continue;
        }
        const unsigned len = std::min<unsigned>(p[at + 4], 24);
        const std::string name(reinterpret_cast<const char*>(&p[at + 8]), len);
        const bool hasStrength = p[at + 5] != 15;
        std::printf("    behaviors[%u] (%s): имя \"%s\", флаги %06X\n", i, chan[i], name.c_str(), f);
        std::printf("        strength %s, inertia %u %%, micro %u %%, limit %u %%, advanced %u, curve %u\n",
            hasStrength ? (std::to_string(p[at + 5] & 127) + " %").c_str() : "не задана (байт 15)", (f >> 1) & 127, (f >> 8) & 127,
            (f >> 15) & 127, (f >> 22) & 1, (f >> 23) & 1);
        std::printf("        magnitude variance %u %%, angle variance %u %%\n", p[at + 6], p[at + 7]);
        for (unsigned axis = 0; axis < 4; ++axis) {
            const size_t pos = at + 32 + 14 * axis;
            std::printf("        кривая[%u]: center deadzone %u %%, anti-deadzone %u %%, change deadband %u %%, точки", axis, p[pos], p[pos + 1],
                p[pos + 2]);
            for (unsigned n = 0; n < 5; ++n) std::printf(" (%u,%u)", p[pos + 4 + 2 * n], p[pos + 5 + 2 * n]);
            std::printf("\n");
        }
    }
    static const char* const tr[4] = {"левый стик/WASD", "правый стик/мышь", "левый триггер", "правый триггер"};
    for (unsigned i = 0; i < 4; ++i) {
        std::printf("    перевод[%u] (%s): включён %u, масштаб %u, тайм-аут %u мс\n", i, tr[i], le16(&p[372 + 6 * i]), le16(&p[374 + 6 * i]),
            le16(&p[376 + 6 * i]));
    }
    std::printf("    [396] разброс мыши (mouse spread) %u, [397..399] %u %u %u\n", p[396], p[397], p[398], p[399]);
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();
    std::string port = "COM5", imagePath = "pad_settings_image.bin";
    double pollSec = 2.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--poll" && more) pollSec = std::atof(argv[++i]);
        else if (a == "--image" && more) imagePath = argv[++i];
        else {
            std::printf("pad_probe_info.exe [--port COM5] [--poll 2] [--image FILE]\n");
            return 2;
        }
    }
    Port p;
    if (!p.open(port)) {
        std::printf("[-] %s не открылся: ошибка %lu (порт занят Toolkit/SDK или USB2 не подключён)\n", port.c_str(), GetLastError());
        return 1;
    }
    timeBeginPeriod(1);

    std::printf("== 1. Плата ==\n");
    uint8_t kinds = 0;
    if (auto r = p.ask(kOpDevice, nullptr, 0, 300000.0)) {
        kinds = r->empty() ? 0 : (*r)[0];
        std::printf("    0x02 DEVICE: %s -> kinds %02X = %s\n", hex(*r).c_str(), kinds, kindsText(kinds).c_str());
        std::printf("    геймпад (kinds & FC): %02X = %s\n", kinds & 0xFC, kindsText(kinds & 0xFC).c_str());
    } else {
        std::printf("[-] плата не ответила на DEVICE (0x02) на 4 Мбод\n");
        return 1;
    }
    if (auto r = p.ask(kOpFirmware)) {
        std::printf("    0x04 FIRMWARE_VERSION: %s", hex(*r).c_str());
        if (r->size() == 4) std::printf(" -> %u", le32(r->data()));
        std::printf("\n");
    } else {
        std::printf("    0x04 FIRMWARE_VERSION: нет ответа\n");
    }

    std::printf("== 2. Текстовые запросы km.* ==\n");
    for (const char* cmd : {"km.version()", "km.device()", "km.controller()", "km.controller_state()", "km.controller_physical()"}) {
        std::printf("    %s -> \"%s\"\n", cmd, escaped(p.text(cmd)).c_str());
    }

    std::printf("== 3. Состояние контроллера ==\n");
    if (auto r = p.ask(kOpCtlState)) {
        std::printf("    GET 0x40 CONTROLLER_STATE (%zu байт): %s\n", r->size(), hex(*r).c_str());
        if (r->size() == 20) printState("заданное программой", parseState(r->data()));
    } else {
        std::printf("    GET 0x40: нет ответа\n");
    }
    if (auto r = p.ask(kOpCtlStream)) std::printf("    GET 0x41 CONTROLLER_STREAM: %s\n", hex(*r).c_str());
    else std::printf("    GET 0x41: нет ответа\n");
    {
        const uint8_t kind = 3;
        if (auto r = p.ask(kOpInputStream, &kind, 1)) std::printf("    GET 0x52 INPUT_STREAM kind=3: %s\n", hex(*r).c_str());
        else std::printf("    GET 0x52 kind=3: нет ответа\n");
    }
    if (auto r = p.ask(kOpCtlPhysical)) {
        std::printf("    GET 0x54 CONTROLLER_PHYSICAL (%zu байт): %s\n", r->size(), hex(*r).c_str());
        if (r->size() == 32) {
            const PadPhysical v = parsePhysical(r->data());
            printState("физическое", v.s);
            std::printf("    sequence %u, usb_timestamp %u (x125 мкс = %.3f с), timing %04X: dt %u uf = %.3f мс%s%s, cadence %u uf = %.3f мс\n", v.seq,
                v.usbTime, v.usbTime * 0.000125, v.timing, v.dt(), v.dt() * 0.125, v.baseline() ? ", baseline" : "",
                v.invalid() ? ", INVALID" : "", v.cadence, v.cadence * 0.125);
        }
    } else {
        std::printf("    GET 0x54: нет ответа\n");
    }

    std::printf("== 4. Настройки обработки (0x3E, запись 0x1D, только INFO и READ) ==\n");
    uint32_t revision = 0;
    bool infoOk = false;
    {
        const uint8_t q[2] = {0x1D, 0x00};
        if (auto r = p.ask(kOpConnection, q, sizeof(q), 300000.0)) {
            std::printf("    INFO: %s\n", hex(*r).c_str());
            if (r->size() == 14 && (*r)[0] == 0x1D && (*r)[1] == 0x00 && (*r)[2] == 0) {
                revision = le32(&(*r)[8]);
                infoOk = true;
                std::printf("    версия %u, разделы %02X (1 контроллер, 2 перевод мыши/клавиатуры, 4 мышь), виды %02X (1 мышь, 2 клавиатура, 4 геймпад),\n"
                            "    состояние сохранения %u, ревизия %u, образ %u байт\n",
                    (*r)[3], (*r)[4], (*r)[5], (*r)[6], revision, le16(&(*r)[12]));
            } else {
                std::printf("    [-] неожиданный ответ INFO\n");
            }
        } else {
            std::printf("    INFO: нет ответа\n");
        }
    }
    if (infoOk) {
        std::vector<uint8_t> image(400, 0);
        bool ok = true;
        for (unsigned off = 0; off < 400 && ok; off += 96) {
            const unsigned len = std::min(96u, 400u - off);
            uint8_t q[9] = {0x1D, 0x01};
            q[2] = static_cast<uint8_t>(revision);
            q[3] = static_cast<uint8_t>(revision >> 8);
            q[4] = static_cast<uint8_t>(revision >> 16);
            q[5] = static_cast<uint8_t>(revision >> 24);
            q[6] = static_cast<uint8_t>(off);
            q[7] = static_cast<uint8_t>(off >> 8);
            q[8] = static_cast<uint8_t>(len);
            auto r = p.ask(kOpConnection, q, sizeof(q), 300000.0);
            if (!r || r->size() != 9 + len || (*r)[0] != 0x1D || (*r)[1] != 0x01 || (*r)[2] != 0 || le32(&(*r)[3]) != revision ||
                le16(&(*r)[7]) != off) {
                std::printf("    [-] READ offset %u: %s\n", off, r ? hex(*r).c_str() : "нет ответа");
                ok = false;
                break;
            }
            std::copy(r->begin() + 9, r->end(), image.begin() + off);
        }
        if (ok) {
            if (FILE* f = std::fopen(imagePath.c_str(), "wb")) {
                std::fwrite(image.data(), 1, image.size(), f);
                std::fclose(f);
                std::printf("    образ 400 байт записан в %s\n", imagePath.c_str());
            }
            for (unsigned off = 0; off < 400; off += 32) {
                std::printf("    %03u: %s\n", off, hex(&image[off], std::min(32u, 400u - off)).c_str());
            }
            decodeImage(image);
        }
    }

    std::printf("== 5. Опрос 0x54 замкнутым циклом, %.1f с (геймпад не трогать) ==\n", pollSec);
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        uint64_t req = 0, ok32 = 0, ff = 0, lost = 0, odd = 0, seqChanges = 0, baseline = 0, invalid = 0;
        std::vector<double> rtt;
        std::map<uint32_t, uint64_t> seqStep, dtHist, cadHist, usbStep;
        std::map<long long, uint64_t> rxVals, ryVals, lxVals, lyVals;
        bool have = false;
        PadPhysical prev;
        const int64_t t0 = now(), end = t0 + us(pollSec * 1e6);
        while (now() < end) {
            double ms = 0.0;
            ++req;
            auto r = p.ask(kOpCtlPhysical, nullptr, 0, 20000.0, &ms);
            if (!r) {
                ++lost;
                continue;
            }
            if (r->size() == 1 && (*r)[0] == 0xFF) {
                ++ff;
                continue;
            }
            if (r->size() != 32) {
                ++odd;
                continue;
            }
            ++ok32;
            rtt.push_back(ms);
            const PadPhysical v = parsePhysical(r->data());
            if (have && v.seq != prev.seq) {
                ++seqChanges;
                ++seqStep[v.seq - prev.seq];
                ++usbStep[v.usbTime - prev.usbTime];
                ++dtHist[v.dt()];
                if (v.baseline()) ++baseline;
                if (v.invalid()) ++invalid;
            }
            ++cadHist[v.cadence];
            ++rxVals[v.s.rx];
            ++ryVals[v.s.ry];
            ++lxVals[v.s.lx];
            ++lyVals[v.s.ly];
            prev = v;
            have = true;
        }
        const double sec = toMs(now() - t0) / 1000.0;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        std::sort(rtt.begin(), rtt.end());
        double sum = 0.0;
        for (double v : rtt) sum += v;
        std::printf("    запросов %llu за %.2f с = %.0f/с; ответов по 32 байта %llu, отказов FF %llu, без ответа %llu, другой длины %llu\n",
            static_cast<unsigned long long>(req), sec, req / sec, static_cast<unsigned long long>(ok32), static_cast<unsigned long long>(ff),
            static_cast<unsigned long long>(lost), static_cast<unsigned long long>(odd));
        if (!rtt.empty()) {
            std::printf("    RTT: среднее %.3f мс, медиана %.3f, p99 %.3f, max %.3f\n", sum / rtt.size(), rtt[rtt.size() / 2],
                rtt[std::min(rtt.size() - 1, rtt.size() * 99 / 100)], rtt.back());
        }
        std::printf("    sequence менялся %llu раз = %.0f/с; baseline %llu, invalid %llu\n", static_cast<unsigned long long>(seqChanges),
            seqChanges / sec, static_cast<unsigned long long>(baseline), static_cast<unsigned long long>(invalid));
        auto dump = [](const char* title, const std::map<uint32_t, uint64_t>& m) {
            std::printf("    %s:", title);
            int n = 0;
            for (const auto& [k, c] : m) {
                if (++n > 14) {
                    std::printf(" ...(%zu значений)", m.size());
                    break;
                }
                std::printf(" %u x%llu", k, static_cast<unsigned long long>(c));
            }
            std::printf("\n");
        };
        dump("шаг sequence между опросами", seqStep);
        dump("шаг usb_timestamp между опросами, x125 мкс", usbStep);
        dump("dt_uframes при смене sequence", dtHist);
        dump("cadence_uframes", cadHist);
        auto dumpAxis = [](const char* title, const std::map<long long, uint64_t>& m) {
            std::printf("    %s: %zu разных значений:", title, m.size());
            int n = 0;
            for (const auto& [k, c] : m) {
                if (++n > 12) {
                    std::printf(" ...");
                    break;
                }
                std::printf(" %lld x%llu", k, static_cast<unsigned long long>(c));
            }
            std::printf("\n");
        };
        dumpAxis("правый стик X в покое", rxVals);
        dumpAxis("правый стик Y в покое", ryVals);
        dumpAxis("левый стик X в покое", lxVals);
        dumpAxis("левый стик Y в покое", lyVals);
    }
    if (!p.async.empty()) {
        std::printf("== Асинхронные кадры за пробу: %zu ==\n", p.async.size());
        for (size_t i = 0; i < p.async.size() && i < 10; ++i) std::printf("    %02X: %s\n", p.async[i].op, hex(p.async[i].pl).c_str());
    }
    timeEndPeriod(1);
    p.close();
    return 0;
}
