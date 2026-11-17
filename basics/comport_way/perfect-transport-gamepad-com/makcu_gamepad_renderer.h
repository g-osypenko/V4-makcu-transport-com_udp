// MAKCU: PadTextureRenderer - рендерер аппаратной текстуры стика геймпада. Ревизия 4 (2026-11-17).
//
// РЕВИЗИЯ 4: presetForTransport(kind, boardOff, tx.commandPeriodUs(), tx.padCadenceUs()) - пресет под режим платы и
// под интервал геймпада (125 / 250 / 500 / 1000 Гц): такт = период команд, модель АЦП подгоняется под такт.
// Пользоваться им вместо presetOff() / presetFor() + ручного tick_hz. HANDOFF_GAMEPAD_COM.md, 9к.
//
// ДВА РЕЖИМА. Описание ниже (такт 2 мс, опережение, шесть каскадов) - для сглаживания платы Auto: presetFor().
// При сглаживании платы Off плата - чистая задержка 4-5 мс и каждая команда раз в 1 мс доходит как есть; для него
// presetOff(): такт 1 мс, без опережения, а каскады 5-6 заменены моделью АЦП геймпада (выборка раз в 2 мс, середина,
// пачки "без середины", разряд ~101 единица, радиальная кривая отклика, обрезка по кругу) - см. PadRegimeProfile и
// step(). Замер 2026-11-15 на плате V4.122 (pad_bench_com --off): сумма W1 до живой руки 3.49 против 15.70 у гладкой
// кривой; рендерер добавляет 2 мс отставания и ~80 единиц средней ошибки слежения. HANDOFF_GAMEPAD_COM.md, 9д.
//
// Один заголовок, C++20, без зависимостей и без обращений к куче в горячем пути. Аналог ABCurves Renderer
// (abc_renderer.h / abc_online.h) для абсолютной координаты стика: гладкая цель (rx, ry) на входе, отсчёт для
// SET 0x40 на выходе. Ось Y как на плате: ВНИЗ.
//
// Жизненный цикл (как у AbcRenderer):
//   профиль: 256 живых снимков 0x54 через observePhysical(rx, ry, dt_uframes) -> prepareProfile() считает режим
//   геймпада (PadRegimeProfile), либо готовый пресет presetFor(controller_kind), либо loadProfile(файл);
//   профиль (PadRendererProfile) - простая структура, копируется по значению за O(1);
//   на каждое ведение: beginEvent(seed, init_rx, init_ry) -> step(target_rx, target_ry, out_rx, out_ry).
//
// ТАКТ step() - ОДИН РАЗ НА КОМАНДУ ПЛАТЕ, то есть раз в 2 мс (transport.commandPeriodUs()), а не раз в 1 мс.
// Замер 2026-11-14 (плата V4.122, interpolation = auto, буфер 8 мс, README): интерполятор платы берёт цель раз в
// 2 мс и сглаживает её как y = 0.5*u + 0.5*y_пред с задержкой 3 команды; при командах раз в 1 мс две трети команд
// пропадают. Поэтому всё, что рендерер может донести до Windows, живёт на шаге 2 мс и крупнее; внутри каждого
// 2-мс цикла плата сама делит шаг примерно 1:10, и командами это не меняется.
// Профиль хранит частоту тактов (tick_hz, по умолчанию 500): для другого периода команд - setTickHz().
//
// Шесть каскадов (номера как в задании, порядок - по ходу сигнала):
//   2. биомеханика: два стохастических резонатора AR(2), ~10 Гц (тремор) и ~22 Гц (палец на грибке), в базисе
//      касательная/нормаль к траектории; амплитуда растёт со скоростью; поперечная часть ослаблена;
//   3. межосевая связь (дуга пальца) и мягкое ограничение по внешнему контуру шахты;
//   4. люфт кардана: оператор play, u = clamp(u_пред, p - h, p + h);
//   5. суб-LSB шум + квантователь на родную сетку АЦП с гистерезисом (триггер Шмитта);
//   6. выборка-хранение: фазовый аккумулятор решает, обновился ли отсчёт АЦП на этом такте;
//   1. опережение под сглаживание платы: out = d + kappa * (d - d_пред). В задании оно стоит первым и применяется
//      к цели; здесь оно последнее и применяется к готовому отсчёту d, потому что через сглаживание платы проходит
//      весь сигнал вместе с текстурой. При kappa = 1 и такте 2 мс оно обращает y = 0.5*u + 0.5*y_пред.
//
// Состояние - целые в формате Q16 (int64), коэффициенты Q14, шум - SplitMix64: результат одинаков на любой машине.
//
// PadTexture19 в конце файла - панель из 19 описателей потока стика (аналог Texture19) и расстояние W1.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace makcu {

// Режим геймпада: то, что измерено по его собственным отчётам.
struct PadRegimeProfile {
    uint8_t controller_kind = 0;        // kinds & 0xFC
    uint16_t q_step_x = 1, q_step_y = 1;      // шаг родной сетки АЦП в шкале -32768..32767 (1 - сетки не видно)
    int16_t q_offset_x = 0, q_offset_y = 0;   // смещение сетки: уровень = n * q_step + q_offset
    float update_rate_hz = 1000.0f;     // частота отчётов геймпада (по dt_uframes)
    float hold_rate = 0.0f;             // доля отчётов без изменения, пока стик движется
    float lsb_noise_sigma = 0.0f;       // шум младшего разряда, в шагах сетки
    float sign_flip_rate = 0.0f;        // доля смен знака среди ненулевых приращений
    float backlash_lsb = 0.0f;          // люфт при развороте, в шагах сетки
    float tremor_gain_8_12hz = 1.0f;    // множители к амплитудам пресета
    float resonance_gain_20_25hz = 1.0f;
    float pair_ratio = 1.0f;            // отношение меньшего к большему из двух соседних приращений (1 - ровно)
    uint16_t observed = 0;              // сколько снимков вошло в оценку (0 - пресет)
    // Модель АЦП геймпада (ревизия 2, для сглаживания платы Off, такт 1 мс). Замер GameSir 2026-11-14 (README,
    // "Текстура живого стика"): АЦП ~10 бит, опрос раз в 2 мс, отчёт между выборками - середина, калибровка нелинейная.
    float raw_lsb = 0.0f;               // единиц на один разряд АЦП в "сырых" координатах (0 - модель АЦП выключена)
    // Калибровка геймпада - радиальная кривая отклика: радиус выхода s = r - dip * rho * (1 - exp(-r / rho)), то есть
    // наклон 1 - dip у центра и 1 вдали; затем обрезка по кругу outer_radius и по осям. Замер GameSir
    // (pad_probes/data/rec2_cal_model.txt и сравнение приращений): шаг ~50 у центра, 86 на радиусе ~10600, 101 у края
    // вдоль радиуса и 87 у края поперёк радиуса -> raw_lsb 101, dip 0.5, rho 8600.
    float raw_cal_dip = 0.0f;           // на сколько ниже чувствительность у центра (0 - калибровки нет)
    float raw_cal_rho = 8000.0f;        // радиус, на котором провал чувствительности спадает в e раз
    float raw_rim_push = 0.0f;          // у внешнего контура "сырой" радиус идёт дальше выхода: стик упёрт в шахту
    uint8_t adc_period_ticks = 1;     // через сколько тактов step() АЦП берёт новую выборку (GameSir: 2 при такте 1 мс)
    bool adc_midpoint = false;          // на такте выборки выдаётся середина между прошлой и новой, на следующем - новая
    // Пачки "без середины": геймпад временами отдаёт на такте выборки прошлый отсчёт, а весь шаг - на следующем
    // (у GameSir на быстром движении приращения "0 618 0 601 0 585" вместо "309 309 300 301"; rec2, круги).
    float adc_stale_start = 0.0f;       // вероятность начать такую пачку на выборке (0 - выключено)
    float adc_stale_len = 1.0f;         // средняя длина пачки, в выборках
};

struct PadRendererProfile {
    PadRegimeProfile regime;
    // Модель платы
    float tick_hz = 500.0f;             // тактов step() в секунду = команд плате в секунду
    float kappa_lead = 1.0f;            // опережение под сглаживание платы (1.0 обращает коэффициент 0.5)
    // Биомеханика (единицы стика; скорость - единиц за мс). Замер живой руки: README, "Тремор".
    float tremor_hz = 10.0f, tremor_bw_hz = 4.0f;
    float tremor_rms = 90.0f, tremor_rms_per_speed = 1.2f, tremor_rms_max = 350.0f;
    float resonance_hz = 22.0f, resonance_bw_hz = 6.0f;
    float resonance_rms = 30.0f, resonance_rms_per_speed = 0.5f, resonance_rms_max = 120.0f;
    float lateral_suppress = 0.25f;     // доля поперечной составляющей (замер 0.12-0.27; в задании 0.35)
    // Механика
    float cross_axis = 0.015f;          // межосевая связь, доля
    float outer_radius = 33750.0f;      // радиус внешнего контура (замер GameSir)
    float outer_knee = 0.97f;           // с какой доли радиуса начинается мягкое ограничение
    float adc_hysteresis_lsb = 0.1f;    // гистерезис компаратора, в шагах сетки
    float zoh_jitter = 0.05f;           // дрожание фазы выборки, доля такта

    // Производные коэффициенты (finalize()): Q14 и Q16.
    int32_t a1[2] = {0, 0}, a2[2] = {0, 0};        // AR(2): z = a1*z1 - a2*z2 + e
    int32_t norm_inv_q14[2] = {0, 0};              // СКО возбуждения на единицу СКО выхода
    int64_t rms_base_q16[2] = {0, 0}, rms_max_q16[2] = {0, 0};
    int32_t rms_per_speed_q8[2] = {0, 0};
    int32_t tick_rate_q8 = 128;                    // tick_hz / 1000 * 256
    int32_t lateral_q14 = 0, cross_q14 = 0, kappa_q14 = 0;
    int64_t radius_q8 = 0, knee_q8 = 0;
    int64_t play_q16[2] = {0, 0}, noise_q16[2] = {0, 0}, schmitt_q16[2] = {0, 0}, grid_q16[2] = {65536, 65536};
    int32_t zoh_step_q16 = 65536, zoh_jitter_q16 = 0;
    int32_t stale_start_q16 = 0, stale_stay_q16 = 0;  // вероятности начать и продолжить пачку "без середины"

    void finalize() {
        stale_start_q16 = static_cast<int32_t>(std::lround(std::clamp(regime.adc_stale_start, 0.0f, 1.0f) * 65536.0));
        stale_stay_q16 = static_cast<int32_t>(std::lround((1.0 - 1.0 / std::max(1.0f, regime.adc_stale_len)) * 65536.0));
        const double fs = std::max(1.0f, tick_hz);
        const double hz[2] = {tremor_hz, resonance_hz}, bw[2] = {tremor_bw_hz, resonance_bw_hz};
        const double base[2] = {tremor_rms * regime.tremor_gain_8_12hz, resonance_rms * regime.resonance_gain_20_25hz};
        const double per[2] = {tremor_rms_per_speed * regime.tremor_gain_8_12hz, resonance_rms_per_speed * regime.resonance_gain_20_25hz};
        const double mx[2] = {tremor_rms_max * regime.tremor_gain_8_12hz, resonance_rms_max * regime.resonance_gain_20_25hz};
        const double kPi = 3.14159265358979323846;
        for (int k = 0; k < 2; ++k) {
            const double f = std::min(hz[k], fs * 0.45);  // резонатор не может стоять выше половины частоты тактов
            const double r = std::exp(-kPi * bw[k] / fs);
            const double c1 = 2.0 * r * std::cos(2.0 * kPi * f / fs), c2 = r * r;
            a1[k] = static_cast<int32_t>(std::lround(c1 * 16384.0));
            a2[k] = static_cast<int32_t>(std::lround(c2 * 16384.0));
            // Дисперсия AR(2) при единичном возбуждении: (1 + c2) / ((1 - c2) * ((1 + c2)^2 - c1^2)).
            const double var = (1.0 + c2) / std::max(1e-9, (1.0 - c2) * ((1.0 + c2) * (1.0 + c2) - c1 * c1));
            norm_inv_q14[k] = static_cast<int32_t>(std::lround(16384.0 / std::sqrt(var)));
            rms_base_q16[k] = static_cast<int64_t>(std::llround(base[k] * 65536.0));
            rms_max_q16[k] = static_cast<int64_t>(std::llround(mx[k] * 65536.0));
            rms_per_speed_q8[k] = static_cast<int32_t>(std::lround(per[k] * 256.0));
        }
        tick_rate_q8 = static_cast<int32_t>(std::lround(fs / 1000.0 * 256.0));
        lateral_q14 = static_cast<int32_t>(std::lround(lateral_suppress * 16384.0));
        cross_q14 = static_cast<int32_t>(std::lround(cross_axis * 16384.0));
        kappa_q14 = static_cast<int32_t>(std::lround(kappa_lead * 16384.0));
        radius_q8 = static_cast<int64_t>(std::llround(outer_radius * 256.0));
        knee_q8 = static_cast<int64_t>(std::llround(outer_radius * outer_knee * 256.0));
        const double q[2] = {static_cast<double>(std::max<uint16_t>(1, regime.q_step_x)), static_cast<double>(std::max<uint16_t>(1, regime.q_step_y))};
        for (int k = 0; k < 2; ++k) {
            grid_q16[k] = static_cast<int64_t>(q[k]) << 16;
            play_q16[k] = static_cast<int64_t>(std::llround(regime.backlash_lsb * q[k] * 65536.0));
            noise_q16[k] = static_cast<int64_t>(std::llround(regime.lsb_noise_sigma * q[k] * 65536.0));
            schmitt_q16[k] = static_cast<int64_t>(std::llround((0.5 + adc_hysteresis_lsb) * q[k] * 65536.0));
        }
        // Доля тактов, на которых АЦП даёт новый отсчёт: не больше 1 (геймпад быстрее тактов - обновляется каждый такт).
        const double upd = std::min(1.0, static_cast<double>(regime.update_rate_hz) / fs);
        zoh_step_q16 = static_cast<int32_t>(std::lround(upd * 65536.0));
        zoh_jitter_q16 = upd < 1.0 ? static_cast<int32_t>(std::lround(zoh_jitter * 65536.0)) : 0;
    }
};

class PadTextureRenderer {
public:
    static constexpr int kProfileReports = 256;

    enum class Preset { XInput16bit, Sony8bit, Xbox10bit, Hall12bit };

    // Пресеты. Измерен на железе только XInput16bit (GameSir в режиме XInput, 2026-11-14): сетки нет, шума в покое
    // нет. Остальные - по документации протокола и типовым данным, на живом геймпаде НЕ проверены.
    static PadRendererProfile preset(Preset p) {
        PadRendererProfile pr;
        switch (p) {
        case Preset::XInput16bit:
            pr.regime.q_step_x = pr.regime.q_step_y = 1;
            pr.regime.lsb_noise_sigma = 0.0f;
            pr.regime.backlash_lsb = 0.0f;
            pr.regime.pair_ratio = 0.5f;
            pr.adc_hysteresis_lsb = 0.0f;
            break;
        case Preset::Sony8bit:
            // 8 бит 0..255: плата переводит как v * 257 - 32768 (128 -> +128, CONTROLLER_PHYSICAL.md).
            pr.regime.q_step_x = pr.regime.q_step_y = 257;
            pr.regime.q_offset_x = pr.regime.q_offset_y = 128;
            pr.regime.lsb_noise_sigma = 0.3f;
            pr.regime.backlash_lsb = 0.6f;
            pr.regime.update_rate_hz = 250.0f;
            pr.outer_radius = 32768.0f * 1.03f;
            break;
        case Preset::Xbox10bit:
            pr.regime.q_step_x = pr.regime.q_step_y = 64;
            pr.regime.lsb_noise_sigma = 0.3f;
            pr.regime.backlash_lsb = 0.6f;
            pr.regime.update_rate_hz = 250.0f;
            break;
        case Preset::Hall12bit:
            pr.regime.q_step_x = pr.regime.q_step_y = 16;
            pr.regime.lsb_noise_sigma = 0.25f;
            pr.regime.backlash_lsb = 0.3f;
            pr.regime.update_rate_hz = 1000.0f;
            break;
        }
        pr.finalize();
        return pr;
    }
    // Пресет под сглаживание платы Off (плата - чистая задержка, команда раз в 1 мс доходит как есть): такт 1 мс,
    // без опережения, модель АЦП геймпада включена. Числа АЦП и калибровки - замер GameSir XInput 2026-11-14 (кабель,
    // 1000 отчётов/с); пачки, гистерезис и амплитуды тремора подобраны по той же записи (pad_probes/pad_render_sim,
    // sweep_render.ps1). Другие геймпады и GameSir по радио не измерены.
    static PadRendererProfile presetOff(uint8_t controllerKind) {
        PadRendererProfile pr = presetFor(controllerKind);
        pr.tick_hz = 1000.0f;
        pr.kappa_lead = 0.0f;
        if (!(controllerKind & 0x78)) {  // XInput / HID 16 бит
            pr.regime.raw_lsb = 101.0f;
            pr.regime.raw_cal_dip = 0.5f;
            pr.regime.raw_cal_rho = 8600.0f;
            pr.regime.raw_rim_push = 2.0f;
            pr.regime.adc_period_ticks = 2;
            pr.regime.adc_midpoint = true;
            pr.regime.adc_stale_start = 0.005f;
            pr.regime.adc_stale_len = 6.0f;
            pr.adc_hysteresis_lsb = 0.75f;
            pr.cross_axis = 0.0f;  // при Off это чистая ошибка наведения (113 единиц в среднем), текстуре не помогает
            pr.tremor_rms = 6.0f;
            pr.tremor_rms_per_speed = 0.6f;
            pr.tremor_rms_max = 250.0f;
            pr.resonance_rms = 3.0f;
            pr.resonance_rms_per_speed = 0.25f;
            pr.resonance_rms_max = 90.0f;
        }
        pr.finalize();
        return pr;
    }
    // Период выборки АЦП в модели presetOff(): замер GameSir XInput (1000 отчётов/с, выборка раз в 2 мс, отчёт между
    // выборками - середина). Для других геймпадов не измерен.
    static constexpr double kAdcSampleUs = 2000.0;

    // Пресет под транспорт (ревизия 4, 2026-11-17): выбирает presetOff() / presetFor() по режиму платы и подгоняет такт
    // под период команд транспорта (tx.commandPeriodUs(), с ревизии 3 транспорта он не короче интервала геймпада):
    //   tick_hz = 1e6 / commandPeriodUs; update_rate_hz = 1e6 / padCadenceUs (если известен);
    //   модель АЦП (presetOff): выборка раз в round(2 мс / период) тактов; если такт уже 2 мс и длиннее (геймпад 125 /
    //   250 / 500 Гц), каждый такт - свежая выборка, без середины и без пачек "без середины" (они - свойство 1000-Гц
    //   GameSir между выборками).
    // Просто поставить prof.tick_hz нельзя: presetOff() держит adc_period_ticks = 2 под такт 1 мс, и при такте 8 мс
    // модель брала бы выборку раз в 16 мс с серединой. На геймпадах медленнее 1000 Гц НЕ ПРОВЕРЕНО; опережение
    // presetFor() (kappa_lead под цикл интерполятора 2 мс при Auto) при такте длиннее 2 мс - тоже.
    static PadRendererProfile presetForTransport(uint8_t controllerKind, bool boardOff, double commandPeriodUs, double padCadenceUs = 0.0) {
        PadRendererProfile pr = boardOff ? presetOff(controllerKind) : presetFor(controllerKind);
        const double period = std::max(250.0, commandPeriodUs);
        pr.tick_hz = static_cast<float>(1e6 / period);
        if (padCadenceUs > 0.0) pr.regime.update_rate_hz = static_cast<float>(1e6 / padCadenceUs);
        if (pr.regime.raw_lsb > 0.0f) {
            const long ticks = std::clamp(std::lround(kAdcSampleUs / period), 1L, 255L);
            pr.regime.adc_period_ticks = static_cast<uint8_t>(ticks);
            if (ticks < 2) {
                pr.regime.adc_midpoint = false;
                pr.regime.adc_stale_start = 0.0f;
            }
        }
        pr.finalize();
        return pr;
    }

    // Пресет по kinds & 0xFC из DEVICE (0x02).
    static PadRendererProfile presetFor(uint8_t controllerKind) {
        PadRendererProfile pr = preset((controllerKind & 0x38) ? Preset::Sony8bit : (controllerKind & 0x40) ? Preset::Xbox10bit : Preset::XInput16bit);
        pr.regime.controller_kind = controllerKind;
        return pr;
    }

    // ---- профиль -------------------------------------------------------------------------------
    // Начать наблюдение: очищает накопленные снимки. base - пресет, от которого берутся настройки, не измеряемые
    // по 256 отчётам (амплитуды тремора, модель платы).
    void reset(const PadRendererProfile& base) {
        base_ = base;
        observed_ = 0;
        ready_ = false;
    }
    // Один валидный снимок 0x54 (sequence изменился; baseline/invalid не подавать). true - набрано 256 и профиль готов.
    bool observePhysical(int16_t rx, int16_t ry, uint16_t dtUframes) {
        if (observed_ >= kProfileReports) return true;
        obsX_[static_cast<size_t>(observed_)] = rx;
        obsY_[static_cast<size_t>(observed_)] = ry;
        obsDt_[static_cast<size_t>(observed_)] = dtUframes;
        if (++observed_ == kProfileReports) {
            prepareProfile();
            return true;
        }
        return false;
    }
    int observed() const { return observed_; }
    bool ready() const { return ready_; }
    const PadRendererProfile& profile() const { return profile_; }
    void setProfile(const PadRendererProfile& p) {
        profile_ = p;
        profile_.finalize();
        ready_ = true;
    }
    void setTickHz(float hz) {
        profile_.tick_hz = hz;
        profile_.finalize();
    }

    // Оценка режима по накопленным снимкам (нужно ровно 256). Измеряется: сетка АЦП, частота отчётов, доля полок,
    // шум младшего разряда, смены знака, люфт, чередование соседних приращений, энергия полос 8-12 и 20-25 Гц
    // (как множитель к пресету, в пределах 0.5..2).
    bool prepareProfile() {
        if (observed_ != kProfileReports) return false;
        PadRendererProfile p = base_;
        PadRegimeProfile& g = p.regime;
        const int n = kProfileReports;
        g.observed = static_cast<uint16_t>(n);
        const int16_t* axis[2] = {obsX_.data(), obsY_.data()};
        uint16_t* step[2] = {&g.q_step_x, &g.q_step_y};
        int16_t* off[2] = {&g.q_offset_x, &g.q_offset_y};
        for (int a = 0; a < 2; ++a) {
            const int q = estimateStep(axis[a], n);
            *step[a] = static_cast<uint16_t>(q);
            *off[a] = static_cast<int16_t>(q > 1 ? ((axis[a][0] % q) + q) % q : 0);
        }
        // частота отчётов
        double dtSum = 0.0;
        int dtN = 0;
        for (int i = 0; i < n; ++i) {
            if (obsDt_[static_cast<size_t>(i)] > 0 && obsDt_[static_cast<size_t>(i)] < 800) {
                dtSum += obsDt_[static_cast<size_t>(i)] * 0.125;
                ++dtN;
            }
        }
        if (dtN) g.update_rate_hz = static_cast<float>(1000.0 / (dtSum / dtN));
        // приращения
        int moving = 0, holds = 0, flips = 0, flipBase = 0, pairs = 0, slow = 0;
        double pairSum = 0.0, slowVar = 0.0, backSum = 0.0;
        int backN = 0;
        for (int a = 0; a < 2; ++a) {
            const double q = *step[a];
            int lastSign = 0, zerosSince = 0;
            double lastMag = 0.0;
            for (int i = 1; i < n; ++i) {
                const int d = axis[a][i] - axis[a][i - 1];
                if (d == 0) {
                    ++zerosSince;
                    continue;
                }
                const int s = d > 0 ? 1 : -1;
                const double m = std::abs(d);
                if (lastSign) {
                    ++flipBase;
                    if (s != lastSign) {
                        ++flips;
                        // люфт: мёртвый ход на развороте ~ число пустых отчётов * средний шаг вокруг
                        backSum += std::min(4.0, zerosSince * 0.5 * (m + lastMag) / q);
                        ++backN;
                    } else if (zerosSince == 0) {
                        pairSum += std::min(m, lastMag) / std::max(m, lastMag);
                        ++pairs;
                    }
                    if (m <= 2.0 * q && lastMag <= 2.0 * q) {  // почти стоит: дребезг младшего разряда
                        slowVar += (s * m - lastSign * lastMag) * (s * m - lastSign * lastMag) / (q * q);
                        ++slow;
                    }
                }
                lastSign = s;
                lastMag = m;
                zerosSince = 0;
            }
        }
        for (int i = 1; i < n; ++i) {
            const bool z = obsX_[static_cast<size_t>(i)] == obsX_[static_cast<size_t>(i - 1)] && obsY_[static_cast<size_t>(i)] == obsY_[static_cast<size_t>(i - 1)];
            // "в движении": в окне +-8 отчётов есть изменение
            bool nearby = false;  // (near - макрос windows.h)
            for (int j = std::max(1, i - 8); j <= std::min(n - 1, i + 8) && !nearby; ++j) {
                nearby = obsX_[static_cast<size_t>(j)] != obsX_[static_cast<size_t>(j - 1)] || obsY_[static_cast<size_t>(j)] != obsY_[static_cast<size_t>(j - 1)];
            }
            if (nearby) {
                ++moving;
                if (z) ++holds;
            }
        }
        g.hold_rate = moving ? static_cast<float>(holds) / static_cast<float>(moving) : 0.0f;
        g.sign_flip_rate = flipBase ? static_cast<float>(flips) / static_cast<float>(flipBase) : 0.0f;
        g.pair_ratio = pairs >= 8 ? static_cast<float>(pairSum / pairs) : base_.regime.pair_ratio;
        // Шум: только если стик заметно дребезжит (смен знака больше 15 %), иначе это движение, а не шум.
        g.lsb_noise_sigma = (slow >= 8 && g.sign_flip_rate > 0.15f) ? static_cast<float>(std::min(1.0, std::sqrt(slowVar / slow) / 2.0)) : 0.0f;
        g.backlash_lsb = backN >= 3 ? static_cast<float>(std::min(2.0, 0.5 * backSum / backN)) : base_.regime.backlash_lsb;
        // Полосы 8-12 и 20-25 Гц в приращениях относительно всей мощности: множитель к пресету.
        const double fsObs = std::max(50.0f, g.update_rate_hz);
        const double e10 = bandShare(8.0, 12.0, fsObs), e22 = bandShare(20.0, 25.0, fsObs);
        g.tremor_gain_8_12hz = static_cast<float>(std::clamp(e10 / 0.10, 0.5, 2.0));     // у живой руки около 0.10
        g.resonance_gain_20_25hz = static_cast<float>(std::clamp(e22 / 0.03, 0.5, 2.0));  // и около 0.03
        p.finalize();
        profile_ = p;
        ready_ = true;
        return true;
    }

    // Профиль в текстовый файл "имя=значение" и обратно (вне горячего пути).
    bool saveProfile(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        const PadRendererProfile& p = profile_;
        const PadRegimeProfile& g = p.regime;
        std::fprintf(f, "# PadRendererProfile, makcu_gamepad_renderer.h ревизия 1\n");
        std::fprintf(f, "controller_kind=%u\nq_step_x=%u\nq_step_y=%u\nq_offset_x=%d\nq_offset_y=%d\nupdate_rate_hz=%g\nhold_rate=%g\n", g.controller_kind,
            g.q_step_x, g.q_step_y, g.q_offset_x, g.q_offset_y, g.update_rate_hz, g.hold_rate);
        std::fprintf(f, "lsb_noise_sigma=%g\nsign_flip_rate=%g\nbacklash_lsb=%g\ntremor_gain_8_12hz=%g\nresonance_gain_20_25hz=%g\npair_ratio=%g\nobserved=%u\n",
            g.lsb_noise_sigma, g.sign_flip_rate, g.backlash_lsb, g.tremor_gain_8_12hz, g.resonance_gain_20_25hz, g.pair_ratio, g.observed);
        std::fprintf(f, "tick_hz=%g\nkappa_lead=%g\ntremor_hz=%g\ntremor_bw_hz=%g\ntremor_rms=%g\ntremor_rms_per_speed=%g\ntremor_rms_max=%g\n", p.tick_hz, p.kappa_lead,
            p.tremor_hz, p.tremor_bw_hz, p.tremor_rms, p.tremor_rms_per_speed, p.tremor_rms_max);
        std::fprintf(f, "resonance_hz=%g\nresonance_bw_hz=%g\nresonance_rms=%g\nresonance_rms_per_speed=%g\nresonance_rms_max=%g\n", p.resonance_hz, p.resonance_bw_hz,
            p.resonance_rms, p.resonance_rms_per_speed, p.resonance_rms_max);
        std::fprintf(f, "lateral_suppress=%g\ncross_axis=%g\nouter_radius=%g\nouter_knee=%g\nadc_hysteresis_lsb=%g\nzoh_jitter=%g\n", p.lateral_suppress, p.cross_axis,
            p.outer_radius, p.outer_knee, p.adc_hysteresis_lsb, p.zoh_jitter);
        std::fprintf(f, "raw_lsb=%g\nraw_cal_dip=%g\nraw_cal_rho=%g\nraw_rim_push=%g\nadc_period_ticks=%u\nadc_midpoint=%d\nadc_stale_start=%g\nadc_stale_len=%g\n",
            g.raw_lsb, g.raw_cal_dip, g.raw_cal_rho, g.raw_rim_push, g.adc_period_ticks, g.adc_midpoint ? 1 : 0, g.adc_stale_start, g.adc_stale_len);
        std::fclose(f);
        return true;
    }
    bool loadProfile(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        PadRendererProfile p;
        char line[160];
        int found = 0;
        while (std::fgets(line, sizeof(line), f)) {
            char key[64];
            double v = 0.0;
            if (line[0] == '#' || std::sscanf(line, "%63[^=]=%lf", key, &v) != 2) continue;
            if (setField(p, key, v)) ++found;
        }
        std::fclose(f);
        if (found < 5) return false;
        setProfile(p);
        return true;
    }
    // Задать одно поле профиля по имени (те же имена, что в файле профиля). После правок нужен finalize()
    // (setProfile() делает его сам). false - имя неизвестно.
    static bool setField(PadRendererProfile& p, const std::string& k, double v) {
        PadRegimeProfile& g = p.regime;
        if (k == "controller_kind") g.controller_kind = static_cast<uint8_t>(v);
        else if (k == "q_step_x") g.q_step_x = static_cast<uint16_t>(v);
        else if (k == "q_step_y") g.q_step_y = static_cast<uint16_t>(v);
        else if (k == "q_offset_x") g.q_offset_x = static_cast<int16_t>(v);
        else if (k == "q_offset_y") g.q_offset_y = static_cast<int16_t>(v);
        else if (k == "update_rate_hz") g.update_rate_hz = static_cast<float>(v);
        else if (k == "hold_rate") g.hold_rate = static_cast<float>(v);
        else if (k == "lsb_noise_sigma") g.lsb_noise_sigma = static_cast<float>(v);
        else if (k == "sign_flip_rate") g.sign_flip_rate = static_cast<float>(v);
        else if (k == "backlash_lsb") g.backlash_lsb = static_cast<float>(v);
        else if (k == "tremor_gain_8_12hz") g.tremor_gain_8_12hz = static_cast<float>(v);
        else if (k == "resonance_gain_20_25hz") g.resonance_gain_20_25hz = static_cast<float>(v);
        else if (k == "pair_ratio") g.pair_ratio = static_cast<float>(v);
        else if (k == "observed") g.observed = static_cast<uint16_t>(v);
        else if (k == "raw_lsb") g.raw_lsb = static_cast<float>(v);
        else if (k == "raw_cal_dip") g.raw_cal_dip = static_cast<float>(v);
        else if (k == "raw_cal_rho") g.raw_cal_rho = static_cast<float>(v);
        else if (k == "raw_rim_push") g.raw_rim_push = static_cast<float>(v);
        else if (k == "adc_stale_start") g.adc_stale_start = static_cast<float>(v);
        else if (k == "adc_stale_len") g.adc_stale_len = static_cast<float>(v);
        else if (k == "adc_period_ticks") g.adc_period_ticks = static_cast<uint8_t>(v);
        else if (k == "adc_midpoint") g.adc_midpoint = v != 0.0;
        else if (k == "tick_hz") p.tick_hz = static_cast<float>(v);
        else if (k == "kappa_lead") p.kappa_lead = static_cast<float>(v);
        else if (k == "tremor_hz") p.tremor_hz = static_cast<float>(v);
        else if (k == "tremor_bw_hz") p.tremor_bw_hz = static_cast<float>(v);
        else if (k == "tremor_rms") p.tremor_rms = static_cast<float>(v);
        else if (k == "tremor_rms_per_speed") p.tremor_rms_per_speed = static_cast<float>(v);
        else if (k == "tremor_rms_max") p.tremor_rms_max = static_cast<float>(v);
        else if (k == "resonance_hz") p.resonance_hz = static_cast<float>(v);
        else if (k == "resonance_bw_hz") p.resonance_bw_hz = static_cast<float>(v);
        else if (k == "resonance_rms") p.resonance_rms = static_cast<float>(v);
        else if (k == "resonance_rms_per_speed") p.resonance_rms_per_speed = static_cast<float>(v);
        else if (k == "resonance_rms_max") p.resonance_rms_max = static_cast<float>(v);
        else if (k == "lateral_suppress") p.lateral_suppress = static_cast<float>(v);
        else if (k == "cross_axis") p.cross_axis = static_cast<float>(v);
        else if (k == "outer_radius") p.outer_radius = static_cast<float>(v);
        else if (k == "outer_knee") p.outer_knee = static_cast<float>(v);
        else if (k == "adc_hysteresis_lsb") p.adc_hysteresis_lsb = static_cast<float>(v);
        else if (k == "zoh_jitter") p.zoh_jitter = static_cast<float>(v);
        else return false;
        return true;
    }

    // ---- ведение -------------------------------------------------------------------------------
    // Начать ведение от положения (init_rx, init_ry) - обычно это текущий физический стик из physical():
    // первый отсчёт выйдет без ступеньки. Копирует профиль в рабочее состояние, сеет SplitMix64.
    void beginEvent(uint64_t seed, int16_t initRx, int16_t initRy) {
        ev_ = profile_;  // копия профиля: другой поток может менять шаблон, ведение его не увидит
        rng_ = seed ? seed : 0x9E3779B97F4A7C15ull;
        const int64_t ix = static_cast<int64_t>(initRx) << 16, iy = static_cast<int64_t>(initRy) << 16;
        prevT_[0] = initT_[0] = u_[0] = ix;
        prevT_[1] = initT_[1] = u_[1] = iy;
        for (auto& z : z_) z = 0;
        dir_[0] = 16384;
        dir_[1] = 0;
        const int q[2] = {std::max<int>(1, ev_.regime.q_step_x), std::max<int>(1, ev_.regime.q_step_y)};
        const int off[2] = {ev_.regime.q_offset_x, ev_.regime.q_offset_y};
        const int init[2] = {initRx, initRy};
        for (int a = 0; a < 2; ++a) {
            level_[a] = floorDiv(static_cast<int64_t>(init[a]) - off[a] + q[a] / 2, q[a]);
            held_[a] = dPrev_[a] = init[a];
        }
        phase_ = static_cast<int32_t>(next() & 0xFFFF);
        if (ev_.regime.raw_lsb > 0.0f) {
            // модель АЦП: начальные выборки = текущее положение в "сырых" координатах, фаза выборки случайная
            const double lsb = ev_.regime.raw_lsb;
            double ux = initRx, uy = initRy;
            calInverse(ev_, initRx, initRy, ux, uy);
            rawPrev_[0] = rawCur_[0] = std::llround(ux / lsb);
            rawPrev_[1] = rawCur_[1] = std::llround(uy / lsb);
            adcCount_ = static_cast<int>(next() % static_cast<uint64_t>(std::max<int>(1, ev_.regime.adc_period_ticks)));
            stale_ = false;
        }
        active_ = true;
    }
    bool active() const { return active_; }

    // Один такт (раз на команду плате). Вход - гладкая цель в координатах платы, выход - отсчёт для setRightStick().
    void step(double targetRx, double targetRy, int16_t& outRx, int16_t& outRy) {
        const PadRendererProfile& p = ev_;
        int64_t t[2] = {toQ16(targetRx), toQ16(targetRy)};
        const int64_t v[2] = {t[0] - prevT_[0], t[1] - prevT_[1]};
        prevT_[0] = t[0];
        prevT_[1] = t[1];
        // скорость и направление (касательная) в Q8 за такт
        const int64_t sx = v[0] >> 8, sy = v[1] >> 8;
        const int64_t sp = isqrt(static_cast<uint64_t>(sx * sx + sy * sy));
        if (sp >= 64) {  // от 0.25 единицы за такт: иначе направление не определено, держим прежнее
            dir_[0] = static_cast<int32_t>((sx << 14) / sp);
            dir_[1] = static_cast<int32_t>((sy << 14) / sp);
        }
        const int64_t speedMsQ8 = (sp * p.tick_rate_q8) >> 8;  // единиц за мс, Q8
        // каскад 2: резонаторы, по две составляющие (вдоль и поперёк) на каждый
        int64_t along = 0, across = 0;
        for (int k = 0; k < 2; ++k) {
            const int64_t rms = std::min(p.rms_max_q16[k], p.rms_base_q16[k] + static_cast<int64_t>(p.rms_per_speed_q8[k]) * speedMsQ8);
            const int64_t sig = (rms * p.norm_inv_q14[k]) >> 14;
            for (int c = 0; c < 2; ++c) {
                int64_t* z = &z_[static_cast<size_t>(k * 4 + c * 2)];
                const int64_t zn = ((static_cast<int64_t>(p.a1[k]) * z[0] - static_cast<int64_t>(p.a2[k]) * z[1]) >> 14) + ((sig * gauss()) >> 12);
                z[1] = z[0];
                z[0] = zn;
                (c == 0 ? along : across) += zn;
            }
        }
        across = (across * p.lateral_q14) >> 14;
        int64_t pos[2] = {t[0] + ((dir_[0] * along - dir_[1] * across) >> 14), t[1] + ((dir_[1] * along + dir_[0] * across) >> 14)};
        // каскад 3: межосевая связь (сдвиг относительно начала ведения) и внешний контур
        pos[0] += (static_cast<int64_t>(p.cross_q14) * (t[1] - initT_[1])) >> 14;
        pos[1] += (static_cast<int64_t>(p.cross_q14) * (t[0] - initT_[0])) >> 14;
        {
            const int64_t px = pos[0] >> 8, py = pos[1] >> 8;
            const int64_t r = isqrt(static_cast<uint64_t>(px * px + py * py));
            if (r > p.knee_q8 && p.radius_q8 > p.knee_q8) {
                const int64_t span = p.radius_q8 - p.knee_q8, over = r - p.knee_q8;
                const int64_t rr = p.knee_q8 + span * over / (span + over);  // плавно подходит к радиусу, не пересекая его
                pos[0] = pos[0] * rr / r;  // Q16 * Q8 / Q8: до 2^54, в int64 помещается
                pos[1] = pos[1] * rr / r;
            }
        }
        // каскад 4: люфт
        for (int a = 0; a < 2; ++a) u_[a] = std::clamp(u_[a], pos[a] - p.play_q16[a], pos[a] + p.play_q16[a]);
        if (p.regime.raw_lsb > 0.0f) {
            // Каскады 5-6 для сглаживания платы Off: модель АЦП геймпада (ревизия 2). Плата при Off передаёт отсчёт как
            // есть, поэтому всё, что отличает живой стик на шаге 1 мс, делается здесь:
            //   выборка раз в adc_period_ticks тактов; обратная калибровка (выход -> "сырые" координаты);
            //   квантование на разряд raw_lsb с гистерезисом (в покое отсчёт стоит точно);
            //   на такте выборки наружу идёт середина между прошлой и новой выборкой (floor(d / 2), как у GameSir:
            //   шаг в 5 разрядов виден как 2 + 3), на следующем такте - новая выборка; прямая калибровка.
            // Опережения и выборки-хранения платы здесь нет. Арифметика double: только + - * / и округление.
            const double lsb = p.regime.raw_lsb;
            const int period = std::max<int>(1, p.regime.adc_period_ticks);
            bool sampled = false;
            if (++adcCount_ >= period) {
                adcCount_ = 0;
                sampled = true;
                const double px = static_cast<double>(u_[0]) / 65536.0, py = static_cast<double>(u_[1]) / 65536.0;
                double ux = px, uy = py;
                calInverse(p, px, py, ux, uy);
                const double cand[2] = {ux / lsb, uy / lsb};
                for (int a = 0; a < 2; ++a) {
                    rawPrev_[a] = rawCur_[a];
                    if (std::fabs(cand[a] - static_cast<double>(rawCur_[a])) > 0.5 + p.adc_hysteresis_lsb) rawCur_[a] = std::llround(cand[a]);
                }
                // пачка "без середины": цепь из двух состояний, одно случайное число на выборку
                if (p.stale_start_q16) stale_ = static_cast<int32_t>(next() & 0xFFFF) < (stale_ ? p.stale_stay_q16 : p.stale_start_q16);
            }
            double uo[2];
            for (int a = 0; a < 2; ++a) {
                const int64_t r = (sampled && p.regime.adc_midpoint && period >= 2) ? rawPrev_[a] + (stale_ ? 0 : floorDiv(rawCur_[a] - rawPrev_[a], 2)) : rawCur_[a];
                uo[a] = static_cast<double>(r) * lsb;
            }
            double cx = 0.0, cy = 0.0;
            calForward(p, uo[0], uo[1], cx, cy);
            const int o[2] = {static_cast<int>(std::clamp<long long>(std::llround(cx), -32768, 32767)),
                static_cast<int>(std::clamp<long long>(std::llround(cy), -32768, 32767))};
            held_[0] = dPrev_[0] = o[0];
            held_[1] = dPrev_[1] = o[1];
            outRx = static_cast<int16_t>(o[0]);
            outRy = static_cast<int16_t>(o[1]);
            return;
        }
        int d[2];
        const int q[2] = {std::max<int>(1, p.regime.q_step_x), std::max<int>(1, p.regime.q_step_y)};
        const int off[2] = {p.regime.q_offset_x, p.regime.q_offset_y};
        for (int a = 0; a < 2; ++a) {
            // каскад 5: шум и квантователь с гистерезисом
            const int64_t c = u_[a] + (p.noise_q16[a] ? (p.noise_q16[a] * gauss()) >> 12 : 0);
            const int64_t centre = ((level_[a] * q[a] + off[a]) << 16);
            if (std::llabs(c - centre) > p.schmitt_q16[a]) {
                level_[a] = floorDiv(c - (static_cast<int64_t>(off[a]) << 16) + (p.grid_q16[a] >> 1), p.grid_q16[a]);
            }
            d[a] = static_cast<int>(std::clamp<int64_t>(level_[a] * q[a] + off[a], -32768, 32767));
        }
        // каскад 6: выборка-хранение
        phase_ += p.zoh_step_q16;
        if (p.zoh_jitter_q16) phase_ += static_cast<int32_t>((static_cast<int64_t>(p.zoh_jitter_q16) * gauss()) >> 12);
        if (phase_ >= 65536) {
            phase_ -= 65536;
            if (phase_ >= 65536) phase_ = 0;
            held_[0] = d[0];
            held_[1] = d[1];
        }
        // каскад 1: опережение под сглаживание платы
        int out[2];
        for (int a = 0; a < 2; ++a) {
            const int64_t lead = (static_cast<int64_t>(p.kappa_q14) * (held_[a] - dPrev_[a]) + 8192) >> 14;
            out[a] = static_cast<int>(std::clamp<int64_t>(held_[a] + lead, -32768, 32767));
            dPrev_[a] = held_[a];
        }
        outRx = static_cast<int16_t>(out[0]);
        outRy = static_cast<int16_t>(out[1]);
    }
    // Отсчёт до опережения: то, что рендерер хочет увидеть на выходе платы на этом такте.
    void desired(int16_t& rx, int16_t& ry) const {
        rx = static_cast<int16_t>(dPrev_[0]);
        ry = static_cast<int16_t>(dPrev_[1]);
    }

private:
    // Калибровка геймпада в модели АЦП (см. PadRegimeProfile::raw_cal_dip): "сырые" координаты -> выход.
    // Радиус выхода s = r - dip * rho * (1 - exp(-r / rho)), не больше outer_radius; направление сохраняется.
    static void calForward(const PadRendererProfile& p, double ux, double uy, double& ox, double& oy) {
        const double r = std::hypot(ux, uy);
        if (r < 1e-9) {
            ox = oy = 0.0;
            return;
        }
        const double dip = std::clamp(p.regime.raw_cal_dip, 0.0f, 0.9f), rho = std::max(1.0f, p.regime.raw_cal_rho);
        const double s = std::min<double>(r - dip * rho * (1.0 - std::exp(-r / rho)), p.outer_radius);
        ox = ux * s / r;
        oy = uy * s / r;
    }
    // Обратная калибровка: по выходу (px, py) найти "сырые" (ux, uy). Ньютон по радиусу, 6 шагов (кривая выпуклая и
    // монотонная, наклон не меньше 0.1). За коленом внешнего контура "сырой" радиус растёт быстрее выхода
    // (raw_rim_push): выход упирается в круг outer_radius, как у стика, прижатого к шахте.
    static void calInverse(const PadRendererProfile& p, double px, double py, double& ux, double& uy) {
        const double s = std::hypot(px, py);
        if (s < 1e-9) {
            ux = uy = 0.0;
            return;
        }
        const double dip = std::clamp(p.regime.raw_cal_dip, 0.0f, 0.9f), rho = std::max(1.0f, p.regime.raw_cal_rho);
        const double target = std::min<double>(s, p.outer_radius);
        double r = target;
        if (dip > 0.0) {
            for (int it = 0; it < 6; ++it) {
                const double e = std::exp(-r / rho);
                r -= (r - dip * rho * (1.0 - e) - target) / (1.0 - dip * e);
            }
        }
        const double knee = static_cast<double>(p.outer_radius) * p.outer_knee;
        if (s > knee) r += (s - knee) * p.regime.raw_rim_push;
        ux = px * r / s;
        uy = py * r / s;
    }
    static int64_t toQ16(double v) { return static_cast<int64_t>(std::llround(std::clamp(v, -40000.0, 40000.0) * 65536.0)); }
    static int64_t floorDiv(int64_t a, int64_t b) {
        int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }
    static int64_t isqrt(uint64_t v) {
        uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<double>(v)));
        while (r * r > v) --r;
        while ((r + 1) * (r + 1) <= v) ++r;
        return static_cast<int64_t>(r);
    }
    uint64_t next() {
        uint64_t z = (rng_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Приближение нормального числа, Q12 (СКО 4096): сумма четырёх 16-битных равномерных.
    int64_t gauss() {
        const uint64_t r = next();
        const int64_t s = static_cast<int64_t>(r & 0xFFFF) + static_cast<int64_t>((r >> 16) & 0xFFFF) + static_cast<int64_t>((r >> 32) & 0xFFFF) +
                          static_cast<int64_t>(r >> 48) - 131070;
        return (s * 7094) >> 16;  // СКО суммы 37837 -> 4096
    }

    // Шаг сетки по одной оси: наименьшее ненулевое приращение, если почти все приращения ему кратны; иначе НОД; иначе 1.
    static int estimateStep(const int16_t* v, int n) {
        int g = 0, mn = 0, cnt = 0;
        for (int i = 1; i < n; ++i) {
            const int d = std::abs(v[i] - v[i - 1]);
            if (!d) continue;
            ++cnt;
            g = gcd(g, d);
            if (!mn || d < mn) mn = d;
        }
        if (cnt < 8) return 1;
        if (g >= 2) return g;
        if (mn >= 4) {
            int ok = 0;
            for (int i = 1; i < n; ++i) {
                const int d = std::abs(v[i] - v[i - 1]);
                if (!d) continue;
                const int rem = d % mn;
                if (std::min(rem, mn - rem) <= std::max(1, mn / 64)) ++ok;  // 8 бит через 257: шаги 256..258
            }
            if (ok * 10 >= cnt * 9) return mn;
        }
        return 1;
    }
    static int gcd(int a, int b) {
        while (b) {
            const int t = a % b;
            a = b;
            b = t;
        }
        return a;
    }
    // Доля мощности величины приращения в полосе lo..hi Гц (прямое ДПФ по 255 приращениям).
    double bandShare(double lo, double hi, double fs) const {
        const int n = kProfileReports - 1;
        std::array<double, kProfileReports> m{};
        double mean = 0.0;
        for (int i = 0; i < n; ++i) {
            const double dx = obsX_[static_cast<size_t>(i + 1)] - obsX_[static_cast<size_t>(i)], dy = obsY_[static_cast<size_t>(i + 1)] - obsY_[static_cast<size_t>(i)];
            m[static_cast<size_t>(i)] = std::hypot(dx, dy);
            mean += m[static_cast<size_t>(i)];
        }
        mean /= n;
        double total = 0.0, band = 0.0;
        const double kPi = 3.14159265358979323846;
        for (int k = 1; k <= n / 2; ++k) {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < n; ++i) {
                const double a = 2.0 * kPi * k * i / n;
                re += (m[static_cast<size_t>(i)] - mean) * std::cos(a);
                im -= (m[static_cast<size_t>(i)] - mean) * std::sin(a);
            }
            const double pw = re * re + im * im, f = k * fs / n;
            total += pw;
            if (f >= lo && f < hi) band += pw;
        }
        return total > 1e-9 ? band / total : 0.0;
    }

    PadRendererProfile base_ = preset(Preset::XInput16bit);
    PadRendererProfile profile_ = preset(Preset::XInput16bit);
    bool ready_ = true;
    int observed_ = 0;
    std::array<int16_t, kProfileReports> obsX_{}, obsY_{};
    std::array<uint16_t, kProfileReports> obsDt_{};

    // состояние ведения
    PadRendererProfile ev_ = preset(Preset::XInput16bit);
    bool active_ = false;
    uint64_t rng_ = 0;
    int64_t prevT_[2] = {0, 0}, initT_[2] = {0, 0}, u_[2] = {0, 0};
    std::array<int64_t, 8> z_{};  // [резонатор][вдоль/поперёк][z1, z2]
    int32_t dir_[2] = {16384, 0};
    int64_t level_[2] = {0, 0};
    int held_[2] = {0, 0}, dPrev_[2] = {0, 0};
    int32_t phase_ = 0;
    int64_t rawPrev_[2] = {0, 0}, rawCur_[2] = {0, 0};  // модель АЦП: прошлая и текущая выборка, в разрядах
    int adcCount_ = 0;                                  // тактов с прошлой выборки
    bool stale_ = false;                                // идёт пачка "без середины"
};

// ---- PadTexture19: панель описателей потока стика (1 отчёт = 1 мс) -------------------------------------------
// Считается по приращениям d = diff(положение): mag = max(|dx|, |dy|) (полка, если 0), norm = sqrt(dx^2 + dy^2).
// 19 пунктов (7 и 18 парные: 21 число) + delta_ac_lag2 сверх панели. Эталон расчёта - pad_probes/pad_texture.py.
struct PadTexture19 {
    static constexpr int kCount = 22;
    static const char* name(int i) {
        static const char* const n[kCount] = {"hold_rate", "step_grid_adherence", "active_delta_mean_log1p", "active_delta_max_log1p", "active_delta_p90_log1p",
            "active_delta_p99_log1p", "sign_flip_rate_x", "sign_flip_rate_y", "hold_run_mean_log1p", "active_run_mean_log1p", "hold_run_std_log1p",
            "hold_run_p95_log1p", "bp_4_8_hz", "bp_8_14_hz", "bp_18_28_hz", "bp_30_120_hz", "bp_120_500_hz", "hf_power_log1p", "delta_ac_lag1", "delta_ac_lag4",
            "quantized_step_entropy", "delta_ac_lag2"};
        return i >= 0 && i < kCount ? n[i] : "?";
    }
    using Row = std::array<double, kCount>;

    // Панель для одного окна: n положений (n - 1 приращение), q - шаг сетки АЦП.
    static Row compute(const int16_t* x, const int16_t* y, size_t n, int q = 1) {
        Row out{};
        if (n < 9) return out;
        const size_t m = n - 1;
        std::vector<double> dx(m), dy(m), norm(m);
        std::vector<char> act(m);
        std::vector<double> an;
        size_t nzN = 0, nzOk = 0;
        for (size_t i = 0; i < m; ++i) {
            dx[i] = static_cast<double>(x[i + 1]) - x[i];
            dy[i] = static_cast<double>(y[i + 1]) - y[i];
            norm[i] = std::hypot(dx[i], dy[i]);
            act[i] = (dx[i] != 0.0 || dy[i] != 0.0) ? 1 : 0;
            if (act[i]) an.push_back(norm[i]);
            for (double d : {dx[i], dy[i]}) {
                if (d != 0.0) {
                    ++nzN;
                    if (static_cast<long long>(std::llabs(static_cast<long long>(d))) % q == 0) ++nzOk;
                }
            }
        }
        out[0] = 1.0 - static_cast<double>(an.size()) / static_cast<double>(m);
        out[1] = nzN ? static_cast<double>(nzOk) / static_cast<double>(nzN) : 1.0;
        if (!an.empty()) {
            double s = 0.0, mx = 0.0;
            for (double v : an) {
                s += v;
                mx = std::max(mx, v);
            }
            out[2] = std::log1p(s / static_cast<double>(an.size()));
            out[3] = std::log1p(mx);
            out[4] = std::log1p(percentile(an, 90.0));
            out[5] = std::log1p(percentile(an, 99.0));
        }
        const std::vector<double>* ax[2] = {&dx, &dy};
        for (int a = 0; a < 2; ++a) {
            int last = 0;
            size_t cmp = 0, flips = 0;
            for (double d : *ax[a]) {
                if (d == 0.0) continue;
                const int s = d > 0 ? 1 : -1;
                if (last) {
                    ++cmp;
                    if (s != last) ++flips;
                }
                last = s;
            }
            out[static_cast<size_t>(6 + a)] = cmp ? static_cast<double>(flips) / static_cast<double>(cmp) : 0.0;
        }
        std::vector<double> hold, active;
        {
            char cur = act[0];
            double len = 1.0;
            for (size_t i = 1; i < m; ++i) {
                if (act[i] == cur) len += 1.0;
                else {
                    (cur ? active : hold).push_back(len);
                    cur = act[i];
                    len = 1.0;
                }
            }
            (cur ? active : hold).push_back(len);
        }
        out[8] = hold.empty() ? 0.0 : std::log1p(mean(hold));
        out[9] = active.empty() ? 0.0 : std::log1p(mean(active));
        out[10] = hold.size() >= 2 ? std::log1p(stddev(hold)) : 0.0;
        out[11] = hold.empty() ? 0.0 : std::log1p(percentile(hold, 95.0));
        // спектр norm: прямое ДПФ (вне горячего пути)
        const double mu = mean(norm);
        static const double kBands[5][2] = {{4, 8}, {8, 14}, {18, 28}, {30, 120}, {120, 500}};
        double total = 0.0, band[5] = {0, 0, 0, 0, 0}, hf = 0.0;
        const double kPi = 3.14159265358979323846;
        for (size_t k = 1; k <= m / 2; ++k) {
            double re = 0.0, im = 0.0;
            const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(m);
            for (size_t i = 0; i < m; ++i) {
                re += (norm[i] - mu) * std::cos(w * static_cast<double>(i));
                im -= (norm[i] - mu) * std::sin(w * static_cast<double>(i));
            }
            const double pw = re * re + im * im, f = static_cast<double>(k) * 1000.0 / static_cast<double>(m);
            total += pw;
            for (int b = 0; b < 5; ++b) {
                if (f >= kBands[b][0] && f < kBands[b][1]) band[b] += pw;
            }
            if (f >= 30.0 && f < 500.0) hf += pw;
        }
        for (int b = 0; b < 5; ++b) out[static_cast<size_t>(12 + b)] = band[b] / std::max(total, 1e-9);
        out[17] = std::log1p(hf / static_cast<double>(m));
        out[18] = autocorr(norm, 1);
        out[19] = autocorr(norm, 4);
        {
            std::array<double, 16> hist{};
            for (double v : norm) {
                const long long s = std::llround(v / q);
                int bin = 0;
                if (s > 0) bin = 1 + static_cast<int>(std::floor(std::log2(static_cast<double>(s))));
                hist[static_cast<size_t>(std::clamp(bin, 0, 15))] += 1.0;
            }
            double e = 0.0;
            for (double h : hist) {
                if (h > 0) {
                    const double pr = h / static_cast<double>(m);
                    e -= pr * std::log(pr);
                }
            }
            out[20] = e / std::log(16.0);
        }
        out[21] = autocorr(norm, 2);
        for (double& v : out) {
            if (!std::isfinite(v)) v = 0.0;
        }
        return out;
    }

    // Панель по непересекающимся окнам size + 1 положений; окна, где активных отчётов меньше minActive, пропускаются.
    static std::vector<Row> windows(const int16_t* x, const int16_t* y, size_t n, int q = 1, size_t size = 512, double minActive = 0.05) {
        std::vector<Row> rows;
        for (size_t i = 0; i + size < n; i += size) {
            const Row r = compute(x + i, y + i, size + 1, q);
            if (1.0 - r[0] >= minActive) rows.push_back(r);
        }
        return rows;
    }

    // W1 по каждому описателю между двумя наборами окон (сетка 199 квантилей) и сумма в total.
    static Row w1(const std::vector<Row>& a, const std::vector<Row>& b, double* total = nullptr) {
        Row out{};
        double sum = 0.0;
        if (a.empty() || b.empty()) {
            if (total) *total = 0.0;
            return out;
        }
        for (int c = 0; c < kCount; ++c) {
            std::vector<double> va, vb;
            for (const auto& r : a) va.push_back(r[static_cast<size_t>(c)]);
            for (const auto& r : b) vb.push_back(r[static_cast<size_t>(c)]);
            std::sort(va.begin(), va.end());
            std::sort(vb.begin(), vb.end());
            double acc = 0.0;
            for (int g = 1; g < 200; ++g) acc += std::fabs(quantileSorted(va, g / 200.0) - quantileSorted(vb, g / 200.0));
            out[static_cast<size_t>(c)] = acc / 199.0;
            sum += out[static_cast<size_t>(c)];
        }
        if (total) *total = sum;
        return out;
    }

private:
    static double mean(const std::vector<double>& v) {
        double s = 0.0;
        for (double x : v) s += x;
        return v.empty() ? 0.0 : s / static_cast<double>(v.size());
    }
    static double stddev(const std::vector<double>& v) {
        const double m = mean(v);
        double s = 0.0;
        for (double x : v) s += (x - m) * (x - m);
        return v.empty() ? 0.0 : std::sqrt(s / static_cast<double>(v.size()));
    }
    // Квантиль с линейной интерполяцией (как numpy.quantile / numpy.percentile по умолчанию).
    static double quantileSorted(const std::vector<double>& s, double qq) {
        if (s.empty()) return 0.0;
        const double pos = qq * static_cast<double>(s.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(pos)), hi = std::min(s.size() - 1, lo + 1);
        return s[lo] + (s[hi] - s[lo]) * (pos - static_cast<double>(lo));
    }
    static double percentile(std::vector<double> v, double pct) {
        std::sort(v.begin(), v.end());
        return quantileSorted(v, pct / 100.0);
    }
    static double autocorr(const std::vector<double>& v, size_t lag) {
        if (v.size() < lag + 4) return 0.0;
        const size_t n = v.size() - lag;
        double ma = 0.0, mb = 0.0;
        for (size_t i = 0; i < n; ++i) {
            ma += v[i];
            mb += v[i + lag];
        }
        ma /= static_cast<double>(n);
        mb /= static_cast<double>(n);
        double sab = 0.0, saa = 0.0, sbb = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sab += (v[i] - ma) * (v[i + lag] - mb);
            saa += (v[i] - ma) * (v[i] - ma);
            sbb += (v[i + lag] - mb) * (v[i + lag] - mb);
        }
        const double den = std::sqrt(saa * sbb);
        return den >= 1e-9 ? sab / den : 0.0;
    }
};

}  // namespace makcu
