// Транспорт команд движения в MAKCU по UDP (Ethernet через USB-хаб платы).
//
// Что выяснено замером (fw 4094, 2026-10-10, подробно - README.md):
//  - Рабочая обёртка - RAW: 0x55 | nonce:8 | кадр MAK_API (DE AD LEN CMD PAYLOAD).
//    Плата возвращает тот же nonce в ответе. Голый кадр (HOST без шифрования)
//    остаётся без ответа.
//  - В датаграмме ровно ОДИН кадр. Два кадра, пакет MBAT, несколько текстовых
//    команд в 0x6B - датаграмма отбрасывается целиком (или ERR).
//  - Плата принимает ~583 датаграммы/с (одна на ~1.72 мс) на любой опкод, лишние
//    теряются ДО обработки: потерянный 0x69 не исполнен (проверено опросом id).
//    Сеть и ПК ни при чём: счётчик RX сетевого интерфейса платы растёт на каждую
//    отправленную датаграмму (probes/udp_netstat.py).
//  - Модель приёма (probes/udp_slot.py, udp_cycle_hist.py): корректный кадр занимает
//    обработчик ~1.72 мс; датаграмма, пришедшая в первые ~0.65 мс после принятой,
//    выбрасывается; одна, пришедшая позже, ждёт и обслуживается сразу по окончании
//    цикла; ответ приходит на ПК через ~1.05 мс после отправки.
//
// Поэтому транспорт сам держит темп датаграмм, два режима:
//   Pace::Reply (по умолчанию для RawMove): в полёте ровно одна датаграмма, следующая
//     уходит сразу по ответу на предыдущую (из потока приёма, между тиками) или через
//     kReplyWaitUs без ответа. Ответ приходит после окна выброса, но до конца цикла:
//     новая датаграмма ждёт в "слоте" платы, плата работает без простоя, ~580/с без
//     потерь. move() всегда принимает дельту в перенос транспорта, путь не теряется.
//   Pace::Bucket (и для 0x18/0x67, у которых нет ответа): ведро токенов, в среднем одна
//     датаграмма на gapUs (1800 мкс), ёмкость burst (1). На сетке тиков 1 мс это
//     2 мс = 500 датаграмм/с без потерь. move() == false, пока темп не позволяет:
//     дельта остаётся у вызывающего и уходит следующим тиком.
//
// Форматы:
//   RawMove - 0x69 RAW_MOVE + квитанции 0x6A: каждый отправленный 0x69 = ровно
//             один HID-отчёт, мимо интерполятора. Каждая квитанция - отдельная
//             датаграмма (контракт: consume=1 освобождает одну запись), поэтому
//             потолок ~290 движений/с (Reply) или 250 (Bucket).
//   Mak     - MOVE 0x18 (без ответа): через интерполятор прошивки.
//   MoveNow - MOVE_NOW 0x67 (без ответа): на 4094 тоже через интерполятор.
//   Batch   - 0x6C (V4.097+): до 7 Raw-записей в одной датаграмме, одна квитанция на пакет.
//             Каждый тик move() = одна запись = один HID-отчёт. В полёте один пакет; по
//             квитанции поток приёма сразу шлёт следующий с накопившимися записями, X =
//             старший терминальный id из квитанции (флаг bit0) освобождает записи платы.
//             Квитанция освобождает не больше 7 строк, поэтому пакет не больше 7 записей;
//             если за один оборот (~7 мс) накопилось больше, записи склеиваются в 7 (путь
//             сохраняется, задержка не растёт). Непринятые записи (нет места) возвращаются
//             в очередь. Замер 2026-10-12, V4.097: ~974 записей/с, HID 970 Гц, путь точный.
//
// Потери UDP в RawMove (путь не теряется и не дублируется):
//   - нет ответа на 0x69 за kLostUs (8 мс): запрос 0x6A(id, consume=0). Ответ "нет
//     такого id" (255/5) - запрос не дошёл, дельта возвращается в перенос; запись
//     есть - продолжаем как обычно (потерян только ответ).
//   - нет ответа на квитанцию: повтор consume=1. Если первая квитанция дошла,
//     повтор получит 255/5 - запись известна плате, поэтому считаем её отданной
//     (счётчик "неоднозначных": отказ 3 в этом случае был бы не виден).
//   - перестановка/дубли: ответ сопоставляется с запросом по nonce, повторный
//     ответ по закрытой записи игнорируется (счётчик поздних ответов).
//
// Кнопки мыши (2026-10-12): open() включает поток INPUT_STREAM mouse (0x52), события
// INPUT_CHANGE 0x53 разбирает поток приёма; стартовая маска и pollButtons() - GET
// PHYS_BUTTONS 0x55 (pollButtons() - только между прогонами). close() возвращает поток.
// События плата шлёт с порта, равного нашему (не 8080), поэтому сокет без connect():
// принимаются любые датаграммы с IP платы. На сети Public брандмауэр Windows режет их
// без разрешающего правила - тогда маску держит только pollButtons().
//
// Контракт как у RawComTransport: move()/finish() из одного потока (поток
// тайминга), query()/prepareRaw() - когда прогон не идёт; свой поток приёма.
#pragma once

#include "mover_common.h"
#include "mover_transport.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace mover {

class RawUdpTransport final : public Transport {
public:
    enum class Wire { Raw, Host };  // 0x55+nonce / голый кадр
    enum class Format { RawMove, Mak, MoveNow, Batch };
    enum class Pace { Reply, Bucket };  // темп датаграмм: по ответам / ведро токенов

    static constexpr double kDefaultGapUs = 1800.0;
    static constexpr double kDefaultBurst = 1.0;
    // Pace::Reply: без ответа столько - следующая датаграмма всё равно (потерю разберёт
    // восстановление Raw). Обычно ответ приходит через 1.05-2.1 мс.
    static constexpr double kReplyWaitUs = 3000.0;

    // Счётчики сети (все датаграммы, включая квитанции и служебные запросы).
    struct NetSnapshot {
        uint64_t datagrams = 0;      // отправлено датаграмм
        uint64_t expected = 0;       // из них ждали ответа
        uint64_t replies = 0;        // ответов, сопоставленных по nonce
        uint64_t unmatched = 0;      // ответов с чужим/повторным nonce, события
        uint64_t junk = 0;           // не разобранных датаграмм
        uint64_t pacerWaits = 0;     // дельта ждала темпа (Bucket: move() == false; Reply: ждёт в переносе)
        uint64_t rxSends = 0;        // Reply: датаграмм, отправленных потоком приёма сразу по ответу
        uint64_t replyTimeouts = 0;  // Reply: следующая датаграмма ушла по тайм-ауту ответа
        uint64_t sendBusy = 0;       // WSAEWOULDBLOCK
        double rttP50Ms = 0.0, rttP99Ms = 0.0, rttMaxMs = 0.0;
        double spanS = 0.0;          // от первой до последней отправки
    };

    RawUdpTransport() = default;
    ~RawUdpTransport() override { close(); }
    RawUdpTransport(const RawUdpTransport&) = delete;
    RawUdpTransport& operator=(const RawUdpTransport&) = delete;

    bool open(const std::string& host, uint16_t port, Wire wire, std::string& error) {
        close();
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            error = "WSAStartup failed";
            return false;
        }
        wsaStarted_ = true;
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &remote.sin_addr) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                error = "не удалось разрешить адрес " + host;
                close();
                return false;
            }
            remote.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            error = "socket() failed, WSA " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        // ICMP "порт недоступен" иначе ломает recv() ошибкой WSAECONNRESET.
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        int rcvbuf = 1 << 20;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        // Без connect(): события 0x53 приходят с другого порта платы (см. шапку).
        remote_ = remote;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (::bind(sock_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            error = "bind() failed, WSA " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        u_long nonblocking = 1;
        ioctlsocket(sock_, FIONBIO, &nonblocking);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        endpoint_ = std::string(ip) + ":" + std::to_string(port);
        wire_ = wire;
        // Случайная база nonce: поздние ответы прошлых сессий не совпадут.
        seq_ = (static_cast<uint64_t>(Clock::now()) << 20) ^ static_cast<uint64_t>(GetCurrentProcessId());
        failed_ = false;
        stopRx_ = false;
        rx_ = std::thread([this] { rxLoop(); });

        auto kinds = query(kOpDevice, nullptr, 0, 150, 3);
        if (!kinds || kinds->size() != 1) {
            error = "MAKCU " + endpoint_ + " не ответил на DEVICE (0x02) - адрес/порт/обёртка?";
            close();
            return false;
        }
        kinds_ = (*kinds)[0];
        if (auto fw = query(kOpFirmware, nullptr, 0, 150, 3); fw && fw->size() == 4) firmware_ = le32(fw->data());
        initButtons();
        return true;
    }

    void close() {
        if (sock_ != INVALID_SOCKET && streamRestore_) {
            // Вернуть INPUT_STREAM mouse, каким он был до open().
            const uint8_t restore[2] = {0x01, *streamRestore_};
            (void)sendFire(kOpInputStream, restore, 2);
        }
        streamRestore_.reset();
        buttonsOk_ = false;
        rawMode_ = false;
        stopRx_ = true;
        if (rx_.joinable()) rx_.join();
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    [[nodiscard]] bool isOpen() const { return sock_ != INVALID_SOCKET; }
    [[nodiscard]] const std::string& endpoint() const { return endpoint_; }
    [[nodiscard]] uint8_t deviceKinds() const { return kinds_; }
    [[nodiscard]] uint32_t firmware() const { return firmware_; }

    // ---- кнопки мыши ------------------------------------------------------------------
    // Маска физических кнопок: bit0 левая, bit1 правая, bit2 средняя, bit3 side1, bit4 side2.
    // Любой поток, не блокируют. Обновляется событием 0x53 сразу по нажатию.
    static constexpr int kButtonCount = 5;
    static const char* buttonName(int i) {
        static const char* const kNames[kButtonCount] = {"left", "right", "middle", "side1", "side2"};
        return i >= 0 && i < kButtonCount ? kNames[i] : "?";
    }
    [[nodiscard]] uint8_t buttons() const { return mask_.load(std::memory_order_acquire); }
    [[nodiscard]] bool buttonDown(int i) const { return (buttons() >> i) & 1u; }
    // Нажатий (переходов 0 -> 1) с open(); кнопка, зажатая при open(), не считается.
    [[nodiscard]] uint32_t pressCount(int i) const { return presses_[i].load(std::memory_order_acquire); }
    [[nodiscard]] bool buttonsAvailable() const { return buttonsOk_.load(); }
    [[nodiscard]] uint64_t buttonEvents() const { return buttonEvents_.load(); }
    // Событий нет, а опрос уже находил изменения: их режет брандмауэр (или поток выключен).
    [[nodiscard]] bool buttonEventsMissing() const { return buttonEvents_.load() == 0 && buttonMissed_.load() >= 2; }
    [[nodiscard]] const std::string& buttonInfo() const { return buttonInfo_; }
    // Перечитать маску (0x55). Только когда прогон не идёт (как query()): исправляет маску,
    // если событие потерялось, и держит её, если события не доходят совсем.
    bool pollButtons() {
        if (!buttonsOk_.load()) return false;
        auto m = query(kOpPhysButtons, nullptr, 0, 60, 1);
        if (!m || m->size() != 1) return false;
        buttonPolls_.fetch_add(1, std::memory_order_relaxed);
        applyMask((*m)[0], true);
        return true;
    }

    void setFormat(Format f) { format_ = f; }
    [[nodiscard]] Format format() const { return format_; }
    // Reply действует только для RawMove (у 0x18/0x67 нет ответа - для них ведро).
    void setPace(Pace p) { pace_ = p; }
    [[nodiscard]] Pace pace() const { return pace_; }
    [[nodiscard]] bool replyPaced() const { return pace_.load() == Pace::Reply && format_.load() == Format::RawMove; }
    // Средний интервал между датаграммами (ведро токенов ёмкостью burst_). С burst_ = 1
    // на сетке тиков 1 мс фактический интервал - 2 мс (500 датаграмм/с).
    void setGapUs(double us) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        gapUs_ = std::max(1.0, us);
        tokensPerTick_ = 1.0 / static_cast<double>(Clock::fromUs(gapUs_));
    }
    [[nodiscard]] double gapUs() const { return gapUs_; }
    // Ёмкость ведра, датаграмм (>= 1). 1 - не чаще раза в 2 мс на сетке 1 мс;
    // 2 - излишек копится и изредка разрешает интервал 1 мс.
    void setBurst(double b) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        burst_ = std::max(1.0, b);
    }
    [[nodiscard]] double burst() const { return burst_; }

    [[nodiscard]] const char* name() const override {
        switch (format_.load()) {
        case Format::RawMove: return "udp-raw69";
        case Format::Mak: return "udp-0x18";
        case Format::MoveNow: return "udp-0x67";
        case Format::Batch: return "udp-batch6c";
        }
        return "udp";
    }

    [[nodiscard]] bool healthy() const override {
        return sock_ != INVALID_SOCKET && !failed_.load() && (!usesRecords() || rawMode_.load());
    }
    // Формат держит записи Raw на плате (0x69 или 0x6C): нужны prepareRaw()/finishRaw().
    [[nodiscard]] bool usesRecords() const {
        const Format f = format_.load();
        return f == Format::RawMove || f == Format::Batch;
    }

    // ---- горячий путь ---------------------------------------------------------
    [[nodiscard]] bool move(int16_t dx, int16_t dy) override {
        if (!healthy()) return false;
        if (format_ == Format::Batch) return moveBatch(dx, dy);
        if (format_ == Format::RawMove) {
            return pace_ == Pace::Reply ? moveRawReply(dx, dy) : moveRaw(dx, dy, dx == 0 && dy == 0);
        }
        if (dx == 0 && dy == 0) return true;
        std::lock_guard<std::mutex> lock(sendMutex_);
        const int64_t now = Clock::now();
        if (!pacerReady(now)) {
            net_.pacerWaits.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto ux = static_cast<uint16_t>(dx);
        const auto uy = static_cast<uint16_t>(dy);
        const uint8_t payload[4] = {static_cast<uint8_t>(ux), static_cast<uint8_t>(ux >> 8), static_cast<uint8_t>(uy),
            static_cast<uint8_t>(uy >> 8)};
        const int64_t t0 = Clock::now();
        if (!sendLocked(format_ == Format::Mak ? kOpMove : kOpMoveNow, payload, 4, Kind::Fire, 0)) return false;
        stats.noteCall(Clock::toUs(Clock::now() - t0));
        stats.writes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Квитанции и перенос без новой дельты.
    void service() {
        if (healthy() && format_ == Format::RawMove) (void)moveRaw(0, 0, true);
        if (healthy() && format_ == Format::Batch) {
            std::lock_guard<std::mutex> lock(sendMutex_);
            (void)pumpBatchLocked(false);
        }
    }

    void finish() override { (void)finishRaw(400); }

    // true - все записи квитированы, перенос (0,0).
    bool finishRaw(int timeoutMs) {
        if (format_ == Format::Batch) return healthy() ? finishBatch(timeoutMs) : true;
        if (format_ != Format::RawMove || !healthy()) return true;
        const int64_t until = Clock::now() + Clock::fromUs(timeoutMs * 1000.0);
        while (Clock::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                if (rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0) return true;
            }
            (void)moveRaw(0, 0, true);
            Sleep(1);
        }
        std::lock_guard<std::mutex> lock(rawMutex_);
        return rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0;
    }

    // ---- служебный обмен (не из горячего пути) --------------------------------
    // Кадр -> payload ответа (RAW: ответ с тем же nonce; HOST: с тем же опкодом).
    // replyOp - опкод ответа. Темп датаграмм соблюдается.
    std::optional<std::vector<uint8_t>> query(uint8_t op, const uint8_t* payload, uint16_t len, int timeoutMs,
        int tries = 1, uint8_t* replyOp = nullptr) {
        for (int attempt = 0; attempt < tries; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                waitPaceLocked();
                std::lock_guard<std::mutex> sl(syncMutex_);
                syncDone_ = false;
                syncOp_ = op;
                syncSeq_ = seq_ + 1;  // sendLocked() возьмёт именно этот nonce
                syncWaiting_ = true;
                if (!sendLocked(op, payload, len, Kind::Sync, 0)) {
                    syncWaiting_ = false;
                    return std::nullopt;
                }
            }
            std::unique_lock<std::mutex> sl(syncMutex_);
            const bool ok = syncCv_.wait_for(sl, std::chrono::milliseconds(timeoutMs), [&] { return syncDone_; });
            syncWaiting_ = false;
            if (ok) {
                if (replyOp) *replyOp = syncReplyOp_;
                return syncReply_;
            }
        }
        return std::nullopt;
    }

    // Текст KM через туннель 0x6B ('k' + текст без первой буквы): "m.device()".
    // Ответ - тоже кадр, где байт опкода = первая буква текста ('k' в "km.MAKCU",
    // 'R' в "R:MK;M:8uf;..."). Возвращает строку без "\r\n>>> ".
    std::optional<std::string> kmQuery(const std::string& cmdWithoutK, int timeoutMs = 150) {
        uint8_t first = 0;
        auto r = query(kOpKmText, reinterpret_cast<const uint8_t*>(cmdWithoutK.data()),
            static_cast<uint16_t>(cmdWithoutK.size()), timeoutMs, 2, &first);
        if (!r) return std::nullopt;
        std::string s(1, static_cast<char>(first));
        s.append(r->begin(), r->end());
        const auto cut = s.find("\r\n");
        return cut == std::string::npos ? s : s.substr(0, cut);
    }

    // Интерполяция 0x1F (0..100, 255 = AUTO): GET пустой, SET 1 байт, ответ - значение.
    std::optional<uint8_t> getInterp() {
        auto r = query(kOpInterp, nullptr, 0, 100, 3);
        if (r && r->size() == 1) return (*r)[0];
        return std::nullopt;
    }
    bool setInterp(uint8_t v) {
        (void)query(kOpInterp, &v, 1, 100, 2);
        const auto now = getInterp();
        return now && *now == v;
    }

    // Raw: метаданные, дочистка записей прошлых сессий, окно. false - info объясняет.
    bool prepareRaw(std::string& info) {
        rawMode_ = false;
        auto meta = queryRawMeta();
        if (!meta) {
            info = "нет ответа на 0x6A(id=0) - прошивка без Raw API (нужна V4.074+)";
            return false;
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "schema %u, last_accepted_id %lu, свободно записей %u из %u", meta->schema,
            static_cast<unsigned long>(meta->lastId), meta->freeRecords, meta->capacity);
        info = buf;
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            const uint32_t consumed = drainLeftoverRecords(meta->lastId, meta->capacity - meta->freeRecords);
            if (auto again = queryRawMeta()) meta = again;
            std::snprintf(buf, sizeof(buf), "; дочищено старых записей %lu -> свободно %u из %u",
                static_cast<unsigned long>(consumed), meta->freeRecords, meta->capacity);
            info += buf;
        }
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            rawNextId_ = meta->lastId + 1;
            rawLive_ = 0;
            rawCarryX_ = 0;
            rawCarryY_ = 0;
            for (auto& r : rawRec_) r = RawRecord{};
            rawWindow_ = std::clamp<uint32_t>(meta->freeRecords > 2 ? meta->freeRecords - 2u : 1u, 1u, kRawMaxLive);
            batchPend_.clear();
            batchFlight_ = BatchFlight{};
            batchHaveX_ = false;
            batchMaxTerm_ = 0;
        }
        batchAwait_.store(0, std::memory_order_release);
        batchMetaPending_ = false;
        rawMode_ = true;
        if (rawWindow_ < kRawMaxLive) info += "; окно уменьшено до " + std::to_string(rawWindow_);
        return true;
    }

    struct RawMeta {
        uint8_t schema = 0;
        uint32_t lastId = 0;
        uint8_t freeRecords = 0;
        uint8_t capacity = 0;
    };
    std::optional<RawMeta> queryRawMeta() {
        const uint8_t zero[5] = {0, 0, 0, 0, 0};
        auto r = query(kOpRawAck, zero, 5, 100, 3);
        if (!r || r->size() != 8) return std::nullopt;
        const uint8_t* p = r->data();
        return RawMeta{p[1], le32(p + 2), p[6], p[7]};
    }

    // Замер приёма платы (стенд): count запросов 0x6A(id=0) с интервалом gapUs
    // мимо регулятора темпа. Курсор не двигается.
    struct IntakeResult {
        uint32_t sent = 0;
        uint32_t answered = 0;
        double seconds = 0.0;
        double rttP50Ms = 0.0, rttP99Ms = 0.0, rttMaxMs = 0.0;
    };
    IntakeResult probeIntake(double gapUs, uint32_t count) {
        IntakeResult res;
        std::vector<uint64_t> seqs;
        seqs.reserve(count);
        const uint8_t zero[5] = {0, 0, 0, 0, 0};
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            waitPaceLocked();
            const int64_t t0 = Clock::now();
            const double step = static_cast<double>(Clock::fromUs(gapUs));
            for (uint32_t i = 0; i < count; ++i) {
                const int64_t due = t0 + static_cast<int64_t>(step * i);
                while (Clock::now() < due) YieldProcessor();
                if (sendLocked(kOpRawAck, zero, 5, Kind::Probe, 0)) seqs.push_back(seq_);
            }
            res.seconds = Clock::toUs(Clock::now() - t0) / 1e6;
            tokens_ = 0.0;  // замер шёл мимо ведра: начать темп заново
            lastRefill_ = Clock::now();
        }
        Sleep(250);
        awaitSeq_.store(0, std::memory_order_release);  // ответы пришли или потеряны: не ждать их
        res.sent = static_cast<uint32_t>(seqs.size());
        std::vector<double> rtt;
        {
            std::lock_guard<std::mutex> lock(sentMutex_);
            for (uint64_t s : seqs) {
                const Sent& e = sent_[s & kSentMask];
                if (e.seq == s && e.replied) {
                    ++res.answered;
                    rtt.push_back(e.rttMs);
                }
            }
        }
        if (!rtt.empty()) {
            std::sort(rtt.begin(), rtt.end());
            res.rttP50Ms = rtt[rtt.size() / 2];
            res.rttP99Ms = rtt[std::min(rtt.size() - 1, rtt.size() * 99 / 100)];
            res.rttMaxMs = rtt.back();
        }
        return res;
    }

    // ---- статистика -------------------------------------------------------------
    struct RawSnapshot {
        uint64_t submitted = 0, accepted = 0, endpointOk = 0, rejected = 0, immediate = 0, noCapacity = 0;
        uint64_t ackSent = 0, ackRetry = 0, ackReplyLost = 0, queries = 0, lostRequests = 0, ambiguous = 0;
        uint64_t lateReplies = 0, ackTurns = 0, windowFull = 0;
        uint32_t maxLive = 0, window = 0, live = 0, lastReason = 0;
        int64_t carryX = 0, carryY = 0;
    };
    [[nodiscard]] RawSnapshot rawStats() const {
        RawSnapshot s;
        s.submitted = raw_.submitted.load();
        s.accepted = raw_.accepted.load();
        s.endpointOk = raw_.endpointOk.load();
        s.rejected = raw_.rejected.load();
        s.immediate = raw_.immediate.load();
        s.noCapacity = raw_.noCapacity.load();
        s.ackSent = raw_.ackSent.load();
        s.ackRetry = raw_.ackRetry.load();
        s.ackReplyLost = raw_.ackReplyLost.load();
        s.queries = raw_.queries.load();
        s.lostRequests = raw_.lostRequests.load();
        s.ambiguous = raw_.ambiguous.load();
        s.lateReplies = raw_.lateReplies.load();
        s.ackTurns = raw_.ackTurns.load();
        s.windowFull = raw_.windowFull.load();
        s.maxLive = raw_.maxLive.load();
        s.lastReason = raw_.lastReason.load();
        std::lock_guard<std::mutex> lock(rawMutex_);
        s.window = rawWindow_;
        s.live = rawLive_;
        s.carryX = rawCarryX_;
        s.carryY = rawCarryY_;
        return s;
    }

    [[nodiscard]] NetSnapshot netStats() const {
        NetSnapshot s;
        s.datagrams = net_.datagrams.load();
        s.expected = net_.expected.load();
        s.replies = net_.replies.load();
        s.unmatched = net_.unmatched.load();
        s.junk = net_.junk.load();
        s.pacerWaits = net_.pacerWaits.load();
        s.rxSends = net_.rxSends.load();
        s.replyTimeouts = net_.replyTimeouts.load();
        s.sendBusy = net_.sendBusy.load();
        const int64_t a = net_.firstSend.load();
        const int64_t b = net_.lastSend.load();
        s.spanS = a && b > a ? Clock::toUs(b - a) / 1e6 : 0.0;
        uint64_t total = 0;
        for (const auto& h : rttHist_) total += h.load();
        auto pct = [&](double p) {
            if (total == 0) return 0.0;
            const auto target = static_cast<uint64_t>(p * static_cast<double>(total));
            uint64_t acc = 0;
            for (size_t i = 0; i < rttHist_.size(); ++i) {
                acc += rttHist_[i].load();
                if (acc > target) return (static_cast<double>(i) + 0.5) * kRttBinMs;
            }
            return static_cast<double>(rttHist_.size()) * kRttBinMs;
        };
        s.rttP50Ms = pct(0.5);
        s.rttP99Ms = pct(0.99);
        s.rttMaxMs = net_.rttMaxUs.load() / 1000.0;
        return s;
    }

    [[nodiscard]] std::string extraStats() const override {
        const NetSnapshot n = netStats();
        char pace[160];
        if (replyPaced()) {
            std::snprintf(pace, sizeof(pace), "темп по ответам: сразу по ответу %llu, по тайм-ауту %llu, дельта ждала %llu",
                u(n.rxSends), u(n.replyTimeouts), u(n.pacerWaits));
        } else {
            std::snprintf(pace, sizeof(pace), "ждали темп %llu (ведро: интервал %.0f мкс, ёмкость %.1f)", u(n.pacerWaits),
                gapUs(), burst());
        }
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "udp %s: датаграмм %llu (%.0f/с) | ждали ответа %llu, пришло %llu, без ответа %lld | RTT p50 %.2f p99 %.2f"
            " max %.2f мс | %s | чужих %llu, мусор %llu, send занят %llu",
            endpoint_.c_str(), u(n.datagrams), n.spanS > 0 ? n.datagrams / n.spanS : 0.0, u(n.expected), u(n.replies),
            static_cast<long long>(n.expected) - static_cast<long long>(n.replies), n.rttP50Ms, n.rttP99Ms, n.rttMaxMs,
            pace, u(n.unmatched), u(n.junk), u(n.sendBusy));
        std::string out = buf;
        if (format_ == Format::Batch) {
            uint32_t pending = 0;
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                pending = static_cast<uint32_t>(batchPend_.size());
            }
            const auto& b = batch_;
            const uint64_t pk = b.packets.load();
            const uint32_t minFree = b.minFree.load();
            std::snprintf(buf, sizeof(buf),
                "\nbatch 0x6C: пакетов %llu, записей %llu (%.2f/пакет), принято платой %llu | квитанций %llu: частичных %llu"
                " (нет места), ошибок конверта %llu (X отклонён %llu), непонятных %llu | потеряно квитанций %llu, опросов %llu"
                " | освобождено %llu, мин. свободно %d, 'more' %llu | терминальных: отдано %llu, отказ %llu"
                " | склеено записей %llu, max в очереди %u, в очереди %u | посл. причина %u",
                u(pk), u(b.records.load()), pk ? static_cast<double>(b.records.load()) / static_cast<double>(pk) : 0.0,
                u(b.admitted.load()), u(b.receipts.load()), u(b.partial.load()), u(b.envelope.load()), u(b.xRejected.load()),
                u(b.badReply.load()), u(b.lostReceipts.load()), u(b.metaQueries.load()), u(b.freed.load()),
                minFree > 255 ? -1 : static_cast<int>(minFree), u(b.moreSet.load()), u(b.termSubmitted.load()),
                u(b.termRejected.load()), u(b.merged.load()), b.maxPending.load(), pending, raw_.lastReason.load());
            out += buf;
        }
        if (format_ == Format::RawMove) {
            const RawSnapshot r = rawStats();
            std::snprintf(buf, sizeof(buf),
                "\nraw 0x69: отправлено %llu | принято в FIFO %llu | отдано в USB-endpoint %llu | отказ %llu"
                " | немедл. ошибок %llu (нет места %llu) | потеряно запросов %llu (дельта в перенос)"
                " | квитанций %llu, повторов %llu, потерянных ответов %llu, опросов %llu, неоднозначных %llu"
                " | поздних ответов %llu | окно %u, max живых %u, живых %u | ход квитанции %llu, ждали окно %llu"
                " | перенос (%lld,%lld) | посл. причина %u",
                u(r.submitted), u(r.accepted), u(r.endpointOk), u(r.rejected), u(r.immediate), u(r.noCapacity),
                u(r.lostRequests), u(r.ackSent), u(r.ackRetry), u(r.ackReplyLost), u(r.queries), u(r.ambiguous),
                u(r.lateReplies), r.window, r.maxLive, r.live, u(r.ackTurns), u(r.windowFull),
                static_cast<long long>(r.carryX), static_cast<long long>(r.carryY), r.lastReason);
            out += buf;
        }
        if (buttonsOk_.load()) {
            std::snprintf(buf, sizeof(buf), "\nкнопки %02X: событий 0x53 %llu, переполнений %llu, опросов 0x55 %llu, найдено опросом %llu%s",
                buttons(), u(buttonEvents_.load()), u(buttonOverflows_.load()), u(buttonPolls_.load()),
                u(buttonMissed_.load()), buttonEventsMissing() ? " | СОБЫТИЯ НЕ ДОХОДЯТ (брандмауэр?)" : "");
            out += buf;
        }
        return out;
    }

    void resetStats() override {
        stats.reset();
        raw_.reset();
        batch_.reset();
        net_.reset();
        for (auto& h : rttHist_) h = 0;
    }

private:
    static constexpr uint8_t kOpDevice = 0x02;
    static constexpr uint8_t kOpFirmware = 0x04;
    static constexpr uint8_t kOpMove = 0x18;
    static constexpr uint8_t kOpInterp = 0x1F;
    static constexpr uint8_t kOpMoveNow = 0x67;
    static constexpr uint8_t kOpRawMove = 0x69;
    static constexpr uint8_t kOpRawAck = 0x6A;
    static constexpr uint8_t kOpKmText = 0x6B;  // 'k' + текст без первой буквы
    static constexpr uint8_t kOpBatch = 0x6C;   // V4.097+: пакет Raw-записей + квитанция
    static constexpr uint8_t kOpInputStream = 0x52;  // GET kind -> enabled; SET kind enabled (без ответа)
    static constexpr uint8_t kOpInputChange = 0x53;  // событие: kind:u8 id:u8 value:u8 (FF FF - переполнение)
    static constexpr uint8_t kOpPhysButtons = 0x55;  // V4.036+: GET -> mask:u8 физических кнопок

    static unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }
    static uint32_t le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
               static_cast<uint32_t>(p[3]) << 24;
    }

    // ---- отправка ---------------------------------------------------------------
    enum class Kind : uint8_t { None, Sync, Probe, Fire, RawMove, RawAck, RawQuery, Batch, BatchMeta };
    struct Sent {
        uint64_t seq = 0;
        int64_t t = 0;
        uint32_t id = 0;
        Kind kind = Kind::None;
        bool expect = false;
        bool replied = false;
        float rttMs = 0.0f;
    };
    static constexpr size_t kSentSize = 4096;
    static constexpr uint64_t kSentMask = kSentSize - 1;

    // Под sendMutex_. Ведро токенов ёмкостью burst_ датаграмм, пополнение 1 / gapUs:
    // излишек копится, и изредка разрешается интервал 1 мс (плата держит одну датаграмму
    // в буфере, пока обрабатывает предыдущую), в среднем темп = 1 / gapUs.
    bool pacerReady(int64_t now) {
        if (lastRefill_ == 0) {
            lastRefill_ = now;
            tokens_ = 1.0;
        }
        tokens_ = std::min(burst_, tokens_ + static_cast<double>(now - lastRefill_) * tokensPerTick_);
        lastRefill_ = now;
        return tokens_ >= 1.0;
    }
    // Под sendMutex_. Можно ли отправить следующую датаграмму сейчас.
    // Reply: предыдущая датаграмма с ответом уже получила его (или ждём дольше kReplyWaitUs).
    bool paceReady(int64_t now) {
        if (!replyPaced()) return pacerReady(now);
        return awaitSeq_.load(std::memory_order_acquire) == 0 || now - awaitT_ >= Clock::fromUs(kReplyWaitUs);
    }
    void waitPaceLocked() {
        while (!paceReady(Clock::now())) YieldProcessor();
    }

    // Под sendMutex_. Датаграмма = обёртка + один кадр.
    bool sendLocked(uint8_t op, const uint8_t* payload, uint16_t len, Kind kind, uint32_t id) {
        std::array<uint8_t, 256> d{};
        size_t n = 0;
        const uint64_t seq = ++seq_;
        if (wire_ == Wire::Raw) {
            d[n++] = 0x55;
            for (int i = 0; i < 8; ++i) d[n++] = static_cast<uint8_t>(seq >> (8 * i));
        }
        d[n++] = 0xDE;
        d[n++] = 0xAD;
        d[n++] = static_cast<uint8_t>(len & 0xFF);
        d[n++] = static_cast<uint8_t>(len >> 8);
        d[n++] = op;
        if (len) std::memcpy(d.data() + n, payload, len);
        n += len;
        const bool expect = kind != Kind::Fire;
        const int64_t now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(sentMutex_);
            sent_[seq & kSentMask] = Sent{seq, now, id, kind, expect, false, 0.0f};
        }
        // Датаграмма с ответом открывает следующую (Pace::Reply). Ставим до send(),
        // чтобы ответ не мог обогнать запись.
        const uint64_t prevAwait = awaitSeq_.load(std::memory_order_acquire);
        const int64_t prevAwaitT = awaitT_;
        if (expect) {
            awaitT_ = now;
            awaitSeq_.store(seq, std::memory_order_release);
        }
        const int rc = ::sendto(sock_, reinterpret_cast<const char*>(d.data()), static_cast<int>(n), 0,
            reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
        if (rc != static_cast<int>(n)) {
            const int e = WSAGetLastError();
            if (expect) {
                uint64_t mine = seq;
                if (awaitSeq_.compare_exchange_strong(mine, prevAwait)) awaitT_ = prevAwaitT;
            }
            std::lock_guard<std::mutex> lock(sentMutex_);
            sent_[seq & kSentMask].seq = 0;
            if (e == WSAEWOULDBLOCK) {
                net_.sendBusy.fetch_add(1, std::memory_order_relaxed);
            } else {
                failed_ = true;
                stats.errors.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        tokens_ -= 1.0;
        // Ушли, хотя предыдущая ещё ждала ответа: разрешил тайм-аут kReplyWaitUs.
        if (expect && prevAwait != 0 && kind != Kind::Probe && replyPaced()) {
            net_.replyTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
        net_.datagrams.fetch_add(1, std::memory_order_relaxed);
        if (expect) net_.expected.fetch_add(1, std::memory_order_relaxed);
        int64_t zero = 0;
        net_.firstSend.compare_exchange_strong(zero, now, std::memory_order_relaxed);
        net_.lastSend.store(now, std::memory_order_relaxed);
        return true;
    }

    // ---- Raw movement 0x69/0x6A -------------------------------------------------
    // 0x69: u32 id, i16 x, i16 y, u16 timeout_uf -> state:u8 reason:u8 id:u32
    //       (1 = в FIFO, ещё НЕ отправлено; 255 = немедленная ошибка, reason 3 = нет места).
    // 0x6A: u32 id, u8 consume -> тот же ответ (2 = отдано в USB-endpoint, 3 = отказ,
    //       255/6 = ещё в очереди, 255/5 = нет такого id / уже подтверждён).
    enum Phase : uint8_t { kFree = 0, kSubmitted = 1, kQueued = 2, kAckSent = 3, kQuerying = 4 };
    struct RawRecord {
        uint32_t id = 0;
        int32_t dx = 0;
        int32_t dy = 0;
        int64_t tSubmit = 0;
        int64_t tAck = 0;     // последняя квитанция/опрос
        uint8_t phase = kFree;
        bool known = false;   // плата подтвердила, что запись у неё есть
        bool ackNow = false;  // опрос показал окончательное состояние - квитировать сразу
    };
    struct RawStats {
        std::atomic<uint64_t> submitted{0}, accepted{0}, endpointOk{0}, rejected{0}, immediate{0}, noCapacity{0};
        std::atomic<uint64_t> ackSent{0}, ackRetry{0}, ackReplyLost{0}, queries{0}, lostRequests{0}, ambiguous{0};
        std::atomic<uint64_t> lateReplies{0}, ackTurns{0}, windowFull{0};
        std::atomic<uint32_t> lastReason{0}, maxLive{0};
        void reset() {
            submitted = accepted = endpointOk = rejected = immediate = noCapacity = 0;
            ackSent = ackRetry = ackReplyLost = queries = lostRequests = ambiguous = 0;
            lateReplies = ackTurns = windowFull = 0;
            lastReason = maxLive = 0;
        }
    };
    struct NetStats {
        std::atomic<uint64_t> datagrams{0}, expected{0}, replies{0}, unmatched{0}, junk{0}, pacerWaits{0}, sendBusy{0};
        std::atomic<uint64_t> rxSends{0}, replyTimeouts{0};
        std::atomic<int64_t> firstSend{0}, lastSend{0};
        std::atomic<double> rttMaxUs{0.0};
        void reset() {
            datagrams = expected = replies = unmatched = junk = pacerWaits = sendBusy = 0;
            rxSends = replyTimeouts = 0;
            firstSend = lastSend = 0;
            rttMaxUs = 0.0;
        }
    };

    static constexpr size_t kRawSlots = 64;             // индекс записи = id % 64 (> ёмкости платы 16)
    static constexpr uint32_t kRawMaxLive = 12;         // своё окно, запас до 16 записей платы
    static constexpr double kRawAckAfterUs = 3000.0;    // квитировать не раньше 3 мс после отправки
    static constexpr double kRawAckRetryUs = 2000.0;    // повтор после 255/6 не чаще раза в 2 мс
    static constexpr double kLostUs = 8000.0;           // нет ответа 8 мс - датаграмма потеряна (RTT p99 ~2 мс)

    // Под rawMutex_: самая старая запись, которой нужна квитанция или опрос.
    RawRecord* dueRecordLocked(int64_t now, bool& urgent) {
        const int64_t ackAfter = Clock::fromUs(kRawAckAfterUs);
        const int64_t retry = Clock::fromUs(kRawAckRetryUs);
        const int64_t lost = Clock::fromUs(kLostUs);
        RawRecord* best = nullptr;
        urgent = false;
        for (auto& r : rawRec_) {
            bool due = false;
            bool isUrgent = false;
            switch (r.phase) {
            case kSubmitted: due = isUrgent = now - r.tSubmit >= lost; break;
            case kQuerying: due = isUrgent = now - r.tAck >= lost; break;
            case kQueued:
                due = r.ackNow || (now - r.tSubmit >= ackAfter && (r.tAck == 0 || now - r.tAck >= retry));
                break;
            case kAckSent: due = isUrgent = now - r.tAck >= lost; break;
            default: break;
            }
            if (!due) continue;
            if (!best || r.id < best->id) best = &r;
            urgent = urgent || isUrgent;
        }
        return best;
    }

    // flushOnly: не новая дельта, только квитанции и перенос.
    bool moveRaw(int16_t dx, int16_t dy, bool flushOnly) {
        if (!rawMode_.load()) {
            failed_ = true;
            return false;
        }
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        return moveRawLocked(dx, dy, flushOnly, nullptr);
    }

    // Pace::Reply, поток тайминга: дельта всегда уходит в перенос транспорта (путь у нас),
    // отправку делает pumpLocked() - здесь, если ответ на датаграмму в полёте уже пришёл,
    // иначе поток приёма сразу по ответу. Если поток приёма как раз отправляет, не ждём его.
    bool moveRawReply(int16_t dx, int16_t dy) {
        if (!rawMode_.load()) {
            failed_ = true;
            return false;
        }
        if (dx != 0 || dy != 0) {
            std::lock_guard<std::mutex> lock(rawMutex_);
            rawCarryX_ += dx;
            rawCarryY_ += dy;
        }
        std::unique_lock<std::mutex> sendLock(sendMutex_, std::try_to_lock);
        bool sent = false;
        if (sendLock.owns_lock()) sent = pumpLocked(false);
        if (!sent && (dx != 0 || dy != 0)) net_.pacerWaits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Под sendMutex_. Pace::Reply: одна датаграмма, если можно - новая дельта из переноса
    // или квитанция/опрос. true - датаграмма ушла.
    bool pumpLocked(bool fromRx) {
        if (!rawMode_.load() || failed_.load()) return false;
        bool sent = false;
        (void)moveRawLocked(0, 0, true, &sent);
        if (sent && fromRx) net_.rxSends.fetch_add(1, std::memory_order_relaxed);
        return sent;
    }

    // Под sendMutex_. sent - ушла ли датаграмма (дельта, квитанция или опрос).
    bool moveRawLocked(int16_t dx, int16_t dy, bool flushOnly, bool* sent) {
        if (sent) *sent = false;
        const int64_t now = Clock::now();
        if (!paceReady(now)) {
            if (!flushOnly) net_.pacerWaits.fetch_add(1, std::memory_order_relaxed);
            return flushOnly;
        }
        uint8_t op = 0;
        uint8_t payload[10]{};
        uint16_t plen = 0;
        Kind kind = Kind::None;
        uint32_t id = 0;
        bool submitted = false;
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            const bool carry = rawCarryX_ != 0 || rawCarryY_ != 0;
            const bool slotFree = rawRec_[rawNextId_ % kRawSlots].phase == kFree;
            const bool canMove = (!flushOnly || carry) && rawLive_ < rawWindow_ && slotFree;
            bool urgent = false;
            RawRecord* due = dueRecordLocked(now, urgent);
            // Новая дельта - в первую очередь; квитанция - когда окно почти
            // заполнено или датаграмма потерялась (опрос/повтор).
            const bool ackFirst = due && (!canMove || urgent || rawLive_ + 2 >= rawWindow_);
            if (canMove && !ackFirst) {
                const int64_t x = static_cast<int64_t>(dx) + rawCarryX_;
                const int64_t y = static_cast<int64_t>(dy) + rawCarryY_;
                const int64_t cx = std::clamp<int64_t>(x, -32767, 32767);
                const int64_t cy = std::clamp<int64_t>(y, -32767, 32767);
                rawCarryX_ = x - cx;
                rawCarryY_ = y - cy;
                id = rawNextId_++;
                rawRec_[id % kRawSlots] =
                    RawRecord{id, static_cast<int32_t>(cx), static_cast<int32_t>(cy), now, 0, kSubmitted, false, false};
                ++rawLive_;
                if (rawLive_ > raw_.maxLive.load(std::memory_order_relaxed)) {
                    raw_.maxLive.store(rawLive_, std::memory_order_relaxed);
                }
                const auto ux = static_cast<uint16_t>(cx);
                const auto uy = static_cast<uint16_t>(cy);
                // timeout_uf = 0: значение платы по умолчанию (8000 uf = 1 с).
                const uint8_t p[10] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                    static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(ux & 0xFF),
                    static_cast<uint8_t>(ux >> 8), static_cast<uint8_t>(uy & 0xFF), static_cast<uint8_t>(uy >> 8), 0, 0};
                std::memcpy(payload, p, sizeof(p));
                plen = 10;
                op = kOpRawMove;
                kind = Kind::RawMove;
                submitted = true;
            } else if (due) {
                RawRecord& r = *due;
                id = r.id;
                bool consume = true;
                if (r.phase == kSubmitted || r.phase == kQuerying) {
                    // Нет ответа на 0x69: сначала узнать, дошёл ли запрос (consume=0).
                    consume = false;
                    r.phase = kQuerying;
                    raw_.queries.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (r.phase == kAckSent) {
                        raw_.ackReplyLost.fetch_add(1, std::memory_order_relaxed);
                        raw_.ackRetry.fetch_add(1, std::memory_order_relaxed);
                    }
                    r.phase = kAckSent;
                    r.ackNow = false;
                    raw_.ackSent.fetch_add(1, std::memory_order_relaxed);
                }
                r.tAck = now;
                const uint8_t p[5] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                    static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(consume ? 1 : 0)};
                std::memcpy(payload, p, sizeof(p));
                plen = 5;
                op = kOpRawAck;
                kind = consume ? Kind::RawAck : Kind::RawQuery;
                if (!flushOnly || carry) raw_.ackTurns.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (!flushOnly || carry) raw_.windowFull.fetch_add(1, std::memory_order_relaxed);
                return flushOnly;
            }
        }
        const int64_t t0 = Clock::now();
        if (!sendLocked(op, payload, plen, kind, id)) {
            if (submitted) {
                // Датаграмма не ушла: вернуть дельту в перенос, освободить окно.
                std::lock_guard<std::mutex> lock(rawMutex_);
                RawRecord& r = rawRec_[id % kRawSlots];
                if (r.id == id && r.phase == kSubmitted) {
                    rawCarryX_ += r.dx;
                    rawCarryY_ += r.dy;
                    r.phase = kFree;
                    --rawLive_;
                }
            }
            return false;
        }
        if (sent) *sent = true;
        if (submitted) {
            stats.noteCall(Clock::toUs(Clock::now() - t0));
            stats.writes.fetch_add(1, std::memory_order_relaxed);
            raw_.submitted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return flushOnly;
    }

    // Поток приёма: ответ на конкретный запрос (по nonce).
    void onRawReply(Kind kind, uint8_t op, uint8_t state, uint8_t reason, uint32_t id) {
        std::lock_guard<std::mutex> lock(rawMutex_);
        RawRecord& r = rawRec_[id % kRawSlots];
        if (r.id != id || r.phase == kFree) {
            raw_.lateReplies.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto release = [&](bool returnDelta) {
            if (returnDelta) {
                rawCarryX_ += r.dx;
                rawCarryY_ += r.dy;
            }
            r.phase = kFree;
            --rawLive_;
        };
        auto noteReason = [&] { raw_.lastReason.store(reason, std::memory_order_relaxed); };

        if (kind == Kind::RawMove && op == kOpRawMove) {
            if (r.phase != kSubmitted && r.phase != kQuerying) {
                raw_.lateReplies.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state == 1) {
                raw_.accepted.fetch_add(1, std::memory_order_relaxed);
                r.known = true;
                r.phase = kQueued;
            } else if (state == 2) {
                raw_.accepted.fetch_add(1, std::memory_order_relaxed);
                r.known = true;
                r.phase = kQueued;  // окончательное, но ещё не подтверждено - квитировать
                r.ackNow = true;
            } else if (state == 3) {
                r.known = true;
                r.phase = kQueued;
                r.ackNow = true;
            } else {  // 255: запрос не принят
                raw_.immediate.fetch_add(1, std::memory_order_relaxed);
                noteReason();
                if (reason == 3) raw_.noCapacity.fetch_add(1, std::memory_order_relaxed);
                if (reason == 2) rawNextId_ += 1000;  // ID занял кто-то ещё - уйти вперёд
                release(true);
            }
            return;
        }
        if (kind == Kind::RawQuery) {
            if (r.phase != kQuerying) {
                raw_.lateReplies.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state == 1 || state == 2 || state == 3 || (state == 255 && reason == 6)) {
                // Запись у платы есть: потерялся только ответ на 0x69.
                if (!r.known) raw_.accepted.fetch_add(1, std::memory_order_relaxed);
                r.known = true;
                r.phase = kQueued;
                r.tAck = 0;
                r.ackNow = state == 2 || state == 3;
            } else if (state == 255 && reason == 5 && !r.known) {
                // Запрос 0x69 не дошёл до платы: движения не было, дельта - в перенос.
                raw_.lostRequests.fetch_add(1, std::memory_order_relaxed);
                release(true);
            } else {
                noteReason();
                r.phase = kQuerying;  // повторим опрос по таймауту
            }
            return;
        }
        if (kind == Kind::RawAck) {
            if (r.phase != kAckSent) {
                raw_.lateReplies.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state == 2) {
                raw_.endpointOk.fetch_add(1, std::memory_order_relaxed);
                release(false);
            } else if (state == 3) {
                raw_.rejected.fetch_add(1, std::memory_order_relaxed);
                noteReason();
                release(true);
            } else if (state == 255 && reason == 6) {
                raw_.ackRetry.fetch_add(1, std::memory_order_relaxed);
                r.phase = kQueued;  // ещё в очереди - повторить позже (tAck уже стоит)
            } else if (state == 255 && reason == 5 && r.known) {
                // Подтверждена раньше (ответ на ту квитанцию потерялся).
                raw_.ambiguous.fetch_add(1, std::memory_order_relaxed);
                raw_.endpointOk.fetch_add(1, std::memory_order_relaxed);
                release(false);
            } else {
                raw_.immediate.fetch_add(1, std::memory_order_relaxed);
                noteReason();
                r.phase = kQueued;
            }
        }
    }

    // ---- Batch 0x6C (V4.097+) ------------------------------------------------------
    // request = 01 | flags | N | 00 | X u32 | N x [id u32 | x i16 | y i16 | dt u16] (8 + 10N байт)
    // receipt = [0]=1 [1]=0 все / 1 частично / 255 ошибка конверта [2]=N [3]=принято
    //           [4]=первый отказ [5]=причина [6]=освобождено [7]=свободно [8]=K строк (<=7)
    //           [9]=more [10]=в очереди [11]=0 [12..15]=last_id, затем K x (id u32 | state | reason).
    // flags bit0: освободить терминальные записи с id <= X. X не больше старшего
    // терминального id, уже сообщённого квитанцией, иначе 255/1 без последствий.
    static constexpr size_t kBatchMax = 7;           // квитанция освобождает не больше 7 строк
    static constexpr double kBatchLostUs = 30000.0;  // нет квитанции 30 мс - спросить last_accepted_id

    struct BatchRec {
        int32_t dx = 0;
        int32_t dy = 0;
    };
    struct BatchFlight {
        bool active = false;
        uint32_t firstId = 0;
        uint8_t n = 0;
        std::array<BatchRec, kBatchMax> recs{};
    };
    struct BatchStats {
        std::atomic<uint64_t> packets{0}, records{0}, admitted{0}, receipts{0}, partial{0}, envelope{0}, xRejected{0};
        std::atomic<uint64_t> badReply{0}, lostReceipts{0}, metaQueries{0}, freed{0}, moreSet{0};
        std::atomic<uint64_t> termSubmitted{0}, termRejected{0}, merged{0};
        std::atomic<uint32_t> minFree{256}, maxPending{0};
        void reset() {
            packets = records = admitted = receipts = partial = envelope = xRejected = 0;
            badReply = lostReceipts = metaQueries = freed = moreSet = 0;
            termSubmitted = termRejected = merged = 0;
            minFree = 256;
            maxPending = 0;
        }
    };

    // Поток тайминга: дельта тика = одна запись в очередь, отправка - если пакета в полёте нет.
    bool moveBatch(int16_t dx, int16_t dy) {
        if (!rawMode_.load()) {
            failed_ = true;
            return false;
        }
        if (dx != 0 || dy != 0) {
            std::lock_guard<std::mutex> lock(rawMutex_);
            batchPend_.push_back(BatchRec{dx, dy});
            const auto n = static_cast<uint32_t>(batchPend_.size());
            if (n > batch_.maxPending.load(std::memory_order_relaxed)) batch_.maxPending.store(n, std::memory_order_relaxed);
        }
        std::unique_lock<std::mutex> sendLock(sendMutex_, std::try_to_lock);
        if (sendLock.owns_lock()) (void)pumpBatchLocked(false);
        return true;
    }

    // Под rawMutex_: записи пакета с индекса from вернуть в начало очереди (порядок сохраняется).
    void returnBatchLocked(const BatchFlight& f, uint8_t from) {
        for (int i = static_cast<int>(f.n) - 1; i >= static_cast<int>(from); --i) batchPend_.push_front(f.recs[i]);
    }

    // Под sendMutex_. Следующий пакет из очереди, если предыдущий получил квитанцию.
    // Без квитанции kBatchLostUs - опрос 0x6A(id=0): last_accepted_id покажет, что принято.
    bool pumpBatchLocked(bool fromRx) {
        if (!rawMode_.load() || failed_.load()) return false;
        const int64_t now = Clock::now();
        if (batchAwait_.load(std::memory_order_acquire) != 0) {
            if (now - batchAwaitT_.load(std::memory_order_relaxed) < Clock::fromUs(kBatchLostUs)) return false;
            if (!batchMetaPending_.load()) batch_.lostReceipts.fetch_add(1, std::memory_order_relaxed);
            const uint8_t zero[5] = {0, 0, 0, 0, 0};
            batchMetaPending_ = true;
            batchAwaitT_.store(now, std::memory_order_relaxed);
            batchAwait_.store(seq_ + 1, std::memory_order_release);  // sendLocked() возьмёт этот nonce
            if (!sendLocked(kOpRawAck, zero, 5, Kind::BatchMeta, 0)) {
                batchAwait_.store(0, std::memory_order_release);
                return false;
            }
            batch_.metaQueries.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        BatchFlight f;
        bool haveX = false;
        uint32_t x = 0;
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            if (batchPend_.empty()) return false;
            const size_t k = batchPend_.size();
            f.active = true;
            f.firstId = rawNextId_;
            if (k <= kBatchMax) {
                f.n = static_cast<uint8_t>(k);
                for (size_t i = 0; i < k; ++i) f.recs[i] = batchPend_[i];
            } else {
                // Накопилось больше, чем принимает пакет: склеить по порядку в 7 записей.
                f.n = static_cast<uint8_t>(kBatchMax);
                for (size_t i = 0; i < k; ++i) {
                    BatchRec& r = f.recs[i * kBatchMax / k];
                    r.dx += batchPend_[i].dx;
                    r.dy += batchPend_[i].dy;
                }
                batch_.merged.fetch_add(k - kBatchMax, std::memory_order_relaxed);
            }
            batchPend_.clear();
            // Поле записи i16: излишек склейки остаётся в очереди.
            for (uint8_t i = 0; i < f.n; ++i) {
                BatchRec& r = f.recs[i];
                const int32_t cx = std::clamp<int32_t>(r.dx, -32767, 32767);
                const int32_t cy = std::clamp<int32_t>(r.dy, -32767, 32767);
                if (cx != r.dx || cy != r.dy) batchPend_.push_back(BatchRec{r.dx - cx, r.dy - cy});
                r.dx = cx;
                r.dy = cy;
            }
            haveX = batchHaveX_;
            x = batchMaxTerm_;
            batchFlight_ = f;
        }
        std::array<uint8_t, 8 + 10 * kBatchMax> pl{};
        pl[0] = 0x01;
        pl[1] = haveX ? 0x01 : 0x00;
        pl[2] = f.n;
        pl[3] = 0x00;
        put32(pl.data() + 4, haveX ? x : 0);
        for (uint8_t i = 0; i < f.n; ++i) {
            uint8_t* r = pl.data() + 8 + 10 * i;
            put32(r, f.firstId + i);
            put16(r + 4, f.recs[i].dx);
            put16(r + 6, f.recs[i].dy);
            put16(r + 8, 0);  // dt
        }
        batchMetaPending_ = false;
        batchAwaitT_.store(now, std::memory_order_relaxed);
        batchAwait_.store(seq_ + 1, std::memory_order_release);
        const int64_t t0 = Clock::now();
        if (!sendLocked(kOpBatch, pl.data(), static_cast<uint16_t>(8 + 10 * f.n), Kind::Batch, f.firstId)) {
            batchAwait_.store(0, std::memory_order_release);
            std::lock_guard<std::mutex> lock(rawMutex_);
            returnBatchLocked(f, 0);
            batchFlight_.active = false;
            return false;
        }
        stats.noteCall(Clock::toUs(Clock::now() - t0));
        stats.writes.fetch_add(f.n, std::memory_order_relaxed);
        batch_.packets.fetch_add(1, std::memory_order_relaxed);
        batch_.records.fetch_add(f.n, std::memory_order_relaxed);
        if (fromRx) net_.rxSends.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Поток приёма: квитанция на пакет в полёте.
    void onBatchReceipt(const uint8_t* p, size_t plen) {
        std::lock_guard<std::mutex> lock(rawMutex_);
        const BatchFlight f = batchFlight_;
        if (!f.active) {
            raw_.lateReplies.fetch_add(1, std::memory_order_relaxed);  // пакет уже разобран опросом
            return;
        }
        batchFlight_.active = false;
        batchMetaPending_ = false;
        batch_.receipts.fetch_add(1, std::memory_order_relaxed);
        if (plen < 16 || p[0] != 1) {
            // Не квитанция 0x6C (прошивка без batch?): записи назад, формат дальше не работает.
            batch_.badReply.fetch_add(1, std::memory_order_relaxed);
            returnBatchLocked(f, 0);
            failed_ = true;
            batchAwait_.store(0, std::memory_order_release);
            return;
        }
        const uint8_t result = p[1];
        const uint8_t adm = std::min(p[3], f.n);
        const uint8_t reason = p[5];
        const uint8_t k = p[8];
        const uint32_t lastId = le32(p + 12);
        if (result == 255) {
            batch_.envelope.fetch_add(1, std::memory_order_relaxed);
            raw_.lastReason.store(reason, std::memory_order_relaxed);
            if (reason == 1) {
                batch_.xRejected.fetch_add(1, std::memory_order_relaxed);
                batchHaveX_ = false;
            }
            returnBatchLocked(f, 0);
        } else {
            if (result == 1) {
                batch_.partial.fetch_add(1, std::memory_order_relaxed);
                raw_.lastReason.store(reason, std::memory_order_relaxed);
            }
            batch_.admitted.fetch_add(adm, std::memory_order_relaxed);
            returnBatchLocked(f, adm);
            rawNextId_ = std::max(f.firstId + adm, adm ? lastId + 1 : f.firstId);
        }
        batch_.freed.fetch_add(p[6], std::memory_order_relaxed);
        if (p[7] < batch_.minFree.load(std::memory_order_relaxed)) batch_.minFree.store(p[7], std::memory_order_relaxed);
        if (p[9]) batch_.moreSet.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < k && 16 + 6 * i + 6 <= plen; ++i) {
            const uint8_t* row = p + 16 + 6 * i;
            const uint32_t id = le32(row);
            if (row[4] == 2) batch_.termSubmitted.fetch_add(1, std::memory_order_relaxed);
            else if (row[4] == 3) batch_.termRejected.fetch_add(1, std::memory_order_relaxed);
            if (id > batchMaxTerm_) batchMaxTerm_ = id;
            batchHaveX_ = true;
        }
        batchAwait_.store(0, std::memory_order_release);
    }

    // Поток приёма: ответ на опрос после потерянной квитанции.
    void onBatchMeta(uint32_t lastAccepted) {
        std::lock_guard<std::mutex> lock(rawMutex_);
        if (!batchMetaPending_.load()) return;  // квитанция всё-таки пришла раньше
        const BatchFlight f = batchFlight_;
        if (f.active) {
            uint8_t adm = 0;
            if (lastAccepted >= f.firstId) adm = static_cast<uint8_t>(std::min<uint32_t>(lastAccepted - f.firstId + 1, f.n));
            batch_.admitted.fetch_add(adm, std::memory_order_relaxed);
            returnBatchLocked(f, adm);
            rawNextId_ = std::max(f.firstId + adm, lastAccepted + 1);
            batchFlight_.active = false;
        }
        batchMetaPending_ = false;
        batchAwait_.store(0, std::memory_order_release);
    }

    // Конец прогона: дослать очередь, дождаться квитанции и освободить записи, чьи
    // терминальные строки пришли уже после последнего X.
    bool finishBatch(int timeoutMs) {
        const int64_t until = Clock::now() + Clock::fromUs(timeoutMs * 1000.0);
        bool done = false;
        while (Clock::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                done = batchPend_.empty() && !batchFlight_.active && batchAwait_.load() == 0;
            }
            if (done) break;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                (void)pumpBatchLocked(false);
            }
            Sleep(1);
        }
        Sleep(30);  // последние записи уходят в USB по одной на опрос мыши
        if (auto meta = queryRawMeta(); meta && meta->freeRecords < meta->capacity && meta->lastId > 0) {
            (void)drainLeftoverRecords(meta->lastId, meta->capacity - meta->freeRecords);
        }
        return done;
    }

    static void put16(uint8_t* p, int32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }
    static void put32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    // ---- кнопки -------------------------------------------------------------------------
    // Команда без ответа (SET 0x52): по темпу, затем переждать передачу на второй чип.
    bool sendFire(uint8_t op, const uint8_t* payload, uint16_t len) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        waitPaceLocked();
        const bool ok = sendLocked(op, payload, len, Kind::Fire, 0);
        Sleep(5);
        return ok;
    }

    // open(): включить поток событий мыши (запомнить, каким был), потом стартовая маска.
    // Поток начинается с "всё отпущено", поэтому маска читается после включения.
    void initButtons() {
        mask_.store(0);
        primed_.store(false);
        for (auto& c : presses_) c.store(0);
        buttonEvents_ = buttonOverflows_ = buttonPolls_ = buttonMissed_ = 0;
        const uint8_t kind[1] = {0x01};
        bool stream = false;
        if (auto was = query(kOpInputStream, kind, 1, 100, 3); was && was->size() == 1) {
            stream = true;
            if ((*was)[0] != 1) {
                const uint8_t on[2] = {0x01, 0x01};
                if (sendFire(kOpInputStream, on, 2)) streamRestore_ = (*was)[0];
            }
        }
        if (auto m = query(kOpPhysButtons, nullptr, 0, 100, 3); m && m->size() == 1) {
            mask_.store((*m)[0]);
            primed_.store(true);
            buttonsOk_ = true;
        }
        char buf[160];
        if (buttonsOk_) {
            std::snprintf(buf, sizeof(buf), "маска %02X, события 0x53 %s", mask_.load(),
                !stream ? "недоступны (нет ответа на 0x52)" : streamRestore_ ? "включены" : "уже были включены");
        } else {
            std::snprintf(buf, sizeof(buf), "0x55 не ответил (нужна V4.036+)");
        }
        buttonInfo_ = buf;
    }

    // Поток приёма: событие 0x53. Мышь: id = кнопка, value 0/1; FF FF - переполнение.
    void onInputChange(const uint8_t* p, size_t plen) {
        if (plen != 3 || p[0] != 1) return;  // не мышь (или колесо в тест-сборках, plen 4)
        if (p[1] == 0xFF && p[2] == 0xFF) {
            buttonOverflows_.fetch_add(1, std::memory_order_relaxed);  // поправит pollButtons()
            return;
        }
        if (p[1] >= 8 || p[2] > 1) return;
        buttonEvents_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t bit = static_cast<uint8_t>(1u << p[1]);
        const uint8_t old = mask_.load(std::memory_order_acquire);
        applyMask(p[2] ? static_cast<uint8_t>(old | bit) : static_cast<uint8_t>(old & ~bit), false);
    }

    // Новая маска; переходы 0 -> 1 - нажатия (после стартовой маски). fromPoll: изменение
    // нашёл опрос, а не событие - признак потерянных событий.
    void applyMask(uint8_t mask, bool fromPoll) {
        const uint8_t old = mask_.exchange(mask, std::memory_order_acq_rel);
        if (!primed_.load(std::memory_order_acquire) || old == mask) return;
        if (fromPoll) buttonMissed_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t pressed = static_cast<uint8_t>(mask & ~old);
        for (int i = 0; i < kButtonCount; ++i) {
            if (pressed & (1u << i)) presses_[i].fetch_add(1, std::memory_order_acq_rel);
        }
    }

    // Подтвердить (consume=1) окончательные записи среди последних id: прерванные
    // сессии оставляют их на плате до перезагрузки. По одной датаграмме, до 1024 id.
    // ВНИМАНИЕ: подтверждает и записи другой программы, если она тоже шлёт Raw.
    uint32_t drainLeftoverRecords(uint32_t lastId, uint32_t want) {
        uint32_t consumed = 0;
        for (uint32_t id = lastId, scanned = 0; id >= 1 && scanned < 1024 && consumed < want; --id, ++scanned) {
            const uint8_t p[5] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id >> 16),
                static_cast<uint8_t>(id >> 24), 1};
            auto r = query(kOpRawAck, p, 5, 30, 2);
            if (r && r->size() == 6 && ((*r)[0] == 2 || (*r)[0] == 3)) ++consumed;
        }
        return consumed;
    }

    // ---- приём --------------------------------------------------------------------
    void rxLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        std::array<uint8_t, 2048> buf{};
        while (!stopRx_.load()) {
            const bool batch = format_.load(std::memory_order_relaxed) == Format::Batch;
            const bool reply = replyPaced() || batch;
            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock_, &set);
            // Pace::Reply: просыпаться и без ответов - досылать созревшие квитанции
            // и переходить дальше после тайм-аута ответа.
            timeval tv{0, reply ? 1000 : 50000};
            const int ready = ::select(0, &set, nullptr, nullptr, &tv);
            if (ready > 0) {
                for (;;) {
                    sockaddr_in from{};
                    int fromLen = sizeof(from);
                    const int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (n <= 0) break;  // WSAEWOULDBLOCK - всё прочитано
                    // Любой порт платы (ответы с 8080, события - с порта, равного нашему), чужие IP - мимо.
                    if (from.sin_addr.s_addr != remote_.sin_addr.s_addr) continue;
                    onDatagram(buf.data(), static_cast<size_t>(n), Clock::now());
                }
            }
            if (reply && rawMode_.load(std::memory_order_relaxed)) {
                // Сразу по ответу: следующая датаграмма попадёт в "слот" платы до конца цикла.
                // Отправку держит другой поток - не ждём (приём не должен вставать).
                std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
                if (lock.owns_lock()) (void)(batch ? pumpBatchLocked(ready > 0) : pumpLocked(ready > 0));
            }
        }
    }

    void onDatagram(const uint8_t* d, size_t n, int64_t now) {
        size_t off = 0;
        uint64_t seq = 0;
        const bool hasSeq = wire_ == Wire::Raw;
        if (hasSeq) {
            if (n < 14 || d[0] != 0x55) {
                net_.junk.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 0; i < 8; ++i) seq |= static_cast<uint64_t>(d[1 + i]) << (8 * i);
            off = 9;
        }
        const uint8_t* f = d + off;
        const size_t fn = n - off;
        if (fn < 5 || f[0] != 0xDE || f[1] != 0xAD) {
            net_.junk.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const size_t plen = static_cast<size_t>(f[2]) | static_cast<size_t>(f[3]) << 8;
        if (5 + plen > fn) {
            net_.junk.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint8_t op = f[4];
        const uint8_t* p = f + 5;
        // Событие кнопок несёт nonce одной из наших прошлых команд: разбирать по опкоду ДО
        // сопоставления по nonce, иначе оно "ответит" на команду в полёте.
        if (op == kOpInputChange) {
            onInputChange(p, plen);
            return;
        }

        Sent info{};
        bool matched = false;
        if (hasSeq) {
            std::lock_guard<std::mutex> lock(sentMutex_);
            Sent& s = sent_[seq & kSentMask];
            if (s.seq == seq && s.expect && !s.replied) {
                s.replied = true;
                uint64_t await = seq;  // ответ на датаграмму в полёте открывает следующую
                awaitSeq_.compare_exchange_strong(await, 0, std::memory_order_acq_rel);
                const double us = Clock::toUs(now - s.t);
                s.rttMs = static_cast<float>(us / 1000.0);
                info = s;
                matched = true;
                const size_t bin = std::min(rttHist_.size() - 1, static_cast<size_t>(us / 1000.0 / kRttBinMs));
                rttHist_[bin].fetch_add(1, std::memory_order_relaxed);
                if (us > net_.rttMaxUs.load(std::memory_order_relaxed)) net_.rttMaxUs.store(us, std::memory_order_relaxed);
            }
        }
        if (matched) {
            net_.replies.fetch_add(1, std::memory_order_relaxed);
        } else {
            net_.unmatched.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(syncMutex_);
            if (syncWaiting_ && !syncDone_ && (hasSeq ? seq == syncSeq_ : op == syncOp_)) {
                syncReply_.assign(p, p + plen);
                syncReplyOp_ = op;
                syncDone_ = true;
                syncCv_.notify_all();
            }
        }

        if (matched && info.kind == Kind::Batch) {
            onBatchReceipt(p, plen);
            return;
        }
        if (matched && info.kind == Kind::BatchMeta) {
            if (plen == 8) onBatchMeta(le32(p + 2));
            return;
        }
        if ((op == kOpRawMove || op == kOpRawAck) && plen == 6 && rawMode_.load(std::memory_order_relaxed)) {
            Kind kind = matched ? info.kind : Kind::None;
            if (!hasSeq) {
                // Без nonce (HOST): тип ответа выводим из опкода и фазы записи.
                std::lock_guard<std::mutex> lock(rawMutex_);
                const RawRecord& r = rawRec_[le32(p + 2) % kRawSlots];
                kind = op == kOpRawMove ? Kind::RawMove : r.phase == kQuerying ? Kind::RawQuery : Kind::RawAck;
            }
            if (kind == Kind::RawMove || kind == Kind::RawAck || kind == Kind::RawQuery) {
                onRawReply(kind, op, p[0], p[1], le32(p + 2));
            }
        }
    }

    // ---- состояние -------------------------------------------------------------------
    SOCKET sock_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
    Wire wire_ = Wire::Raw;
    std::atomic<Format> format_{Format::RawMove};  // читает и поток приёма
    std::atomic<Pace> pace_{Pace::Reply};
    std::string endpoint_;
    uint8_t kinds_ = 0;
    uint32_t firmware_ = 0;
    std::atomic<bool> failed_{false};

    std::mutex sendMutex_;       // отправка, темп, seq_
    uint64_t seq_ = 0;
    double gapUs_ = kDefaultGapUs;
    double tokensPerTick_ = 1.0 / static_cast<double>(Clock::fromUs(kDefaultGapUs));
    double tokens_ = 1.0;
    double burst_ = kDefaultBurst;
    int64_t lastRefill_ = 0;
    std::atomic<uint64_t> awaitSeq_{0};  // Pace::Reply: датаграмма в полёте (0 - ответ пришёл)
    int64_t awaitT_ = 0;                 // когда она ушла (под sendMutex_)

    mutable std::mutex sentMutex_;
    std::array<Sent, kSentSize> sent_{};
    static constexpr double kRttBinMs = 0.1;
    std::array<std::atomic<uint64_t>, 300> rttHist_{};  // 0..30 мс по 0.1 мс
    NetStats net_;

    std::thread rx_;
    std::atomic<bool> stopRx_{false};

    std::mutex syncMutex_;
    std::condition_variable syncCv_;
    bool syncWaiting_ = false;
    bool syncDone_ = false;
    uint8_t syncOp_ = 0;
    uint8_t syncReplyOp_ = 0;
    uint64_t syncSeq_ = 0;
    std::vector<uint8_t> syncReply_;

    mutable std::mutex rawMutex_;
    std::array<RawRecord, kRawSlots> rawRec_{};
    uint32_t rawNextId_ = 1;
    uint32_t rawLive_ = 0;
    uint32_t rawWindow_ = kRawMaxLive;
    int64_t rawCarryX_ = 0;
    int64_t rawCarryY_ = 0;
    std::atomic<bool> rawMode_{false};
    RawStats raw_;

    // Batch 0x6C: очередь записей (по одной на тик), пакет в полёте, X для освобождения.
    std::deque<BatchRec> batchPend_;         // под rawMutex_
    BatchFlight batchFlight_;                // под rawMutex_
    uint32_t batchMaxTerm_ = 0;              // под rawMutex_: старший терминальный id из квитанций
    bool batchHaveX_ = false;                // под rawMutex_
    std::atomic<uint64_t> batchAwait_{0};    // nonce пакета/опроса в полёте (0 - можно слать)
    std::atomic<int64_t> batchAwaitT_{0};
    std::atomic<bool> batchMetaPending_{false};
    BatchStats batch_;

    // Кнопки мыши: маску пишут поток приёма и pollButtons(), читают все.
    sockaddr_in remote_{};                  // адрес платы (сокет без connect())
    std::atomic<bool> buttonsOk_{false};
    std::atomic<bool> primed_{false};       // стартовая маска получена (раньше - не нажатия)
    std::optional<uint8_t> streamRestore_;  // INPUT_STREAM mouse до open(), если меняли
    std::string buttonInfo_;
    std::atomic<uint8_t> mask_{0};
    std::array<std::atomic<uint32_t>, kButtonCount> presses_{};
    std::atomic<uint64_t> buttonEvents_{0}, buttonOverflows_{0}, buttonPolls_{0}, buttonMissed_{0};
};

}  // namespace mover
