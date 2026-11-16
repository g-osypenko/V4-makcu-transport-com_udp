// pad_probe_watch.cpp - сторож: ждёт, пока плата MAKCU увидит геймпад на USB3. ТОЛЬКО ЧТЕНИЕ, окна нет.
// Раз в 500 мс: GET 0x02 DEVICE и GET 0x54 CONTROLLER_PHYSICAL. Каждую смену kinds печатает со временем
// и с ответом km.device(). Выходит с кодом 0, когда в kinds есть геймпад (kinds & FC) и 0x54 отдаёт
// 32 байта 2 с подряд; с кодом 4 - по истечении --wait секунд; с кодом 1 - порт не открылся.
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_watch.cpp -lwinmm -static -o pad_probe_watch.exe
// Запуск: pad_probe_watch.exe [--port COM5] [--wait 540]
#include "pad_probe_common.h"

using namespace padprobe;

namespace {
std::string clockStr() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[16];
    std::snprintf(s, sizeof(s), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    setupConsole();
    std::string port = "COM5";
    double waitSec = 540.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--wait" && more) waitSec = std::atof(argv[++i]);
        else {
            std::printf("pad_probe_watch.exe [--port COM5] [--wait 540]\n");
            return 2;
        }
    }
    Port p;
    if (!p.open(port)) {
        std::printf("[-] %s не открылся: ошибка %lu\n", port.c_str(), GetLastError());
        return 1;
    }
    std::printf("[i] %s сторож: жду геймпад на USB3 платы, до %.0f с\n", clockStr().c_str(), waitSec);
    const int64_t t0 = now();
    int last = -1, good = 0, lastPhys = -1;
    while (toMs(now() - t0) < waitSec * 1000.0) {
        int kinds = -2;
        if (auto r = p.ask(kOpDevice, nullptr, 0, 200000.0)) kinds = r->empty() ? -2 : (*r)[0];
        int phys = 0;  // 0 нет ответа, 1 отказ FF, 2 снимок 32 байта
        std::vector<uint8_t> snap;
        if (auto r = p.ask(kOpCtlPhysical, nullptr, 0, 50000.0)) {
            phys = r->size() == 32 ? 2 : 1;
            snap = *r;
        }
        if (kinds != last || phys != lastPhys) {
            const std::string dev = escaped(p.text("km.device()", 60));
            if (kinds == -2) std::printf("[i] %s плата не отвечает на DEVICE\n", clockStr().c_str());
            else
                std::printf("[i] %s kinds %02X = %s; 0x54: %s; km.device() -> \"%s\"\n", clockStr().c_str(), kinds, kindsText(static_cast<uint8_t>(kinds)).c_str(),
                    phys == 2 ? "снимок 32 байта" : phys == 1 ? "отказ FF" : "нет ответа", dev.c_str());
            if (phys == 2) {
                const PadPhysical v = parsePhysical(snap.data());
                std::printf("    правый стик %d, %d; левый стик %d, %d; LT %u RT %u; seq %u, dt %u uf, cadence %u uf\n", v.s.rx, v.s.ry, v.s.lx, v.s.ly,
                    v.s.lt, v.s.rt, v.seq, v.dt(), v.cadence);
            }
            last = kinds;
            lastPhys = phys;
        }
        good = (kinds > 0 && (kinds & 0xFC) && phys == 2) ? good + 1 : 0;
        if (good >= 4) {
            std::printf("[+] %s геймпад на плате: kinds %02X, 0x54 отвечает снимком\n", clockStr().c_str(), kinds);
            p.close();
            return 0;
        }
        Sleep(500);
    }
    std::printf("[-] %s за %.0f с геймпад на плате не появился\n", clockStr().c_str(), waitSec);
    p.close();
    return 4;
}
