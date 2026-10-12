// MAKCU UDP batch transport (Ethernet, 0x6C) - замороженная эталонная версия.
//
// Один заголовок, без зависимостей кроме WinAPI (Winsock 2). Доставляет целые
// отчёты (dx, dy) в MAKCU по UDP командой batch 0x6C (тест-сборка V4.097):
// до 7 Raw-записей в одной датаграмме, одна квитанция на пакет. Каждый
// принятый move() = ровно одна запись = ровно один HID-отчёт, мимо интерполятора.
//
// Что транспорт делает:
//   - свой UDP-сокет (обёртка RAW: 0x55 + nonce u64 + кадр MAK_API), поток приёма;
//   - открытие: DEVICE 0x02 (маршрут мыши), FIRMWARE 0x04 (>= 4097), Raw-метаданные
//     0x6A(id=0), дочистка записей прошлых сессий;
//   - closed loop: в полёте один пакет; поток приёма по квитанции сразу шлёт
//     следующий с накопившимися записями (<= 7), X = старший терминальный id из
//     квитанций (flags bit0) освобождает записи платы;
//   - непринятые платой записи (нет места) возвращаются в начало очереди;
//     нет квитанции 30 мс - опрос 0x6A(id=0): last_accepted_id показывает, что принято;
//   - finish(): дослать очередь, дождаться квитанции, освободить хвост записей;
//   - кнопки мыши: open() включает поток INPUT_STREAM mouse (0x52), события
//     INPUT_CHANGE 0x53 приходят сразу по нажатию; стартовая маска и страховочный
//     опрос (раз в 50 мс) - GET PHYS_BUTTONS 0x55 в том же замкнутом цикле. Если опрос
//     находит изменения, которых не принесли события (их режет брандмауэр?), опрос
//     сам учащается до 5 мс. close() возвращает поток. buttons()/buttonDown()/pressCount().
//     ВАЖНО: события плата шлёт на наш порт, но С ПОРТА, РАВНОГО НАШЕМУ, а не с 8080,
//     поэтому сокет без connect(): принимаются любые датаграммы с IP платы.
// Чего транспорт НЕ делает (это забота вызывающего):
//   - темп (часы 1 мс), нарезка траектории, квантование/рендерер. Частота HID =
//     частота ненулевых принятых move(), не выше опроса USB клонированной мыши.
//
// Почему так (замеры 2026-10-11/12, подробно - README.md):
//   - каждая UDP-команда идёт на второй чип MAKCU по внутреннему каналу, который
//     везёт одну команду за раз; датаграмма, пришедшая во время передачи, молча
//     выбрасывается. Поэтому одна команда в полёте и много записей в одной команде;
//   - квитанция сообщает не больше 7 терминальных строк, поэтому пакет <= 7 записей
//     (16 дают ту же скорость: лишние возвращаются с reason 3 "нет места").
//
// Контракт вызова:
//   - move()/service()/finish() - из ОДНОГО потока (обычно поток тайминга).
//     Никогда не блокируются на сети (finish() - до timeoutMs).
//   - move() == false: очередь полна (7 записей ждут пакета) или транспорт
//     неисправен - вызывающий оставляет дельту у себя и добавляет к следующему вызову.
//   - move(0, 0) ничего не отправляет на мышь (= service()).
//   - stats()/statsLine(), buttons()/buttonDown()/pressCount() - из любого потока.
//
// Ревизия 2026-10-12 (2): кнопки мыши. Логика движения не менялась.
//
// Происхождение: basics/udp_way/makcu_udp_transport.h (RawUdpTransport,
// Format::Batch) от 2026-10-12. Отличия: только batch 0x6C (без 0x69/0x18/0x67,
// ведра и проб), переполнение очереди - move() == false вместо склейки записей,
// ответы сопоставляются по nonce без кольца отправленных. Не править: новая
// версия - новая папка.
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

namespace makcu {

namespace detail {

#ifndef MAKCU_DETAIL_QPC_DEFINED
#define MAKCU_DETAIL_QPC_DEFINED
// QPC-часы транспорта (свои, чтобы не зависеть от окружения).
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
#endif  // MAKCU_DETAIL_QPC_DEFINED (общие для COM- и UDP-транспорта)

inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline void put16(uint8_t* p, int32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace detail

#ifndef MAKCU_MOUSE_BUTTONS_DEFINED
#define MAKCU_MOUSE_BUTTONS_DEFINED
// Физические кнопки мыши в host-порту MAKCU: номер = бит маски PHYS_BUTTONS (0x55)
// и id события INPUT_CHANGE (0x53). Общий блок для makcu_raw_transport.h (COM) и
// makcu_udp_batch_transport.h (UDP): можно подключать оба заголовка.
enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2, Side1 = 3, Side2 = 4 };
inline constexpr uint8_t kMouseButtonCount = 5;
inline constexpr uint8_t buttonBit(MouseButton b) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(b)); }
inline const char* buttonName(MouseButton b) {
    switch (b) {
    case MouseButton::Left: return "left";
    case MouseButton::Right: return "right";
    case MouseButton::Middle: return "middle";
    case MouseButton::Side1: return "side1";
    case MouseButton::Side2: return "side2";
    }
    return "?";
}
#endif  // MAKCU_MOUSE_BUTTONS_DEFINED

class UdpBatchTransport {
public:
    static constexpr uint16_t kDefaultPort = 8080;
    static constexpr uint32_t kMinFirmware = 4097;  // первая сборка с 0x6C (тест-сборка V4.097)
    static constexpr size_t kMaxRecords = 7;        // записей в пакете: квитанция освобождает <= 7 строк
    static constexpr size_t kMaxPending = 7;        // очередь move(); полна - move() == false
    static constexpr double kLostUs = 30000.0;      // нет квитанции 30 мс - спросить last_accepted_id
    static constexpr double kDefaultButtonPollMs = 50.0;  // страховочный опрос 0x55 (0 - выключить)
    static constexpr double kFallbackPollMs = 5.0;        // опрос, если события 0x53 не доходят
    static constexpr uint32_t kMissedForFallback = 2;     // изменений, найденных только опросом
    static constexpr double kPollLostUs = 10000.0;        // нет ответа на опрос 10 мс - считать потерянным

    // Снимок счётчиков (stats()).
    struct Stats {
        uint64_t records = 0;        // принятых move() (= записей = HID-отчётов)
        uint64_t packets = 0;        // отправлено пакетов 0x6C
        uint64_t admitted = 0;       // записей, принятых платой
        uint64_t receipts = 0;       // квитанций
        uint64_t partial = 0;        // result 1: часть не принята (обычно reason 3 = нет места)
        uint64_t envelope = 0;       // result 255: пакет отклонён целиком
        uint64_t xRejected = 0;      // 255/1: X больше сообщённого (транспорт сбрасывает X)
        uint64_t badReplies = 0;     // ответ не похож на квитанцию 0x6C
        uint64_t lostReceipts = 0;   // нет квитанции kLostUs - разобрано опросом
        uint64_t metaQueries = 0;    // опросов 0x6A(id=0) после потери
        uint64_t lateReplies = 0;    // ответ на уже разобранный пакет/опрос
        uint64_t freed = 0;          // освобождено платой по X
        uint64_t termSubmitted = 0;  // терминальных строк: отдано в USB-endpoint
        uint64_t termRejected = 0;   // терминальных строк: отказ
        uint64_t moreSet = 0;        // квитанций с флагом more
        uint64_t busy = 0;           // move() == false из-за полной очереди
        uint64_t sendErrors = 0;     // send() не прошёл (занят буфер или ошибка)
        uint64_t datagrams = 0;      // всех датаграмм, включая служебные
        uint32_t minFree = 256;      // минимум свободных записей платы по квитанциям (256 - квитанций не было)
        uint32_t maxPending = 0;     // максимум записей в очереди
        uint32_t pending = 0;        // записей в очереди сейчас
        bool inFlight = false;       // пакет ждёт квитанции
        uint32_t lastReason = 0;     // последняя причина отказа из квитанции
        double rttAvgMs = 0.0;       // квитанция: средняя и максимальная задержка
        double rttMaxMs = 0.0;
        // Кнопки
        uint8_t buttons = 0;           // маска физических кнопок сейчас
        uint64_t buttonEvents = 0;     // событий 0x53 (мышь)
        uint64_t buttonOverflows = 0;  // переполнений потока событий (после них - опрос сразу)
        uint64_t buttonPolls = 0;      // опросов 0x55
        uint64_t buttonReplies = 0;    // ответов на опрос
        uint64_t buttonLost = 0;       // опросов без ответа kPollLostUs
        uint64_t buttonMissed = 0;     // изменений, которые нашёл опрос, а не событие
        uint64_t buttonChanges = 0;    // изменений маски
        bool eventsMissing = false;    // событий нет, а опрос видит нажатия: опрос учащён до 5 мс
    };

    UdpBatchTransport() = default;
    ~UdpBatchTransport() { close(); }
    UdpBatchTransport(const UdpBatchTransport&) = delete;
    UdpBatchTransport& operator=(const UdpBatchTransport&) = delete;

    // host - IP или имя платы, port - UDP-порт команд (8080). false + причина в err.
    bool open(const std::string& host, uint16_t port, std::string& err) {
        close();
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            err = "WSAStartup failed";
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
                err = "не удалось разрешить адрес " + host;
                close();
                return false;
            }
            remote.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            err = "socket() failed, WSA " + std::to_string(WSAGetLastError());
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
            err = "bind() failed, WSA " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        u_long nonblocking = 1;
        ioctlsocket(sock_, FIONBIO, &nonblocking);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        endpoint_ = std::string(ip) + ":" + std::to_string(port);
        // Случайная база nonce: поздние ответы прошлых сессий не совпадут.
        seq_ = (static_cast<uint64_t>(detail::Qpc::now()) << 20) ^ static_cast<uint64_t>(GetCurrentProcessId());
        failed_ = false;
        stopRx_ = false;
        rx_ = std::thread([this] { rxLoop(); });

        auto kinds = query(kOpDevice, nullptr, 0, 150, 3);
        if (!kinds || kinds->size() != 1) {
            err = "MAKCU " + endpoint_ + " не ответил на DEVICE (0x02) - адрес/порт, плата в сети?";
            close();
            return false;
        }
        if (!((*kinds)[0] & 1)) {
            err = "маршрут мыши не активен (DEVICE без mouse) - мышь в host-порту MAKCU?";
            close();
            return false;
        }
        if (auto fw = query(kOpFirmware, nullptr, 0, 150, 3); fw && fw->size() == 4) firmware_ = detail::le32(fw->data());
        if (firmware_ < kMinFirmware) {
            err = "прошивка " + std::to_string(firmware_) + " без batch 0x6C (нужна V4.097+)";
            close();
            return false;
        }
        auto meta = queryMeta();
        if (!meta) {
            err = "нет ответа на 0x6A(id=0) - Raw API недоступен";
            close();
            return false;
        }
        uint32_t cleaned = 0;
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            cleaned = consumeLeftovers(meta->lastId, meta->capacity - meta->freeRecords);
            if (auto again = queryMeta()) meta = again;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            pend_.clear();
            flight_ = Flight{};
            nextId_ = meta->lastId + 1;
            haveX_ = false;
            maxTerm_ = 0;
        }
        // Кнопки: включить поток событий мыши (запомнить, каким был), потом стартовая маска.
        // Порядок важен: поток начинается с "всё отпущено", маска читается после включения.
        resetButtons();
        const uint8_t kindMouse[1] = {0x01};
        if (auto was = query(kOpInputStream, kindMouse, 1, 100, 3); was && was->size() == 1) {
            if ((*was)[0] != 1) {
                const uint8_t on[2] = {0x01, 0x01};
                if (sendNoReply(kOpInputStream, on, 2)) streamRestore_ = (*was)[0];
            }
            streamOk_ = true;
        }
        if (auto mask = query(kOpPhysButtons, nullptr, 0, 100, 3); mask && mask->size() == 1) {
            mask_.store((*mask)[0], std::memory_order_release);
            primed_.store(true, std::memory_order_release);
            buttonsOk_ = true;
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "прошивка %u, schema %u, last_accepted_id %lu, свободно записей %u из %u",
            firmware_, meta->schema, static_cast<unsigned long>(meta->lastId), meta->freeRecords, meta->capacity);
        info_ = buf;
        if (cleaned) info_ += ", дочищено записей прошлых сессий " + std::to_string(cleaned);
        char bb[160];
        if (buttonsOk_) {
            std::snprintf(bb, sizeof(bb), ", кнопки: маска %02X, события 0x53 %s, страховочный опрос %.0f мс", mask_.load(),
                !streamOk_ ? "недоступны" : streamRestore_ ? "включены" : "уже были включены",
                detail::Qpc::toUs(pollTicks_.load()) / 1000.0);
            info_ += bb;
        } else {
            info_ += ", кнопки: 0x55 не ответил";
        }
        nextPollT_ = detail::Qpc::now();
        ready_ = true;
        return true;
    }

    void close() {
        const bool wasReady = ready_.exchange(false);
        if (wasReady && streamRestore_ && sock_ != INVALID_SOCKET) {
            // Вернуть INPUT_STREAM mouse, каким он был до open().
            const uint8_t restore[2] = {0x01, *streamRestore_};
            (void)sendNoReply(kOpInputStream, restore, 2);
        }
        streamRestore_.reset();
        streamOk_ = false;
        buttonsOk_ = false;
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

    [[nodiscard]] bool healthy() const { return sock_ != INVALID_SOCKET && ready_.load() && !failed_.load(); }
    [[nodiscard]] const std::string& endpoint() const { return endpoint_; }
    [[nodiscard]] uint32_t firmware() const { return firmware_; }
    [[nodiscard]] const std::string& info() const { return info_; }

    // ---- кнопки мыши (любой поток, не блокируют) ---------------------------------------
    // Маска физических кнопок: bit0 левая, bit1 правая, bit2 средняя, bit3 side1, bit4 side2.
    // Обновляется событием 0x53 сразу по изменению (~1 мс); страховочный опрос 0x55
    // исправляет маску, если событие потерялось (UDP) или события не доходят совсем.
    [[nodiscard]] uint8_t buttons() const { return mask_.load(std::memory_order_acquire); }
    [[nodiscard]] bool buttonDown(MouseButton b) const { return (buttons() & buttonBit(b)) != 0; }
    // Нажатий (переходов 0 -> 1) с open(). Разность двух чтений = нажатия между ними:
    // так не теряются клики, если вызывающий читает buttons() редко. Кнопка, зажатая
    // уже при open(), нажатием не считается.
    [[nodiscard]] uint32_t pressCount(MouseButton b) const {
        return presses_[static_cast<uint8_t>(b)].load(std::memory_order_acquire);
    }
    // false - прошивка не ответила на 0x55 (кнопки не обновляются).
    [[nodiscard]] bool buttonsAvailable() const { return buttonsOk_.load(); }
    // Интервал страховочного опроса 0x55, мс (по умолчанию 50; 0 - не опрашивать совсем).
    // Каждый опрос занимает один оборот команды (~1 мс). Если события не доходят, опрос
    // сам идёт не реже kFallbackPollMs (5 мс = 200 опросов/с, движению 1000 Гц хватает).
    void setButtonPollMs(double ms) {
        pollTicks_.store(ms > 0.0 ? detail::Qpc::fromUs(ms * 1000.0) : 0, std::memory_order_release);
    }

    // ---- горячий путь -------------------------------------------------------------
    // true - запись принята и уйдёт ровно одним HID-отчётом. false - очередь полна
    // или транспорт неисправен: дельту хранит вызывающий.
    [[nodiscard]] bool move(int16_t dx, int16_t dy) {
        if (!healthy()) return false;
        if (dx == 0 && dy == 0) {
            service();
            return true;
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (pend_.size() < kMaxPending) {
                pend_.push_back(Rec{dx, dy});
                ++st_.records;
                st_.maxPending = std::max<uint32_t>(st_.maxPending, static_cast<uint32_t>(pend_.size()));
                accepted = true;
            } else {
                ++st_.busy;
            }
        }
        service();
        return accepted;
    }

    // Отправить пакет, если предыдущий уже квитирован (обычно это делает поток приёма).
    void service() {
        if (!healthy()) return;
        std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
        if (lock.owns_lock()) (void)pumpLocked();
    }

    // Конец движения: дослать очередь, дождаться квитанции, освободить записи платы.
    // true - всё отдано и плата чистая.
    bool finish(int timeoutMs = 300) {
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        bool done = false;
        while (detail::Qpc::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                done = pend_.empty() && !flight_.active && awaitSeq_.load() == 0;
            }
            if (done) break;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                (void)pumpLocked();
            }
            Sleep(1);
        }
        if (!done || !healthy()) return false;
        Sleep(30);  // последние записи уходят в USB по одной на опрос мыши
        auto meta = queryMeta();
        if (!meta) return false;
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            (void)consumeLeftovers(meta->lastId, meta->capacity - meta->freeRecords);
            meta = queryMeta();
        }
        return meta && meta->freeRecords == meta->capacity;
    }

    // ---- статистика ------------------------------------------------------------------
    [[nodiscard]] Stats stats() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        Stats s = st_;
        s.pending = static_cast<uint32_t>(pend_.size());
        s.inFlight = flight_.active;
        s.datagrams = datagrams_.load();
        s.sendErrors = sendErrors_.load();
        s.rttAvgMs = rttCount_ ? rttSumUs_ / static_cast<double>(rttCount_) / 1000.0 : 0.0;
        s.rttMaxMs = rttMaxUs_ / 1000.0;
        s.buttons = mask_.load();
        s.buttonEvents = buttonEvents_.load();
        s.buttonOverflows = buttonOverflows_.load();
        s.buttonPolls = buttonPolls_.load();
        s.buttonReplies = buttonReplies_.load();
        s.buttonLost = buttonLost_.load();
        s.buttonMissed = buttonMissed_.load();
        s.buttonChanges = buttonChanges_.load();
        s.eventsMissing = eventsMissing();
        return s;
    }

    [[nodiscard]] std::string statsLine() const {
        const Stats s = stats();
        char buf[1100];
        std::snprintf(buf, sizeof(buf),
            "batch 0x6C %s: записей %llu, пакетов %llu (%.2f/пакет), принято платой %llu | квитанций %llu, частичных %llu,"
            " отклонено %llu (X %llu), непонятных %llu | потеряно квитанций %llu, опросов %llu, поздних %llu"
            " | освобождено %llu, мин. свободно %d, more %llu | терминальных: отдано %llu, отказ %llu"
            " | очередь max %u, сейчас %u, полна %llu раз | RTT ср %.2f max %.2f мс | датаграмм %llu, ошибок send %llu"
            " | посл. причина %u | кнопки %02X: событий %llu, переполнений %llu, опросов %llu, ответов %llu,"
            " потеряно %llu, найдено опросом %llu, изменений %llu%s",
            endpoint_.c_str(), u(s.records), u(s.packets),
            s.packets ? static_cast<double>(s.records) / static_cast<double>(s.packets) : 0.0, u(s.admitted),
            u(s.receipts), u(s.partial), u(s.envelope), u(s.xRejected), u(s.badReplies), u(s.lostReceipts),
            u(s.metaQueries), u(s.lateReplies), u(s.freed), s.minFree > 255 ? -1 : static_cast<int>(s.minFree),
            u(s.moreSet), u(s.termSubmitted), u(s.termRejected), s.maxPending, s.pending, u(s.busy), s.rttAvgMs,
            s.rttMaxMs, u(s.datagrams), u(s.sendErrors), s.lastReason, s.buttons, u(s.buttonEvents),
            u(s.buttonOverflows), u(s.buttonPolls), u(s.buttonReplies), u(s.buttonLost), u(s.buttonMissed),
            u(s.buttonChanges), s.eventsMissing ? " | СОБЫТИЯ НЕ ДОХОДЯТ (брандмауэр?) - опрос 5 мс" : "");
        return buf;
    }

private:
    static constexpr uint8_t kOpDevice = 0x02;
    static constexpr uint8_t kOpFirmware = 0x04;
    static constexpr uint8_t kOpRawAck = 0x6A;  // 0x6A(id=0): метаданные; (id, consume=1): освободить запись
    static constexpr uint8_t kOpBatch = 0x6C;   // V4.097+: пакет Raw-записей + квитанция
    static constexpr uint8_t kOpPhysButtons = 0x55;  // V4.036+: GET -> mask:u8 физических кнопок
    static constexpr uint8_t kOpInputStream = 0x52;  // GET kind -> enabled; SET kind enabled (без ответа)
    static constexpr uint8_t kOpInputChange = 0x53;  // событие: kind:u8 id:u8 value:u8 (FF FF - переполнение)

    static unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }
    static constexpr DWORD kTimerHighResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+)

    struct Rec {
        int32_t dx = 0;
        int32_t dy = 0;
    };
    struct Flight {
        bool active = false;
        uint64_t seq = 0;  // nonce пакета
        uint32_t firstId = 0;
        uint8_t n = 0;
        std::array<Rec, kMaxRecords> recs{};
    };
    struct Meta {
        uint8_t schema = 0;
        uint32_t lastId = 0;
        uint8_t freeRecords = 0;
        uint8_t capacity = 0;
    };

    // ---- служебный обмен (open/finish, не горячий путь) -------------------------------
    std::optional<std::vector<uint8_t>> query(uint8_t op, const uint8_t* payload, uint16_t len, int timeoutMs,
        int tries) {
        for (int attempt = 0; attempt < tries; ++attempt) {
            uint64_t seq = 0;
            {
                // Одна команда в полёте: дождаться ответа на пакет/опрос цикла. Пока держим
                // sendMutex_, поток приёма новых не отправит.
                std::lock_guard<std::mutex> lock(sendMutex_);
                const int64_t idleBy = detail::Qpc::now() + detail::Qpc::fromUs(kLostUs);
                while (awaitSeq_.load(std::memory_order_acquire) != 0 && detail::Qpc::now() < idleBy) YieldProcessor();
                seq = ++seq_;
                {
                    std::lock_guard<std::mutex> sl(syncMutex_);
                    syncSeq_ = seq;
                    syncDone_ = false;
                    syncWaiting_ = true;
                }
                awaitT_.store(detail::Qpc::now(), std::memory_order_relaxed);
                awaitSeq_.store(seq, std::memory_order_release);
                if (!sendFrameLocked(seq, op, payload, len)) {
                    clearAwait(seq);
                    std::lock_guard<std::mutex> sl(syncMutex_);
                    syncWaiting_ = false;
                    continue;
                }
            }
            std::unique_lock<std::mutex> sl(syncMutex_);
            const bool ok = syncCv_.wait_for(sl, std::chrono::milliseconds(timeoutMs), [&] { return syncDone_; });
            syncWaiting_ = false;
            clearAwait(seq);
            if (ok) return syncReply_;
        }
        return std::nullopt;
    }

    std::optional<Meta> queryMeta() {
        const uint8_t zero[5] = {0, 0, 0, 0, 0};
        auto r = query(kOpRawAck, zero, 5, 100, 3);
        if (!r || r->size() != 8) return std::nullopt;
        const uint8_t* p = r->data();
        return Meta{p[1], detail::le32(p + 2), p[6], p[7]};
    }

    // Освободить (consume=1) терминальные записи среди последних id: прерванные сессии
    // оставляют их на плате до перезагрузки. ВНИМАНИЕ: освобождает и записи другой
    // программы, если она тоже шлёт Raw.
    uint32_t consumeLeftovers(uint32_t lastId, uint32_t want) {
        uint32_t consumed = 0;
        for (uint32_t id = lastId, scanned = 0; id >= 1 && scanned < 1024 && consumed < want; --id, ++scanned) {
            uint8_t p[5];
            detail::put32(p, id);
            p[4] = 1;
            auto r = query(kOpRawAck, p, 5, 30, 2);
            if (r && r->size() == 6 && ((*r)[0] == 2 || (*r)[0] == 3)) ++consumed;
        }
        return consumed;
    }

    // ---- отправка -----------------------------------------------------------------------
    // Под sendMutex_. Датаграмма = 0x55 | nonce u64 | DE AD | LEN u16 | op | payload.
    bool sendFrameLocked(uint64_t seq, uint8_t op, const uint8_t* payload, uint16_t len) {
        std::array<uint8_t, 14 + 8 + 10 * kMaxRecords> d{};
        size_t n = 0;
        d[n++] = 0x55;
        for (int i = 0; i < 8; ++i) d[n++] = static_cast<uint8_t>(seq >> (8 * i));
        d[n++] = 0xDE;
        d[n++] = 0xAD;
        d[n++] = static_cast<uint8_t>(len & 0xFF);
        d[n++] = static_cast<uint8_t>(len >> 8);
        d[n++] = op;
        if (len) std::memcpy(d.data() + n, payload, len);
        n += len;
        const int rc = ::sendto(sock_, reinterpret_cast<const char*>(d.data()), static_cast<int>(n), 0,
            reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
        if (rc != static_cast<int>(n)) {
            sendErrors_.fetch_add(1, std::memory_order_relaxed);
            if (WSAGetLastError() != WSAEWOULDBLOCK) failed_ = true;
            return false;
        }
        datagrams_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Служебная команда без ответа (SET 0x52): дождаться, пока цикл свободен, отправить и
    // переждать передачу на второй чип (иначе следующая команда может потеряться).
    bool sendNoReply(uint8_t op, const uint8_t* payload, uint16_t len) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        const int64_t idleBy = detail::Qpc::now() + detail::Qpc::fromUs(kLostUs);
        while (awaitSeq_.load(std::memory_order_acquire) != 0 && detail::Qpc::now() < idleBy) YieldProcessor();
        const bool ok = sendFrameLocked(++seq_, op, payload, len);
        Sleep(5);
        return ok;
    }

    // Под sendMutex_. Следующий пакет из очереди, если предыдущий получил квитанцию.
    // Нет квитанции kLostUs - опрос 0x6A(id=0) (повторяется, пока нет ответа).
    bool pumpLocked() {
        if (!healthy()) return false;
        const int64_t now = detail::Qpc::now();
        if (const uint64_t a = awaitSeq_.load(std::memory_order_acquire); a != 0) {
            const int64_t waited = now - awaitT_.load(std::memory_order_relaxed);
            if (a == physSeq_.load(std::memory_order_acquire)) {
                // Опрос кнопок без ответа: забыть его, движение не ждёт.
                if (waited < detail::Qpc::fromUs(kPollLostUs)) return false;
                buttonLost_.fetch_add(1, std::memory_order_relaxed);
                physSeq_.store(0, std::memory_order_relaxed);
                clearAwait(a);
                return false;
            }
            if (a != batchSeq_.load() && a != metaSeq_.load()) return false;  // служебный запрос: ждёт сам
            if (waited < detail::Qpc::fromUs(kLostUs)) return false;
            const bool wasPacket = metaSeq_.load() == 0;
            const uint64_t seq = ++seq_;
            metaSeq_.store(seq, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(seq, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (wasPacket) ++st_.lostReceipts;
                ++st_.metaQueries;
            }
            const uint8_t zero[5] = {0, 0, 0, 0, 0};
            (void)sendFrameLocked(seq, kOpRawAck, zero, 5);  // не ушёл - повтор через kLostUs
            return true;
        }
        // Опрос кнопок по расписанию: одна команда, ответ ~1 мс, потом снова движение.
        int64_t poll = pollTicks_.load(std::memory_order_acquire);
        if (poll > 0 && eventsMissing()) poll = std::min(poll, detail::Qpc::fromUs(kFallbackPollMs * 1000.0));
        const bool pollNow = pollNow_.exchange(false, std::memory_order_acq_rel);  // после переполнения потока
        if (poll > 0 && buttonsOk_.load() && (pollNow || now >= nextPollT_)) {
            nextPollT_ = now - nextPollT_ > poll ? now + poll : nextPollT_ + poll;  // опоздали - от текущего момента
            const uint64_t ps = ++seq_;
            physSeq_.store(ps, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(ps, std::memory_order_release);
            if (!sendFrameLocked(ps, kOpPhysButtons, nullptr, 0)) {
                physSeq_.store(0, std::memory_order_relaxed);
                clearAwait(ps);
                return false;
            }
            buttonPolls_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        Flight f;
        bool haveX = false;
        uint32_t x = 0;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (pend_.empty()) return false;
            seq = ++seq_;
            f.active = true;
            f.seq = seq;
            f.firstId = nextId_;
            f.n = static_cast<uint8_t>(std::min(pend_.size(), kMaxRecords));
            for (uint8_t i = 0; i < f.n; ++i) {
                f.recs[i] = pend_.front();
                pend_.pop_front();
            }
            haveX = haveX_;
            x = maxTerm_;
            flight_ = f;
        }
        std::array<uint8_t, 8 + 10 * kMaxRecords> pl{};
        pl[0] = 0x01;
        pl[1] = haveX ? 0x01 : 0x00;  // bit0: освободить терминальные записи с id <= X
        pl[2] = f.n;
        pl[3] = 0x00;
        detail::put32(pl.data() + 4, haveX ? x : 0);
        for (uint8_t i = 0; i < f.n; ++i) {
            uint8_t* r = pl.data() + 8 + 10 * i;
            detail::put32(r, f.firstId + i);
            detail::put16(r + 4, f.recs[i].dx);
            detail::put16(r + 6, f.recs[i].dy);
            detail::put16(r + 8, 0);  // dt
        }
        metaSeq_.store(0, std::memory_order_relaxed);
        batchSeq_.store(seq, std::memory_order_release);
        awaitT_.store(now, std::memory_order_relaxed);
        awaitSeq_.store(seq, std::memory_order_release);
        if (!sendFrameLocked(seq, kOpBatch, pl.data(), static_cast<uint16_t>(8 + 10 * f.n))) {
            batchSeq_.store(0, std::memory_order_relaxed);
            awaitSeq_.store(0, std::memory_order_release);
            std::lock_guard<std::mutex> lock(stateMutex_);
            requeueLocked(f, 0);
            flight_.active = false;
            return false;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++st_.packets;
        return true;
    }

    // Снять ожидание, если ждём именно этот nonce (иначе там уже другая команда).
    void clearAwait(uint64_t seq) {
        uint64_t expected = seq;
        awaitSeq_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    // Поток приёма: новая маска кнопок; переходы 0 -> 1 считаются нажатиями (после стартовой
    // маски). fromPoll: изменение нашёл опрос, а не событие - признак потерянных событий.
    void applyMask(uint8_t mask, bool fromPoll) {
        const uint8_t old = mask_.exchange(mask, std::memory_order_acq_rel);
        if (!primed_.load(std::memory_order_acquire)) return;
        if (old == mask) return;
        if (fromPoll) buttonMissed_.fetch_add(1, std::memory_order_relaxed);
        buttonChanges_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t pressed = static_cast<uint8_t>(mask & ~old);
        for (uint8_t i = 0; i < kMouseButtonCount; ++i) {
            if (pressed & (1u << i)) presses_[i].fetch_add(1, std::memory_order_acq_rel);
        }
    }

    // Поток приёма: событие 0x53. Мышь: id = номер кнопки, value 0/1; FF FF - переполнение.
    void onInputChange(const uint8_t* p, size_t plen) {
        if (plen != 3 || p[0] != 1) return;  // не мышь (или колесо в тест-сборках, plen 4)
        if (p[1] == 0xFF && p[2] == 0xFF) {
            buttonOverflows_.fetch_add(1, std::memory_order_relaxed);
            pollNow_.store(true, std::memory_order_release);
            return;
        }
        if (p[1] >= 8 || p[2] > 1) return;
        buttonEvents_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t bit = static_cast<uint8_t>(1u << p[1]);
        const uint8_t old = mask_.load(std::memory_order_acquire);
        applyMask(p[2] ? static_cast<uint8_t>(old | bit) : static_cast<uint8_t>(old & ~bit), false);
    }

    // События не доходят: ни одного, а опрос уже находил изменения.
    [[nodiscard]] bool eventsMissing() const {
        return buttonEvents_.load(std::memory_order_relaxed) == 0 &&
               buttonMissed_.load(std::memory_order_relaxed) >= kMissedForFallback;
    }

    void resetButtons() {
        mask_.store(0, std::memory_order_release);
        primed_.store(false, std::memory_order_release);
        pollNow_.store(false, std::memory_order_release);
        for (auto& c : presses_) c.store(0, std::memory_order_release);
        buttonEvents_ = buttonOverflows_ = buttonPolls_ = buttonReplies_ = buttonLost_ = buttonMissed_ = buttonChanges_ = 0;
    }

    // Под stateMutex_: записи пакета с индекса from - обратно в начало очереди (порядок сохраняется).
    void requeueLocked(const Flight& f, uint8_t from) {
        for (int i = static_cast<int>(f.n) - 1; i >= static_cast<int>(from); --i) pend_.push_front(f.recs[i]);
    }

    // Под stateMutex_: пакет в полёте разобран.
    void settleLocked() {
        flight_.active = false;
        batchSeq_.store(0, std::memory_order_relaxed);
        metaSeq_.store(0, std::memory_order_relaxed);
        awaitSeq_.store(0, std::memory_order_release);
    }

    // ---- приём ----------------------------------------------------------------------------
    // Ждёт датаграмму или тик 1 мс (опрос кнопок, тайм-ауты). Тик - high-resolution
    // waitable timer: он точен без timeBeginPeriod(1), а select()/Sleep() без него
    // просыпаются по тику планировщика (~15.6 мс) и опрос кнопок шёл бы реже.
    void rxLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, kTimerHighResolution, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // старая Windows: грубее
        LARGE_INTEGER due{};
        due.QuadPart = -10000;  // 1 мс, затем период 1 мс
        if (timer) SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
        WSAEVENT readable = WSACreateEvent();
        WSAEventSelect(sock_, readable, FD_READ);
        const HANDLE handles[2] = {readable, timer};
        std::array<uint8_t, 2048> buf{};
        while (!stopRx_.load()) {
            const DWORD w = WaitForMultipleObjects(timer ? 2 : 1, handles, FALSE, 20);
            if (w == WAIT_OBJECT_0) WSAResetEvent(readable);
            for (;;) {
                sockaddr_in from{};
                int fromLen = sizeof(from);
                const int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;  // WSAEWOULDBLOCK - всё прочитано
                // Любой порт платы (ответы с 8080, события - с порта, равного нашему), чужие IP - мимо.
                if (from.sin_addr.s_addr != remote_.sin_addr.s_addr) continue;
                onDatagram(buf.data(), static_cast<size_t>(n));
            }
            // Сразу по квитанции следующий пакет. Отправку держит другой поток - не ждём.
            if (ready_.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
                if (lock.owns_lock()) (void)pumpLocked();
            }
        }
        WSAEventSelect(sock_, readable, 0);
        WSACloseEvent(readable);
        if (timer) {
            CancelWaitableTimer(timer);
            CloseHandle(timer);
        }
    }

    void onDatagram(const uint8_t* d, size_t n) {
        if (n < 14 || d[0] != 0x55 || d[9] != 0xDE || d[10] != 0xAD) return;
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i) seq |= static_cast<uint64_t>(d[1 + i]) << (8 * i);
        const size_t plen = static_cast<size_t>(d[11]) | static_cast<size_t>(d[12]) << 8;
        if (14 + plen > n) return;
        const uint8_t* p = d + 14;  // d[13] - опкод ответа
        // Событие 0x53 несёт nonce одной из наших прошлых команд - разбирать по опкоду
        // ДО сопоставления по nonce, иначе его можно принять за квитанцию пакета.
        if (d[13] == kOpInputChange) {
            onInputChange(p, plen);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(syncMutex_);
            if (syncWaiting_ && !syncDone_ && seq == syncSeq_) {
                syncReply_.assign(p, p + plen);
                syncDone_ = true;
                clearAwait(seq);
                syncCv_.notify_all();
                return;
            }
        }
        if (seq != 0 && seq == physSeq_.load(std::memory_order_acquire)) {
            if (plen == 1) {
                applyMask(p[0], true);
                buttonReplies_.fetch_add(1, std::memory_order_relaxed);
            }
            physSeq_.store(0, std::memory_order_relaxed);
            clearAwait(seq);
            return;
        }
        if (seq == batchSeq_.load(std::memory_order_acquire)) {
            onReceipt(seq, p, plen);
        } else if (seq == metaSeq_.load(std::memory_order_acquire)) {
            if (plen == 8) onMeta(detail::le32(p + 2));
        } else {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++st_.lateReplies;
        }
    }

    // Квитанция 0x6C: [0]=1 [1]=0 все / 1 частично / 255 отклонено [2]=N [3]=принято
    // [4]=первый отказ [5]=причина [6]=освобождено [7]=свободно [8]=K строк (<=7) [9]=more
    // [10]=в очереди [11]=0 [12..15]=last_id, затем K x (id u32 | state u8 | reason u8).
    void onReceipt(uint64_t seq, const uint8_t* p, size_t plen) {
        const double rttUs = detail::Qpc::toUs(detail::Qpc::now() - awaitT_.load(std::memory_order_relaxed));
        std::lock_guard<std::mutex> lock(stateMutex_);
        const Flight f = flight_;
        if (!f.active || f.seq != seq) {
            ++st_.lateReplies;
            return;
        }
        ++st_.receipts;
        rttSumUs_ += rttUs;
        ++rttCount_;
        rttMaxUs_ = std::max(rttMaxUs_, rttUs);
        if (plen < 16 || p[0] != 1) {
            // Не квитанция 0x6C: записи назад, транспорт дальше не работает.
            ++st_.badReplies;
            requeueLocked(f, 0);
            failed_ = true;
            settleLocked();
            return;
        }
        const uint8_t result = p[1];
        const uint8_t adm = std::min(p[3], f.n);
        const uint8_t reason = p[5];
        const uint8_t k = p[8];
        const uint32_t lastId = detail::le32(p + 12);
        if (result == 255) {
            ++st_.envelope;
            st_.lastReason = reason;
            if (reason == 1) {
                ++st_.xRejected;
                haveX_ = false;
            }
            requeueLocked(f, 0);
        } else {
            if (result == 1) {
                ++st_.partial;
                st_.lastReason = reason;
            }
            st_.admitted += adm;
            requeueLocked(f, adm);
            nextId_ = std::max(f.firstId + adm, adm ? lastId + 1 : f.firstId);
        }
        st_.freed += p[6];
        st_.minFree = std::min<uint32_t>(st_.minFree, p[7]);
        if (p[9]) ++st_.moreSet;
        for (size_t i = 0; i < k && 16 + 6 * i + 6 <= plen; ++i) {
            const uint8_t* row = p + 16 + 6 * i;
            const uint32_t id = detail::le32(row);
            if (row[4] == 2) ++st_.termSubmitted;
            else if (row[4] == 3) ++st_.termRejected;
            maxTerm_ = std::max(maxTerm_, id);
            haveX_ = true;
        }
        settleLocked();
    }

    // Ответ на опрос после потерянной квитанции: last_accepted_id показывает, сколько
    // записей пакета плата приняла (приём идёт подряд с первой).
    void onMeta(uint32_t lastAccepted) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const Flight f = flight_;
        if (f.active) {
            uint8_t adm = 0;
            if (lastAccepted >= f.firstId) adm = static_cast<uint8_t>(std::min<uint32_t>(lastAccepted - f.firstId + 1, f.n));
            st_.admitted += adm;
            requeueLocked(f, adm);
            nextId_ = std::max(f.firstId + adm, lastAccepted + 1);
        }
        settleLocked();
    }

    // ---- состояние ------------------------------------------------------------------------
    SOCKET sock_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
    std::string endpoint_;
    std::string info_;
    uint32_t firmware_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};

    std::mutex sendMutex_;  // отправка и seq_
    uint64_t seq_ = 0;
    std::atomic<uint64_t> awaitSeq_{0};  // nonce пакета/опроса в полёте (0 - можно слать)
    std::atomic<uint64_t> batchSeq_{0};  // nonce пакета, ждущего квитанции
    std::atomic<uint64_t> metaSeq_{0};   // nonce опроса после потери
    std::atomic<uint64_t> physSeq_{0};   // nonce опроса кнопок 0x55
    int64_t nextPollT_ = 0;              // под sendMutex_: когда следующий опрос кнопок
    std::atomic<int64_t> pollTicks_{detail::Qpc::fromUs(kDefaultButtonPollMs * 1000.0)};
    std::atomic<int64_t> awaitT_{0};
    std::atomic<uint64_t> datagrams_{0};
    std::atomic<uint64_t> sendErrors_{0};

    // Кнопки: маска пишет только поток приёма (и open()), читают все.
    std::atomic<bool> buttonsOk_{false};
    std::atomic<uint8_t> mask_{0};
    std::array<std::atomic<uint32_t>, kMouseButtonCount> presses_{};
    std::atomic<uint64_t> buttonEvents_{0}, buttonOverflows_{0}, buttonPolls_{0}, buttonReplies_{0}, buttonLost_{0},
        buttonMissed_{0}, buttonChanges_{0};
    std::atomic<bool> primed_{false};   // стартовая маска получена (раньше - не нажатия)
    std::atomic<bool> pollNow_{false};  // переполнение потока: опросить сразу
    std::optional<uint8_t> streamRestore_;  // INPUT_STREAM mouse до open(), если меняли
    bool streamOk_ = false;                 // плата ответила на GET 0x52
    sockaddr_in remote_{};                  // адрес платы (сокет без connect())

    std::thread rx_;
    std::atomic<bool> stopRx_{false};

    std::mutex syncMutex_;
    std::condition_variable syncCv_;
    bool syncWaiting_ = false;
    bool syncDone_ = false;
    uint64_t syncSeq_ = 0;
    std::vector<uint8_t> syncReply_;

    mutable std::mutex stateMutex_;  // очередь, пакет в полёте, X, счётчики
    std::deque<Rec> pend_;
    Flight flight_;
    uint32_t nextId_ = 1;
    uint32_t maxTerm_ = 0;  // старший терминальный id из квитанций
    bool haveX_ = false;
    Stats st_;
    double rttSumUs_ = 0.0;
    uint64_t rttCount_ = 0;
    double rttMaxUs_ = 0.0;
};

}  // namespace makcu
