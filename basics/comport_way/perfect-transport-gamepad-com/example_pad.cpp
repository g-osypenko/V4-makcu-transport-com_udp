// example_pad.cpp - пример и самопроверка makcu_gamepad_raw_transport.h + makcu_gamepad_renderer.h (COM).
//
//   example_pad.exe [--port COM5] --check        связь, прошивка, геймпад, настройки, 1 с опроса 0x54. Ничего не инжектит.
//   example_pad.exe [--port COM5] --buttons S    S секунд печатает нажатия контролов (жать кнопки, триггеры).
//   example_pad.exe [--port COM5] [--plain]      2 с круга правым стиком (радиус 12000, 1 оборот в секунду) через
//                                                PadTextureRenderer, затем отпускание нулями. --plain: чистая кривая
//                                                без рендерера. ГЕЙМПАД НЕ ТРОГАТЬ. Окна нет.
//   --smoothing off|fixed|auto                   к любому режиму: поставить сглаживание платы кодом (setSmoothing,
//                                                APPLY без SAVE). При выходе возвращается прежнее; --keep - оставить.
//
// Что показывает пример:
//   1. цель задаётся раз в commandPeriodUs(): 2 мс при сглаживании платы Auto (чаще плата не берёт), 1 мс при Off;
//      пресет рендерера под режим платы: presetFor() при Auto, presetOff() при Off;
//   2. ось Y платы направлена вниз: вверх = отрицательное ry;
//   3. в конце ОБЯЗАТЕЛЬНО releaseRightStick(): без него стик остался бы зажатым.
//
// Сборка: build_example.bat
#include "makcu_gamepad_raw_transport.h"
#include "makcu_gamepad_renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using Q = makcu::detail::Qpc;

const char* controlName(int id) {
    static const char* const n[23] = {"SOUTH (A)", "EAST (B)", "WEST (X)", "NORTH (Y)", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "LEFT_SHOULDER",
        "RIGHT_SHOULDER", "LEFT_TRIGGER", "RIGHT_TRIGGER", "LX", "LY", "RX", "RY", "LEFT_STICK_BUTTON", "RIGHT_STICK_BUTTON", "SELECT", "START", "MODE",
        "GRIP_LEFT", "GRIP_RIGHT"};
    static char extra[16];
    if (id < 23) return n[id];
    std::snprintf(extra, sizeof(extra), "EXTRA_%d", id - 22);
    return extra;
}

void printPhysical(const makcu::PadPhysical& p) {
    std::printf("    физика: кнопки %08X %08X | LT %u RT %u | левый стик %d, %d | правый стик %d, %d (Y вниз)\n", p.digitalLow, p.digitalHigh, p.lt, p.rt,
        p.lx, p.ly, p.rx, p.ry);
    std::printf("    sequence %u, usb_timestamp %u, dt %.3f мс%s%s, cadence %.3f мс, возраст %.0f мкс, live %s, отчётов получено %llu\n", p.sequence, p.usbTimestamp,
        p.dtMs(), p.baselineRestarted ? " (baseline)" : "", p.timingInvalid ? " (INVALID)" : "", p.cadenceUframes * 0.125, p.ageUs, p.live ? "да" : "НЕТ",
        static_cast<unsigned long long>(p.samples));
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string port;
    bool check = false, plain = false, keep = false;
    int smoothing = -1;  // --smoothing: 0 Off, 1 Fixed, 2 Auto; -1 - не трогать
    double buttonsSec = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--check") check = true;
        else if (a == "--plain") plain = true;
        else if (a == "--buttons" && more) buttonsSec = std::atof(argv[++i]);
        else if (a == "--smoothing" && more) {
            const std::string m = argv[++i];
            smoothing = m == "off" ? 0 : m == "fixed" ? 1 : m == "auto" ? 2 : -2;
        } else if (a == "--keep") keep = true;
        else smoothing = -2;
        if (smoothing == -2) {
            std::printf("example_pad.exe [--port COM5] [--check | --buttons S | --plain] [--smoothing off|fixed|auto [--keep]]\n");
            return 2;
        }
    }
    makcu::GamepadRawTransport tx;
    if (smoothing >= 0) {
        tx.setSmoothing(static_cast<uint8_t>(smoothing));  // до open(): применится при открытии, если геймпад на месте
        tx.keepSmoothingOnClose(keep);
    }
    std::string error;
    if (!tx.open(port, error)) {
        std::printf("[-] %s\n", error.c_str());
        return 1;
    }
    std::printf("[+] %s\n", tx.info().c_str());
    const makcu::PadSettings st = tx.settings();
    if (st.valid) {
        std::printf("    настройки платы: интерполяция %u (0 Off, 1 Fixed, 2 Auto), буфер %u мс, разброс времени %u %%, ревизия %u, записей настроек %llu\n",
            st.interpolation, st.bufferMs, st.timingVariance, st.revision, static_cast<unsigned long long>(tx.stats().settingsWrites));
        if (smoothing >= 0) {
            std::printf("[%c] --smoothing: на плате %s; при выходе %s\n", st.interpolation == smoothing ? '+' : '-',
                st.interpolation == smoothing ? "стоит заданное значение" : "заданное значение НЕ встало", keep ? "останется как есть (--keep)" : "вернётся прежнее");
        }
        std::printf("    правый стик: strength %s, inertia %u %%, micro %u %%, limit %u %%, разброс величины %u %%, угла %u %%, мёртвая зона %u %%, change deadband %u %%\n",
            st.strengthPresent ? std::to_string(st.strength).c_str() : "не задана", st.inertia, st.micro, st.limit, st.magnitudeVariance, st.angleVariance,
            st.centerDeadzone, st.changeDeadband);
        std::printf("    транспорт: период команд %.0f мкс, плавный вход %.0f мс, выход %.0f мс\n", tx.commandPeriodUs(), tx.blendEnterMs(), tx.blendExitMs());
    } else {
        std::printf("[!] настройки платы не прочитаны\n");
    }
    Sleep(1000);
    printPhysical(tx.physical());
    std::printf("    %s\n", tx.statsLine().c_str());
    if (tx.controllerKind() == 0) {
        std::printf("[-] плата не видит геймпад на USB3 (kinds без геймпада). Транспорт будет ждать его сам.\n");
        tx.close();
        return check ? 0 : 1;
    }
    if (check) {
        tx.close();
        return 0;
    }

    if (buttonsSec > 0) {
        std::printf("[i] %.0f с: нажимай кнопки, D-pad, бамперы, триггеры, стики\n", buttonsSec);
        std::array<uint32_t, makcu::kPadControlCount> seen{};
        const int64_t end = Q::now() + Q::fromUs(buttonsSec * 1e6);
        while (Q::now() < end) {
            for (int id = 0; id < makcu::kPadControlCount; ++id) {
                const uint32_t n = tx.pressCount(static_cast<makcu::MakcuControlId>(id));
                if (n != seen[static_cast<size_t>(id)]) {
                    seen[static_cast<size_t>(id)] = n;
                    const makcu::PadPhysical p = tx.physical();
                    std::printf("    %-18s нажатий %u (LT %u RT %u)\n", controlName(id), n, p.lt, p.rt);
                }
            }
            Sleep(2);
        }
        printPhysical(tx.physical());
        std::printf("    %s\n", tx.statsLine().c_str());
        tx.close();
        return 0;
    }

    // --- движение: круг правым стиком ---
    const makcu::PadPhysical start = tx.physical();
    makcu::PadTextureRenderer renderer;
    const double periodUs = tx.commandPeriodUs();
    {
        // Пресет под режим платы: при Off - модель АЦП геймпада на такте 1 мс, при Auto - каскады под интерполятор платы.
        makcu::PadRendererProfile prof = tx.boardSmoothingOff() ? makcu::PadTextureRenderer::presetOff(tx.controllerKind())
                                                                : makcu::PadTextureRenderer::presetFor(tx.controllerKind());
        prof.tick_hz = static_cast<float>(1e6 / periodUs);  // такт рендерера = период команд
        renderer.setProfile(prof);
    }
    renderer.beginEvent(0x1234ABCDull, start.rx, start.ry);
    const double kPi = 3.14159265358979323846;
    std::printf("[i] 2 с: круг правым стиком, радиус 12000, 1 оборот в секунду, команда раз в %.0f мкс, %s. ГЕЙМПАД НЕ ТРОГАТЬ\n", periodUs,
        plain ? "чистая кривая" : "через PadTextureRenderer");
    int64_t stepNs = 0;
    int steps = 0;
    tx.setRightStick(start.rx, start.ry);  // первая команда: текущее положение стика, без ступеньки
    const uint64_t c0 = tx.commandCount();
    for (;;) {
        // Ждём, пока транспорт отправит прошлое значение: шаг рендерера раз на команду. Время берётся из счётчика
        // команд транспорта, а не из числа оборотов цикла: если этот поток опоздал, цель не съезжает по времени.
        if (!tx.waitCommand(50)) {
            std::printf("[-] транспорт не отправил команду за 50 мс (связь?)\n");
            break;
        }
        const double t = static_cast<double>(tx.commandCount() - c0 + 1) * periodUs / 1e6;
        if (t >= 2.0) break;
        // плавный вход по радиусу за 0.25 с, чтобы не начинать ступенькой
        const double r = 12000.0 * std::min(1.0, t / 0.25);
        const double tx_ = r * std::cos(2.0 * kPi * t), ty_ = r * std::sin(2.0 * kPi * t);  // +Y вниз: по часовой стрелке на экране
        int16_t ox = 0, oy = 0;
        if (plain) {
            ox = static_cast<int16_t>(std::lround(tx_));
            oy = static_cast<int16_t>(std::lround(ty_));
        } else {
            const int64_t a = Q::now();
            renderer.step(tx_, ty_, ox, oy);
            stepNs += static_cast<int64_t>(Q::toUs(Q::now() - a) * 1000.0);
            ++steps;
        }
        tx.setRightStick(ox, oy);
    }
    // Отпускание: без него стик остался бы зажатым на последнем значении.
    const bool released = tx.releaseRightStick(200);
    std::printf("[%c] стик отпущен нулями, подтверждение GET 0x40: %s\n", released ? '+' : '-', released ? "да" : "НЕТ");
    Sleep(50);
    printPhysical(tx.physical());
    if (steps) std::printf("    renderer.step(): в среднем %.0f нс\n", static_cast<double>(stepNs) / steps);
    std::printf("    %s\n", tx.statsLine().c_str());
    tx.close();
    return 0;
}
