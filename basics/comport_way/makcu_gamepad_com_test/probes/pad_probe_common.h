// pad_probe_common.h - общий код проб геймпада по COM (USB2, CH343, 4 Мбод).
// Синхронный порт, одна команда в полёте, как в com_soak.cpp. Только для проб: в эталоне свой ввод-вывод.
//
// Кадр MAK_API: DE AD | LEN u16 | CMD u8 | PAYLOAD[LEN]. Отказ: DE AD 01 00 CMD FF.
// Команды SET ответа не дают. Асинхронные кадры (события 0x53, уведомления 0x3E с первым байтом 0x12)
// складываются в g_async и не считаются ответом.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace padprobe {

inline int64_t qpcFreq() {
    static const int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return static_cast<int64_t>(v.QuadPart);
    }();
    return f;
}
inline int64_t now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
inline int64_t us(double v) { return static_cast<int64_t>(v * static_cast<double>(qpcFreq()) / 1e6); }
inline double toMs(int64_t t) { return static_cast<double>(t) * 1000.0 / static_cast<double>(qpcFreq()); }
inline double toUs(int64_t t) { return static_cast<double>(t) * 1e6 / static_cast<double>(qpcFreq()); }

inline constexpr uint8_t kOpDevice = 0x02;
inline constexpr uint8_t kOpFirmware = 0x04;
inline constexpr uint8_t kOpConnection = 0x3E;
inline constexpr uint8_t kOpCtlState = 0x40;
inline constexpr uint8_t kOpCtlStream = 0x41;
inline constexpr uint8_t kOpCtlMask = 0x51;
inline constexpr uint8_t kOpInputStream = 0x52;
inline constexpr uint8_t kOpInputChange = 0x53;
inline constexpr uint8_t kOpCtlPhysical = 0x54;

struct Frame {
    uint8_t op = 0;
    std::vector<uint8_t> pl;
    int64_t t = 0;  // QPC, когда кадр разобран
};

struct Port {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> rx;
    std::vector<Frame> async;  // события 0x53 и уведомления, пришедшие во время ask()

    bool open(const std::string& name) {
        const std::string path = "\\\\.\\" + name;
        h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        SetupComm(h, 16384, 4096);
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;  // чтение возвращается сразу
        t.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(h, &t);
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        GetCommState(h, &dcb);
        dcb.BaudRate = 4000000;
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
        if (!SetCommState(h, &dcb)) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
            return false;
        }
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        rx.clear();
        return true;
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }

    // Дочитать всё, что лежит в драйвере. true - что-то пришло.
    bool pump() {
        uint8_t buf[1024];
        DWORD n = 0;
        bool any = false;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            rx.insert(rx.end(), buf, buf + n);
            any = true;
        }
        return any;
    }

    // Следующий целый кадр из rx. Байты до DE AD выбрасываются (текстовые ответы km. сюда не попадают:
    // для них есть text()).
    bool nextFrame(Frame& f) {
        size_t i = 0;
        for (; i + 5 <= rx.size(); ++i) {
            if (rx[i] != 0xDE || rx[i + 1] != 0xAD) continue;
            const size_t plen = static_cast<size_t>(rx[i + 2]) | static_cast<size_t>(rx[i + 3]) << 8;
            if (plen > 1024) continue;
            if (i + 5 + plen > rx.size()) break;
            f.op = rx[i + 4];
            f.pl.assign(rx.begin() + static_cast<std::ptrdiff_t>(i + 5), rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            f.t = now();
            rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            return true;
        }
        rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(std::min(i, rx.size())));
        return false;
    }

    static bool isAsync(const Frame& f) {
        if (f.op == kOpInputChange) return true;
        if (f.op == kOpConnection && f.pl.size() == 13 && f.pl[0] == 0x12) return true;  // уведомление о топологии
        return false;
    }

    bool send(uint8_t op, const uint8_t* pl = nullptr, size_t len = 0) {
        uint8_t d[160] = {0xDE, 0xAD, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), op};
        if (len > sizeof(d) - 5) return false;
        if (len) std::memcpy(d + 5, pl, len);
        DWORD done = 0;
        return WriteFile(h, d, static_cast<DWORD>(5 + len), &done, nullptr) && done == 5 + len;
    }

    // Запрос и первый кадр с тем же опкодом. rttMs - время от записи до разбора ответа.
    std::optional<std::vector<uint8_t>> ask(uint8_t op, const uint8_t* pl = nullptr, size_t len = 0, double timeoutUs = 100000.0,
        double* rttMs = nullptr) {
        const int64_t t0 = now();
        if (!send(op, pl, len)) return std::nullopt;
        const int64_t end = t0 + us(timeoutUs);
        for (;;) {
            Frame f;
            while (nextFrame(f)) {
                if (isAsync(f)) {
                    async.push_back(f);
                    continue;
                }
                if (f.op != op) continue;
                if (rttMs) *rttMs = toMs(f.t - t0);
                return f.pl;
            }
            if (pump()) continue;
            if (now() >= end) return std::nullopt;
            YieldProcessor();
        }
    }

    // Текстовая команда km.*: отправить с CR LF и вернуть все байты, пришедшие за waitMs.
    std::string text(const std::string& cmd, int waitMs = 120) {
        pump();
        rx.clear();
        const std::string line = cmd + "\r\n";
        DWORD done = 0;
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &done, nullptr);
        const int64_t end = now() + us(waitMs * 1000.0);
        while (now() < end) {
            pump();
            Sleep(1);
        }
        std::string s(rx.begin(), rx.end());
        rx.clear();
        return s;
    }
};

inline std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X ", p[i]);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}
inline std::string hex(const std::vector<uint8_t>& v) { return hex(v.data(), v.size()); }

// Непечатные байты как \xNN, CR и LF как \r и \n.
inline std::string escaped(const std::string& s) {
    std::string o;
    char b[8];
    for (unsigned char c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c >= 32 && c < 127) o += static_cast<char>(c);
        else {
            std::snprintf(b, sizeof(b), "\\x%02X", c);
            o += b;
        }
    }
    return o;
}

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline int16_t les16(const uint8_t* p) { return static_cast<int16_t>(le16(p)); }

// Состояние геймпада: первые 20 байт ответов 0x40 и 0x54 (<IIHHhhhh).
struct PadState {
    uint32_t low = 0, high = 0;
    uint16_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
inline PadState parseState(const uint8_t* p) {
    PadState s;
    s.low = le32(p);
    s.high = le32(p + 4);
    s.lt = le16(p + 8);
    s.rt = le16(p + 10);
    s.lx = les16(p + 12);
    s.ly = les16(p + 14);
    s.rx = les16(p + 16);
    s.ry = les16(p + 18);
    return s;
}

// Снимок физического геймпада: 32 байта ответа 0x54 (<IIHHhhhhIIHH).
struct PadPhysical {
    PadState s;
    uint32_t seq = 0, usbTime = 0;
    uint16_t timing = 0, cadence = 0;
    uint16_t dt() const { return timing & 0x3FFF; }
    bool baseline() const { return (timing & 0x4000) != 0; }
    bool invalid() const { return (timing & 0x8000) != 0; }
};
inline PadPhysical parsePhysical(const uint8_t* p) {
    PadPhysical v;
    v.s = parseState(p);
    v.seq = le32(p + 20);
    v.usbTime = le32(p + 24);
    v.timing = le16(p + 28);
    v.cadence = le16(p + 30);
    return v;
}

inline std::string kindsText(uint8_t k) {
    static const char* const n[8] = {"мышь", "клавиатура", "HID-геймпад", "DS4", "DualSense", "DualSense Edge", "Xbox GIP", "Xbox 360/XInput"};
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (k & (1u << i)) {
            if (!s.empty()) s += " + ";
            s += n[i];
        }
    }
    return s.empty() ? "ничего" : s;
}

inline void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
}

}  // namespace padprobe
