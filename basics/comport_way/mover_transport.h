// Интерфейс транспорта движения (без SDK): общий для COM-стенда
// (makcu_transport.h) и UDP-стенда (..\udp_way\makcu_udp_transport.h).
// motion_engine.h зависит только от него.
#pragma once

#include "mover_common.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace mover {

struct TransportStats {
    std::atomic<uint64_t> writes{0};        // команд реально отправлено в драйвер
    std::atomic<uint64_t> deferred{0};      // тик пропущен из-за занятого кольца (дельта перенесена)
    std::atomic<uint64_t> errors{0};
    std::atomic<uint32_t> maxInflight{0};
    std::atomic<double> maxCallUs{0.0};     // самый долгий вызов отправки
    std::atomic<double> sumCallUs{0.0};

    void reset() {
        writes = 0;
        deferred = 0;
        errors = 0;
        maxInflight = 0;
        maxCallUs = 0.0;
        sumCallUs = 0.0;
    }
    void noteCall(double us) {
        sumCallUs.store(sumCallUs.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
        if (us > maxCallUs.load(std::memory_order_relaxed)) {
            maxCallUs.store(us, std::memory_order_relaxed);
        }
    }
};

class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    // true  - команда принята (отправлена/поставлена в очередь);
    // false - сейчас отправить нельзя, вызывающий сохраняет дельту у себя.
    [[nodiscard]] virtual bool move(int16_t dx, int16_t dy) = 0;
    [[nodiscard]] virtual bool healthy() const = 0;
    // Конец прогона: дослать то, что транспорт держит у себя (квитанции, перенос).
    virtual void finish() {}
    // Статистика сверх общей (пусто - нечего добавить).
    [[nodiscard]] virtual std::string extraStats() const { return {}; }
    virtual void resetStats() { stats.reset(); }
    TransportStats stats;
};

}  // namespace mover
