// pad_probe_record.cpp - проба 3 (фаза 1): запись живого геймпада одновременно с платы и из Windows.
// ТОЛЬКО ЧТЕНИЕ: плате уходит один запрос GET 0x54 замкнутым циклом, ничего не инжектится.
//
// Два источника:
//   плата   - GET 0x54 CONTROLLER_PHYSICAL (вход геймпада в MAKCU, до обработки и инжекта); строка в CSV на каждую
//             смену sequence;
//   Windows - XInputGetState (выход платы через USB1 в ПК); строка в CSV на каждую смену dwPacketNumber.
// Оба со временем QPC в микросекундах от общего нуля, по ним считаются задержка тракта, шаг сетки АЦП,
// полярность осей и текстура (PadTexture19).
//
// Окно ведёт по фазам. Надписи в окне (их же писать в чат перед запуском):
//   старт:  "Нажми кнопку A на геймпаде, чтобы начать"
//   покой:  "НЕ ТРОГАЙ геймпад: положи на стол и убери руки"
//   круг:   "Плавно вращай ПРАВЫЙ стик по кругу"
//   наводка:"Наводись ПРАВЫМ стиком: короткие точные движения, как в игре"
//   вверх:  "Отклони ПРАВЫЙ стик ВВЕРХ до упора и держи"
//   вправо: "Отклони ПРАВЫЙ стик ВПРАВО до упора и держи"
//   конец:  "ГОТОВО. Отпусти стик, окно закроется само"
// Esc или крестик - выход (записанное сохраняется).
//
// Сборка: g++ -std=c++20 -O2 -Wall -Wextra pad_probe_record.cpp -lwinmm -lgdi32 -static -o pad_probe_record.exe
// Запуск: pad_probe_record.exe [--port COM5] [--out PREFIX] [--rest 5] [--circle 10] [--aim 10] [--hold 3] [--nostart]
#include "pad_probe_common.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

using namespace padprobe;

namespace {

struct Phase {
    const wchar_t* text;
    const char* tag;
    double sec;
    COLORREF color;
};

std::atomic<bool> g_stop{false};
std::atomic<int> g_phase{-1};           // -1 ждём старт, 0..N-1 фазы, N конец
std::atomic<int64_t> g_phaseEnd{0};     // QPC конца текущей фазы
std::atomic<const wchar_t*> g_hint{L""};
std::atomic<COLORREF> g_hintColor{RGB(0, 0, 0)};
std::atomic<bool> g_boardOk{false}, g_xiOk{false};
std::atomic<int> g_bRx{0}, g_bRy{0}, g_bLx{0}, g_bLy{0}, g_xRx{0}, g_xRy{0}, g_xLx{0}, g_xLy{0};
std::atomic<uint32_t> g_bLow{0};
std::atomic<uint64_t> g_seqChanges{0}, g_xiPackets{0}, g_ff{0}, g_lost{0}, g_polls{0};
int64_t g_t0 = 0;

struct BoardRow {
    int64_t t;
    int phase;
    PadPhysical v;
};
struct XiRow {
    int64_t t;
    int phase;
    uint32_t pkt;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
std::vector<BoardRow> g_board;
std::vector<XiRow> g_xi;

#pragma pack(push, 1)
struct XState {
    DWORD pkt;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
using XGetState = DWORD(WINAPI*)(DWORD, XState*);

void boardLoop(Port* p) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    bool have = false;
    uint32_t lastSeq = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        g_polls.fetch_add(1, std::memory_order_relaxed);
        auto r = p->ask(kOpCtlPhysical, nullptr, 0, 20000.0);
        if (!r) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            g_boardOk = false;
            continue;
        }
        if (r->size() != 32) {
            g_ff.fetch_add(1, std::memory_order_relaxed);
            g_boardOk = false;
            Sleep(1);
            continue;
        }
        const PadPhysical v = parsePhysical(r->data());
        g_boardOk = true;
        g_bRx = v.s.rx;
        g_bRy = v.s.ry;
        g_bLx = v.s.lx;
        g_bLy = v.s.ly;
        g_bLow = v.s.low;
        if (!have || v.seq != lastSeq) {
            have = true;
            lastSeq = v.seq;
            g_seqChanges.fetch_add(1, std::memory_order_relaxed);
            if (g_phase.load(std::memory_order_relaxed) >= 0) g_board.push_back({now() - g_t0, g_phase.load(std::memory_order_relaxed), v});
        }
    }
}

void xinputLoop() {
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
        g_xiOk = true;
        g_xRx = s.rx;
        g_xRy = s.ry;
        g_xLx = s.lx;
        g_xLy = s.ly;
        if (!have || s.pkt != lastPkt) {
            have = true;
            lastPkt = s.pkt;
            g_xiPackets.fetch_add(1, std::memory_order_relaxed);
            if (g_phase.load(std::memory_order_relaxed) >= 0)
                g_xi.push_back({now() - g_t0, g_phase.load(std::memory_order_relaxed), s.pkt, s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry});
        }
        // ~8 кГц: XInputGetState сам по себе занимает единицы микросекунд
        const int64_t until = now() + us(120.0);
        while (now() < until) YieldProcessor();
    }
}

void drawStick(HDC dc, int cx, int cy, int r, int x, int y, bool yDown, COLORREF color, const wchar_t* title) {
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

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
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
        const wchar_t* hint = g_hint.load();
        DrawTextW(dc, hint, -1, &tr, DT_WORDBREAK | DT_CENTER);
        wchar_t line[256];
        const int ph = g_phase.load();
        if (ph >= 0) {
            const double left = toMs(g_phaseEnd.load() - now()) / 1000.0;
            if (left > 0) {
                swprintf(line, 256, L"осталось %.0f с", std::ceil(left));
                RECT cr = {20, 150, rc.right - 20, 200};
                DrawTextW(dc, line, -1, &cr, DT_CENTER);
            }
        }
        SelectObject(dc, small);
        SetTextColor(dc, RGB(0, 0, 0));
        const int r = 110, cy = 340;
        drawStick(dc, 150, cy, r, g_bLx.load(), g_bLy.load(), true, RGB(30, 80, 230), L"плата, левый");
        drawStick(dc, 410, cy, r, g_bRx.load(), g_bRy.load(), true, RGB(30, 80, 230), L"плата, правый");
        drawStick(dc, 670, cy, r, g_xLx.load(), g_xLy.load(), false, RGB(220, 30, 30), L"Windows, левый");
        drawStick(dc, 930, cy, r, g_xRx.load(), g_xRy.load(), false, RGB(220, 30, 30), L"Windows, правый");
        swprintf(line, 256, L"плата 0x54: %ls, новых снимков %llu, отказов FF %llu, без ответа %llu   |   Windows XInput: %ls, пакетов %llu   |   Esc - выход",
            g_boardOk.load() ? L"есть" : L"НЕТ ГЕЙМПАДА", static_cast<unsigned long long>(g_seqChanges.load()),
            static_cast<unsigned long long>(g_ff.load()), static_cast<unsigned long long>(g_lost.load()), g_xiOk.load() ? L"есть" : L"НЕТ",
            static_cast<unsigned long long>(g_xiPackets.load()));
        TextOutW(dc, 20, rc.bottom - 30, line, static_cast<int>(wcslen(line)));
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

void pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void say(const wchar_t* text, COLORREF color) {
    g_hintColor = color;
    g_hint = text;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();
    std::string port = "COM5", prefix = "pad_record";
    double rest = 5, circle = 10, aim = 10, hold = 3;
    bool noStart = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--port" && more) port = argv[++i];
        else if (a == "--out" && more) prefix = argv[++i];
        else if (a == "--rest" && more) rest = std::atof(argv[++i]);
        else if (a == "--circle" && more) circle = std::atof(argv[++i]);
        else if (a == "--aim" && more) aim = std::atof(argv[++i]);
        else if (a == "--hold" && more) hold = std::atof(argv[++i]);
        else if (a == "--nostart") noStart = true;
        else {
            std::printf("pad_probe_record.exe [--port COM5] [--out PREFIX] [--rest 5] [--circle 10] [--aim 10] [--hold 3] [--nostart]\n");
            return 2;
        }
    }
    const Phase phases[] = {
        {L"НЕ ТРОГАЙ геймпад: положи на стол и убери руки", "rest", rest, RGB(200, 0, 0)},
        {L"Плавно вращай ПРАВЫЙ стик по кругу", "circle", circle, RGB(0, 120, 0)},
        {L"Наводись ПРАВЫМ стиком: короткие точные движения, как в игре", "aim", aim, RGB(0, 120, 0)},
        {L"Отклони ПРАВЫЙ стик ВВЕРХ до упора и держи", "up", hold, RGB(0, 60, 200)},
        {L"Отклони ПРАВЫЙ стик ВПРАВО до упора и держи", "right", hold, RGB(0, 60, 200)},
    };
    const int nPhases = static_cast<int>(sizeof(phases) / sizeof(phases[0]));

    Port p;
    if (!p.open(port)) {
        std::printf("[-] %s не открылся: ошибка %lu (порт занят Toolkit/SDK или USB2 не подключён)\n", port.c_str(), GetLastError());
        return 1;
    }
    uint8_t kinds = 0;
    if (auto r = p.ask(kOpDevice, nullptr, 0, 300000.0)) kinds = r->empty() ? 0 : (*r)[0];
    else {
        std::printf("[-] плата не ответила на DEVICE (0x02)\n");
        return 1;
    }
    std::printf("[i] kinds %02X = %s\n", kinds, kindsText(kinds).c_str());
    if (!(kinds & 0xFC)) {
        std::printf("[-] плата не видит геймпад на USB3: записывать нечего\n");
        return 1;
    }
    timeBeginPeriod(1);
    g_board.reserve(200000);
    g_xi.reserve(200000);
    g_t0 = now();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PadProbeRecord";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"MAKCU: запись живого геймпада (только чтение)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        120, 80, 1100, 560, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, 1, 33, nullptr);

    std::thread board(boardLoop, &p);
    std::thread xi(xinputLoop);

    // Старт по кнопке A (SOUTH, бит 0): сначала она должна быть отпущена, потом нажата.
    if (!noStart) {
        say(L"Нажми кнопку A на геймпаде, чтобы начать", RGB(0, 0, 0));
        const int64_t deadline = now() + us(300e6);
        bool released = false;
        while (!g_stop && now() < deadline) {
            pumpMessages();
            const bool down = g_boardOk.load() && (g_bLow.load() & 1u);
            if (!down && g_boardOk.load()) released = true;
            if (down && released) break;
            Sleep(5);
        }
        if (now() >= deadline) {
            std::printf("[-] за 300 с кнопку A не нажали\n");
            g_stop = true;
        }
    }
    for (int i = 0; i < nPhases && !g_stop; ++i) {
        if (phases[i].sec <= 0) continue;
        say(phases[i].text, phases[i].color);
        g_phaseEnd = now() + us(phases[i].sec * 1e6);
        g_phase = i;
        while (!g_stop && now() < g_phaseEnd.load()) {
            pumpMessages();
            Sleep(5);
        }
    }
    g_phase = nPhases;
    if (!g_stop) {
        say(L"ГОТОВО. Отпусти стик, окно закроется само", RGB(0, 0, 0));
        const int64_t until = now() + us(2e6);
        while (now() < until) {
            pumpMessages();
            Sleep(5);
        }
    }
    g_stop = true;
    board.join();
    xi.join();
    DestroyWindow(hwnd);
    timeEndPeriod(1);

    const std::string bPath = prefix + "_board.csv", xPath = prefix + "_xinput.csv";
    if (FILE* f = std::fopen(bPath.c_str(), "w")) {
        std::fprintf(f, "t_us,phase,seq,usb_time,timing,cadence,low,high,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_board) {
            // после последней фазы (g_phase == nPhases) потоки ещё успевают дописать строки: тег "end", а не чтение за массивом
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), r.phase < nPhases ? phases[r.phase].tag : "end", r.v.seq, r.v.usbTime, r.v.timing, r.v.cadence,
                r.v.s.low, r.v.s.high, r.v.s.lt, r.v.s.rt, r.v.s.lx, r.v.s.ly, r.v.s.rx, r.v.s.ry);
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen(xPath.c_str(), "w")) {
        std::fprintf(f, "t_us,phase,pkt,buttons,lt,rt,lx,ly,rx,ry\n");
        for (const auto& r : g_xi) {
            std::fprintf(f, "%.1f,%s,%u,%u,%u,%u,%d,%d,%d,%d\n", toUs(r.t), r.phase < nPhases ? phases[r.phase].tag : "end", r.pkt, r.buttons, r.lt, r.rt, r.lx, r.ly, r.rx, r.ry);
        }
        std::fclose(f);
    }
    std::printf("[+] плата: %zu строк в %s; Windows: %zu строк в %s\n", g_board.size(), bPath.c_str(), g_xi.size(), xPath.c_str());
    std::printf("    опросов 0x54 %llu, отказов FF %llu, без ответа %llu\n", static_cast<unsigned long long>(g_polls.load()),
        static_cast<unsigned long long>(g_ff.load()), static_cast<unsigned long long>(g_lost.load()));
    for (int i = 0; i < nPhases; ++i) {
        size_t nb = 0, nx = 0;
        for (const auto& r : g_board) nb += r.phase == i;
        for (const auto& r : g_xi) nx += r.phase == i;
        if (phases[i].sec > 0)
            std::printf("    фаза %-6s %.0f с: плата %zu снимков (%.0f/с), Windows %zu пакетов (%.0f/с)\n", phases[i].tag, phases[i].sec, nb, nb / phases[i].sec, nx,
                nx / phases[i].sec);
    }
    p.close();
    return 0;
}
