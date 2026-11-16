// pad_probe_ui.h - общее окно проб геймпада: крупная подсказка, обратный отсчёт, четыре стика (плата и Windows),
// строка состояния; поток чтения XInput с журналом; ожидание старта по кнопке A.
// Подсказка в окне - единственное, что видит владелец во время теста: её текст должен совпадать с сообщением в чате.
#pragma once

#include "pad_probe_common.h"

#include <atomic>
#include <cmath>
#include <thread>

namespace padprobe {

inline std::atomic<bool> g_stop{false};
inline std::atomic<const wchar_t*> g_hint{L""};
inline std::atomic<COLORREF> g_hintColor{RGB(0, 0, 0)};
inline std::atomic<int64_t> g_countEnd{0};  // QPC конца текущего шага (0 - отсчёта нет)
inline std::atomic<int> g_tag{-1};          // номер текущего отрезка для журналов (-1 - не писать)
inline std::atomic<bool> g_boardOk{false}, g_xiOk{false};
inline std::atomic<bool> g_boardUsed{true};  // false - проба не открывает COM (pad_probe_view --rate 0): окно пишет об этом, а не "НЕТ ГЕЙМПАДА"
inline std::atomic<int> g_bRx{0}, g_bRy{0}, g_bLx{0}, g_bLy{0}, g_xRx{0}, g_xRy{0}, g_xLx{0}, g_xLy{0};
inline std::atomic<int> g_txRx{0}, g_txRy{0};  // что сейчас инжектится (для рисунка)
inline std::atomic<bool> g_txOn{false};
inline wchar_t g_status[256] = L"";  // вторая строка состояния (пишет только главный поток)
inline int64_t g_t0 = 0;

inline void say(const wchar_t* text, COLORREF color = RGB(0, 0, 0), double countSec = 0.0) {
    g_hintColor = color;
    g_hint = text;
    g_countEnd = countSec > 0 ? now() + us(countSec * 1e6) : 0;
}

#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);

struct XiRow {
    int64_t t;
    int tag;
    uint32_t pkt;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
inline std::vector<XiRow> g_xi;
inline std::atomic<uint64_t> g_xiPackets{0};
// Кнопки и триггеры в Windows для окна: текущие, "были нажаты с прошлого чтения" (окно обновляется раз в 250 мс, а
// нажатие может длиться 20 мс) и счётчик нажатий (фронты кнопок; триггеры - переход через 0).
inline std::atomic<int> g_xButtons{0}, g_xButtonsSeen{0}, g_xLt{0}, g_xRt{0}, g_xLtMax{0}, g_xRtMax{0}, g_xPresses{0};

// Поток XInput: ~8 кГц, строка на каждую смену номера пакета (пока g_tag >= 0).
inline void xinputLoop() {
    HMODULE dll = LoadLibraryA("xinput1_4.dll");
    if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
    if (!dll) return;
    const auto get = reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    if (!get) return;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int slot = -1;
    uint32_t lastPkt = 0;
    bool have = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
        XState s{};
        if (slot < 0) {
            for (DWORD i = 0; i < 4; ++i) {
                if (get(i, &s) == ERROR_SUCCESS) {
                    slot = static_cast<int>(i);
                    break;
                }
            }
            if (slot < 0) {
                g_xiOk = false;
                Sleep(200);
                continue;
            }
        } else if (get(static_cast<DWORD>(slot), &s) != ERROR_SUCCESS) {
            slot = -1;
            g_xiOk = false;
            continue;
        }
        const int64_t t = now();
        g_xiOk = true;
        g_xRx = s.rx;
        g_xRy = s.ry;
        g_xLx = s.lx;
        g_xLy = s.ly;
        {
            const int b = s.buttons, prevB = g_xButtons.exchange(b), rise = b & ~prevB;
            const int prevLt = g_xLt.exchange(s.lt), prevRt = g_xRt.exchange(s.rt);
            if (rise) g_xButtonsSeen.fetch_or(rise);
            int presses = 0;
            for (int bit = rise; bit; bit &= bit - 1) ++presses;
            if (s.lt > 0 && prevLt == 0) ++presses;
            if (s.rt > 0 && prevRt == 0) ++presses;
            if (presses) g_xPresses.fetch_add(presses);
            if (s.lt > g_xLtMax.load()) g_xLtMax = s.lt;
            if (s.rt > g_xRtMax.load()) g_xRtMax = s.rt;
        }
        if (!have || s.pkt != lastPkt) {
            have = true;
            lastPkt = s.pkt;
            g_xiPackets.fetch_add(1, std::memory_order_relaxed);
            const int tag = g_tag.load(std::memory_order_relaxed);
            if (tag >= 0) g_xi.push_back({t - g_t0, tag, s.pkt, s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry});
        }
        const int64_t until = t + us(120.0);
        while (now() < until) YieldProcessor();
    }
}

inline void drawStick(HDC dc, int cx, int cy, int r, int x, int y, bool yDown, COLORREF color, const wchar_t* title) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    MoveToEx(dc, cx - r, cy, nullptr);
    LineTo(dc, cx + r, cy);
    MoveToEx(dc, cx, cy - r, nullptr);
    LineTo(dc, cx, cy + r);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    const int px = cx + x * r / 32768;
    const int py = cy + (yDown ? y : -y) * r / 32768;
    HBRUSH br = CreateSolidBrush(color);
    oldBr = SelectObject(dc, br);
    Ellipse(dc, px - 7, py - 7, px + 8, py + 8);
    SelectObject(dc, oldBr);
    DeleteObject(br);
    wchar_t line[96];
    swprintf(line, 96, L"%ls: %d, %d", title, x, y);
    TextOutW(dc, cx - r, cy + r + 6, line, static_cast<int>(wcslen(line)));
}

inline LRESULT CALLBACK probeWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_stop = true;
        return 0;
    case WM_CLOSE:
        g_stop = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        HFONT big = CreateFontW(40, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT small = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, big);
        SetTextColor(dc, g_hintColor.load());
        RECT tr = {20, 16, rc.right - 20, 150};
        DrawTextW(dc, g_hint.load(), -1, &tr, DT_WORDBREAK | DT_CENTER);
        wchar_t line[320];
        const int64_t ce = g_countEnd.load();
        if (ce) {
            const double left = toMs(ce - now()) / 1000.0;
            if (left > 0) {
                swprintf(line, 320, L"осталось %.0f с", std::ceil(left));
                RECT cr = {20, 150, rc.right - 20, 200};
                DrawTextW(dc, line, -1, &cr, DT_CENTER);
            }
        }
        SelectObject(dc, small);
        SetTextColor(dc, RGB(0, 0, 0));
        const int r = 110, cy = 340;
        const bool boardUsed = g_boardUsed.load();
        drawStick(dc, 150, cy, r, g_bLx.load(), g_bLy.load(), true, RGB(30, 80, 230), boardUsed ? L"плата, левый" : L"плата не опрашивается");
        drawStick(dc, 410, cy, r, g_bRx.load(), g_bRy.load(), true, RGB(30, 80, 230), boardUsed ? L"плата, правый (физика)" : L"плата не опрашивается");
        drawStick(dc, 670, cy, r, g_xLx.load(), g_xLy.load(), false, RGB(220, 30, 30), L"Windows, левый");
        drawStick(dc, 930, cy, r, g_xRx.load(), g_xRy.load(), false, RGB(220, 30, 30), L"Windows, правый");
        if (g_txOn.load()) {
            swprintf(line, 320, L"инжект правого стика: %d, %d", g_txRx.load(), g_txRy.load());
            TextOutW(dc, 410 - r, cy + r + 28, line, static_cast<int>(wcslen(line)));
        }
        swprintf(line, 320, L"плата 0x54: %ls   |   Windows XInput: %ls, пакетов %llu   |   Esc - выход",
            !boardUsed ? L"COM закрыт, команд нет" : g_boardOk.load() ? L"есть" : L"НЕТ ГЕЙМПАДА",
            g_xiOk.load() ? L"есть" : L"НЕТ", static_cast<unsigned long long>(g_xiPackets.load()));
        TextOutW(dc, 20, rc.bottom - 52, line, static_cast<int>(wcslen(line)));
        TextOutW(dc, 20, rc.bottom - 28, g_status, static_cast<int>(wcslen(g_status)));
        SelectObject(dc, oldFont);
        DeleteObject(big);
        DeleteObject(small);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

inline HWND openWindow(const wchar_t* title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = probeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PadProbeWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 120, 80, 1100, 590, nullptr, nullptr,
        wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, 1, 33, nullptr);
    return hwnd;
}

inline void pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Крутить окно sec секунд (или до Esc). false - прервано.
inline bool waitUi(double sec) {
    const int64_t until = now() + us(sec * 1e6);
    while (!g_stop && now() < until) {
        pumpMessages();
        Sleep(5);
    }
    return !g_stop;
}

// Обновить живые значения платы одним опросом 0x54 (из потока, который владеет портом).
inline bool pollBoardOnce(Port& p, PadPhysical* out = nullptr) {
    auto r = p.ask(kOpCtlPhysical, nullptr, 0, 20000.0);
    if (!r || r->size() != 32) {
        g_boardOk = false;
        return false;
    }
    const PadPhysical v = parsePhysical(r->data());
    g_boardOk = true;
    g_bRx = v.s.rx;
    g_bRy = v.s.ry;
    g_bLx = v.s.lx;
    g_bLy = v.s.ly;
    if (out) *out = v;
    return true;
}

// Старт по кнопке A (SOUTH, бит 0 digital_low): сначала отпущена, потом нажата. Порт опрашивает этот же поток.
// Надпись: "Нажми кнопку A на геймпаде, чтобы начать". false - Esc или 300 с без нажатия.
inline bool waitForA(Port& p) {
    say(L"Нажми кнопку A на геймпаде, чтобы начать");
    const int64_t deadline = now() + us(300e6);
    bool released = false;
    while (!g_stop && now() < deadline) {
        pumpMessages();
        PadPhysical v;
        if (pollBoardOnce(p, &v)) {
            const bool down = (v.s.low & 1u) != 0;
            if (!down) released = true;
            if (down && released) return true;
        }
        Sleep(4);
    }
    return false;
}

}  // namespace padprobe
