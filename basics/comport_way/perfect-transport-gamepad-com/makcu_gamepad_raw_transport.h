// MAKCU: транспорт геймпада по COM (USB2, CH343, 4 Мбод) - эталон, ревизия 4 (2026-11-20).
//
// Один заголовок, C++20, без зависимостей кроме WinAPI (SetupAPI для поиска порта).
// Проверено на плате V4.122 с геймпадом GameSir в режиме XInput (kinds 0x82), interpolation = auto, буфер 8 мс.
//
// РЕВИЗИЯ 4: ЖИВУЧЕСТЬ ПРИ ОТКАЗАХ ПЛАТЫ И ГЕЙМПАДА (HANDOFF_GAMEPAD_COM.md, 9м). На 4133 безопасного темпа нет:
// геймпад пропадает с платы (kinds 00, 0x54 -> FF) и сам встаёт за ~2 с, после первого отказа идёт буря. До ревизии 4
// транспорт всё это время бил плату на полном темпе (опрос до 2000/с, SET 0x40, починки сверки) - вероятный катализатор
// бури. Теперь:
//   - отказ (DEVICE -> kinds 0 или 3 ответа FF на 0x54 подряд): команды SET 0x40 приостановлены, опрос 0x54 раз в 50 мс,
//     сверка не идёт; после возврата (новый sequence) ещё 250 мс тишины, затем инжект продолжается сам (при Off - снова
//     с плавным входом). padAbsent(), commandsSuspended();
//   - буря (3 отказа за 60 с): 60 с после последнего отказа опрос не чаще 100/с. stormActive();
//   - зависание геймпада (снимки 0x54 приходят, sequence стоит > 300 мс; GameSir 2026-11-20 так висел до перезапуска
//     самого геймпада): padStalled() - сообщить пользователю; инжект продолжается (плата отдаёт его Windows и так);
//   - по совету MAKCU (2026-11-20 14:57): команды не чаще 200/с (раз в 5 мс) при любом режиме сглаживания (было 2 мс при
//     Auto / Fixed, 1 мс при Off; утром 20.11 - 4 мс); неизменная команда повторяется раз в 250 мс, а не каждый период;
//     удержание кнопок без стика - тоже раз в 250 мс (было 100); отпускание - один пакет нулей (повтор только если
//     сверка GET 0x40 разошлась);
//     опрос по умолчанию - 250/с (раз в 4 мс; в ревизии 3 было 2 мс). Под инжектом плата получает от геймпада ~500
//     отчётов/с, поэтому чаще 500/с опрос новых значений не даёт, а стику (абсолютная величина) хватает 250/с.
//     Заданные вызывающим периоды берутся как есть.
//
// РЕВИЗИЯ 3: ГЕЙМПАДЫ 125 / 250 / 500 / 1000 ГЦ (HANDOFF_GAMEPAD_COM.md, 9к). Клон на плате объявляет Windows тот же
// интервал опроса, что и настоящий геймпад (cadence в 0x54), поэтому:
//   - период команд не короче интервала геймпада: commandPeriodUs() = max(период режима или заданный, padCadenceUs());
//   - опрос 0x54 по умолчанию max(2 мс, интервал геймпада) вместо 0.5 мс: 500 запросов/с на 1000-Гц геймпаде
//     (2000 запросов/с ронял USB3 платы: V4.126 5 из 5, 4133 раз за ~15 мин), меньше на медленных;
//   - padCadenceUs(), padReportRate() (измеренная частота), в Stats и statsLine() - интервал, частота и оба периода.
// На настоящем геймпаде 125 / 250 / 500 Гц НЕ ПРОВЕРЕНО (у нас только GameSir с cadence 1 мс). Сборка 2026-11-17 на
// плате ещё не запускалась.
//
// РЕВИЗИЯ 2: РАБОТА ПРИ СГЛАЖИВАНИИ ПЛАТЫ OFF (HANDOFF_GAMEPAD_COM.md, 9б-9е). Описание ниже написано для Auto;
// при Off плата - чистая задержка 4-5 мс, каждая команда раз в 1 мс доходит как есть, а вход в инжект и отпускание -
// скачки. Поэтому, когда настройки платы говорят Off, транспорт сам:
//   - шлёт команды раз в 1 мс (commandPeriodUs() = 1000; при Auto 2000), если период не задан явно;
//   - делает плавный вход и выход (setBlendMs): выход = физика + (цель - физика) * s(w), s - smoothstep, w идёт
//     0 -> 1 за 16 мс при входе и 1 -> 0 за 24 мс при отпускании; нули (handoff) уходят только когда w дошёл до 0.
//     Физика - свежий снимок 0x54, поэтому рука на стике во время перехода продолжает работать.
// setSmoothing(0) ставит Off кодом (документированные BEGIN / WRITE / APPLY записи 0x1D, БЕЗ SAVE: в NOR не пишется,
// перезагрузка платы возвращает сохранённое) и close() возвращает прежнее значение.
// Кнопки и триггеры: setButton(id, down), setTrigger(id, 0..1023), tap(id, holdMs) - нажать и отпустить без ожидания.
//
// Что транспорт делает:
//   - инжект ТОЛЬКО правого стика: setRightStick(rx, ry) / aim(rx, ry) - абсолютное отклонение -32768..32767,
//     SET 0x40 CONTROLLER_STATE (20 байт, все остальные поля 0 = "не перехватывать");
//   - отпускание: releaseRightStick() - SET 0x40 с нулями (zero-state handoff): плата сама плавно возвращает выход к
//     физическому стику. Без этого стик остаётся зажатым на последнем значении НАВСЕГДА (до следующего open());
//   - чтение всего физического геймпада: physical() - снимок GET 0x54 CONTROLLER_PHYSICAL (55 контролов, триггеры,
//     оба стика, sequence, usb_timestamp, dt, cadence), buttonDown(), pressCount();
//   - чтение настроек обработки (0x3E, запись 0x1D, INFO и READ): settings(). Записывает только по просьбе
//     вызывающего и только одно поле - интерполяцию (setSmoothing); без вызова setSmoothing() ничего не пишет;
//   - чистый старт и выход: open() и close() шлют SET 0x40 с нулями и проверяют GET 0x40;
//   - сверка: раз в 250 мс и после каждого отпускания GET 0x40 сравнивается с отправленным, расхождение чинится;
//   - переподключение: ошибка ввода-вывода или молчание платы 750 мс - порт закрывается, ищется заново, после
//     восстановления перехват снят (цель устарела), вызывающий продолжает слать setRightStick().
// Чего транспорт НЕ делает:
//   - не смешивает инжект с физическим стиком (это дело вызывающего и платы);
//   - не трогает левый стик (всегда 0 в SET 0x40 = физика проходит насквозь); кнопки и триггеры - только если
//     вызывающий зовёт setButton() / setTrigger() / tap() (ревизия 2), иначе они тоже 0;
//   - не может отжать кнопку, которую держит рука (нужна маска CONTROLLER_MASK 0x51, её здесь нет);
//   - не сохраняет настройки в NOR (SAVE) и не включает потоки событий.
//
// ОСЬ Y НАПРАВЛЕНА ВНИЗ. На плате -32768 = стик вверх до упора, +32767 = вниз. В Windows XInput наоборот,
// пересчёт точный: sThumbRY = -ry - 1 (замер: задано ry -12000 -> Windows +11999). Ось X совпадает.
// padToXInputY() / padFromXInputY() делают этот пересчёт.
//
// ПАРА (0, 0) - ЭТО НЕ ЦЕНТР, А "ОТПУСТИТЬ СТИК". Центр под управлением программы - (0, -1): в Windows это ровно
// (0, 0). setRightStick(0, 0) транспорт сам отправляет как (0, -1); отпускает только releaseRightStick().
//
// Темп команд (замер 2026-11-14, README): плата при interpolation = auto берёт цель раз в 2 мс.
//   раз в 2 мс: выход линейно следует за командами (R2 0.96-0.99), отставание ~6.5 мс;
//   раз в 1 мс: две трети команд пропадают, рисунок с периодом 3 отчёта, R2 0.60.
// Поэтому транспорт сам шлёт SET 0x40 раз в 2 мс (до ревизии 4; с ревизии 4 по умолчанию раз в 4 мс - см. выше;
// setCommandPeriodUs), каждый раз самое свежее значение
// (latest-wins, без суммирования). Вызывающему достаточно обновлять цель раз в 2 мс: commandPeriodUs().
// До ревизии 4 команда повторялась каждый период, даже если цель не менялась; с ревизии 4 неизменная команда - раз в
// 250 мс (MAKCU: плата держит состояние сама). Влияние на интерполятор при неизменной цели не перемерялось.
// Побочный эффект платы: пока идут команды, физический геймпад читается ~500 раз/с вместо 1000 (dt 1 и 3 мс).
//
// Ввод-вывод: один рабочий поток, синхронный порт, такт 500 мкс. В начале такта читается всё, что пришло
// (без ожидания), в конце уходят запросы (GET 0x54 раз в physicalPollUs(), SET 0x40 раз в commandPeriodUs(),
// GET 0x40 раз в 250 мс);
// ответы разбираются на следующем такте. Поток не ждёт порт и не пользуется тайм-аутами драйвера.
// ТАЙМ-АУТЫ ЧТЕНИЯ COM-ПОРТА НЕ ИСПОЛЬЗОВАТЬ: первая версия с ожиданием первого байта до 2 мс 2026-11-14 дала
// синий экран 0xD1 в CH343S64.SYS 2.2.2026.5 через ~12 тыс. запросов (README, "Синий экран в драйвере CH343").
//
// Контракт вызова: все методы можно звать из любого потока. Ни один не блокируется на порту, кроме
// open(), close(), releaseRightStick(waitMs > 0) и clearAllOverrides() (ждут подтверждения до waitMs).
// Порт эксклюзивный: MAKCU Toolkit и SDK mak-suite должны быть закрыты.
//
// Сборка: g++ -std=c++20 -O2 ... -lsetupapi -lwinmm
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace makcu {

namespace detail {
#ifndef MAKCU_DETAIL_QPC_DEFINED
#define MAKCU_DETAIL_QPC_DEFINED
// QPC-часы транспорта (общий блок с makcu_raw_transport.h и makcu_udp_batch_transport.h).
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
    static int64_t fromUs(double us) {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(freq()) / 1e6));
    }
    static double toUs(int64_t ticks) { return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq()); }
};
#endif  // MAKCU_DETAIL_QPC_DEFINED
}  // namespace detail

#ifndef MAKCU_FIND_PORTS_DEFINED
#define MAKCU_FIND_PORTS_DEFINED
// COM-порты MAKCU (мост CH343, VID 1A86 PID 55D3), например {"COM5"}.
inline std::vector<std::string> findMakcuComPorts() {
    static const GUID kPortsClass = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
    std::vector<std::string> ports;
    HDEVINFO set = SetupDiGetClassDevsA(&kPortsClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return ports;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        char hwid[1024]{};
        if (!SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(hwid), sizeof(hwid) - 1, nullptr)) continue;
        std::string id(hwid);
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (id.find("VID_1A86&PID_55D3") == std::string::npos) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char name[256]{};
        DWORD size = sizeof(name) - 1;
        if (RegQueryValueExA(key, "PortName", nullptr, nullptr, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS) ports.emplace_back(name);
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(ports.begin(), ports.end());
    return ports;
}
#endif  // MAKCU_FIND_PORTS_DEFINED

#ifndef MAKCU_PAD_TYPES_DEFINED
#define MAKCU_PAD_TYPES_DEFINED
// Общий блок для COM- и UDP-транспорта геймпада: можно подключать оба заголовка.

// Контролы геймпада MAK_API (mak-suite/protocol/MAK_API.md, раздел Controller): номер = бит в digital_low
// (0..31) и digital_high (32..54). Имена - по положению, а не по надписи на кнопке.
enum class MakcuControlId : uint8_t {
    South = 0, East = 1, West = 2, North = 3,
    DpadUp = 4, DpadDown = 5, DpadLeft = 6, DpadRight = 7,
    LeftShoulder = 8, RightShoulder = 9,
    LeftTrigger = 10, RightTrigger = 11,                       // аналоговые 0..1023
    LeftStickX = 12, LeftStickY = 13, RightStickX = 14, RightStickY = 15,  // оси -32768..32767
    LeftStickButton = 16, RightStickButton = 17,
    Select = 18, Start = 19, Mode = 20, GripLeft = 21, GripRight = 22,
    Extra1 = 23, Extra32 = 54,                                 // Extra1..Extra32 = 23..54
};
inline constexpr uint8_t kPadControlCount = 55;
inline constexpr uint16_t kPadTriggerMax = 1023;
inline constexpr uint16_t kPadTriggerDown = 512;  // триггер считается нажатым для buttonDown()/pressCount() с этого значения

// Тип геймпада: kinds & 0xFC из ответа DEVICE (0x02).
inline constexpr uint8_t kPadKindGenericHid = 0x04, kPadKindDs4 = 0x08, kPadKindDualSense = 0x10, kPadKindDualSenseEdge = 0x20,
                         kPadKindXboxGip = 0x40, kPadKindXInput = 0x80;
inline const char* padKindName(uint8_t kind) {
    if (kind & kPadKindXInput) return "Xbox 360 / XInput";
    if (kind & kPadKindXboxGip) return "Xbox GIP";
    if (kind & kPadKindDualSenseEdge) return "DualSense Edge";
    if (kind & kPadKindDualSense) return "DualSense";
    if (kind & kPadKindDs4) return "DS4";
    if (kind & kPadKindGenericHid) return "HID";
    return "нет";
}

// Ось Y платы направлена вниз, XInput - вверх. Пересчёт точный в обе стороны.
inline constexpr int16_t padToXInputY(int16_t boardY) { return static_cast<int16_t>(-static_cast<int32_t>(boardY) - 1); }
inline constexpr int16_t padFromXInputY(int16_t xinputY) { return static_cast<int16_t>(-static_cast<int32_t>(xinputY) - 1); }

// Снимок физического геймпада (GET 0x54): то, что сообщил сам геймпад, до масок, кривых, интерполяции и инжекта.
struct PadPhysical {
    uint32_t digitalLow = 0;        // контролы 0..31
    uint32_t digitalHigh = 0;       // контролы 32..54 в битах 0..22
    uint16_t lt = 0, rt = 0;        // 0..1023
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // -32768..32767, +Y ВНИЗ
    uint32_t sequence = 0;          // растёт на каждый отчёт геймпада
    uint32_t usbTimestamp = 0;      // время отчёта по USB, единицы 125 мкс
    uint16_t dtUframes = 0;         // промежуток до предыдущего отчёта, единицы 125 мкс (8 = 1 мс)
    bool baselineRestarted = false; // отсчёт dt начат заново: dt не интегрировать
    bool timingInvalid = false;     // dt непригоден
    uint16_t cadenceUframes = 0;    // заданный период отчётов геймпада, единицы 125 мкс
    double ageUs = 0.0;             // сколько назад транспорт получил этот sequence
    bool live = false;              // геймпад на месте: последний опрос вернул снимок
    uint8_t controllerKind = 0;     // kinds & 0xFC
    uint64_t samples = 0;           // сколько разных sequence получено с open()
    double dtMs() const { return dtUframes * 0.125; }
    bool down(MakcuControlId id) const {
        const auto i = static_cast<uint8_t>(id);
        if (i == 10) return lt >= kPadTriggerDown;
        if (i == 11) return rt >= kPadTriggerDown;
        if (i >= 12 && i <= 15) return false;
        if (i < 32) return (digitalLow >> i) & 1u;
        if (i < kPadControlCount) return (digitalHigh >> (i - 32)) & 1u;
        return false;
    }
};

// Настройки обработки геймпада на плате (образ 400 байт 0x3E/0x1D, только чтение).
struct PadSettings {
    bool valid = false;
    uint32_t revision = 0;
    uint8_t interpolation = 0;      // 0 Off, 1 Fixed, 2 Auto. Образ: u32 по смещению 4
    uint8_t bufferMs = 0;           // u32 по смещению 8
    uint8_t timingVariance = 0;     // %, биты 22..28 флагов по смещению 0
    // behaviors[0] = правый стик
    bool rightStickEnabled = false;
    bool strengthPresent = false;
    uint8_t strength = 0, inertia = 0, micro = 0, limit = 0, magnitudeVariance = 0, angleVariance = 0;
    uint8_t centerDeadzone = 0, antiDeadzone = 0, changeDeadband = 0;  // кривая[0] правого стика, %
    bool interpolationAuto() const { return valid && interpolation == 2; }
};
#endif  // MAKCU_PAD_TYPES_DEFINED

class GamepadRawTransport {
public:
    // Ревизия 4 (2026-11-20): 200 команд/с при любом режиме - совет MAKCU (ответ 2026-11-20 14:57, проверен их ведущим
    // по прошивке): «состояние контроллера слать равномерно, не чаще ~200/с (не ближе 5 мс), без пачек; 1000/с не
    // отзывчивее, плата только копит очередь». Сначала было 250/с. До ревизии 4: 2000 при Auto / Fixed, 1000 при Off - на
    // 4126 / 4133 это отказы геймпада (HANDOFF_GAMEPAD_COM.md, 9и-9м). Чаще - только setCommandPeriodUs().
    static constexpr double kDefaultCommandPeriodUs = 5000.0;
    static constexpr double kOffCommandPeriodUs = 5000.0;
    // MAKCU 2026-11-20: «держать одно состояние долго - повтор раз в 250-500 мс достаточно; keepalive плата держит сама».
    // Поэтому неизменная команда (стик, кнопки, триггеры те же) повторяется не каждый период, а раз в 250 мс.
    static constexpr double kHoldResendUs = 250000.0;
    static constexpr double kOffBlendEnterMs = 16.0;           // плавный вход при Off (при Auto плата сглаживает сама ~12 мс)
    static constexpr double kOffBlendExitMs = 24.0;            // плавный выход при Off
    // Повтор удерживаемых кнопок и триггеров, пока стик не перехвачен: 250 мс (MAKCU 2026-11-20: 250-500 мс достаточно;
    // сторож молчания платы 1 с). Было 100 мс.
    static constexpr double kPadKeepaliveUs = 250000.0;
    static constexpr double kTickUs = 500.0;                  // такт рабочего потока (опрос 0x54 - раз в physicalPollUs())
    // Опрос 0x54 по умолчанию: не чаще раза в 4 мс (250/с; ревизия 4, решение владельца 2026-11-20) и не чаще интервала
    // геймпада. Стик - абсолютная величина: снимок сразу даёт всё положение, пропуск теряет только форму движения между
    // снимками; физика нужна для плавного входа / выхода, смешивания с рукой и признака "жив" - 250/с хватает (снимок не
    // старше 4 мс). Под инжектом плата и так получает от геймпада ~500 отчётов/с. Для записи текстуры руки -
    // setPhysicalPollUs(). Было: 2000 мкс (ревизия 3), 500 мкс до неё (2000/с: V4.126 5 отказов USB3 из 5).
    static constexpr double kDefaultPollUs = 4000.0;
    static constexpr double kMaxAutoPollUs = 32000.0;           // потолок авто-опроса (снимок свежий 50 мс)
    static constexpr double kRateWindowUs = 500000.0;          // окно замера частоты отчётов геймпада
    // Поток физики (setPhysicalSource(Stream)): 0x54 нужен только ради sequence / cadence / частоты и признака "живой".
    static constexpr double kStreamPollUs = 250000.0;
    static constexpr double kStreamCheckUs = 1000000.0;        // как часто спрашивать 0x73 / 0x74, не выключился ли поток
    static constexpr double kVerifyUs = 250000.0;              // сверка GET 0x40
    static constexpr double kReleaseVerifyUs = 5000.0;         // сверка после отпускания и после починки
    static constexpr double kReplyTimeoutUs = 20000.0;         // нет ответа столько - запрос потерян
    static constexpr double kLinkLostUs = 750000.0;            // плата молчит столько - связь потеряна
    static constexpr double kReconnectRetryUs = 250000.0;
    static constexpr double kKindsPollUs = 500000.0;           // геймпада нет: как часто спрашивать DEVICE
    static constexpr double kLiveUs = 50000.0;                 // снимок старше - геймпад не "live"
    // Ревизия 4: живучесть (HANDOFF_GAMEPAD_COM.md, 9м).
    static constexpr int kAbsentFfRun = 3;                     // столько FF на 0x54 подряд - геймпада нет
    static constexpr double kAbsentPollUs = 50000.0;           // опрос 0x54, пока геймпада нет
    static constexpr double kResumeSettleUs = 250000.0;        // тишина после возврата геймпада до первой команды
    static constexpr double kStallUs = 300000.0;               // снимки идут, sequence стоит столько - геймпад завис
    static constexpr int kStormDrops = 3;                      // столько отказов за kStormWindowUs - буря
    static constexpr double kStormWindowUs = 60e6;
    static constexpr double kCalmUs = 60e6;                    // после последнего отказа в буре столько опрос ограничен
    static constexpr double kCalmPollUs = 10000.0;             //   не чаще 100/с

    struct Stats {
        bool connected = false;
        uint8_t controllerKind = 0;
        uint64_t polls = 0;          // запросов 0x54
        uint64_t snapshots = 0;      // ответов по 32 байта
        uint64_t newSamples = 0;     //   из них с новым sequence
        uint64_t rejects = 0;        // отказов 0x54 (FF): геймпада нет или снимок недоступен; координаты не затираются
        uint64_t lost = 0;           // запросов без ответа
        uint64_t skipped = 0;        // пропущенных отчётов геймпада (шаг sequence больше 1)
        uint64_t stickCommands = 0;  // SET 0x40 с целью
        uint64_t releases = 0;       // SET 0x40 с нулями
        uint64_t padCommands = 0;    // SET 0x40 с изменением кнопок / триггеров или их удержанием без стика
        uint64_t stickRejects = 0;   // отказов 0x40 (FF): нет маршрута геймпада
        uint64_t verifies = 0;       // сверок GET 0x40
        uint64_t repairs = 0;        // сверка разошлась - команда повторена
        uint64_t linkLost = 0, reconnects = 0;
        uint64_t settingsWrites = 0; // сколько раз транспорт записал интерполяцию (BEGIN / WRITE / APPLY)
        bool boardSmoothingOff = false;  // настройки прочитаны, сглаживание платы Off
        double lastOutageMs = 0.0;
        double cmdIntervalAvgUs = 0.0, cmdIntervalMaxUs = 0.0;  // между командами цели
        double tickLateMaxUs = 0.0;  // наибольшее опоздание такта рабочего потока
        bool injecting = false;      // перехват правого стика сейчас активен
        int16_t sentRx = 0, sentRy = 0;
        double padCadenceUs = 0.0;   // интервал отчётов, объявленный геймпадом (cadence из 0x54); 0 - ещё не известен
        double padReportRate = 0.0;  // измеренная частота отчётов геймпада, в с (по sequence за ~0.5 с)
        double commandPeriodUs = 0.0, physicalPollUs = 0.0;  // действующие периоды
        // Поток физики 0x73 / 0x74 -> 0x75 (setPhysicalSource)
        bool streamWanted = false;   // вызывающий просил поток
        bool streamActive = false;   // плата подтвердила оба потока в двоичном режиме
        bool streamUnsupported = false;  // плата ответила FF на запрос 0x73 / 0x74
        uint64_t streamEvents = 0;   // событий 0x75 разобрано
        uint64_t streamUnavailable = 0;  // событий с id 255 ("снимок недоступен")
        uint64_t streamWrites = 0;   // сколько раз транспорт включал / выключал поток
        uint64_t streamFailures = 0; // включение не подтвердилось (повтор через kStreamCheckUs)
        // Ревизия 4: живучесть
        bool padAbsent = false;      // плата сообщила, что геймпада нет (kinds 0 / FF подряд)
        bool padStalled = false;     // снимки идут, sequence стоит > 300 мс (геймпад завис)
        bool commandsSuspended = false;  // SET 0x40 сейчас не шлются (геймпада нет или пауза после возврата)
        bool storm = false;          // буря: опрос ограничен 100/с
        uint64_t padDrops = 0, padStalls = 0, storms = 0;
        uint64_t heldSkips = 0;      // тактов, на которых неизменная команда не повторялась (повтор раз в 250 мс)
        double lastPadDownMs = 0.0;  // сколько длился последний отказ (до возврата sequence)
    };

    GamepadRawTransport() { cmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~GamepadRawTransport() {
        close();
        if (cmdEvent_) CloseHandle(cmdEvent_);
    }
    GamepadRawTransport(const GamepadRawTransport&) = delete;
    GamepadRawTransport& operator=(const GamepadRawTransport&) = delete;

    // Открыть порт (пустая строка - первый найденный MAKCU), снять оставшийся перехват, прочитать настройки.
    // true, если плата отвечает; геймпада при этом может ещё не быть (controllerKind() == 0): транспорт ждёт его сам.
    bool open(const std::string& port, std::string& error) {
        close();
        autoPort_ = port.empty();
        std::string name = port;
        if (name.empty()) {
            const auto found = findMakcuComPorts();
            if (found.empty()) {
                error = "MAKCU (CH343 1A86:55D3) не найден";
                return false;
            }
            name = found.front();
        }
        resetStats();
        if (!connect(name, error)) {
            closeHandle();
            return false;
        }
        timeBeginPeriod(1);
        timerPeriod_ = true;
        stop_ = false;
        connected_ = true;
        lastRxT_ = detail::Qpc::now();
        worker_ = std::thread([this] { workerLoop(); });
        return true;
    }

    // Отпустить стик (с подтверждением), остановить поток, вернуть интерполяцию платы, если её менял setSmoothing(),
    // закрыть порт.
    void close() {
        if (worker_.joinable()) {
            clearAllOverrides(150);
            stop_ = true;
            worker_.join();
            // Рабочий поток остановлен: порт теперь у этого потока. Если связь потеряна, вернуть нечем: на плате
            // останется заданное значение до её перезагрузки (APPLY в NOR не пишет).
            // Без геймпада плата отдаёт в INFO "разделы 00" и раздела "контроллер" для записи нет (снимки во время
            // отказов, pad_probes/data/drop_dongle2_info_t10.txt). 2026-11-19 (4133) геймпад отвалился перед close(), и
            // плата осталась в Fixed / 2 мс. Поэтому ждать до 4 с: 4133 сама возвращает геймпад за ~1.5 с.
            restoreFailed_ = false;
            if (haveRestore_ && !keepSmoothing_.load() && connected_.load() && handle_ != INVALID_HANDLE_VALUE && !ioError_.load()) {
                bool ok = false;
                const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(4e6);
                while (!(ok = writeSmoothing(restoreInterp_, restoreBuffer_)) && !ioError_.load() && detail::Qpc::now() < until) Sleep(200);
                if (ok) readSettings();
                restoreFailed_ = !ok;
            }
            haveRestore_ = false;
            // Поток физики, если транспорт его включал: выключить (mode 0), чтобы плата не слала события в закрытый порт.
            if (streamTouched_ && connected_.load() && handle_ != INVALID_HANDLE_VALUE && !ioError_.load()) disableStream();
        }
        closeHandle();
        connected_ = false;
        if (timerPeriod_) {
            timeEndPeriod(1);
            timerPeriod_ = false;
        }
    }

    bool isOpen() const { return worker_.joinable(); }
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint32_t firmware() const { return firmware_.load(); }
    uint8_t controllerKind() const { return kind_.load(std::memory_order_acquire); }

    // ---- инжект правого стика ------------------------------------------------------------------
    // Задать цель правого стика (координаты платы, +Y ВНИЗ). Не блокируется: значение ложится в слот,
    // рабочий поток шлёт самое свежее раз в commandPeriodUs(). (0, 0) уходит как (0, -1) - центр, а не отпускание.
    void setRightStick(int16_t rx, int16_t ry) {
        if (rx == 0 && ry == 0) ry = -1;
        slot_.store(pack(true, rx, ry), std::memory_order_release);
    }
    void aim(int16_t rx, int16_t ry) { setRightStick(rx, ry); }

    // Отпустить правый стик: zero-state handoff. Плата сама переводит выход к физическому положению
    // (замер: начало через ~4-5 мс, конец через ~12-15 мс). waitMs > 0 - дождаться, пока GET 0x40 вернёт нули.
    // При плавном выходе (setBlendMs, по умолчанию включён при сглаживании платы Off) нули уходят после перехода;
    // ожидание waitMs продлевается на его длительность.
    bool releaseRightStick(int waitMs = 0) {
        slot_.store(pack(false, 0, 0), std::memory_order_release);
        if (waitMs <= 0) return true;
        const uint64_t want = releaseSeq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const int64_t deadline = detail::Qpc::now() + detail::Qpc::fromUs((waitMs + blendExitMs()) * 1000.0);
        while (detail::Qpc::now() < deadline) {
            if (releaseDone_.load(std::memory_order_acquire) >= want) return true;
            if (!worker_.joinable() || !connected()) return false;
            Sleep(1);
        }
        return false;
    }

    // Снять все перехваты: SET 0x40 со всеми нулями и проверка GET 0x40. (Транспорт перехватывает только
    // правый стик, но после чужой программы на плате могло остаться что угодно.)
    bool clearAllOverrides(int waitMs = 100) {
        releaseButtons();
        return releaseRightStick(std::max(waitMs, 1));
    }

    bool injecting() const { return injecting_.load(std::memory_order_acquire); }

    // ---- инжект кнопок и триггеров -------------------------------------------------------------
    // Правило платы (MAK_API.md, Controller): в SET 0x40 бит кнопки 1 - кнопка нажата программой, 0 - проходит
    // физическая; триггер 1..1023 - значение программы, 0 - проходит физический. То есть программа может НАЖАТЬ, но не
    // может ОТЖАТЬ кнопку, которую держит рука (для этого у платы отдельная маска CONTROLLER_MASK 0x51, здесь её нет).
    // Все методы не блокируются. Пока стик не перехвачен, изменение уходит на ближайшем такте (до 0.5 мс) и повторяется
    // раз в 100 мс (сторож молчания платы 1 с); пока стик перехвачен - едет в той же команде, что и стик (до периода
    // команд). После обрыва связи всё отпускается, как и стик.
    //
    // Держать кнопку нажатой / отпустить. Триггеры (LeftTrigger, RightTrigger): down = 1023. Оси стиков игнорируются.
    void setButton(MakcuControlId id, bool down) {
        const auto i = static_cast<uint8_t>(id);
        if (!padControlInjectable(i)) return;
        tapUntil_[i].store(0, std::memory_order_release);
        applyControl(i, down ? kPadTriggerMax : 0);
    }
    // Аналоговое значение триггера 0..1023 (0 - отпустить). id - LeftTrigger или RightTrigger.
    void setTrigger(MakcuControlId id, uint16_t value) {
        const auto i = static_cast<uint8_t>(id);
        if (i != 10 && i != 11) return;
        tapUntil_[i].store(0, std::memory_order_release);
        applyControl(i, std::min(value, kPadTriggerMax));
    }
    // Нажать и отпустить через holdMs: отпускает рабочий поток, вызывающему ждать не надо. Для триггера
    // triggerValue - глубина нажатия 1..1023. Повторный tap() той же кнопки до отпускания продлевает нажатие.
    void tap(MakcuControlId id, int holdMs = 60, uint16_t triggerValue = kPadTriggerMax) {
        const auto i = static_cast<uint8_t>(id);
        if (!padControlInjectable(i)) return;
        applyControl(i, std::clamp<uint16_t>(triggerValue, 1, kPadTriggerMax));
        tapUntil_[i].store(detail::Qpc::now() + detail::Qpc::fromUs(std::max(holdMs, 1) * 1000.0), std::memory_order_release);
        anyTap_.store(true, std::memory_order_release);
    }
    // Отпустить все кнопки и триггеры, заданные программой (стик не трогает).
    void releaseButtons() {
        for (auto& t : tapUntil_) t.store(0, std::memory_order_release);
        btnLow_.store(0, std::memory_order_release);
        btnHigh_.store(0, std::memory_order_release);
        lt_.store(0, std::memory_order_release);
        rt_.store(0, std::memory_order_release);
    }
    // Что сейчас задано программой: кнопка нажата / значение триггера.
    bool buttonInjected(MakcuControlId id) const {
        const auto i = static_cast<uint8_t>(id);
        if (i == 10) return lt_.load(std::memory_order_acquire) != 0;
        if (i == 11) return rt_.load(std::memory_order_acquire) != 0;
        if (i < 32) return (btnLow_.load(std::memory_order_acquire) >> i) & 1u;
        if (i < kPadControlCount) return (btnHigh_.load(std::memory_order_acquire) >> (i - 32)) & 1u;
        return false;
    }
    uint16_t triggerInjected(MakcuControlId id) const {
        return id == MakcuControlId::LeftTrigger ? lt_.load(std::memory_order_acquire) : id == MakcuControlId::RightTrigger ? rt_.load(std::memory_order_acquire) : 0;
    }

    // Период команд цели, мкс. Не задан (или us <= 0) - 5000 (ревизия 4, совет MAKCU, при любом режиме; до неё 2000 при Auto / Fixed
    // и 1000 при Off). Округляется до целого числа тактов по 500 мкс, меньше 1000 не ставится.
    // Ревизия 3 (2026-11-17): и заданный, и по умолчанию - НЕ КОРОЧЕ интервала геймпада (padCadenceUs()). Клон на плате
    // объявляет Windows тот же интервал опроса, что и настоящий геймпад (CONTROLLER_PHYSICAL.md: cadence - "configured
    // report cadence"), поэтому чаще плата всё равно не отдаст, а лишние команды только грузят COM. Геймпад 125 / 250 /
    // 500 Гц -> команда раз в 8 / 4 / 2 мс. Пока cadence не пришёл (первые мс после open()), действует период режима.
    void setCommandPeriodUs(double us) { periodUs_.store(us > 0.0 ? std::max(us, 1000.0) : 0.0, std::memory_order_release); }
    double commandPeriodUs() const {
        const double p = periodUs_.load(std::memory_order_acquire);
        const double mode = p > 0.0 ? p : (boardOff_.load(std::memory_order_acquire) ? kOffCommandPeriodUs : kDefaultCommandPeriodUs);
        return std::max(mode, padCadenceUs_.load(std::memory_order_acquire));
    }

    // Период опроса физического геймпада (GET 0x54), мкс: целое число тактов по 500 мкс.
    // По умолчанию (us <= 0) - max(4000, интервал геймпада) (ревизия 4; в ревизии 3 max(2000, ...)), то есть 250
    // запросов/с на 1000-, 500- и 250-Гц геймпаде и 125/с на 125-Гц. Чаще интервала геймпада опрос не даёт новых отчётов, только свежесть снимка.
    // Заданное значение (500..10000) берётся как есть - для проб нагрузки (pad_soak.cpp). Было до ревизии 3: 500.
    void setPhysicalPollUs(double us) { pollUs_.store(us > 0.0 ? std::clamp(us, kTickUs, 10000.0) : 0.0, std::memory_order_release); }
    double physicalPollUs() const {
        const double p = pollUs_.load(std::memory_order_acquire);
        if (p > 0.0) return p;
        if (streamOn_.load(std::memory_order_acquire)) return kStreamPollUs;
        return std::clamp(padCadenceUs_.load(std::memory_order_acquire), kDefaultPollUs, kMaxAutoPollUs);
    }

    // ---- источник физики: опрос 0x54 или поток 0x73 / 0x74 (ревизия 3, 2026-11-17, НА ПЛАТЕ НЕ ПРОВЕРЕНО) ------
    // Poll (по умолчанию) - как раньше: GET 0x54 раз в physicalPollUs(), транспорт 0x73 / 0x74 не трогает вовсе.
    // Stream - плата сама шлёт события 0x75 (kind 1 оси id 10-15, kind 2 кнопки; value u16 LE; только изменившиеся
    // значения, проверка раз в periodMs 1..1000) и physical() обновляется по ним; GET 0x54 остаётся раз в 250 мс ради
    // sequence, cadence, частоты и признака live. Включение: запрос с пустым телом (ответ mode u8 + period u16 LE), если
    // не "2, periodMs" - SET [2, period], затем снова запрос и сверка. Плата ответила FF - поток не поддерживается
    // (streamUnsupported), транспорт остаётся на опросе. По документации makcu.com потоки есть "с V4.105 (test build)",
    // выключены после включения питания и после переподключения; поэтому транспорт спрашивает их состояние раз в 1 с и
    // включает заново, а при появлении геймпада и после переподключения платы - сразу. close() выключает (mode 0).
    // MAKCU 2026-11-17: поток "должен быть легче опроса", данных нет. Минусы: в событиях нет sequence и времени отчёта;
    // во время редких синхронных обменов (настройки, включение потока) пришедшие события пропускаются - кнопки и оси
    // догоняет ближайший снимок 0x54 (до 250 мс).
    enum class PhysicalSource : uint8_t { Poll = 0, Stream = 1 };
    void setPhysicalSource(PhysicalSource s, int streamPeriodMs = 1) {
        streamPeriodMs_.store(static_cast<uint16_t>(std::clamp(streamPeriodMs, 1, 1000)), std::memory_order_release);
        wantStream_.store(s == PhysicalSource::Stream, std::memory_order_release);
        streamSeq_.fetch_add(1, std::memory_order_acq_rel);
    }
    PhysicalSource physicalSource() const { return wantStream_.load(std::memory_order_acquire) ? PhysicalSource::Stream : PhysicalSource::Poll; }
    bool streamActive() const { return streamOn_.load(std::memory_order_acquire); }

    // Интервал отчётов, который объявил геймпад (cadence из 0x54, мкс; 0 - ещё не известен) - этот интервал плата
    // объявляет Windows для клона. И измеренная частота отчётов (новых sequence в с за последние ~0.5 с; 0 - не
    // измерена или геймпада нет). Если геймпад шлёт заметно реже объявленного (GameSir с открытым Connect: ~225 при
    // cadence 1 мс), свободные слоты клона заполнит инжект - Windows увидит больше отчётов, чем шлёт геймпад
    // (225 + 500 команд/с = ~722 пакета/с, HANDOFF_GAMEPAD_COM.md, 9к). Тогда вызывающий может сам поставить
    // setCommandPeriodUs(1e6 / padReportRate()).
    double padCadenceUs() const { return padCadenceUs_.load(std::memory_order_acquire); }
    double padReportRate() const { return padRate_.load(std::memory_order_acquire); }

    // ---- живучесть (ревизия 4) ---------------------------------------------------------------------------
    // Геймпада нет по словам платы (DEVICE -> kinds 0 или 3 FF на 0x54 подряд): отказ 4133 (встаёт сам за ~2 с) или
    // геймпад вынут / уснул. Пока true, SET 0x40 не шлются, опрос раз в 50 мс; setRightStick() можно звать как обычно -
    // после возврата и 250 мс тишины инжект продолжится сам.
    bool padAbsent() const { return padAbsent_.load(std::memory_order_acquire); }
    // Команды сейчас не шлются: геймпада нет или пауза после возврата.
    bool commandsSuspended() const { return suspended_.load(std::memory_order_acquire); }
    // Геймпад завис: плата отдаёт снимки 0x54, но sequence не растёт > 300 мс (у GameSir так бывает до перезапуска самого
    // геймпада; HANDOFF_GAMEPAD_COM.md, 9м). Инжект продолжается - плата отдаёт его Windows и так, но рука не доходит.
    // Вызывающему - сказать пользователю «перезапусти геймпад». НЕ ПРОВЕРЕНО на геймпадах, которые шлют отчёты только при
    // изменениях (у GameSir отчёты идут всегда, 1000/с и без касаний).
    bool padStalled() const { return padStalled_.load(std::memory_order_acquire); }
    // Буря: 3 отказа за 60 с - опрос не чаще 100/с ещё 60 с после последнего отказа.
    bool stormActive() const { return storm_.load(std::memory_order_acquire); }

    // Плавный вход и выход, мс: вес цели w идёт 0 -> 1 за enterMs после первого setRightStick() и 1 -> 0 за exitMs
    // после releaseRightStick(); на плату уходит физика + (цель - физика) * smoothstep(w). 0 - без перехода (скачок,
    // как отдаёт плата). Отрицательное - по настройкам платы: при Off 16 и 24 мс, при Auto / Fixed 0 (плата
    // сглаживает сама). Если физического снимка нет (геймпад молчит), перехода нет.
    void setBlendMs(double enterMs, double exitMs) {
        blendEnterMs_.store(enterMs, std::memory_order_release);
        blendExitMs_.store(exitMs, std::memory_order_release);
    }
    double blendEnterMs() const {
        const double v = blendEnterMs_.load(std::memory_order_acquire);
        return v >= 0.0 ? v : (boardOff_.load(std::memory_order_acquire) ? kOffBlendEnterMs : 0.0);
    }
    double blendExitMs() const {
        const double v = blendExitMs_.load(std::memory_order_acquire);
        return v >= 0.0 ? v : (boardOff_.load(std::memory_order_acquire) ? kOffBlendExitMs : 0.0);
    }

    // Сглаживание (интерполяция) платы: 0 Off, 1 Fixed, 2 Auto. Меняет ОДНО поле образа настроек (u32 по смещению 4)
    // документированной записью 0x3E / 0x1D: INFO, READ, BEGIN, WRITE x5, APPLY (mak-suite/protocol/DEVICE_SETTINGS.md),
    // раздел "контроллер". SAVE не шлётся: в NOR ничего не пишется, перезагрузка платы возвращает сохранённое.
    // Транспорт запоминает значение, которое было до первой удачной записи, и возвращает его в close()
    // (keepSmoothingOnClose(true) - оставить как задано). Желаемое значение помнится: после переподключения платы и
    // после появления геймпада ставится заново. Можно звать до open() (тогда применится при открытии, возврат true)
    // и из любого потока; waitMs - сколько ждать результата. false - не применилось (геймпада нет, плата отказала).
    // Если программа упала, на плате останется заданное значение до перезагрузки платы или до Toolkit.
    bool setSmoothing(uint8_t mode, int waitMs = 1000) {
        if (mode > 2) return false;
        wantInterp_.store(mode, std::memory_order_release);
        if (!worker_.joinable()) return true;
        const uint64_t want = interpSeq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const int64_t deadline = detail::Qpc::now() + detail::Qpc::fromUs(std::max(waitMs, 1) * 1000.0);
        while (detail::Qpc::now() < deadline) {
            if (interpDone_.load(std::memory_order_acquire) >= want) return interpOk_.load(std::memory_order_acquire);
            Sleep(1);
        }
        return false;
    }
    void keepSmoothingOnClose(bool keep) { keepSmoothing_.store(keep, std::memory_order_release); }
    // После close(): true - сглаживание меняли, но вернуть прежнее не удалось (геймпад не вернулся за 4 с, связь
    // потеряна). На плате осталось заданное значение до перезагрузки платы, Toolkit или следующего setSmoothing().
    bool smoothingRestoreFailed() const { return restoreFailed_; }
    // Буфер сглаживания платы, мс (1..64; u32 по смещению 8 образа настроек): пишется той же записью, что и
    // интерполяция, тоже без SAVE, и тоже возвращается в close(). Звать ДО setSmoothing() или open(): само по себе
    // ничего не шлёт, значение уйдёт со следующим применением сглаживания. ms < 0 - буфер не трогать (по умолчанию).
    // Добавлено 2026-11-15 для опыта "Fixed + буфер 2 мс" (HANDOFF_GAMEPAD_COM.md, 9з); смысл буфера не документирован.
    void setSmoothingBuffer(int ms) { wantBuffer_.store(ms < 0 ? -1 : std::clamp(ms, 1, 64), std::memory_order_release); }
    bool boardSmoothingOff() const { return boardOff_.load(std::memory_order_acquire); }
    // Сколько тактов команд цели прошло: отправленные команды плюс такты, на которых неизменную команду не повторяли
    // (ревизия 4). По нему вызывающие считают время (commandCount() * commandPeriodUs()): если бы считались только
    // отправки, при неподвижной цели часы вызывающего вставали бы до повтора раз в 250 мс. Отправки - Stats::stickCommands.
    uint64_t commandCount() const { return cmdSlots_.load(std::memory_order_acquire); }
    // Дождаться отправки очередной команды цели (до timeoutMs). Так вызывающий шагает рендерер ровно раз на команду:
    //   tx.setRightStick(x0, y0);  for (;;) { tx.waitCommand(); renderer.step(...); tx.setRightStick(x, y); }
    // Значение, заданное сразу после waitCommand(), уйдёт следующей командой. false - тайм-аут (связи нет или
    // перехват не активен).
    bool waitCommand(int timeoutMs = 20) { return WaitForSingleObject(cmdEvent_, static_cast<DWORD>(std::max(timeoutMs, 0))) == WAIT_OBJECT_0; }

    // ---- физический геймпад -------------------------------------------------------------------
    PadPhysical physical() const {
        std::lock_guard<std::mutex> lock(physMutex_);
        PadPhysical p = phys_;
        const int64_t now = detail::Qpc::now();
        p.ageUs = physT_ ? detail::Qpc::toUs(now - physT_) : 0.0;
        p.live = snapT_ != 0 && detail::Qpc::toUs(now - snapT_) < liveWindowUs() && connected();
        p.controllerKind = kind_.load();
        return p;
    }
    bool buttonDown(MakcuControlId id) const {
        std::lock_guard<std::mutex> lock(physMutex_);
        return phys_.down(id);
    }
    // Сколько раз контрол был нажат с open() (фронты по снимкам 0x54; для триггеров - переход через kPadTriggerDown).
    uint32_t pressCount(MakcuControlId id) const {
        const auto i = static_cast<uint8_t>(id);
        return i < kPadControlCount ? presses_[i].load(std::memory_order_acquire) : 0;
    }

    // Настройки обработки на плате, прочитанные при открытии (и заново при появлении геймпада).
    PadSettings settings() const {
        std::lock_guard<std::mutex> lock(physMutex_);
        return settings_;
    }

    Stats stats() const {
        Stats s;
        s.connected = connected();
        s.controllerKind = kind_.load();
        s.polls = polls_.load();
        s.snapshots = snapshots_.load();
        s.newSamples = newSamples_.load();
        s.rejects = rejects_.load();
        s.lost = lost_.load();
        s.skipped = skipped_.load();
        s.stickCommands = stickCommands_.load();
        s.releases = releases_.load();
        s.padCommands = padCommands_.load();
        s.stickRejects = stickRejects_.load();
        s.verifies = verifies_.load();
        s.repairs = repairs_.load();
        s.linkLost = linkLost_.load();
        s.reconnects = reconnects_.load();
        s.settingsWrites = settingsWrites_.load();
        s.boardSmoothingOff = boardOff_.load();
        s.lastOutageMs = lastOutageMs_.load();
        const uint64_t m = cmdIntCount_.load();
        s.cmdIntervalAvgUs = m ? cmdIntSumUs_.load() / static_cast<double>(m) : 0.0;
        s.cmdIntervalMaxUs = cmdIntMaxUs_.load();
        s.tickLateMaxUs = tickLateMaxUs_.load();
        s.injecting = injecting();
        const uint64_t sent = sent_.load();
        s.sentRx = unpackX(sent);
        s.sentRy = unpackY(sent);
        s.padCadenceUs = padCadenceUs();
        s.padReportRate = padReportRate();
        s.commandPeriodUs = commandPeriodUs();
        s.physicalPollUs = physicalPollUs();
        s.streamWanted = wantStream_.load();
        s.streamActive = streamOn_.load();
        s.streamUnsupported = streamUnsupported_.load();
        s.streamEvents = streamEvents_.load();
        s.streamUnavailable = streamUnavailable_.load();
        s.streamWrites = streamWrites_.load();
        s.streamFailures = streamFailures_.load();
        s.padAbsent = padAbsent();
        s.padStalled = padStalled();
        s.commandsSuspended = commandsSuspended();
        s.storm = stormActive();
        s.padDrops = padDrops_.load();
        s.padStalls = padStalls_.load();
        s.storms = storms_.load();
        s.heldSkips = heldSkips_.load();
        s.lastPadDownMs = lastPadDownMs_.load();
        return s;
    }
    void resetStats() {
        polls_ = snapshots_ = newSamples_ = rejects_ = lost_ = skipped_ = 0;
        answered_ = 0;
        stickCommands_ = releases_ = stickRejects_ = verifies_ = repairs_ = 0;
        padCommands_ = 0;
        linkLost_ = reconnects_ = 0;
        lastOutageMs_ = 0.0;
        cmdIntCount_ = 0;
        cmdIntSumUs_ = 0.0;
        cmdIntMaxUs_ = 0.0;
        tickLateMaxUs_ = 0.0;
        for (auto& p : presses_) p = 0;
        padRate_ = 0.0;
        streamEvents_ = streamUnavailable_ = streamWrites_ = streamFailures_ = 0;
        padDrops_ = padStalls_ = storms_ = heldSkips_ = 0;
        lastPadDownMs_ = 0.0;
    }
    std::string statsLine() const {
        const Stats s = stats();
        char b[640];
        std::snprintf(b, sizeof(b),
            "связь %s, геймпад %s (интервал %.0f мкс, отчётов %.0f/с; периоды: команд %.0f, опроса %.0f мкс) | "
            "0x54: запросов %llu, новых отчётов %llu, отказов FF %llu, без ответа %llu, пропущено отчётов %llu | "
            "стик: команд %llu (интервал %.0f/%.0f мкс), отпусканий %llu, команд кнопок %llu, отказов %llu, сверок %llu, починок %llu | "
            "такт опоздал до %.0f мкс | обрывов %llu, восстановлений %llu",
            s.connected ? "есть" : "НЕТ", padKindName(s.controllerKind), s.padCadenceUs, s.padReportRate, s.commandPeriodUs, s.physicalPollUs,
            u(s.polls), u(s.newSamples), u(s.rejects), u(s.lost), u(s.skipped),
            u(s.stickCommands), s.cmdIntervalAvgUs, s.cmdIntervalMaxUs, u(s.releases), u(s.padCommands), u(s.stickRejects), u(s.verifies), u(s.repairs),
            s.tickLateMaxUs,
            u(s.linkLost), u(s.reconnects));
        std::string line = b;
        std::snprintf(b, sizeof(b), " | без повтора неизменной команды %llu | отказов геймпада %llu (последний %.1f с), зависаний %llu, бурь %llu%s%s%s", u(s.heldSkips),
            u(s.padDrops), s.lastPadDownMs / 1000.0, u(s.padStalls), u(s.storms), s.padAbsent ? ", СЕЙЧАС НЕТ ГЕЙМПАДА" : "", s.padStalled ? ", СЕЙЧАС ЗАВИС" : "", s.storm ? ", буря: опрос <= 100/с" : "");
        line += b;
        if (s.streamWanted || s.streamEvents) {
            std::snprintf(b, sizeof(b), " | поток 0x73/0x74: %s, событий %llu, недоступно %llu, включений %llu, неудач %llu",
                s.streamActive ? "вкл" : (s.streamUnsupported ? "НЕ ПОДДЕРЖИВАЕТСЯ (FF)" : "выкл"), u(s.streamEvents), u(s.streamUnavailable),
                u(s.streamWrites), u(s.streamFailures));
            line += b;
        }
        return line;
    }
    // Строка о подключении: порт, скорость, прошивка, геймпад, интерполяция, снят ли перехват.
    std::string info() const {
        std::lock_guard<std::mutex> lock(physMutex_);
        return info_;
    }

private:
    static constexpr uint8_t kOpDevice = 0x02, kOpFirmware = 0x04, kOpConnection = 0x3E, kOpCtlState = 0x40, kOpCtlPhysical = 0x54,
                             kOpInputChange = 0x53, kOpAxesStream = 0x73, kOpButtonsStream = 0x74, kOpStreamEvent = 0x75;
    static unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }

    // Слот цели: бит 32 - перехват активен, биты 0..15 rx, 16..31 ry.
    static uint64_t pack(bool active, int16_t rx, int16_t ry) {
        return (active ? (1ull << 32) : 0ull) | static_cast<uint16_t>(rx) | (static_cast<uint64_t>(static_cast<uint16_t>(ry)) << 16);
    }
    static bool unpackActive(uint64_t v) { return (v >> 32) & 1ull; }
    static int16_t unpackX(uint64_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }
    static int16_t unpackY(uint64_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v >> 16)); }

    static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
    static uint32_t le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    }

    struct Frame {
        uint8_t op = 0;
        std::vector<uint8_t> pl;
    };

    // ---- порт (все обращения - из одного потока: open()/close() или рабочий) ----------------------
    bool openHandle(const std::string& name, std::string& error) {
        const std::string path = "\\\\.\\" + name;
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = "CreateFile(" + name + ") failed, error " + std::to_string(GetLastError()) + " (порт занят другой программой?)";
            return false;
        }
        SetupComm(handle_, 16384, 4096);
        // ЧТЕНИЕ ТОЛЬКО БЕЗ ОЖИДАНИЯ: ReadFile сразу возвращает то, что уже лежит в драйвере. Тайм-ауты чтения
        // (ReadTotalTimeoutConstant и подобные) НЕ ИСПОЛЬЗОВАТЬ: 2026-11-14 первая версия этого транспорта ждала первый
        // байт до 2 мс и через ~12 тыс. запросов уронила Windows в синий экран 0xD1 в CH343S64.SYS 2.2.2026.5
        // (README, "Синий экран"). Пробы с чтением без ожидания сделали в тот же день больше 650 тыс. запросов чисто.
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = 0;
        t.ReadTotalTimeoutConstant = 0;
        t.WriteTotalTimeoutConstant = 200;  // как в пробах и com_soak (3.48 млн запросов чисто)
        SetCommTimeouts(handle_, &t);
        port_ = name;
        rx_.clear();
        return true;
    }
    bool configure(uint32_t baud) {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) return false;
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;  // иначе можно перезапустить ESP32
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        if (!SetCommState(handle_, &dcb)) return false;
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        rx_.clear();
        return true;
    }
    void closeHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    bool sendFrame(uint8_t op, const uint8_t* pl = nullptr, size_t len = 0) {
        uint8_t d[160] = {0xDE, 0xAD, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), op};  // WRITE настроек: 104 байта
        if (len > sizeof(d) - 5) return false;
        if (len) std::memcpy(d + 5, pl, len);
        DWORD done = 0;
        if (!WriteFile(handle_, d, static_cast<DWORD>(5 + len), &done, nullptr) || done != 5 + len) {
            ioError_ = true;
            return false;
        }
        return true;
    }
    // Забрать всё, что уже лежит в драйвере (без ожидания). true - что-то пришло.
    bool drain() {
        uint8_t buf[1024];
        bool any = false;
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(handle_, buf, sizeof(buf), &n, nullptr)) {
                ioError_ = true;
                return any;
            }
            if (!n) return any;
            rx_.insert(rx_.end(), buf, buf + n);
            lastRxT_ = detail::Qpc::now();
            any = true;
        }
    }
    bool nextFrame(Frame& f) {
        size_t i = 0;
        for (; i + 5 <= rx_.size(); ++i) {
            if (rx_[i] != 0xDE || rx_[i + 1] != 0xAD) continue;
            const size_t plen = static_cast<size_t>(rx_[i + 2]) | static_cast<size_t>(rx_[i + 3]) << 8;
            if (plen > 512) continue;
            if (i + 5 + plen > rx_.size()) break;
            f.op = rx_[i + 4];
            f.pl.assign(rx_.begin() + static_cast<std::ptrdiff_t>(i + 5), rx_.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            return true;
        }
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(std::min(i, rx_.size())));
        return false;
    }
    // Синхронный запрос (открытие, настройки, сверка при выходе): отправить и ждать ответ с тем же опкодом,
    // опрашивая порт без ожидания. Чужие кадры (события 0x53, уведомления 0x3E, ответы на прежние запросы)
    // пропускаются. wantLen: 0 - любая длина; иначе кадр другой длины, кроме отказа (1 байт FF), пропускается.
    std::optional<std::vector<uint8_t>> ask(uint8_t op, const uint8_t* pl, size_t len, double timeoutUs, size_t wantLen = 0) {
        const int64_t t0 = detail::Qpc::now();
        if (!sendFrame(op, pl, len)) return std::nullopt;
        const int64_t end = t0 + detail::Qpc::fromUs(timeoutUs);
        for (;;) {
            Frame f;
            while (nextFrame(f)) {
                if (f.op != op) continue;
                if (op == kOpConnection && f.pl.size() == 13 && f.pl[0] == 0x12) continue;  // уведомление о топологии
                if (wantLen && f.pl.size() != wantLen && !(f.pl.size() == 1 && f.pl[0] == 0xFF)) continue;
                return f.pl;
            }
            if (drain()) continue;
            if (ioError_.load() || detail::Qpc::now() >= end) return std::nullopt;
            YieldProcessor();
        }
    }

    using PadState = std::array<uint8_t, 20>;  // полное состояние SET / GET 0x40: <IIHHhhhh
    static PadState encodeState(uint32_t low, uint32_t high, uint16_t lt, uint16_t rt, int16_t rx, int16_t ry) {
        PadState pl{};
        const auto put16 = [&pl](size_t at, uint16_t v) {
            pl[at] = static_cast<uint8_t>(v);
            pl[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        put16(0, static_cast<uint16_t>(low));
        put16(2, static_cast<uint16_t>(low >> 16));
        put16(4, static_cast<uint16_t>(high));
        put16(6, static_cast<uint16_t>(high >> 16));
        put16(8, lt);
        put16(10, rt);
        put16(16, static_cast<uint16_t>(rx));
        put16(18, static_cast<uint16_t>(ry));
        return pl;
    }
    bool sendStick(int16_t rx, int16_t ry) {
        const PadState pl = encodeState(0, 0, 0, 0, rx, ry);
        return sendFrame(kOpCtlState, pl.data(), pl.size());
    }
    static bool padControlInjectable(uint8_t i) { return i < kPadControlCount && !(i >= 12 && i <= 15); }
    // Записать значение контрола в слоты кнопок и триггеров (любой поток).
    void applyControl(uint8_t i, uint16_t value) {
        if (i == 10) lt_.store(value, std::memory_order_release);
        else if (i == 11) rt_.store(value, std::memory_order_release);
        else if (i < 32) {
            if (value) btnLow_.fetch_or(1u << i, std::memory_order_acq_rel);
            else btnLow_.fetch_and(~(1u << i), std::memory_order_acq_rel);
        } else if (i < kPadControlCount) {
            if (value) btnHigh_.fetch_or(1u << (i - 32), std::memory_order_acq_rel);
            else btnHigh_.fetch_and(~(1u << (i - 32)), std::memory_order_acq_rel);
        }
    }
    // Отпустить кнопки, у которых вышло время tap() (рабочий поток).
    void expireTaps(int64_t now) {
        anyTap_.store(false, std::memory_order_release);  // до просмотра: tap() из другого потока взведёт заново
        bool left = false;
        for (uint8_t i = 0; i < kPadControlCount; ++i) {
            int64_t until = tapUntil_[i].load(std::memory_order_acquire);
            if (!until) continue;
            if (now >= until) {
                if (tapUntil_[i].compare_exchange_strong(until, 0, std::memory_order_acq_rel)) applyControl(i, 0);
                else left = true;  // tap() успел продлить
            } else {
                left = true;
            }
        }
        if (left) anyTap_.store(true, std::memory_order_release);
    }
    // Все нули и проверка GET 0x40 (до 5 попыток). Вызывается при открытии, переподключении и выходе.
    bool clearBoardState() {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (!sendStick(0, 0)) return false;
            releases_.fetch_add(1, std::memory_order_relaxed);
            Sleep(3);
            auto st = ask(kOpCtlState, nullptr, 0, kReplyTimeoutUs, 20);
            if (st && st->size() == 20 && std::all_of(st->begin(), st->end(), [](uint8_t b) { return b == 0; })) return true;
        }
        return false;
    }

    // Образ настроек 400 байт: INFO + READ кусками по 96. sections - какие разделы есть (1 - контроллер).
    bool readImage(std::array<uint8_t, 400>& img, uint32_t& rev, uint8_t& sections) {
        const uint8_t q[2] = {0x1D, 0x00};
        auto info = ask(kOpConnection, q, sizeof(q), 300000.0);
        if (!(info && info->size() == 14 && (*info)[0] == 0x1D && (*info)[1] == 0x00 && (*info)[2] == 0 && le16(&(*info)[12]) == 400)) return false;
        rev = le32(&(*info)[8]);
        sections = (*info)[4];
        for (unsigned off = 0; off < 400; off += 96) {
            const unsigned len = std::min(96u, 400u - off);
            const uint8_t rd[9] = {0x1D, 0x01, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16),
                static_cast<uint8_t>(rev >> 24), static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8), static_cast<uint8_t>(len)};
            auto r = ask(kOpConnection, rd, sizeof(rd), 300000.0);
            if (!(r && r->size() == 9 + len && (*r)[0] == 0x1D && (*r)[1] == 0x01 && (*r)[2] == 0 && le32(&(*r)[3]) == rev && le16(&(*r)[7]) == off)) return false;
            std::copy(r->begin() + 9, r->end(), img.begin() + off);
        }
        return true;
    }
    static bool settingsReply(const std::optional<std::vector<uint8_t>>& r, uint8_t op, size_t size) {
        return r && r->size() == size && (*r)[0] == 0x1D && (*r)[1] == op && (*r)[2] == 0;
    }
    // Записать интерполяцию (u32 по смещению 4 образа) и применить вживую. Остальные 396 байт уходят такими, какими
    // прочитаны. Порядок по DEVICE_SETTINGS.md: BEGIN (ревизия, раздел 1) -> токен; WRITE все 400 байт по порядку;
    // APPLY. Ошибка на любом шаге - DISCARD. SAVE не шлётся. Вызывать только из потока, владеющего портом.
    // true - APPLY подтверждён или значение уже такое; после вызова нужен readSettings() для сверки.
    // interp и bufferMs: отрицательное - поле не трогать (буфер - u32 по смещению 8).
    bool writeSmoothing(int interp, int bufferMs) {
        std::array<uint8_t, 400> img{};
        uint32_t rev = 0;
        uint8_t sections = 0;
        if (!readImage(img, rev, sections) || !(sections & 1u)) return false;
        const bool sameInterp = interp < 0 || le32(&img[4]) == static_cast<uint32_t>(interp);
        const bool sameBuffer = bufferMs < 0 || le32(&img[8]) == static_cast<uint32_t>(bufferMs);
        if (sameInterp && sameBuffer) return true;
        if (interp >= 0) {
            img[4] = static_cast<uint8_t>(interp);
            img[5] = img[6] = img[7] = 0;
        }
        if (bufferMs >= 0) {
            img[8] = static_cast<uint8_t>(bufferMs);
            img[9] = img[10] = img[11] = 0;
        }
        const uint8_t bg[7] = {0x1D, 0x02, static_cast<uint8_t>(rev), static_cast<uint8_t>(rev >> 8), static_cast<uint8_t>(rev >> 16), static_cast<uint8_t>(rev >> 24), 0x01};
        auto b = ask(kOpConnection, bg, sizeof(bg), 300000.0);
        if (!settingsReply(b, 0x02, 7)) return false;
        const uint8_t token[4] = {(*b)[3], (*b)[4], (*b)[5], (*b)[6]};
        bool ok = true;
        for (unsigned off = 0; off < 400 && ok; off += 96) {
            const unsigned len = std::min(96u, 400u - off);
            uint8_t wr[8 + 96] = {0x1D, 0x03, token[0], token[1], token[2], token[3], static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8)};
            std::memcpy(wr + 8, &img[off], len);
            auto r = ask(kOpConnection, wr, 8 + len, 300000.0);
            ok = settingsReply(r, 0x03, 5) && le16(&(*r)[3]) == off + len;
        }
        if (ok) {
            const uint8_t ap[6] = {0x1D, 0x04, token[0], token[1], token[2], token[3]};
            ok = settingsReply(ask(kOpConnection, ap, sizeof(ap), 300000.0), 0x04, 3);
        }
        if (!ok) {
            const uint8_t dc[6] = {0x1D, 0x06, token[0], token[1], token[2], token[3]};
            ask(kOpConnection, dc, sizeof(dc), 100000.0);
            return false;
        }
        settingsWrites_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Привести интерполяцию платы к заданной setSmoothing(). Вызывается после readSettings() при открытии,
    // переподключении, появлении геймпада и по запросу. true - менять не просили или на плате уже нужное значение.
    bool enforceSmoothing() {
        const int want = wantInterp_.load(std::memory_order_acquire), wantBuf = wantBuffer_.load(std::memory_order_acquire);
        if (want < 0 && wantBuf < 0) return true;
        const auto asWanted = [&](const PadSettings& q) { return q.valid && (want < 0 || q.interpolation == want) && (wantBuf < 0 || q.bufferMs == wantBuf); };
        PadSettings s = settings();
        if (asWanted(s)) return true;
        const bool hadValue = s.valid;
        const uint8_t was = s.interpolation, wasBuf = s.bufferMs;
        if (!writeSmoothing(want, wantBuf)) return false;
        readSettings();
        s = settings();
        if (!asWanted(s)) return false;
        if (hadValue && !haveRestore_) {
            restoreInterp_ = was;
            restoreBuffer_ = wasBuf;
            haveRestore_ = true;
        }
        return true;
    }

    void readSettings() {
        PadSettings s;
        {
            std::array<uint8_t, 400> img{};
            uint32_t rev = 0;
            uint8_t sections = 0;
            const bool ok = readImage(img, rev, sections);
            if (ok) {
                const uint32_t flags = le32(&img[0]);
                s.valid = true;
                s.revision = rev;
                s.interpolation = static_cast<uint8_t>(le32(&img[4]));
                s.bufferMs = static_cast<uint8_t>(le32(&img[8]));
                s.timingVariance = static_cast<uint8_t>((flags >> 22) & 127);
                const uint32_t bf = le32(&img[20]);  // behaviors[0] - правый стик
                if (bf & 1u) {
                    s.rightStickEnabled = true;
                    s.inertia = static_cast<uint8_t>((bf >> 1) & 127);
                    s.micro = static_cast<uint8_t>((bf >> 8) & 127);
                    s.limit = static_cast<uint8_t>((bf >> 15) & 127);
                    s.strengthPresent = img[25] != 15;
                    s.strength = s.strengthPresent ? static_cast<uint8_t>(img[25] & 127) : 0;
                    s.magnitudeVariance = img[26];
                    s.angleVariance = img[27];
                    s.centerDeadzone = img[52];
                    s.antiDeadzone = img[53];
                    s.changeDeadband = img[54];
                }
            }
        }
        boardOff_.store(s.valid && s.interpolation == 0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(physMutex_);
        settings_ = s;
    }
    // Строка info(): порт, прошивка, геймпад, интерполяция (и кто её поставил), буфер, снят ли перехват.
    void updateInfo(bool clean) {
        const PadSettings s = settings();
        const char* const names[3] = {"off", "fixed", "auto"};
        char mode[96];
        if (!s.valid) std::snprintf(mode, sizeof(mode), "не прочитана");
        else if (haveRestore_) std::snprintf(mode, sizeof(mode), "%s (поставил транспорт, было %s)", names[std::min<int>(s.interpolation, 2)], names[std::min<int>(restoreInterp_, 2)]);
        else std::snprintf(mode, sizeof(mode), "%s", names[std::min<int>(s.interpolation, 2)]);
        char b[400];
        std::snprintf(b, sizeof(b), "%s @ %u бод, прошивка %u, геймпад: %s, интерполяция %s, буфер %u мс, период команд %.0f мкс, перехват снят: %s", port_.c_str(),
            baud_, firmware_.load(), padKindName(kind_.load()), mode, s.bufferMs, commandPeriodUs(), clean ? "да" : "НЕТ");
        std::lock_guard<std::mutex> lock(physMutex_);
        info_ = b;
    }

    // Открыть порт, подобрать скорость, прочитать прошивку, тип геймпада и настройки, снять перехват.
    bool connect(const std::string& name, std::string& error) {
        if (!openHandle(name, error)) return false;
        ioError_ = false;
        bool found = false;
        for (uint32_t candidate : {4000000u, 1000000u, 115200u}) {
            if (!configure(candidate)) continue;
            Sleep(20);
            if (auto r = ask(kOpDevice, nullptr, 0, 300000.0, 1)) {
                kind_ = static_cast<uint8_t>((*r)[0] & 0xFC);
                baud_ = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            error = "MAKCU на " + name + " не ответил на DEVICE (0x02) ни на одной скорости";
            return false;
        }
        if (auto r = ask(kOpFirmware, nullptr, 0, 100000.0, 4)) firmware_ = le32(r->data());
        readSettings();
        enforceSmoothing();  // если вызывающий просил setSmoothing(): нет геймпада - применится, когда он появится
        streamOn_ = false;   // после переподключения поток выключен (документация); включить заново, если просили
        enforceStream();
        const bool clean = clearBoardState();
        sent_ = pack(false, 0, 0);
        injecting_ = false;
        // Ревизия 4: геймпада нет при подключении - команды ждут его появления (это не отказ, не считается).
        padAbsent_ = kind_.load() == 0;
        padStalled_ = false;
        ffRun_ = 0;
        lastNewT_ = 0;
        resumeT_ = 0;
        absentT_ = 0;
        updateInfo(clean);
        return true;
    }

    // ---- рабочий поток ------------------------------------------------------------------------
    // Ждать до момента due: таймер ядра высокого разрешения до due - 120 мкс, остаток - короткое активное ожидание.
    // (Это таймер Windows, а не драйвера порта: к порту во время ожидания никто не обращается.)
    void sleepUntil(int64_t due, HANDLE timer) {
        for (;;) {
            const double left = detail::Qpc::toUs(due - detail::Qpc::now());
            if (left <= 0) return;
            if (left > 200.0 && timer) {
                LARGE_INTEGER rel;
                rel.QuadPart = -static_cast<LONGLONG>((left - 120.0) * 10.0);  // единицы 100 нс, относительное время
                if (SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, 5);
                    continue;
                }
            }
            if (left > 1500.0) Sleep(1);
            else YieldProcessor();
        }
    }

    // Сколько снимок считается свежим: 50 мс, но не меньше 2.5 периодов опроса (при потоке опрос раз в 250 мс, а стик,
    // который стоит, событий не шлёт - значение верно, пока геймпад на месте).
    double liveWindowUs() const { return std::max(kLiveUs, 2.5 * physicalPollUs()); }

    // ---- поток физики 0x73 / 0x74 -> 0x75 (только рабочий поток или open() / close()) ------------------------
    // Привести поток к желаемому: wantStream_ - включить оба в двоичном режиме с периодом streamPeriodMs_, иначе -
    // выключить, если транспорт его включал. Синхронный обмен: запрос (пустое тело) -> при расхождении SET [mode, period]
    // -> запрос и сверка. true - состояние платы совпало с желаемым.
    bool enforceStream() {
        const bool want = wantStream_.load(std::memory_order_acquire);
        if (!want) {
            if (streamTouched_) disableStream();
            return true;
        }
        if (streamUnsupported_.load()) return false;
        const uint16_t period = streamPeriodMs_.load(std::memory_order_acquire);
        for (const uint8_t op : {kOpAxesStream, kOpButtonsStream}) {
            auto q = ask(op, nullptr, 0, 100000.0, 3);
            if (q && q->size() == 1 && (*q)[0] == 0xFF) {  // команды нет в этой прошивке
                streamUnsupported_ = true;
                streamOn_ = false;
                return false;
            }
            if (q && q->size() == 3 && (*q)[0] == 2 && le16(q->data() + 1) == period) continue;
            const uint8_t pl[3] = {2, static_cast<uint8_t>(period), static_cast<uint8_t>(period >> 8)};
            streamTouched_ = true;
            if (!sendFrame(op, pl, sizeof(pl))) break;
            streamWrites_.fetch_add(1, std::memory_order_relaxed);
            auto v = ask(op, nullptr, 0, 100000.0, 3);
            if (!v || v->size() != 3 || (*v)[0] != 2) {
                streamFailures_.fetch_add(1, std::memory_order_relaxed);
                streamOn_ = false;
                return false;
            }
        }
        streamOn_ = !ioError_.load();
        return streamOn_.load();
    }
    void disableStream() {
        const uint16_t period = streamPeriodMs_.load(std::memory_order_acquire);
        for (const uint8_t op : {kOpAxesStream, kOpButtonsStream}) {
            const uint8_t pl[3] = {0, static_cast<uint8_t>(period), static_cast<uint8_t>(period >> 8)};
            if (sendFrame(op, pl, sizeof(pl))) streamWrites_.fetch_add(1, std::memory_order_relaxed);
            (void)ask(op, nullptr, 0, 50000.0, 3);  // дождаться подтверждения (или тайм-аута), заодно вычитать события
        }
        streamOn_ = false;
        streamTouched_ = false;
    }
    // Событие 0x75: kind (1 оси, 2 кнопки), id, value u16 LE. Обновляет снимок физики; фронты нажатий - как у 0x54.
    void onStreamEvent(uint8_t kind, uint8_t id, uint16_t value, int64_t now) {
        streamEvents_.fetch_add(1, std::memory_order_relaxed);
        if (id == 255) {  // снимок недоступен (геймпада нет): тип устройства спросит шаг 5 рабочего цикла
            streamUnavailable_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::lock_guard<std::mutex> lock(physMutex_);
        PadPhysical& p = phys_;
        if (kind == 1) {
            const auto s = static_cast<int16_t>(value);
            switch (id) {
            case 10:
                if (value >= kPadTriggerDown && p.lt < kPadTriggerDown) presses_[10].fetch_add(1, std::memory_order_relaxed);
                p.lt = value;
                break;
            case 11:
                if (value >= kPadTriggerDown && p.rt < kPadTriggerDown) presses_[11].fetch_add(1, std::memory_order_relaxed);
                p.rt = value;
                break;
            case 12: p.lx = s; break;
            case 13: p.ly = s; break;
            case 14: p.rx = s; break;
            case 15: p.ry = s; break;
            default: return;
            }
        } else if (kind == 2) {
            if (id >= kPadControlCount) return;
            uint32_t& word = id < 32 ? p.digitalLow : p.digitalHigh;
            const uint32_t bit = 1u << (id < 32 ? id : id - 32);
            if (value && !(word & bit)) presses_[id].fetch_add(1, std::memory_order_relaxed);
            word = value ? (word | bit) : (word & ~bit);
        } else {
            return;
        }
        snapT_ = now;  // геймпад на месте
        physT_ = now;
    }

    void onSnapshot(const uint8_t* p, int64_t now) {
        PadPhysical v;
        v.digitalLow = le32(p);
        v.digitalHigh = le32(p + 4);
        v.lt = le16(p + 8);
        v.rt = le16(p + 10);
        v.lx = static_cast<int16_t>(le16(p + 12));
        v.ly = static_cast<int16_t>(le16(p + 14));
        v.rx = static_cast<int16_t>(le16(p + 16));
        v.ry = static_cast<int16_t>(le16(p + 18));
        v.sequence = le32(p + 20);
        v.usbTimestamp = le32(p + 24);
        const uint16_t timing = le16(p + 28);
        v.dtUframes = timing & 0x3FFF;
        v.baselineRestarted = (timing & 0x4000) != 0;
        v.timingInvalid = (timing & 0x8000) != 0;
        v.cadenceUframes = le16(p + 30);
        // Интервал, объявленный геймпадом: 1..512 микрокадров (0.125..64 мс); иначе - не трогать прежний.
        if (v.cadenceUframes >= 1 && v.cadenceUframes <= 512) padCadenceUs_.store(v.cadenceUframes * 125.0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(physMutex_);
        snapT_ = now;
        const bool first = phys_.samples == 0;
        if (!first && v.sequence == phys_.sequence) return;  // тот же отчёт: опрос чаще геймпада
        if (!first) {
            const uint32_t step = v.sequence - phys_.sequence;
            if (step > 1 && step < 100000) skipped_.fetch_add(step - 1, std::memory_order_relaxed);
            // фронты нажатий
            const uint32_t upLow = v.digitalLow & ~phys_.digitalLow, upHigh = v.digitalHigh & ~phys_.digitalHigh;
            for (uint8_t i = 0; i < 32; ++i) {
                if ((upLow >> i) & 1u) presses_[i].fetch_add(1, std::memory_order_relaxed);
            }
            for (uint8_t i = 0; i + 32 < kPadControlCount; ++i) {
                if ((upHigh >> i) & 1u) presses_[32 + i].fetch_add(1, std::memory_order_relaxed);
            }
            if (v.lt >= kPadTriggerDown && phys_.lt < kPadTriggerDown) presses_[10].fetch_add(1, std::memory_order_relaxed);
            if (v.rt >= kPadTriggerDown && phys_.rt < kPadTriggerDown) presses_[11].fetch_add(1, std::memory_order_relaxed);
        }
        v.samples = phys_.samples + 1;
        phys_ = v;
        physT_ = now;
        newSamples_.fetch_add(1, std::memory_order_relaxed);
        // Частота отчётов: прирост sequence за окно ~0.5 с (только рабочий поток, под physMutex_).
        if (rateT0_ == 0 || v.sequence < rateSeq0_) {
            rateT0_ = now;
            rateSeq0_ = v.sequence;
        } else if (const double w = detail::Qpc::toUs(now - rateT0_); w >= kRateWindowUs) {
            padRate_.store(static_cast<double>(v.sequence - rateSeq0_) * 1e6 / w, std::memory_order_release);
            rateT0_ = now;
            rateSeq0_ = v.sequence;
        }
    }

    // Состояние сверки GET 0x40 (только рабочий поток).
    struct Verify {
        bool pending = false;       // запрос ушёл, ответа ещё нет
        PadState expect{};          // что было задано плате на момент запроса (все 20 байт)
        bool stickActive = false;   //   и был ли в этом состоянии перехвачен стик
        int64_t sentT = 0;
        bool afterRelease = false;  // сверка после отпускания: при совпадении отметить releaseDone_
        uint64_t releaseSeen = 0;
        int64_t nextT = 0;
    };

    void onStateReply(const uint8_t* st, Verify& v, int64_t now) {
        if (!v.pending) return;
        v.pending = false;
        if (std::memcmp(st, v.expect.data(), v.expect.size()) != 0) {
            // Команда потерялась (или на плате чужое состояние): повторить то, что должно быть сейчас.
            repairs_.fetch_add(1, std::memory_order_relaxed);
            sendFrame(kOpCtlState, lastState_.data(), lastState_.size());
            v.nextT = now + detail::Qpc::fromUs(kReleaseVerifyUs);
        } else if (!v.stickActive) {
            releaseDone_.store(v.releaseSeen, std::memory_order_release);
        }
    }

    // ---- живучесть (ревизия 4; только рабочий поток) ----------------------------------------------------------
    // Плата сказала, что геймпада нет (kinds 0 или FF подряд). Повторные сигналы того же отказа не считаются.
    void onPadGone(int64_t now) {
        if (padAbsent_.load(std::memory_order_relaxed)) return;
        padAbsent_.store(true, std::memory_order_release);
        padStalled_.store(false, std::memory_order_release);
        absentT_ = now;
        padDrops_.fetch_add(1, std::memory_order_relaxed);
        // Буря: kStormDrops отказов за kStormWindowUs - опрос ограничен до kCalmUs после последнего отказа.
        dropTimes_[dropIdx_++ % dropTimes_.size()] = now;
        int recent = 0;
        for (const int64_t t : dropTimes_) {
            if (t && detail::Qpc::toUs(now - t) <= kStormWindowUs) ++recent;
        }
        if (recent >= kStormDrops) {
            if (!storm_.load(std::memory_order_relaxed)) storms_.fetch_add(1, std::memory_order_relaxed);
            storm_.store(true, std::memory_order_release);
            calmUntilT_ = now + detail::Qpc::fromUs(kCalmUs);
        }
    }
    // Пришёл новый sequence: геймпад жив. После отказа - ещё kResumeSettleUs тишины до команд.
    void onPadAlive(int64_t now) {
        lastNewT_ = now;
        padStalled_.store(false, std::memory_order_release);
        if (padAbsent_.load(std::memory_order_relaxed)) {
            padAbsent_.store(false, std::memory_order_release);
            if (absentT_) lastPadDownMs_.store(detail::Qpc::toUs(now - absentT_) / 1000.0, std::memory_order_relaxed);
            resumeT_ = now + detail::Qpc::fromUs(kResumeSettleUs);
        }
    }
    // Действующий период опроса 0x54 с учётом отказа и бури.
    double effectivePollUs(int64_t now) {
        double p = physicalPollUs();
        if (padAbsent_.load(std::memory_order_relaxed)) return std::max(p, kAbsentPollUs);
        if (storm_.load(std::memory_order_relaxed)) {
            if (now < calmUntilT_) p = std::max(p, kCalmPollUs);
            else storm_.store(false, std::memory_order_release);
        }
        return p;
    }

    // Разобрать всё пришедшее. Конвейер: запросы уходят в конце такта, ответы читаются в начале следующего,
    // поэтому поток никогда не ждёт порт.
    void processIncoming(Verify& verify, bool& kindAppeared) {
        drain();
        const int64_t now = detail::Qpc::now();
        Frame f;
        while (nextFrame(f)) {
            if (f.op == kOpCtlPhysical) {
                answered_.fetch_add(1, std::memory_order_relaxed);
                if (f.pl.size() == 32) {
                    snapshots_.fetch_add(1, std::memory_order_relaxed);
                    const uint64_t before = newSamples_.load(std::memory_order_relaxed);
                    onSnapshot(f.pl.data(), now);
                    ffRun_ = 0;
                    if (newSamples_.load(std::memory_order_relaxed) != before) onPadAlive(now);
                } else {
                    // Отказ FF: геймпада нет, маршрут меняется или снимок занят. Прежний снимок НЕ затирается.
                    rejects_.fetch_add(1, std::memory_order_relaxed);
                    if (++ffRun_ >= kAbsentFfRun) onPadGone(now);
                }
            } else if (f.op == kOpCtlState) {
                if (f.pl.size() == 20) onStateReply(f.pl.data(), verify, now);
                else if (f.pl.size() == 1 && f.pl[0] == 0xFF) {
                    stickRejects_.fetch_add(1, std::memory_order_relaxed);
                    verify.pending = false;
                }
            } else if (f.op == kOpDevice && f.pl.size() == 1) {
                const uint8_t k = static_cast<uint8_t>(f.pl[0] & 0xFC);
                if (k != 0 && kind_.load() == 0) kindAppeared = true;
                if (k == 0) {  // геймпада нет: частота не измерена, окно - заново
                    {
                        std::lock_guard<std::mutex> lock(physMutex_);
                        padRate_.store(0.0, std::memory_order_release);
                        rateT0_ = 0;
                    }
                    onPadGone(now);
                }
                kind_ = k;
            } else if (f.op == kOpStreamEvent && f.pl.size() == 4) {
                onStreamEvent(f.pl[0], f.pl[1], le16(f.pl.data() + 2), now);
            } else if ((f.op == kOpAxesStream || f.op == kOpButtonsStream) && f.pl.size() == 3) {
                // ответ на проверку раз в 1 с: поток выключился (перезапуск USB3, чужая программа) - включить заново
                if (wantStream_.load(std::memory_order_acquire) && (f.pl[0] != 2 || le16(f.pl.data() + 1) != streamPeriodMs_.load())) streamRecheck_ = true;
            }
            // 0x53 (события), 0x3E (уведомления о топологии) и прочее - не нужны
        }
    }

    void workerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2): Windows 10 1803+. Нет - остаётся Sleep(1) и активное ожидание.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        int64_t due = detail::Qpc::now();
        int64_t lastCmdT = 0, nextKindsT = 0, lostT = 0, nextRetryT = 0;
        uint64_t tickNo = 0, lastCmdTick = 0;
        uint64_t releaseSeen = releaseSeq_.load();
        uint64_t interpSeen = interpSeq_.load();
        uint64_t streamSeen = streamSeq_.load();  // open() уже применил поток в connect()
        int64_t nextStreamCheckT = 0;
        double blendW = 0.0;        // вес цели в плавном входе и выходе, 0..1
        int goalX = 0, goalY = -1;  // последняя заданная цель
        uint32_t sentBl = 0, sentBh = 0;  // кнопки и триггеры, которые сейчас заданы на плате
        uint16_t sentLt = 0, sentRt = 0;
        uint64_t lastSendTick = 0;        // такт последней отправки SET 0x40 (любой)
        int64_t lastSendT = 0;            // время последней отправки SET 0x40 (для повтора неизменной команды)
        Verify verify;
        const int64_t tickQ = detail::Qpc::fromUs(kTickUs);
        while (!stop_.load(std::memory_order_acquire)) {
            sleepUntil(due, timer);
            int64_t now = detail::Qpc::now();
            const double late = detail::Qpc::toUs(now - due);
            if (late > tickLateMaxUs_.load(std::memory_order_relaxed)) tickLateMaxUs_.store(late, std::memory_order_relaxed);
            due = std::max(due + tickQ, now - tickQ);

            if (!connected_.load(std::memory_order_acquire)) {
                if (now >= nextRetryT) {
                    nextRetryT = now + detail::Qpc::fromUs(kReconnectRetryUs);
                    std::string name = port_, err;
                    if (autoPort_) {
                        const auto found = findMakcuComPorts();
                        name = found.empty() ? std::string() : found.front();
                    }
                    closeHandle();
                    if (!name.empty() && connect(name, err)) {
                        // Цель устарела: после обрыва перехват снят, вызывающий задаст новую цель сам. Кнопки и триггеры
                        // тоже отпущены: зажатый после обрыва триггер хуже пропущенного нажатия.
                        slot_.store(pack(false, 0, 0), std::memory_order_release);
                        releaseButtons();
                        sentBl = sentBh = 0;
                        sentLt = sentRt = 0;
                        lastState_ = PadState{};
                        lastRxT_ = detail::Qpc::now();
                        lastOutageMs_ = detail::Qpc::toUs(lastRxT_ - lostT) / 1000.0;
                        reconnects_.fetch_add(1, std::memory_order_relaxed);
                        answered_.store(polls_.load());
                        verify = Verify{};
                        lastCmdT = 0;
                        blendW = 0.0;
                        connected_ = true;
                        due = detail::Qpc::now();
                    } else {
                        closeHandle();
                    }
                    // Попытка подключения длится до ~1 с (перебор скоростей): это не опоздание такта, сетку начать заново.
                    // (Прогон 2026-11-14: без этого tickLateMaxUs показал 981 мс после обрыва.)
                    due = detail::Qpc::now();
                }
                continue;
            }

            // 1. Ответы на запросы прошлого такта.
            bool kindAppeared = false;
            processIncoming(verify, kindAppeared);
            if (kindAppeared) {
                readSettings();  // редкий синхронный обмен: геймпад только что появился
                enforceSmoothing();
                if (wantStream_.load(std::memory_order_acquire)) enforceStream();
                updateInfo(true);
                answered_.store(polls_.load());
                due = detail::Qpc::now();
            }
            // Поток физики: просьба setPhysicalSource() или проверка увидела его выключенным - синхронно привести к желаемому.
            if (const uint64_t seq = streamSeq_.load(std::memory_order_acquire); seq != streamSeen || streamRecheck_) {
                streamSeen = seq;
                streamRecheck_ = false;
                enforceStream();
                answered_.store(polls_.load());
                due = detail::Qpc::now();
            }
            // Запрос setSmoothing() из другого потока: тоже редкий синхронный обмен (INFO, READ, BEGIN, WRITE, APPLY).
            if (const uint64_t seq = interpSeq_.load(std::memory_order_acquire); seq != interpSeen) {
                interpSeen = seq;
                readSettings();
                interpOk_.store(enforceSmoothing(), std::memory_order_release);
                interpDone_.store(seq, std::memory_order_release);
                updateInfo(true);
                answered_.store(polls_.load());
                due = detail::Qpc::now();
            }
            now = detail::Qpc::now();
            ++tickNo;

            // 1а. Живучесть (ревизия 4). Зависание: снимки свежие, а sequence стоит дольше kStallUs.
            if (!padAbsent_.load(std::memory_order_relaxed) && lastNewT_ && !padStalled_.load(std::memory_order_relaxed)) {
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> lock(physMutex_);
                    fresh = snapT_ != 0 && detail::Qpc::toUs(now - snapT_) < liveWindowUs();
                }
                if (fresh && detail::Qpc::toUs(now - lastNewT_) > std::max(kStallUs, 3.0 * effectivePollUs(now))) {
                    padStalled_.store(true, std::memory_order_release);
                    padStalls_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Геймпада нет или пауза после возврата: SET 0x40 и сверка не идут. При возврате плавный вход начнётся заново
            // (при Off - с физики; при Auto / Fixed blendW сразу 1). Если цель отпустили, пока геймпада не было, а перехват
            // на плате ещё числится (sent_), отпускание уйдёт после паузы обычным путём.
            const bool suspended = padAbsent_.load(std::memory_order_relaxed) || now < resumeT_;
            suspended_.store(suspended, std::memory_order_release);
            if (suspended) {
                blendW = 0.0;
                lastCmdT = 0;
                verify.pending = false;
                verify.nextT = now + detail::Qpc::fromUs(kReleaseVerifyUs);
                injecting_ = false;
            }

            // 2. Цель правого стика: самое свежее значение, каждый N-й такт (сетка тактов, а не время прошлой
            // отправки: иначе дрожание такта превращало интервал 2 мс в 3 мс).
            //
            // Плавный вход и выход (setBlendMs): на плату уходит физика + (цель - физика) * smoothstep(blendW).
            // blendW растёт до 1, пока вызывающий задаёт цель, и падает до 0 после releaseRightStick(); нули уходят,
            // только когда blendW дошёл до 0. Во время выхода целью остаётся последнее заданное значение. Новый
            // setRightStick() посреди выхода продолжает с текущего веса, без скачка.
            const uint64_t want = slot_.load(std::memory_order_acquire);
            const uint64_t sent = sent_.load(std::memory_order_relaxed);
            const uint64_t ticksPerCmd = static_cast<uint64_t>(std::max(1.0, std::round(commandPeriodUs() / kTickUs)));
            const bool wantActive = unpackActive(want);
            if (wantActive) {
                goalX = unpackX(want);
                goalY = unpackY(want);
            }
            // Кнопки и триггеры: вышедшие по времени tap() отпускаются, затем берётся текущее заданное состояние.
            if (anyTap_.load(std::memory_order_acquire)) expireTaps(now);
            const uint32_t bl = btnLow_.load(std::memory_order_acquire), bh = btnHigh_.load(std::memory_order_acquire);
            const uint16_t lt = lt_.load(std::memory_order_acquire), rt = rt_.load(std::memory_order_acquire);
            const bool padActive = bl || bh || lt || rt;
            const bool padChanged = bl != sentBl || bh != sentBh || lt != sentLt || rt != sentRt;
            bool releaseNow = false, stickCmd = false, sendNow = false;
            int16_t outX = 0, outY = 0;  // стик в команде этого такта; (0, 0) - стик не перехвачен
            if (wantActive || blendW > 0.0) {
                if (!lastCmdT || tickNo - lastCmdTick >= ticksPerCmd) {
                    const double ms = wantActive ? blendEnterMs() : blendExitMs();
                    int px = 0, py = 0;
                    bool havePhys = false;
                    {
                        std::lock_guard<std::mutex> lock(physMutex_);
                        havePhys = phys_.samples > 0 && snapT_ != 0 && detail::Qpc::toUs(now - snapT_) < liveWindowUs();
                        px = phys_.rx;
                        py = phys_.ry;
                    }
                    const double stepMs = static_cast<double>(ticksPerCmd) * kTickUs / 1000.0;
                    if (ms <= 0.0 || !havePhys) blendW = wantActive ? 1.0 : 0.0;
                    else blendW = std::clamp(blendW + (wantActive ? stepMs : -stepMs) / ms, 0.0, 1.0);
                    if (!wantActive && blendW <= 0.0) {
                        releaseNow = true;
                    } else {
                        int ox = goalX, oy = goalY;
                        if (blendW < 1.0) {
                            const double s = blendW * blendW * (3.0 - 2.0 * blendW);
                            ox = static_cast<int>(std::lround(px + (goalX - px) * s));
                            oy = static_cast<int>(std::lround(py + (goalY - py) * s));
                            if (ox == 0 && oy == 0) oy = -1;  // (0, 0) было бы отпусканием
                        }
                        outX = static_cast<int16_t>(std::clamp(ox, -32768, 32767));
                        outY = static_cast<int16_t>(std::clamp(oy, -32768, 32767));
                        stickCmd = sendNow = true;  // кнопки и триггеры едут в этой же команде
                    }
                }
            } else {
                releaseNow = unpackActive(sent) || releaseSeq_.load(std::memory_order_acquire) != releaseSeen;
                // Стик не перехвачен: изменение кнопок уходит сразу, удержание повторяется раз в kPadKeepaliveUs.
                if (!releaseNow) sendNow = padChanged || (padActive && tickNo - lastSendTick >= static_cast<uint64_t>(kPadKeepaliveUs / kTickUs));
            }
            // Отпускание стика: обе координаты 0, кнопки и триггеры - как заданы. При Auto плата сама доводит выход до
            // физического положения; при Off это скачок, поэтому к этому моменту плавный выход уже привёл команду к физике.
            if (releaseNow) sendNow = true;
            // Неизменная команда (MAKCU 2026-11-20): стик, кнопки и триггеры те же, что ушли последними, - повтор не чаще раза
            // в kHoldResendUs. Такт при этом считается отработанным (waitCommand() просыпается, сетка не сбивается).
            if (sendNow && stickCmd && !releaseNow && !padChanged && unpackActive(sent) && unpackX(sent) == outX && unpackY(sent) == outY && lastSendT &&
                detail::Qpc::toUs(now - lastSendT) < kHoldResendUs) {
                sendNow = false;
                lastCmdTick = tickNo;
                heldSkips_.fetch_add(1, std::memory_order_relaxed);
                cmdSlots_.fetch_add(1, std::memory_order_release);
                SetEvent(cmdEvent_);
            }
            if (sendNow && !suspended) {
                const PadState st = encodeState(bl, bh, lt, rt, outX, outY);
                if (releaseNow) releaseSeen = releaseSeq_.load(std::memory_order_acquire);
                if (sendFrame(kOpCtlState, st.data(), st.size())) {
                    lastSendTick = tickNo;
                    lastSendT = now;
                    lastState_ = st;
                    if (padChanged || (!stickCmd && !releaseNow)) padCommands_.fetch_add(1, std::memory_order_relaxed);
                    sentBl = bl;
                    sentBh = bh;
                    sentLt = lt;
                    sentRt = rt;
                    if (stickCmd) {
                        lastCmdTick = tickNo;
                        const int64_t t = detail::Qpc::now();
                        if (lastCmdT && unpackActive(sent)) {
                            const double gap = detail::Qpc::toUs(t - lastCmdT);
                            cmdIntCount_.fetch_add(1, std::memory_order_relaxed);
                            cmdIntSumUs_.store(cmdIntSumUs_.load(std::memory_order_relaxed) + gap, std::memory_order_relaxed);
                            if (gap > cmdIntMaxUs_.load(std::memory_order_relaxed)) cmdIntMaxUs_.store(gap, std::memory_order_relaxed);
                        }
                        lastCmdT = t;
                        sent_.store(pack(true, outX, outY), std::memory_order_release);
                        injecting_ = true;
                        stickCommands_.fetch_add(1, std::memory_order_release);
                        cmdSlots_.fetch_add(1, std::memory_order_release);
                        SetEvent(cmdEvent_);
                    } else if (releaseNow) {
                        releases_.fetch_add(1, std::memory_order_relaxed);
                        sent_.store(pack(false, 0, 0), std::memory_order_release);
                        injecting_ = false;
                        blendW = 0.0;
                        lastCmdT = 0;
                        verify.pending = false;
                        verify.nextT = now + detail::Qpc::fromUs(kReleaseVerifyUs);
                    } else if (padChanged) {
                        verify.pending = false;  // кнопки изменились: сверить вскоре, а не через 250 мс
                        verify.nextT = now + detail::Qpc::fromUs(kReleaseVerifyUs);
                    }
                }
            }

            // 3. Запрос физического геймпада: ответ будет прочитан на следующем такте.
            {
                const uint64_t asked = polls_.load(std::memory_order_relaxed), got = answered_.load(std::memory_order_relaxed);
                if (got > asked) answered_.store(asked);  // resetStats() из другого потока обнулил счётчики
                else if (asked - got >= 40) {             // 40 запросов (20 мс) без ответа: считаем потерянными
                    lost_.fetch_add(asked - got, std::memory_order_relaxed);
                    answered_.store(asked);
                }
            }
            if (tickNo % static_cast<uint64_t>(std::max(1.0, std::round(effectivePollUs(now) / kTickUs))) == 0) {
                if (sendFrame(kOpCtlPhysical)) polls_.fetch_add(1, std::memory_order_relaxed);
            }

            // 4. Сверка заданного состояния: GET 0x40 идёт следом за командой этого же такта.
            if (verify.pending && detail::Qpc::toUs(now - verify.sentT) > kReplyTimeoutUs) verify.pending = false;
            if (!verify.pending && now >= verify.nextT && !suspended) {
                if (sendFrame(kOpCtlState)) {
                    verify.pending = true;
                    verify.expect = lastState_;
                    verify.stickActive = unpackActive(sent_.load(std::memory_order_relaxed));
                    verify.sentT = now;
                    verify.releaseSeen = releaseSeen;
                    verify.nextT = now + detail::Qpc::fromUs(kVerifyUs);
                    verifies_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // 5. Геймпада нет: изредка спрашивать тип устройства (ответ разберёт processIncoming).
            if (now >= nextKindsT) {
                nextKindsT = now + detail::Qpc::fromUs(kKindsPollUs);
                bool have = false;
                {
                    std::lock_guard<std::mutex> lock(physMutex_);
                    have = snapT_ != 0 && detail::Qpc::toUs(now - snapT_) < liveWindowUs();
                }
                if (!have || kind_.load() == 0) sendFrame(kOpDevice);
            }

            // 5а. Поток физики: раз в 1 с спросить состояние 0x73 / 0x74 (ответ разберёт processIncoming); если включить
            // не удалось - попробовать снова. Если плата сказала FF - больше не трогать.
            if (wantStream_.load(std::memory_order_acquire) && !streamUnsupported_.load() && now >= nextStreamCheckT) {
                nextStreamCheckT = now + detail::Qpc::fromUs(kStreamCheckUs);
                if (!streamOn_.load()) streamRecheck_ = true;
                else {
                    sendFrame(kOpAxesStream);
                    sendFrame(kOpButtonsStream);
                }
            }

            // 6. Связь.
            if (ioError_.load() || detail::Qpc::toUs(detail::Qpc::now() - lastRxT_.load()) > kLinkLostUs) {
                connected_ = false;
                injecting_ = false;
                linkLost_.fetch_add(1, std::memory_order_relaxed);
                lostT = detail::Qpc::now();
                nextRetryT = lostT + detail::Qpc::fromUs(kReconnectRetryUs);
                closeHandle();
                ioError_ = false;
                lastCmdT = 0;
                blendW = 0.0;
            }
        }
        // Выход: последний раз снять перехват (close() уже просил, это страховка).
        if (connected_.load() && handle_ != INVALID_HANDLE_VALUE) clearBoardState();
        if (timer) CloseHandle(timer);
    }

    // Живучесть (ревизия 4). Атомики читают другие потоки; остальное - только рабочий поток и connect().
    std::atomic<bool> padAbsent_{false}, padStalled_{false}, suspended_{false}, storm_{false};
    std::atomic<uint64_t> padDrops_{0}, padStalls_{0}, storms_{0}, heldSkips_{0};
    std::atomic<uint64_t> cmdSlots_{0};  // такты команд (отправки + пропущенные повторы) - commandCount(); resetStats() не трогает
    std::atomic<double> lastPadDownMs_{0.0};
    int ffRun_ = 0;
    int64_t lastNewT_ = 0, resumeT_ = 0, absentT_ = 0, calmUntilT_ = 0;
    std::array<int64_t, 8> dropTimes_{};
    size_t dropIdx_ = 0;

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE cmdEvent_ = nullptr;  // взводится после каждой команды цели (waitCommand)
    std::string port_, info_;
    bool autoPort_ = true;
    bool timerPeriod_ = false;
    uint32_t baud_ = 0;
    std::atomic<uint32_t> firmware_{0};
    std::atomic<uint8_t> kind_{0};
    std::vector<uint8_t> rx_;
    std::atomic<bool> ioError_{false};
    std::atomic<int64_t> lastRxT_{0};

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};

    std::atomic<uint64_t> slot_{0};  // запрошенная цель (pack)
    std::atomic<uint64_t> sent_{0};  // что сейчас задано на плате (pack)
    std::atomic<bool> injecting_{false};
    std::atomic<double> periodUs_{0.0};  // 0 - по настройкам платы (commandPeriodUs())
    std::atomic<double> pollUs_{0.0};  // период GET 0x54 (setPhysicalPollUs); 0 - по интервалу геймпада (physicalPollUs())
    std::atomic<double> padCadenceUs_{0.0};  // cadence из 0x54, мкс
    std::atomic<double> padRate_{0.0};       // измеренная частота отчётов геймпада, в с
    int64_t rateT0_ = 0;                     // начало окна замера частоты (под physMutex_)
    uint32_t rateSeq0_ = 0;
    std::atomic<uint64_t> releaseSeq_{0}, releaseDone_{0};
    std::atomic<double> blendEnterMs_{-1.0}, blendExitMs_{-1.0};  // < 0 - по настройкам платы
    std::atomic<bool> boardOff_{false};       // настройки прочитаны и интерполяция платы Off
    std::atomic<int> wantInterp_{-1};         // setSmoothing(): какую интерполяцию держать на плате; -1 - не трогать
    std::atomic<int> wantBuffer_{-1};         // setSmoothingBuffer(): буфер, мс; -1 - не трогать
    std::atomic<uint64_t> interpSeq_{0}, interpDone_{0};
    std::atomic<bool> interpOk_{false}, keepSmoothing_{false};
    std::atomic<uint32_t> btnLow_{0}, btnHigh_{0};  // кнопки, нажатые программой (биты = номера контролов)
    std::atomic<uint16_t> lt_{0}, rt_{0};           // триггеры, заданные программой (0 - не перехвачен)
    std::array<std::atomic<int64_t>, kPadControlCount> tapUntil_{};  // tap(): когда отпустить (QPC), 0 - нет
    std::atomic<bool> anyTap_{false};
    std::atomic<uint64_t> padCommands_{0};
    PadState lastState_{};                    // последнее отправленное состояние (только поток, владеющий портом)
    bool haveRestore_ = false;                // интерполяцию меняли: в close() вернуть restoreInterp_
    uint8_t restoreInterp_ = 0;               // (эти поля - только поток, владеющий портом)
    int restoreBuffer_ = -1;                  // буфер до первой записи; -1 - не трогать
    bool restoreFailed_ = false;              // close() не смог вернуть сглаживание
    std::atomic<uint64_t> settingsWrites_{0};

    mutable std::mutex physMutex_;  // phys_, physT_, snapT_, settings_, info_
    PadPhysical phys_;
    int64_t physT_ = 0;  // когда пришёл текущий sequence
    int64_t snapT_ = 0;  // когда пришёл последний ответ по 32 байта
    PadSettings settings_;
    std::array<std::atomic<uint32_t>, kPadControlCount> presses_{};

    std::atomic<uint64_t> polls_{0}, answered_{0}, snapshots_{0}, newSamples_{0}, rejects_{0}, lost_{0}, skipped_{0};
    std::atomic<uint64_t> stickCommands_{0}, releases_{0}, stickRejects_{0}, verifies_{0}, repairs_{0};
    std::atomic<uint64_t> linkLost_{0}, reconnects_{0};
    // Поток физики (setPhysicalSource)
    std::atomic<bool> wantStream_{false};
    std::atomic<uint16_t> streamPeriodMs_{1};
    std::atomic<uint64_t> streamSeq_{0};          // растёт при каждом setPhysicalSource(): рабочий поток применяет
    std::atomic<bool> streamOn_{false};           // плата подтвердила оба потока
    std::atomic<bool> streamUnsupported_{false};  // FF на запрос 0x73 / 0x74
    bool streamTouched_ = false;                  // транспорт слал SET 0x73 / 0x74 (выключить в close())
    bool streamRecheck_ = false;                  // проверка раз в 1 с увидела поток выключенным
    std::atomic<uint64_t> streamEvents_{0}, streamUnavailable_{0}, streamWrites_{0}, streamFailures_{0};
    std::atomic<double> lastOutageMs_{0.0};
    std::atomic<uint64_t> cmdIntCount_{0};
    std::atomic<double> cmdIntSumUs_{0.0}, cmdIntMaxUs_{0.0}, tickLateMaxUs_{0.0};
};

}  // namespace makcu
