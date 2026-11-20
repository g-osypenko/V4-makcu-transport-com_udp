// Стенд замеров MAKCU: окно с Raw Input, анализ и режимы. Общий для UDP (udp_way/probes/track_test.cpp)
// и COM (comport_way/track_test_com.cpp). Сначала подключить заголовок транспорта (нужен makcu::detail::Qpc).
//
//   --accuracy S   S секунд опрос GET 0x63 GETPOS подряд против Raw Input тестового окна.
//                  Водите мышью как угодно, последние 2 с - не трогать. На время замера
//                  km.screen = 32767x32767 (позиция в центре, запас +-16k отсчётов), потом
//                  размер возвращается.
//   --screen W H   km.screen на время замера (accuracy/lock: по умолчанию 32767x32767,
//                  lockloss: не трогать); после замера возвращается прежний
//   --lock N       N циклов: 1 с свободно (опрос 0x63), блокировка X и Y, 400 мс держать,
//                  снять. Через раз: 4 x SET 0x60 (как их слал транспорт этого канала) и одна
//                  SET 0x16 MOVE_MASK. Всё время водите мышью кругами. Задержка = от первой
//                  команды блокировки до последнего физического отсчёта в Windows.
//   --lockloss S   S секунд попыток блокировки X+Y (4 x 0x60 с паузами канала и одна 0x16) со
//                  снимком 0x56: сколько команд дошло, отдельно - когда рука двигала мышь и когда
//                  нет. Водите мышью кругами с остановками. --prepoll MS: перед каждой попыткой
//                  MS мс опроса 0x63, команда блокировки - сразу после ответа (как в жизни).
//   --inject N     [--dur MS,MS..] [--lag N,N..] [--stop] N раз на каждую длительность: круг 1 кГц
//                  через эталон, стоп. Хвост = от последнего принятого move() до последнего отчёта
//                  в Windows. Мышь не трогать.
//                  --period US: тик круга (меньше 1000 - очередь набирается быстрее).
//                  --busy: на ходу клик левой раз в 2 с (+1 с) и блокировка X+Y на 0.5 с раз в 10 с (+5 с).
//                  --withsens %: весь прогон держать tx.setSensitivity(%) (инжект от него не зависит).
//                  --mark: последняя запись - метка +600 по X, хвост считается до неё: так он
//                  виден и с рукой на мыши (без руки мышь не трогать).
//   --reflock N    N раз блокировка через эталон, 400 мс, снять; через раз tx.setLock(X) +
//                  tx.setLock(Y) и tx.setLocks({X, Y}).
//                  Задержка = от вызова setLock() до последнего физического отсчёта в Windows.
//                  Всё время водите мышью кругами.
//   --track S      слежение эталона: tx.setTracking(true), S секунд водите мышью, раз в 1.5 с
//                  обе оси блокируются на 0.4 с. tx.physical() против Raw Input.
//   --trackinject N  слежение + N всплесков инжекта по 1 с (мышь не трогать): physical()
//                  должен остаться 0,0; через сколько слежение снова идёт.
//   --pwm N        [--duty %,%..] [--pwmperiod US,US..] [--oneway] частичный пропуск физики: маска X
//                  (SET 0x16) переключается с периодом US мкс, доля пропуска %. N отрезков по 1 с на
//                  каждую пару, между ними 0.5 с свободно. Ось Y свободна и служит мерой: фактическая
//                  доля = (путь X / путь Y) под переключением : то же на свободе. --oneway: маска
//                  только на +X (вправо), мера - путь вправо / путь влево. --both: маска на обеих
//                  осях, как в работе; мера - доля сообщений Raw Input с движением (под маской
//                  отчёты идут, но пустые).
//                  --seg MS: длина отрезка (по умолчанию 1000). Окно показывает текущую долю и
//                  период. Руку плата не читает. Всё время водите мышью кругами.
//   --assist N     [--duty %] [--pwmperiod US,US..] N попыток: в окне красный круг, инжект через эталон
//                  тянет курсор к точке в 30-45 пикс. от его центра, рука доводит в круг и держит
//                  0.25 с. Варианты по очереди: без маски и с переключением маски X+Y через
//                  tx.setLocks() (доля --duty, по умолчанию 50; периоды --pwmperiod, по умолчанию
//                  16000). Считает, сколько команд маски эталон отправил рядом с инжектом, долю руки
//                  после инжекта, время доведения. Доводите курсор рукой в круг.
//   --sens N       [--duty %,%..] [--seg MS] чувствительность через эталон: tx.setSensitivity(%) на
//                  отрезок (по умолчанию 100,75,50,33,25,10 и 1000 мс), между отрезками 100 %. N кругов.
//                  Фактическая доля - по сообщениям Raw Input с движением. Водите мышью кругами.
//   --stress S     [--load inject,sens,track,busy,poll,getpos | idle] [--burst ON,OFF] [--withsens %] [--hand] [--slog FILE]
//                  прогон на стабильность платы: S секунд нагрузка выбранного состава (инжект кругами 1 кГц,
//                  чувствительность --withsens или 50 %, слежение, клик раз в 2 с + блокировка X+Y на 0.5 с
//                  раз в 10 с, poll - опрос кнопок 0x55 1000/с, getpos - опрос позиции 0x63 1000/с рядом с инжектом, только COM; по UDP не включать: темп). --burst: инжект порциями ON мс через OFF мс. Строка раз в секунду в журнал
//                  (fflush, последняя переживёт зависание), раз в 10 с в консоль. Обрыв связи: нагрузка
//                  снимается, 30 с ожидания самовосстановления, выход с кодом 3. --hand: надпись "водите".
//
// Тестовое окно должно быть в фокусе: Raw Input фонового окна Windows 11 прореживает.
// Курсор заперт в окне. Esc - выход; блокировки снимаются и проверяются снимком 0x56.
#pragma once

#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace bench {

using Q = makcu::detail::Qpc;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_cleanDone{false};
int g_scrW = 0, g_scrH = 0;  // --screen: km.screen на время замера (0 - по умолчанию режима)
double g_prePollMs = 0.0;              // --prepoll: lockloss опрашивает 0x63 столько мс перед каждой попыткой
std::vector<int> g_injectDurMs{300};  // --dur: длительности круга в --inject, мс
std::vector<int> g_maxLags;           // --lag N,N..: tx.setMaxLag() в --inject по очереди (пусто - по умолчанию)
bool g_stopCall = false;              // --stop: в конце круга tx.stopMovement()
bool g_busy = false;                  // --busy: клики и блокировки во время движения
bool g_mark = false;                  // --mark: последняя запись - метка, хвост до неё
constexpr int kMarkDx = 600;          //   больше любого отчёта руки
double g_tickUs = 1000.0;             // --period US: тик круга в --inject (меньше 1000 - очередь набирается быстрее)
std::vector<int> g_pwmDuty;           // --duty %,%..: доля пропуска в --pwm (пусто - 50)
std::vector<int> g_pwmPeriodUs;       // --pwmperiod US,US..: период переключения маски в --pwm (пусто - 2000)
bool g_pwmOneWay = false;             // --oneway: в --pwm маска только на +X
bool g_pwmBoth = false;               // --both: в --pwm маска на обеих осях (мера - доля сообщений с движением)
double g_pwmSegMs = 1000.0;           // --seg MS: длина отрезка под переключением в --pwm

// Есть ли у эталона setSensitivity() (в UDP-эталоне пока нет).
template <class Tx, class = void>
struct HasSens : std::false_type {};
template <class Tx>
struct HasSens<Tx, std::void_t<decltype(std::declval<Tx&>().setSensitivity(0))>> : std::true_type {};
// Есть ли у эталона pollButtons() (в UDP-эталоне нет: там опрос 0x55 только внутренний).
template <class Tx, class = void>
struct HasPollButtons : std::false_type {};
template <class Tx>
struct HasPollButtons<Tx, std::void_t<decltype(std::declval<Tx&>().pollButtons())>> : std::true_type {};
// Есть ли у эталона pollPosition() (опрос 0x63 вызывающим; пока только COM).
template <class Tx, class = void>
struct HasPollPosition : std::false_type {};
template <class Tx>
struct HasPollPosition<Tx, std::void_t<decltype(std::declval<Tx&>().pollPosition())>> : std::true_type {};
int g_withSens = 100;  // --withsens %: в --inject держать tx.setSensitivity(%) весь прогон

template <class Tx>
void applySens(Tx& tx, int pct) {
    if constexpr (HasSens<Tx>::value) tx.setSensitivity(pct);
    else (void)tx, (void)pct;
}

BOOL WINAPI onCtrl(DWORD type) {
    g_stop = true;
    if (type == CTRL_CLOSE_EVENT) {  // консоль закрывают: дождаться снятия блокировок
        for (int i = 0; i < 300 && !g_cleanDone.load(); ++i) Sleep(10);
    }
    return TRUE;
}

// ---- Raw Input: каждое сообщение с меткой QPC ------------------------------------------
struct RawEv {
    int64_t t;
    int32_t dx, dy;
    HANDLE dev;
};
constexpr size_t kRawCap = size_t{1} << 20;  // ~17 мин при 1 кГц
std::vector<RawEv> g_raw(kRawCap);
std::atomic<size_t> g_rawN{0};
std::atomic<uint64_t> g_rawTotal{0};              // все сообщения, без предела kRawCap
std::atomic<int64_t> g_rawSumX{0}, g_rawSumY{0};  // путь всех сообщений
std::atomic<int64_t> g_rawLastT{0};               // QPC последнего сообщения
std::atomic<int64_t> g_rawMaxGap{0};              // наибольшая пауза, QPC (--stress сбрасывает раз в секунду)

struct Ui {
    HWND hwnd = nullptr;
    RECT clip{};
    std::atomic<bool> ready{false};
    std::atomic<bool> clicked{false};  // клик в окно - сигнал человека "готов"
    std::atomic<const wchar_t*> big{L""};
    std::atomic<const wchar_t*> small{L""};
    std::atomic<double> rate{0.0};
    std::atomic<uint32_t> polls{0};
    std::atomic<int> tgtX{-1}, tgtY{-1};  // --assist: цель в координатах клиента (-1 - нет)
};
Ui g_ui;
HFONT g_bigFont = nullptr;
constexpr int kTgtR = 14;  // радиус круга цели, пикс.

bool foreground() { return GetForegroundWindow() == g_ui.hwnd; }

void drawStatus(HDC dc, HWND h) {
    RECT r;
    GetClientRect(h, &r);
    FillRect(dc, &r, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    wchar_t line[256];
    swprintf(line, 256, L"Raw Input: %llu сообщ., %.0f /с | опросов 0x63: %u | окно %ls   (Esc - выход)",
        static_cast<unsigned long long>(g_rawN.load()), g_ui.rate.load(), g_ui.polls.load(),
        foreground() ? L"в фокусе" : L"НЕ В ФОКУСЕ - кликните в окно");
    TextOutW(dc, 10, 8, line, static_cast<int>(wcslen(line)));
    const wchar_t* s = g_ui.small.load();
    TextOutW(dc, 10, 30, s, static_cast<int>(wcslen(s)));
    const wchar_t* b = g_ui.big.load();
    HGDIOBJ old = SelectObject(dc, g_bigFont);
    const COLORREF oc = SetTextColor(dc, RGB(200, 0, 0));
    RECT br{0, 70, r.right, r.bottom};
    DrawTextW(dc, b, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oc);
    SelectObject(dc, old);
    const int tx = g_ui.tgtX.load(), ty = g_ui.tgtY.load();
    if (tx >= 0) {
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(220, 0, 0));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, tx - kTgtR, ty - kTgtR, tx + kTgtR + 1, ty + kTgtR + 1);
        MoveToEx(dc, tx - 4, ty, nullptr);
        LineTo(dc, tx + 5, ty);
        MoveToEx(dc, tx, ty - 4, nullptr);
        LineTo(dc, tx, ty + 5);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INPUT: {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        const int64_t now = Q::now();
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            // --stress: счёт без предела буфера, сумма пути и наибольшая пауза между сообщениями.
            g_rawTotal.fetch_add(1, std::memory_order_relaxed);
            g_rawSumX.fetch_add(raw.data.mouse.lLastX, std::memory_order_relaxed);
            g_rawSumY.fetch_add(raw.data.mouse.lLastY, std::memory_order_relaxed);
            if (const int64_t prev = g_rawLastT.exchange(now, std::memory_order_relaxed); prev) {
                if (now - prev > g_rawMaxGap.load(std::memory_order_relaxed)) g_rawMaxGap.store(now - prev, std::memory_order_relaxed);
            }
            const size_t n = g_rawN.load(std::memory_order_relaxed);
            if (n < kRawCap) {
                g_raw[n] = RawEv{now, raw.data.mouse.lLastX, raw.data.mouse.lLastY, raw.header.hDevice};
                g_rawN.store(n + 1, std::memory_order_release);
            }
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_TIMER: {
        if (foreground()) ClipCursor(&g_ui.clip);  // Windows снимает ограничение при смене активного окна
        static size_t prevN = 0;
        static int64_t prevT = 0;
        const int64_t now = Q::now();
        const size_t n = g_rawN.load();
        if (prevT) g_ui.rate = static_cast<double>(n - prevN) * 1e6 / Q::toUs(now - prevT);
        prevN = n;
        prevT = now;
        HDC dc = GetDC(h);
        drawStatus(dc, h);
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        drawStatus(dc, h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_ui.clicked = true;
        return 0;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP:
    case WM_MBUTTONUP: case WM_CONTEXTMENU:
        return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_APPCOMMAND:
        return TRUE;
    case WM_CLOSE:
        g_stop = true;
        return 0;
    default:
        return DefWindowProcW(h, m, wp, lp);
    }
}

void uiThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakcuTrackTest";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_bigFont = CreateFontW(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    const int w = 900, h = 420;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_ui.hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"MAKCU track test (Esc - выход)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    RECT client;
    GetClientRect(g_ui.hwnd, &client);
    POINT tl{client.left + 20, client.top + 60}, br{client.right - 20, client.bottom - 20};
    ClientToScreen(g_ui.hwnd, &tl);
    ClientToScreen(g_ui.hwnd, &br);
    g_ui.clip = RECT{tl.x, tl.y, br.x, br.y};
    SetForegroundWindow(g_ui.hwnd);  // из фонового процесса может не сработать: тест ждёт клика в окно
    if (foreground()) {
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        ClipCursor(&g_ui.clip);
    }
    SetTimer(g_ui.hwnd, 1, 100, nullptr);
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, g_ui.hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    g_ui.ready = true;
    MSG msg;
    while (!g_stop.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    ClipCursor(nullptr);
    DestroyWindow(g_ui.hwnd);
}

void say(const wchar_t* big, const wchar_t* small = L"") {
    g_ui.big = big;
    g_ui.small = small;
    if (g_ui.hwnd) InvalidateRect(g_ui.hwnd, nullptr, FALSE);
}

void checkEsc() {
    static int64_t next = 0;
    const int64_t now = Q::now();
    if (now < next) return;
    next = now + Q::fromUs(20000.0);
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_stop = true;
}

void spinUntil(int64_t t) {
    while (Q::now() < t && !g_stop.load()) {
        checkEsc();
        YieldProcessor();
    }
}

void sleepMs(double ms) { spinUntil(Q::now() + Q::fromUs(ms * 1000.0)); }

// Тест начинается по клику в окно: фокус окно может получить и само, без человека.
bool waitClick(double seconds) {
    const int64_t end = Q::now() + Q::fromUs(seconds * 1e6);
    while (!(g_ui.clicked.load() && foreground()) && Q::now() < end && !g_stop.load()) Sleep(20);
    return g_ui.clicked.load() && foreground();
}

// ---- разбор: Raw Input главного устройства и опросы позиции -----------------------------
struct Poll {
    int64_t ts = 0, tr = 0;
    int x = 0, y = 0;
    int phase = 0;  // 0 свободно, 1 под блокировкой
    int cycle = -1;
};

struct Track {
    HANDLE dev = nullptr;
    std::vector<int64_t> t;
    std::vector<int64_t> cx{0}, cy{0};  // сумма первых k сообщений
    std::vector<int32_t> dx, dy;
    size_t devices = 0;
    // Сколько сообщений пришло не позже at.
    size_t upto(int64_t at) const { return static_cast<size_t>(std::upper_bound(t.begin(), t.end(), at) - t.begin()); }
};

// Главное устройство = больше всего сообщений в окне [from, to].
Track buildTrack(int64_t from, int64_t to) {
    const size_t n = g_rawN.load(std::memory_order_acquire);
    std::map<HANDLE, size_t> count;
    for (size_t i = 0; i < n; ++i) {
        if (g_raw[i].t >= from && g_raw[i].t <= to) ++count[g_raw[i].dev];
    }
    Track tr;
    tr.devices = count.size();
    size_t best = 0;
    for (auto& [d, c] : count) {
        if (c > best) {
            best = c;
            tr.dev = d;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const RawEv& e = g_raw[i];
        if (e.dev != tr.dev || e.t < from || e.t > to) continue;
        tr.t.push_back(e.t);
        tr.dx.push_back(e.dx);
        tr.dy.push_back(e.dy);
        tr.cx.push_back(tr.cx.back() + e.dx);
        tr.cy.push_back(tr.cy.back() + e.dy);
    }
    return tr;
}

double pct(size_t a, size_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }

// Доля интервалов ~1 мс между сообщениями при движении: прорежен ли Raw Input.
void printRawRate(const Track& tr) {
    size_t moving = 0, oneMs = 0;
    for (size_t i = 1; i < tr.t.size(); ++i) {
        const double us = Q::toUs(tr.t[i] - tr.t[i - 1]);
        if (us > 20000.0) continue;
        ++moving;
        if (us >= 500.0 && us < 1500.0) ++oneMs;
    }
    std::printf("Raw Input: %zu сообщений от главного устройства (устройств %zu), интервалов ~1 мс %.1f %%%s\n",
        tr.t.size(), tr.devices, pct(oneMs, moving),
        pct(oneMs, moving) < 60.0 ? "  <-- прорежен? окно было не в фокусе" : "");
}

struct Fit {
    double lagUs = 0.0;
    int64_t offX = 0, offY = 0;
    size_t used = 0, exact = 0, near1 = 0;  // точно / с точностью до соседнего отчёта
    int64_t maxErr = 0;
    std::vector<int64_t> errs;  // по модулю, для полезных опросов
};

// Позиция платы P_i против суммы Raw Input до момента ts_i + lag. Разность D_i = P_i - R должна
// быть постоянной; lag ищется по максимуму совпадений. phase < 0 - все опросы.
Fit fit(const std::vector<Poll>& polls, const Track& tr, int phase) {
    Fit best;
    size_t bestHits = 0;
    bool first = true;
    for (double lag = -3000.0; lag <= 6000.0; lag += 50.0) {
        std::map<std::pair<int64_t, int64_t>, size_t> hist;
        for (const Poll& p : polls) {
            if (phase >= 0 && p.phase != phase) continue;
            const size_t k = tr.upto(p.ts + Q::fromUs(lag));
            ++hist[{p.x - tr.cx[k], p.y - tr.cy[k]}];
        }
        for (auto& [d, c] : hist) {
            if (first || c > bestHits) {
                first = false;
                bestHits = c;
                best.lagUs = lag;
                best.offX = d.first;
                best.offY = d.second;
            }
        }
    }
    for (const Poll& p : polls) {
        if (phase >= 0 && p.phase != phase) continue;
        ++best.used;
        const size_t k = tr.upto(p.ts + Q::fromUs(best.lagUs));
        int64_t err = INT64_MAX;
        for (int dk = -1; dk <= 1; ++dk) {
            const int64_t kk = static_cast<int64_t>(k) + dk;
            if (kk < 0 || kk >= static_cast<int64_t>(tr.cx.size())) continue;
            const int64_t ex = p.x - tr.cx[kk] - best.offX, ey = p.y - tr.cy[kk] - best.offY;
            const int64_t e = std::max(std::llabs(ex), std::llabs(ey));
            if (dk == 0 && e == 0) ++best.exact;
            err = std::min(err, e);
        }
        if (err == 0) ++best.near1;
        best.maxErr = std::max(best.maxErr, err);
        best.errs.push_back(err);
    }
    return best;
}

int64_t quantile(std::vector<int64_t> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
}

void printFit(const char* label, const Fit& f) {
    std::printf("%s: опросов %zu, совпало точно %.2f %%, с точностью до соседнего отчёта %.2f %% | "
                "ошибка (отсчёты) p99 %lld, max %lld | сдвиг времени %+.2f мс\n",
        label, f.used, pct(f.exact, f.used), pct(f.near1, f.used), static_cast<long long>(quantile(f.errs, 0.99)),
        static_cast<long long>(f.maxErr), f.lagUs / 1000.0);
}

// ---- --accuracy ------------------------------------------------------------------------
template <class Board>
int runAccuracy(Board& b, double seconds) {
    const auto scr = b.screen();
    if (!scr) {
        std::printf("[-] нет ответа на GET 0x64\n");
        return 1;
    }
    b.setScreen(g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767);
    int cx = 0, cy = 0;
    b.pos(cx, cy);
    std::printf("km.screen был %dx%d, на время замера %dx%d, позиция %d,%d\n", scr->first, scr->second,
        g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767, cx, cy);
    say(L"ВОДИТЕ МЫШЬЮ", L"как угодно: медленно, быстро, рывки; за 2 с до конца - стоп");
    std::vector<Poll> polls;
    polls.reserve(static_cast<size_t>(seconds * 700.0) + 100);
    const int64_t t0 = Q::now(), tEnd = t0 + Q::fromUs(seconds * 1e6);
    bool stopShown = false;
    size_t clamped = 0;
    while (Q::now() < tEnd && !g_stop.load()) {
        checkEsc();
        if (!stopShown && Q::now() > tEnd - Q::fromUs(2e6)) {
            say(L"СТОП - НЕ ТРОГАЙТЕ МЫШЬ");
            stopShown = true;
        }
        Poll p;
        if (b.pos(p.x, p.y, &p.ts, &p.tr)) {
            if (p.x == 0 || p.y == 0 || p.x >= (g_scrW ? g_scrW : 32767) - 1 || p.y >= (g_scrH ? g_scrH : 32767) - 1) {
                ++clamped;
            }
            polls.push_back(p);
            g_ui.polls = static_cast<uint32_t>(polls.size());
        } else {
            sleepMs(2.0);
        }
    }
    b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    if (polls.size() < 10) {
        std::printf("[-] мало опросов\n");
        return 1;
    }
    Sleep(50);
    const Track tr = buildTrack(polls.front().ts - Q::fromUs(2e6), polls.back().tr + Q::fromUs(50000.0));
    const double dur = Q::toUs(polls.back().tr - polls.front().ts) / 1e6;
    std::printf("опросов 0x63: %zu за %.1f с = %.0f Гц, потеряно %llu, у края экрана %zu\n", polls.size(), dur,
        static_cast<double>(polls.size()) / dur, static_cast<unsigned long long>(b.lost()), clamped);
    printRawRate(tr);
    const Fit f = fit(polls, tr, -1);
    printFit("позиция платы против Raw Input", f);
    // Отчётов между опросами и размер одного шага позиции.
    size_t k0 = tr.upto(polls.front().ts + Q::fromUs(f.lagUs)), maxPer = 0;
    int64_t maxStep = 0, path = 0;
    for (size_t i = 1; i < polls.size(); ++i) {
        const size_t k = tr.upto(polls[i].ts + Q::fromUs(f.lagUs));
        maxPer = std::max(maxPer, k - k0);
        k0 = k;
        maxStep = std::max<int64_t>(maxStep, std::max(std::abs(polls[i].x - polls[i - 1].x), std::abs(polls[i].y - polls[i - 1].y)));
        path += std::abs(polls[i].x - polls[i - 1].x) + std::abs(polls[i].y - polls[i - 1].y);
    }
    std::printf("Raw Input между опросами: в среднем %.2f сообщ., max %zu | путь %lld отсчётов, max шаг за опрос %lld\n",
        static_cast<double>(tr.t.size()) / static_cast<double>(polls.size()), maxPer, static_cast<long long>(path),
        static_cast<long long>(maxStep));
    // Итог после "стоп": расхождение суммы за весь прогон.
    const Poll& last = polls.back();
    const size_t kl = tr.upto(last.ts + Q::fromUs(f.lagUs));
    std::printf("итог (мышь стоит): плата %+lld,%+lld, Raw Input %+lld,%+lld -> расхождение %+lld,%+lld\n",
        static_cast<long long>(last.x - polls.front().x), static_cast<long long>(last.y - polls.front().y),
        static_cast<long long>(tr.cx[kl] - tr.cx[tr.upto(polls.front().ts + Q::fromUs(f.lagUs))]),
        static_cast<long long>(tr.cy[kl] - tr.cy[tr.upto(polls.front().ts + Q::fromUs(f.lagUs))]),
        static_cast<long long>(last.x - tr.cx[kl] - f.offX), static_cast<long long>(last.y - tr.cy[kl] - f.offY));
    return 0;
}

// ---- --lock ----------------------------------------------------------------------------
struct LockCycle {
    int variant = 0;  // 0: 4 x 0x60 с паузой 2 мс, 1: одна 0x16
    int64_t t0 = 0, tLast = 0, tSnap = 0;
    uint16_t snap = 0;
    bool snapOk = false;
    int64_t tU0 = 0, tUSnap = 0;
    uint16_t usnap = 0xFFFF;
};

const char* kVariant[2] = {"4x0x60", "0x16  "};

// Направление: 0 +X, 1 -X, 2 +Y, 3 -Y. Компонента сообщения в этом направлении.
int32_t comp(const Track& tr, size_t i, int d) {
    switch (d) {
    case 0: return std::max(tr.dx[i], 0);
    case 1: return std::max(-tr.dx[i], 0);
    case 2: return std::max(tr.dy[i], 0);
    default: return std::max(-tr.dy[i], 0);
    }
}

template <class Board>
void lockCmd(Board& b, LockCycle& c, bool on) {
    const uint8_t s = on ? 1 : 0;
    int64_t first = 0, last = 0;
    if (c.variant == 0) {
        b.lockSeries(s, first, last);  // +X, -X, +Y, -Y по одной 0x60: так, как их слал транспорт этого канала
    } else {
        const uint8_t pl[4] = {s, s, s, s};  // left right down up
        first = last = b.noReply(0x16, pl, 4);
    }
    int64_t tr = 0;
    const auto l = b.locks(&tr);
    if (on) {
        c.t0 = first;
        c.tLast = last;
        c.tSnap = tr;
        c.snapOk = l.has_value();
        c.snap = l.value_or(0);
    } else {
        c.tU0 = first;
        c.tUSnap = tr;
        c.usnap = l.value_or(0xFFFF);
    }
}

template <class Board>
int runLock(Board& b, int cycles) {
    const auto scr = b.screen();
    if (!scr) {
        std::printf("[-] нет ответа на GET 0x64\n");
        return 1;
    }
    if (!b.unlockAll()) {
        std::printf("[-] не удалось снять блокировки перед тестом\n");
        return 1;
    }
    b.setScreen(g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767);
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ",
        L"мышь будет замирать на 0.4 с - это блокировка, продолжайте водить");
    sleepMs(1500.0);
    std::vector<Poll> polls;
    std::vector<LockCycle> cs;
    const int64_t tStart = Q::now();
    for (int c = 0; c < cycles && !g_stop.load(); ++c) {
        LockCycle lc;
        lc.variant = c % 2;
        auto pollFor = [&](double ms, int phase) {
            const int64_t end = Q::now() + Q::fromUs(ms * 1000.0);
            while (Q::now() < end && !g_stop.load()) {
                checkEsc();
                Poll p;
                p.phase = phase;
                p.cycle = c;
                if (b.pos(p.x, p.y, &p.ts, &p.tr)) {
                    polls.push_back(p);
                    g_ui.polls = static_cast<uint32_t>(polls.size());
                } else {
                    sleepMs(2.0);
                }
            }
        };
        pollFor(1000.0, 0);
        if (g_stop.load()) break;
        lockCmd(b, lc, true);
        pollFor(400.0, 1);
        lockCmd(b, lc, false);
        if (lc.usnap != 0 && !b.unlockAll()) {
            std::printf("[-] блокировка не снялась\n");
            break;
        }
        cs.push_back(lc);
    }
    {
        // хвост свободного движения после последнего цикла: для сверки сдвигов
        const int64_t end = Q::now() + Q::fromUs(300000.0);
        while (Q::now() < end && !g_stop.load()) {
            Poll p;
            p.cycle = static_cast<int>(cs.size());
            if (b.pos(p.x, p.y, &p.ts, &p.tr)) polls.push_back(p);
        }
    }
    const bool clean = b.unlockAll();
    b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    std::printf("блокировки сняты: %s\n", clean ? "да (снимок 0x56 = 0000)" : "НЕТ - запустите example_line.exe --check");
    if (cs.empty()) return 1;
    Sleep(50);
    const Track tr = buildTrack(tStart - Q::fromUs(1e6), Q::now());
    printRawRate(tr);
    const Fit f = fit(polls, tr, 0);
    printFit("до блокировки (свободно): позиция платы против Raw Input", f);

    std::printf("\nцикл вариант | снимок мс | последний физ. отсчёт после 1-й команды, мс (+X -X +Y -Y; '-' рука не шла) "
                "| любое движение | утечка отсчётов | позиция под блок. | сдвиг после | снятие мс\n");
    std::vector<double> anyLat[2], dirLat[2][4];
    for (size_t ci = 0; ci < cs.size(); ++ci) {
        const LockCycle& c = cs[ci];
        const size_t a = tr.upto(c.t0), z = tr.upto(c.tU0);
        char dirs[128] = {0};
        int64_t leak = 0;
        for (int d = 0; d < 4; ++d) {
            // рука шла в этом направлении перед блокировкой (последние 15 мс)?
            int64_t before = 0;
            for (size_t i = tr.upto(c.t0 - Q::fromUs(15000.0)); i < a; ++i) before += comp(tr, i, d);
            int64_t lastT = 0;
            for (size_t i = a; i < z; ++i) {
                const int32_t v = comp(tr, i, d);
                if (v) {
                    lastT = tr.t[i];
                    leak += v;
                }
            }
            char cell[24];
            if (before < 2) {
                std::snprintf(cell, sizeof(cell), "%6s", "-");
            } else {
                const double ms = lastT ? Q::toUs(lastT - c.t0) / 1000.0 : 0.0;
                std::snprintf(cell, sizeof(cell), "%6.2f", ms);
                dirLat[c.variant][d].push_back(ms);
            }
            std::strncat(dirs, cell, sizeof(dirs) - std::strlen(dirs) - 1);
        }
        int64_t lastAny = 0;
        for (size_t i = a; i < z; ++i) {
            if (tr.dx[i] || tr.dy[i]) lastAny = tr.t[i];
        }
        const double anyMs = lastAny ? Q::toUs(lastAny - c.t0) / 1000.0 : 0.0;
        anyLat[c.variant].push_back(anyMs);
        // позиция платы под блокировкой: от снимка до снятия
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        bool h0 = false;
        for (const Poll& p : polls) {
            if (p.cycle != static_cast<int>(ci) || p.phase != 1 || p.ts < c.tSnap) continue;
            if (!h0) {
                hx0 = p.x;
                hy0 = p.y;
                h0 = true;
            }
            hx1 = p.x;
            hy1 = p.y;
        }
        // сдвиг "позиция - Raw Input" между последним опросом до блокировки и первым после снятия
        const Poll* pre = nullptr;
        const Poll* post = nullptr;
        for (const Poll& p : polls) {
            if (p.cycle == static_cast<int>(ci) && p.phase == 0) pre = &p;
            if (p.cycle == static_cast<int>(ci) + 1 && p.phase == 0 && !post) post = &p;
        }
        char shift[32] = "?";
        if (pre && post) {
            const size_t k0 = tr.upto(pre->ts + Q::fromUs(f.lagUs)), k1 = tr.upto(post->ts + Q::fromUs(f.lagUs));
            const int64_t sx = (post->x - pre->x) - (tr.cx[k1] - tr.cx[k0]);
            const int64_t sy = (post->y - pre->y) - (tr.cy[k1] - tr.cy[k0]);
            std::snprintf(shift, sizeof(shift), "%+lld,%+lld", static_cast<long long>(sx), static_cast<long long>(sy));
        }
        // снятие: первое движение после первой команды снятия
        double unl = -1.0;
        for (size_t i = z; i < tr.t.size(); ++i) {
            if (tr.dx[i] || tr.dy[i]) {
                unl = Q::toUs(tr.t[i] - c.tU0) / 1000.0;
                break;
            }
        }
        std::printf("%4zu %s | %04X %5.2f | %s | %6.2f | %6lld | %+5d,%+5d | %9s | %6.2f%s\n", ci, kVariant[c.variant],
            c.snap, c.snapOk ? Q::toUs(c.tSnap - c.t0) / 1000.0 : -1.0, dirs, anyMs, static_cast<long long>(leak),
            hx1 - hx0, hy1 - hy0, shift, unl, c.usnap == 0 ? "" : "  (снято повтором)");
    }
    auto med = [](std::vector<double> v) {
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto mx = [](const std::vector<double>& v) { return v.empty() ? -1.0 : *std::max_element(v.begin(), v.end()); };
    std::printf("\nитог: последний физический отсчёт после первой команды блокировки (мс), медиана / max\n");
    for (int v = 0; v < 2; ++v) {
        std::printf("  %s: любое движение %.2f / %.2f |", kVariant[v], med(anyLat[v]), mx(anyLat[v]));
        const char* dn[4] = {"+X", "-X", "+Y", "-Y"};
        for (int d = 0; d < 4; ++d) {
            std::printf(" %s %.2f / %.2f (n=%zu)", dn[d], med(dirLat[v][d]), mx(dirLat[v][d]), dirLat[v][d].size());
        }
        std::printf("\n");
    }
    return 0;
}

// ---- --lockloss ------------------------------------------------------------------------
// Сколько команд блокировки доходит, когда рука двигает мышь и когда нет. Попытка: блокировка
// (4 x 0x60 с паузой 2/3/4 мс или одна 0x16), снимок 0x56, снятие одной 0x16 со сверкой, 30 мс
// свободно. "Рука шла" = в последние 20 мс перед попыткой были сообщения Raw Input.
template <class Board>
int runLockLoss(Board& b, double seconds) {
    if (!b.unlockAll()) {
        std::printf("[-] не удалось снять блокировки перед тестом\n");
        return 1;
    }
    const auto scr = b.screen();
    if (g_scrW && scr) {
        b.setScreen(g_scrW, g_scrH);
        std::printf("km.screen был %dx%d, на время замера %dx%d\n", scr->first, scr->second, g_scrW, g_scrH);
    } else if (scr) {
        std::printf("km.screen %dx%d\n", scr->first, scr->second);
    }
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ", L"мышь будет дёргаться - это блокировки, продолжайте");
    sleepMs(1500.0);
    char names[4][32];
    for (int v = 0; v < 3; ++v) std::snprintf(names[v], sizeof(names[v]), "4x0x60 пауза %.0f мс", Board::kLossGapUs[v] / 1000.0);
    std::snprintf(names[3], sizeof(names[3]), "0x16 одна        ");
    // [вариант][рука шла?][дошло направлений 0..4, 5 = снимок не пришёл]
    size_t hist[4][2][6] = {};
    size_t snapLost = 0, unlockRepeats = 0;
    const int64_t tEnd = Q::now() + Q::fromUs(seconds * 1e6);
    for (int n = 0; Q::now() < tEnd && !g_stop.load(); ++n) {
        checkEsc();
        const int v = n % 4;
        const int64_t t0 = Q::now();
        const size_t rawNow = g_rawN.load(std::memory_order_acquire);
        bool moving = false;
        for (size_t i = rawNow; i > 0 && g_raw[i - 1].t > t0 - Q::fromUs(20000.0); --i) {
            if (g_raw[i - 1].dx || g_raw[i - 1].dy) {
                moving = true;
                break;
            }
        }
        if (v < 3) {
            const double gap = Board::kLossGapUs[v];
            if (gap <= 0.0) {
                int64_t first = 0, last = 0;
                b.lockSeries(1, first, last);
            } else {
                for (const uint8_t t : {6, 7, 9, 10}) {
                    const uint8_t pl[2] = {t, 1};
                    const int64_t ts = b.noReply(0x60, pl, 2);
                    spinUntil(ts + Q::fromUs(gap));
                }
            }
        } else {
            const uint8_t pl[4] = {1, 1, 1, 1};
            b.noReply(0x16, pl, 4);
        }
        sleepMs(3.0);
        std::optional<uint16_t> l;
        for (int k = 0; k < 3 && !l; ++k) {
            l = b.locks();
            if (!l) {
                ++snapLost;
                sleepMs(3.0);
            }
        }
        int landed = 5;
        if (l) {
            landed = 0;
            for (const int bit : {6, 7, 9, 10}) landed += (*l >> bit) & 1;
        }
        ++hist[v][moving ? 1 : 0][landed];
        for (int k = 0;; ++k) {
            const uint8_t pl[4] = {0, 0, 0, 0};
            b.noReply(0x16, pl, 4);
            sleepMs(1.0);
            const auto u = b.locks();
            if (u && *u == 0) break;
            ++unlockRepeats;
            if (k >= 20) {
                std::printf("[-] блокировка не снимается\n");
                g_stop = true;
                break;
            }
        }
        if (g_prePollMs > 0.0) {
            // как в жизни: позиция читается до самой блокировки, команда - сразу после ответа
            const int64_t end = Q::now() + Q::fromUs(g_prePollMs * 1000.0);
            int x = 0, y = 0;
            while (Q::now() < end && !g_stop.load()) {
                if (!b.pos(x, y)) sleepMs(2.0);
            }
        } else {
            sleepMs(30.0);
        }
    }
    const bool clean = b.unlockAll();
    if (g_scrW && scr) b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    std::printf("блокировки сняты: %s | снимков без ответа %zu, снятий с повтором %zu\n",
        clean ? "да (снимок 0x56 = 0000)" : "НЕТ - запустите example_line.exe --check", snapLost, unlockRepeats);
    std::printf("вариант            | рука | попыток | дошли все 4 | дошло 3 | 2 | 1 | 0 | снимок потерян\n");
    for (int v = 0; v < 4; ++v) {
        for (int m = 1; m >= 0; --m) {
            const size_t* h = hist[v][m];
            size_t total = 0;
            for (int k = 0; k < 6; ++k) total += h[k];
            if (!total) continue;
            std::printf("%s | %s | %7zu | %6zu %5.1f%% | %7zu | %zu | %zu | %zu | %zu\n", names[v], m ? "шла " : "нет ",
                total, h[4], pct(h[4], total), h[3], h[2], h[1], h[0], h[5]);
        }
    }
    return 0;
}

// Все дыры потока отчётов: промежуток > 3 мс между сообщениями. "Догнал" - за дырой пришла пачка
// (отчёты задержались); иначе поток идёт дальше ровно, и за дыру столько отчётов пропало.
inline size_t printHoles(const Track& tr, int64_t tA) {
    size_t holes = 0;
    for (size_t i = 1; i < tr.t.size(); ++i) {
        const double gap = Q::toUs(tr.t[i] - tr.t[i - 1]) / 1000.0;
        if (gap <= 3.0) continue;
        ++holes;
        size_t burst = 0;
        for (size_t k = i + 1; k < tr.t.size() && k < i + 64; ++k) {
            if (Q::toUs(tr.t[k] - tr.t[k - 1]) < 300.0) ++burst;
        }
        std::printf("         дыра: +%.3f с, сообщение №%zu, %.1f мс, за ней быстрых сообщений %zu%s\n", Q::toUs(tr.t[i] - tA) / 1e6, i, gap,
            burst, burst * 2 >= static_cast<size_t>(gap) ? " (догнал)" : "");
    }
    return holes;
}

// Откуда расхождение пути: сравнить сообщения Raw Input с отправленными дельтами по порядку.
// Первое и последнее сообщение, где они разошлись, и что там пришло. Рука на мыши - чужие дельты
// внутри потока при том же числе сообщений; дыра Raw Input - сообщений меньше, чем записей.
inline void explainMismatch(const Track& tr, const std::vector<std::pair<int, int>>& sent) {
    const size_t n = std::min(sent.size(), tr.t.size());
    size_t first = n, last = 0, bad = 0;
    int64_t extraX = 0, extraY = 0;
    int32_t maxExtra = 0;
    for (size_t i = 0; i < n; ++i) {
        const int ex = tr.dx[i] - sent[i].first, ey = tr.dy[i] - sent[i].second;
        if (!ex && !ey) continue;
        if (first == n) first = i;
        last = i;
        ++bad;
        extraX += ex;
        extraY += ey;
        maxExtra = std::max({maxExtra, std::abs(ex), std::abs(ey)});
    }
    if (first == n) {
        std::printf("         разбор: первые %zu сообщений совпали с отправленным один в один, отправлено %zu, сообщений %zu\n", n,
            sent.size(), tr.t.size());
        return;
    }
    std::printf("         разбор: разошлись %zu сообщений из %zu (с №%zu по №%zu, %.1f мс), лишнее %+lld,%+lld, макс. %d; "
                "отправлено %zu, сообщений %zu\n",
        bad, n, first, last, Q::toUs(tr.t[last] - tr.t[first]) / 1000.0, static_cast<long long>(extraX),
        static_cast<long long>(extraY), maxExtra, sent.size(), tr.t.size());
}

// ---- --inject --------------------------------------------------------------------------
template <class T>
int runInject(int trials) {
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] эталон: %s%s\n", tx.info().c_str(), g_stopCall ? " | в конце stopMovement()" : "");
    if (g_withSens < 100) {
        applySens(tx, g_withSens);
        std::printf("[+] чувствительность физической мыши %d %% на весь прогон%s\n", g_withSens,
            HasSens<typename T::Tx>::value ? "" : " - НЕТ: в этом эталоне setSensitivity() нет");
    }
    say(g_mark && !g_busy ? L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ" : L"НЕ ТРОГАЙТЕ МЫШЬ", L"курсор будет описывать круги");
    sleepMs(1500.0);
    std::printf("maxLag | длит., мс | отправлено X,Y | Windows X,Y | первый отчёт, мс | хвост после %s: мс / отчётов | "
                "интервалов 1 мс | склеено | очередь max | ack повторов | записей / сообщений Raw Input | макс. дыра, макс. шаг\n", g_stopCall ? "stopMovement()" : "последнего move()");
    std::vector<double> tails;
    if (g_maxLags.empty()) g_maxLags.push_back(static_cast<int>(tx.maxLag()));
    const int perLag = trials * static_cast<int>(g_injectDurMs.size());
    for (int n = 0; n < perLag * static_cast<int>(g_maxLags.size()) && !g_stop.load(); ++n) {
        const int lagCap = g_maxLags[static_cast<size_t>(n / perLag)];
        tx.setMaxLag(static_cast<uint32_t>(lagCap));
        const int durMs = g_injectDurMs[static_cast<size_t>((n % perLag) / trials)];
        double phase = 0.0, fx = 0.0, fy = 0.0;
        int64_t pendX = 0, pendY = 0, sentX = 0, sentY = 0, tFirst = 0, tLast = 0;
        std::vector<std::pair<int, int>> sentSeq;
        sentSeq.reserve(static_cast<size_t>(durMs) + 16);
        const uint64_t merged0 = T::merged(tx.stats());
        const uint64_t retries0 = T::retries(tx.stats());
        const uint64_t records0 = T::records(tx.stats());
        const int64_t tA = Q::now();
        int64_t due = tA;
        for (int tick = 0; tick < durMs && !g_stop.load(); ++tick) {
            spinUntil(due);
            due += Q::fromUs(g_tickUs);
            if (g_busy) {
                using makcu::LockTarget;
                if (tick % 2000 == 1000) tx.press(makcu::MouseButton::Left);
                if (tick % 2000 == 1040) tx.release(makcu::MouseButton::Left);
                if (tick % 10000 == 5000) tx.setLocks({LockTarget::X, LockTarget::Y}, true);
                if (tick % 10000 == 5500) tx.unlockAll();
            }
            const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
            fx += -r * w * std::sin(phase);
            fy += r * w * std::cos(phase);
            phase += w;
            const int64_t ix = std::llround(fx), iy = std::llround(fy);
            fx -= static_cast<double>(ix);
            fy -= static_cast<double>(iy);
            pendX += ix;
            pendY += iy;
            if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) {
                tLast = Q::now();
                if (!tFirst) tFirst = tLast;
                sentX += pendX;
                sentY += pendY;
                sentSeq.emplace_back(static_cast<int>(pendX), static_cast<int>(pendY));
                pendX = pendY = 0;
            }
        }
        int64_t tEnd = tLast;
        if (g_mark) {
            spinUntil(due);
            pendX += kMarkDx;
            const int64_t giveUp = Q::now() + Q::fromUs(1e6);  // связь пропала: не ждать вечно
            while (!tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY)) && !g_stop.load() && Q::now() < giveUp) YieldProcessor();
            tEnd = tLast = Q::now();
            sentX += pendX;
            sentY += pendY;
            sentSeq.emplace_back(static_cast<int>(pendX), static_cast<int>(pendY));
            pendX = pendY = 0;
        }
        if (g_stopCall) {
            tEnd = Q::now();
            int32_t ddx = 0, ddy = 0;
            tx.stopMovement(&ddx, &ddy);
            sentX -= ddx;  // выброшенное в ПК не попадёт
            sentY -= ddy;
        }
        sleepMs(400.0);
        const Track tr = buildTrack(tA, Q::now());
        int64_t lastT = 0, firstT = 0, prevT = 0;
        size_t after = 0, intervals = 0, oneMs = 0;
        double maxGapMs = 0.0;
        int32_t maxStep = 0;
        for (size_t i = 0; i < tr.t.size(); ++i) {
            if (!tr.dx[i] && !tr.dy[i]) continue;
            if (!firstT) firstT = tr.t[i];
            if (prevT) maxGapMs = std::max(maxGapMs, Q::toUs(tr.t[i] - prevT) / 1000.0);
            maxStep = std::max({maxStep, std::abs(tr.dx[i]), std::abs(tr.dy[i])});
            if (prevT && tr.t[i] <= tEnd) {
                ++intervals;
                const double us = Q::toUs(tr.t[i] - prevT);
                if (us >= 500.0 && us < 1500.0) ++oneMs;
            }
            prevT = tr.t[i];
            lastT = tr.t[i];
            if (tr.t[i] > tEnd) ++after;
        }
        double tail = lastT ? Q::toUs(lastT - tEnd) / 1000.0 : -1.0;
        if (g_mark) {
            tail = -1.0;
            for (size_t i = tr.upto(tEnd); i < tr.t.size(); ++i) {
                if (tr.dx[i] >= kMarkDx / 2) {
                    tail = Q::toUs(tr.t[i] - tEnd) / 1000.0;
                    break;
                }
            }
        }
        tails.push_back(tail);
        const auto st = tx.stats();
        std::printf("%6d | %9d | %+5lld,%+5lld | %+5lld,%+5lld | %6.2f | %6.2f / %zu | %5.1f %% | %llu | %u | %llu | %llu / %zu | %.2f мс, %d%s\n", lagCap, durMs,
            static_cast<long long>(sentX), static_cast<long long>(sentY), static_cast<long long>(tr.cx.back()),
            static_cast<long long>(tr.cy.back()), firstT ? Q::toUs(firstT - tFirst) / 1000.0 : -1.0, tail, after,
            pct(oneMs, intervals), static_cast<unsigned long long>(T::merged(st) - merged0), T::depth(st),
            static_cast<unsigned long long>(T::retries(st) - retries0),
            static_cast<unsigned long long>(T::records(st) - records0), tr.t.size(), maxGapMs, maxStep,
            (tr.cx.back() != sentX || tr.cy.back() != sentY) ? "  <-- путь не совпал (мышь трогали?)" : "");
        if (tr.cx.back() != sentX || tr.cy.back() != sentY) explainMismatch(tr, sentSeq);
        printHoles(tr, tA);
    }
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    if (!tails.empty()) {
        std::sort(tails.begin(), tails.end());
        std::printf("хвост после %s: медиана %.2f мс, max %.2f мс\n",
            g_stopCall ? "stopMovement()" : "последнего move()", tails[tails.size() / 2], tails.back());
    }
    return 0;
}

// ---- --reflock -------------------------------------------------------------------------
// Блокировка так, как её делает программа на эталоне: tx.setLock(X), tx.setLock(Y) - и сколько
// после этого физическое движение ещё доходит до Windows. Водить мышью кругами.
template <class T>
int runRefLock(int cycles) {
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] эталон: %s\n", tx.info().c_str());
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ",
        L"мышь будет замирать на 0.4 с - это блокировка, продолжайте водить");
    sleepMs(1500.0);
    using makcu::LockTarget;
    const uint16_t want = static_cast<uint16_t>(makcu::lockBit(LockTarget::X) | makcu::lockBit(LockTarget::XPos) |
        makcu::lockBit(LockTarget::XNeg) | makcu::lockBit(LockTarget::Y) | makcu::lockBit(LockTarget::YPos) |
        makcu::lockBit(LockTarget::YNeg));
    struct C {
        int64_t t0 = 0, tBoard = 0, tU = 0;
        uint64_t repairs = 0;
        int style = 0;  // 0: setLock(X) + setLock(Y), 1: setLocks({X, Y})
    };
    std::vector<C> cs;
    const int64_t tStart = Q::now();
    for (int c = 0; c < cycles && !g_stop.load(); ++c) {
        sleepMs(1000.0);
        if (g_stop.load()) break;
        C r;
        const uint64_t rep0 = tx.stats().repairs;
        r.style = c % 2;
        r.t0 = Q::now();
        if (r.style == 0) {
            tx.setLock(LockTarget::X, true);
            tx.setLock(LockTarget::Y, true);
        } else {
            tx.setLocks({LockTarget::X, LockTarget::Y}, true);
        }
        const int64_t holdEnd = r.t0 + Q::fromUs(400000.0);
        while (Q::now() < holdEnd && !g_stop.load()) {
            if (!r.tBoard && tx.locksOnBoard() == want) r.tBoard = Q::now();
            YieldProcessor();
        }
        r.tU = Q::now();
        tx.unlockAll();
        const int64_t uEnd = Q::now() + Q::fromUs(300000.0);
        while (tx.locksOnBoard() != 0 && Q::now() < uEnd) Sleep(1);
        r.repairs = tx.stats().repairs - rep0;
        cs.push_back(r);
    }
    sleepMs(300.0);
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    if (cs.empty()) return 1;
    const Track tr = buildTrack(tStart, Q::now());
    printRawRate(tr);
    std::printf("цикл вызов           | последний физ. отсчёт после setLock(), мс (+X -X +Y -Y; '-' рука не шла) | любое движение | "
                "утечка отсчётов | снимок платы мс | повторов\n");
    std::vector<double> anyLat[2];
    const char* styleName[2] = {"setLock X, Y  ", "setLocks({X,Y})"};
    for (size_t ci = 0; ci < cs.size(); ++ci) {
        const C& c = cs[ci];
        const size_t a = tr.upto(c.t0), z = tr.upto(c.tU);
        char dirs[128] = {0};
        int64_t leak = 0;
        for (int d = 0; d < 4; ++d) {
            int64_t before = 0;
            for (size_t i = tr.upto(c.t0 - Q::fromUs(15000.0)); i < a; ++i) before += comp(tr, i, d);
            int64_t lastT = 0;
            for (size_t i = a; i < z; ++i) {
                if (const int32_t v = comp(tr, i, d)) {
                    lastT = tr.t[i];
                    leak += v;
                }
            }
            char cell[24];
            if (before < 2) std::snprintf(cell, sizeof(cell), "%7s", "-");
            else std::snprintf(cell, sizeof(cell), "%7.2f", lastT ? Q::toUs(lastT - c.t0) / 1000.0 : 0.0);
            std::strncat(dirs, cell, sizeof(dirs) - std::strlen(dirs) - 1);
        }
        int64_t lastAny = 0;
        for (size_t i = a; i < z; ++i) {
            if (tr.dx[i] || tr.dy[i]) lastAny = tr.t[i];
        }
        const double anyMs = lastAny ? Q::toUs(lastAny - c.t0) / 1000.0 : 0.0;
        anyLat[c.style].push_back(anyMs);
        std::printf("%4zu %s | %s | %6.2f | %5lld | %6.2f | %llu\n", ci, styleName[c.style], dirs, anyMs, static_cast<long long>(leak),
            c.tBoard ? Q::toUs(c.tBoard - c.t0) / 1000.0 : -1.0, static_cast<unsigned long long>(c.repairs));
    }
    for (int s = 0; s < 2; ++s) {
        std::vector<double>& v = anyLat[s];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        std::printf("итог %s: последнее физическое движение после вызова: медиана %.2f мс, max %.2f мс (n=%zu)\n",
            styleName[s], v[v.size() / 2], v.back(), v.size());
    }
    return 0;
}

// ---- --track / --trackinject: слежение эталона (setTracking/physical) --------------------
// Один шаг круга 1 кГц (тот же, что в --inject): накопленный остаток в fx/fy.
struct Circle {
    double phase = 0.0, fx = 0.0, fy = 0.0;
    void step(int64_t& ix, int64_t& iy) {
        const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
        fx += -r * w * std::sin(phase);
        fy += r * w * std::cos(phase);
        phase += w;
        ix = std::llround(fx);
        iy = std::llround(fy);
        fx -= static_cast<double>(ix);
        fy -= static_cast<double>(iy);
    }
};

// Рука водит мышью; раз в 1.5 с обе оси блокируются на 0.4 с. physical() эталона против Raw Input.
template <class T>
int runTrack(double seconds) {
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    if (!tx.setTracking(true)) {
        std::printf("[-] setTracking(true) не прошёл\n");
        return 1;
    }
    std::printf("[+] эталон: %s | слежение включено\n", tx.info().c_str());
    say(L"ВОДИТЕ МЫШЬЮ", L"мышь будет замирать на 0.4 с (блокировка) - продолжайте; за 2 с до конца - стоп");
    using makcu::LockTarget;
    std::vector<Poll> polls;
    uint64_t lastSamples = tx.physical().samples;
    const int64_t t0 = Q::now(), tEnd = t0 + Q::fromUs(seconds * 1e6);
    int64_t nextLock = t0 + Q::fromUs(1500000.0), lockT = 0;
    bool locked = false, stopShown = false;
    int lockCycles = 0;
    while (Q::now() < tEnd && !g_stop.load()) {
        checkEsc();
        const int64_t now = Q::now();
        if (!stopShown && now > tEnd - Q::fromUs(2e6)) {
            say(L"СТОП - НЕ ТРОГАЙТЕ МЫШЬ");
            stopShown = true;
        }
        if (!locked && now >= nextLock && now < tEnd - Q::fromUs(2.5e6)) {
            tx.setLocks({LockTarget::X, LockTarget::Y}, true);
            locked = true;
            lockT = now;
            ++lockCycles;
        } else if (locked && now >= lockT + Q::fromUs(400000.0)) {
            tx.unlockAll();
            locked = false;
            nextLock = now + Q::fromUs(1100000.0);
        }
        const auto ph = tx.physical();
        if (ph.samples != lastSamples) {
            lastSamples = ph.samples;
            if (ph.live) {
                Poll p;
                p.ts = p.tr = ph.sampleQpc;
                p.x = static_cast<int>(ph.x);
                p.y = static_cast<int>(ph.y);
                polls.push_back(p);
                g_ui.polls = static_cast<uint32_t>(polls.size());
            }
        }
        YieldProcessor();
    }
    if (locked) tx.unlockAll();
    sleepMs(100.0);
    const auto fin = tx.physical();
    tx.setTracking(false);
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    if (polls.size() < 10) {
        std::printf("[-] мало отсчётов слежения\n");
        return 1;
    }
    const Track tr = buildTrack(t0 - Q::fromUs(1e6), Q::now());
    const double dur = Q::toUs(polls.back().ts - polls.front().ts) / 1e6;
    std::printf("отсчётов physical(): %zu за %.1f с = %.0f /с (с паузами под блокировкой), блокировок %d\n", polls.size(),
        dur, static_cast<double>(polls.size()) / dur, lockCycles);
    printRawRate(tr);
    // Время ответа на опрос: плата сняла позицию раньше. Сдвиг ищется в fit() шире.
    std::vector<Poll> shifted = polls;
    for (Poll& p : shifted) p.ts -= Q::fromUs(2000.0);
    const Fit f = fit(shifted, tr, -1);
    printFit("physical() эталона против Raw Input", f);
    const Poll& last = shifted.back();
    const size_t kl = tr.upto(last.ts + Q::fromUs(f.lagUs));
    std::printf("итог (мышь стоит): physical() %+lld,%+lld | расхождение с Raw Input %+lld,%+lld\n",
        static_cast<long long>(fin.x), static_cast<long long>(fin.y), static_cast<long long>(last.x - tr.cx[kl] - f.offX),
        static_cast<long long>(last.y - tr.cy[kl] - f.offY));
    return 0;
}

// Мышь не трогать: всплески инжекта со смещением, physical() должен остаться 0,0.
template <class T>
int runTrackInject(int trials) {
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    if (!tx.setTracking(true)) {
        std::printf("[-] setTracking(true) не прошёл\n");
        return 1;
    }
    std::printf("[+] эталон: %s | слежение включено\n", tx.info().c_str());
    say(L"НЕ ТРОГАЙТЕ МЫШЬ", L"курсор будет описывать круги");
    sleepMs(800.0);
    const auto ph0 = tx.physical();
    std::printf("попытка | инжект X,Y | Windows X,Y | physical() за попытку | слежение снова через, мс после последнего move()\n");
    int bad = 0;
    for (int n = 0; n < trials && !g_stop.load(); ++n) {
        const auto before = tx.physical();
        const int64_t tA = Q::now();
        Circle c;
        int64_t pendX = 0, pendY = 0, sentX = 0, sentY = 0, tLast = 0, due = tA;
        std::vector<std::pair<int, int>> sentSeq;
        sentSeq.reserve(1016);
        for (int tick = 0; tick < 1000 && !g_stop.load(); ++tick) {
            spinUntil(due);
            due += Q::fromUs(1000.0);
            int64_t ix = 0, iy = 0;
            c.step(ix, iy);
            pendX += ix;
            pendY += iy;
            if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) {
                tLast = Q::now();
                sentX += pendX;
                sentY += pendY;
                sentSeq.emplace_back(static_cast<int>(pendX), static_cast<int>(pendY));
                pendX = pendY = 0;
            }
        }
        // Когда слежение снова пошло: первый засчитанный ответ после конца инжекта.
        double resumeMs = -1.0;
        const uint64_t s0 = tx.physical().samples;
        const int64_t waitEnd = Q::now() + Q::fromUs(300000.0);
        while (Q::now() < waitEnd) {
            const auto ph = tx.physical();
            if (ph.samples != s0 && ph.live) {
                resumeMs = Q::toUs(ph.sampleQpc - tLast) / 1000.0;
                break;
            }
            YieldProcessor();
        }
        sleepMs(300.0);
        const auto after = tx.physical();
        const Track tr = buildTrack(tA, Q::now());
        const int64_t dx = after.x - before.x, dy = after.y - before.y;
        if (dx || dy) ++bad;
        std::printf("%7d | %+5lld,%+5lld | %+5lld,%+5lld | %+lld,%+lld%s | %6.2f\n", n, static_cast<long long>(sentX),
            static_cast<long long>(sentY), static_cast<long long>(tr.cx.back()), static_cast<long long>(tr.cy.back()),
            static_cast<long long>(dx), static_cast<long long>(dy), (dx || dy) ? " <-- инжект попал в физику?" : "",
            resumeMs);
        if (tr.cx.back() != sentX || tr.cy.back() != sentY) explainMismatch(tr, sentSeq);
        printHoles(tr, tA);
    }
    const auto fin = tx.physical();
    std::printf("physical() за весь прогон: %+lld,%+lld (должно быть 0,0), попыток с утечкой %d\n",
        static_cast<long long>(fin.x - ph0.x), static_cast<long long>(fin.y - ph0.y), bad);
    tx.setTracking(false);
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    return 0;
}

// ---- --pwm: доля физического движения быстрым переключением маски --------------------------
// Маска направления (SET 0x16) умеет только "пропустить" или "выбросить" отчёт целиком. Если
// переключать её каждые 1-4 мс, до Windows доходит часть отчётов: чувствительность руки падает
// до доли пропуска. Читать руку для этого не нужно (ни 0x63, ни инжекта). Маска ставится только
// на X, ось Y свободна и служит мерой: рука водит кругами, путь по X и по Y на свободе относится
// как base; под переключением отношение падает до base * доля. С --oneway маска только на +X,
// мера - путь вправо к пути влево.
struct PwmSeg {
    int duty = 0, periodUs = 0;
    int64_t t0 = 0, t1 = 0;  // начало первого периода и команда снятия
    uint32_t cmds = 0;
    double lateMaxUs = 0.0;  // на сколько команда ушла позже расписания (ПК)
};

struct PwmSum {
    int64_t num = 0, den = 0;  // путь под маской и путь-мера
    size_t msgs = 0, gaps = 0, oneMs = 0;
    double maxGapMs = 0.0;
    double ratio() const { return den ? static_cast<double>(num) / static_cast<double>(den) : 0.0; }
};

// Сообщения Raw Input в [a, z): путь под маской и путь-мера, ровность потока отчётов.
inline void pwmAdd(PwmSum& s, const Track& tr, int64_t a, int64_t z) {
    const size_t i0 = tr.upto(a), i1 = tr.upto(z);
    for (size_t i = i0; i < i1; ++i) {
        if (g_pwmBoth) {
            // Под маской отчёты идут каждую мс, но пустые: доля = сообщения с движением / все сообщения.
            s.num += (tr.dx[i] || tr.dy[i]) ? 1 : 0;
            ++s.den;
        } else if (g_pwmOneWay) {
            s.num += std::max(tr.dx[i], 0);
            s.den += std::max(-tr.dx[i], 0);
        } else {
            s.num += std::abs(tr.dx[i]);
            s.den += std::abs(tr.dy[i]);
        }
        ++s.msgs;
        if (i > i0) {
            const double us = Q::toUs(tr.t[i] - tr.t[i - 1]);
            if (us < 20000.0) {
                ++s.gaps;
                if (us >= 500.0 && us < 1500.0) ++s.oneMs;
            }
            s.maxGapMs = std::max(s.maxGapMs, us / 1000.0);
        }
    }
}

template <class Board>
int runPwm(Board& b, int rounds) {
    if (!b.unlockAll()) {
        std::printf("[-] не удалось снять блокировки перед тестом\n");
        return 1;
    }
    if (g_pwmDuty.empty()) g_pwmDuty.push_back(50);
    if (g_pwmPeriodUs.empty()) g_pwmPeriodUs.push_back(2000);
    static const wchar_t* const kPwmHint =
        L"водите кругами, не останавливаясь: курсор будет идти медленнее или вставать, это замер";
    static const wchar_t* const kPwmFree = L"СВОБОДНО - ВОДИТЕ КРУГАМИ";
    say(kPwmFree, kPwmHint);
    sleepMs(1500.0);
    auto mask = [&](bool on) {
        const uint8_t s = on ? 1 : 0;
        const uint8_t y = g_pwmBoth ? s : 0;
        const uint8_t pl[4] = {static_cast<uint8_t>(g_pwmOneWay ? 0 : s), s, y, y};  // left right down up
        return b.noReply(0x16, pl, 4);
    };
    // Надписи окна живут до конца прогона: say() хранит указатель.
    static std::vector<std::wstring> labels;
    labels.clear();
    labels.reserve(static_cast<size_t>(rounds) * g_pwmDuty.size() * g_pwmPeriodUs.size() + 1);
    std::vector<PwmSeg> segs;
    const int64_t tStart = Q::now();
    bool stuck = false;
    for (int r = 0; r < rounds && !g_stop.load() && !stuck; ++r) {
        for (const int duty : g_pwmDuty) {
            for (size_t pi = 0; pi < g_pwmPeriodUs.size() && !g_stop.load() && !stuck; ++pi) {
                PwmSeg s;
                s.duty = std::min(std::max(duty, 0), 100);
                s.periodUs = std::max(g_pwmPeriodUs[pi], 500);
                if ((s.duty == 0 || s.duty == 100) && pi > 0) continue;  // без переключения период ни на что не влияет
                sleepMs(500.0);
                wchar_t label[96];
                if (s.duty == 0 || s.duty == 100) {
                    swprintf(label, 96, L"ДОЛЯ %d %% - ВОДИТЕ КРУГАМИ", s.duty);
                } else {
                    swprintf(label, 96, L"ДОЛЯ %d %%, ПЕРИОД %.0f мс - ВОДИТЕ КРУГАМИ", s.duty, s.periodUs / 1000.0);
                }
                labels.emplace_back(label);
                say(labels.back().c_str(), kPwmHint);
                const int64_t period = Q::fromUs(static_cast<double>(s.periodUs)), pass = period * s.duty / 100;
                s.t0 = Q::now();
                const int64_t end = s.t0 + Q::fromUs(g_pwmSegMs * 1000.0);
                auto at = [&](int64_t when, bool on) {
                    spinUntil(when);
                    const int64_t ts = mask(on);
                    ++s.cmds;
                    s.lateMaxUs = std::max(s.lateMaxUs, Q::toUs(ts - when));
                };
                if (s.duty == 0) {
                    at(s.t0, true);
                } else if (s.duty < 100) {
                    // период: сначала пропуск [0, pass), потом маска [pass, period)
                    for (int64_t tk = s.t0; tk + period <= end && !g_stop.load(); tk += period) {
                        at(tk, false);
                        at(tk + pass, true);
                    }
                }
                spinUntil(end);
                s.t1 = mask(false);
                say(kPwmFree, kPwmHint);
                const auto l = b.locks();
                if (!(l && *l == 0) && !b.unlockAll()) {
                    std::printf("[-] маска не снялась\n");
                    stuck = true;
                }
                segs.push_back(s);
            }
        }
    }
    sleepMs(500.0);
    const int64_t tEnd = Q::now();
    const bool clean = b.unlockAll();
    const auto fw = b.ask(0x04);
    say(L"ГОТОВО");
    std::printf("блокировки сняты: %s | плата после прогона %s\n",
        clean ? "да (снимок 0x56 = 0000)" : "НЕТ - запустите example_line.exe --check", fw ? "отвечает" : "НЕ ОТВЕЧАЕТ");
    if (segs.empty()) return 1;
    Sleep(50);
    const Track tr = buildTrack(tStart, Q::now());
    printRawRate(tr);

    // На свободе: между отрезками, с запасом 20 мс после снятия маски.
    PwmSum base;
    pwmAdd(base, tr, tStart, segs.front().t0);
    for (size_t i = 0; i < segs.size(); ++i) {
        pwmAdd(base, tr, segs[i].t1 + Q::fromUs(20000.0), i + 1 < segs.size() ? segs[i + 1].t0 : tEnd);
    }
    const char* what = g_pwmBoth ? "сообщений с движением / всех сообщений" : g_pwmOneWay ? "путь вправо / путь влево" : "путь X / путь Y";
    std::printf("на свободе: %s = %.3f (%lld / %lld), интервалов ~1 мс %.1f %%\n", what, base.ratio(),
        static_cast<long long>(base.num), static_cast<long long>(base.den), pct(base.oneMs, base.gaps));
    if (base.den < 500 || base.num < 500) {
        std::printf("[-] на свободе мало движения: рука не водила кругами, прогон не зачётный\n");
        return 1;
    }
    std::printf("маска: %s. Фактическая доля = (%s под переключением) / (то же на свободе)\n",
        g_pwmBoth ? "X и Y" : g_pwmOneWay ? "только +X (вправо)" : "X в обе стороны", what);
    if (g_pwmBoth) {
        std::printf("    под маской отчёты приходят каждую мс, но пустые; мера верна, пока рука не останавливается\n");
    }
    std::printf("доля %% | период мс | команд/с | опоздание команды max, мс | факт. доля %% | по отрезкам min..max | "
                "интервалов ~1 мс %% | max пауза мс | путь под маской по 8 долям периода, %% | замечание\n");
    for (const int dutyRaw : g_pwmDuty) {
        const int duty = std::min(std::max(dutyRaw, 0), 100);
        for (size_t pi = 0; pi < g_pwmPeriodUs.size(); ++pi) {
            const int per = std::max(g_pwmPeriodUs[pi], 500);
            if ((duty == 0 || duty == 100) && pi > 0) continue;
            PwmSum all;
            double lo = 1e9, hi = -1.0, late = 0.0;
            uint64_t cmds = 0;
            size_t n = 0;
            int64_t phase[8] = {0};
            for (const PwmSeg& s : segs) {
                if (s.duty != duty || s.periodUs != per) continue;
                ++n;
                cmds += s.cmds;
                late = std::max(late, s.lateMaxUs);
                // первые 5 мс не в счёт: первая команда ещё в пути
                const int64_t a = s.t0 + Q::fromUs(5000.0);
                pwmAdd(all, tr, a, s.t1);
                PwmSum one;
                pwmAdd(one, tr, a, s.t1);
                if (one.den >= 100) {
                    const double f = 100.0 * one.ratio() / base.ratio();
                    lo = std::min(lo, f);
                    hi = std::max(hi, f);
                }
                const int64_t period = Q::fromUs(static_cast<double>(per));
                for (size_t i = tr.upto(a); i < tr.upto(s.t1); ++i) {
                    const int64_t ph = ((tr.t[i] - s.t0) % period) * 8 / period;
                    phase[ph] += g_pwmBoth ? std::abs(tr.dx[i]) + std::abs(tr.dy[i]) :
                        g_pwmOneWay ? std::max(tr.dx[i], 0) : std::abs(tr.dx[i]);
                }
            }
            if (!n) continue;
            char hist[96] = {0};
            int64_t total = 0;
            for (const int64_t v : phase) total += v;
            for (const int64_t v : phase) {
                char cell[12];
                std::snprintf(cell, sizeof(cell), "%4.0f", total ? 100.0 * static_cast<double>(v) / static_cast<double>(total) : 0.0);
                std::strncat(hist, cell, sizeof(hist) - std::strlen(hist) - 1);
            }
            const bool moved = g_pwmBoth || all.den >= 300;
            char spread[32] = "-";
            if (hi >= 0.0) std::snprintf(spread, sizeof(spread), "%.0f..%.0f", lo, hi);
            std::printf("%6d | %9.1f | %8.0f | %25.2f | %12.1f | %20s | %19.1f | %12.1f | %s | %s\n", duty, per / 1000.0,
                static_cast<double>(cmds) / (static_cast<double>(n) * g_pwmSegMs / 1000.0), late / 1000.0,
                moved ? 100.0 * all.ratio() / base.ratio() : 0.0, spread, pct(all.oneMs, all.gaps), all.maxGapMs,
                (duty == 0 || duty == 100) ? "                               -" : hist,
                moved ? "" : "рука не шла - не проверено");
        }
    }
    // Паузы в потоке отчётов под переключением: остановка руки или дыра? Мышь без движения отчётов
    // не шлёт, поэтому смотрим путь за 30 мс до и после: быстрая рука за 100+ мс не встаёт и не разгоняется.
    for (const PwmSeg& s : segs) {
        const size_t i0 = tr.upto(s.t0 + Q::fromUs(5000.0)), i1 = tr.upto(s.t1);
        for (size_t i = i0 + 1; i < i1; ++i) {
            const double gapMs = Q::toUs(tr.t[i] - tr.t[i - 1]) / 1000.0;
            if (gapMs < 20.0) continue;
            int64_t before = 0, after = 0;
            for (size_t k = tr.upto(tr.t[i - 1] - Q::fromUs(30000.0)); k < i; ++k) before += std::abs(tr.dx[k]) + std::abs(tr.dy[k]);
            for (size_t k = i; k < tr.upto(tr.t[i] + Q::fromUs(30000.0)); ++k) after += std::abs(tr.dx[k]) + std::abs(tr.dy[k]);
            std::printf("пауза %.1f мс: доля %d %%, период %.1f мс, на %.0f-й мс отрезка | путь за 30 мс до паузы %lld, после %lld отсчётов "
                        "(под маской проходит только доля)\n", gapMs, s.duty, s.periodUs / 1000.0, Q::toUs(tr.t[i - 1] - s.t0) / 1000.0,
                static_cast<long long>(before), static_cast<long long>(after));
        }
    }
    return 0;
}

// ---- --assist: инжект тянет к точке, рука доводит под частичной блокировкой ----------------
// Сценарий владельца: в окне красный круг. Инжект через эталон (замкнутый контур по GetCursorPos,
// 1 кГц) тянет курсор к точке в 30-45 пикс. от центра круга, то есть мимо. Рука доводит курсор в
// круг и держит 0.25 с. Варианты по очереди: без маски (рука 100 %) и с переключением маски X+Y
// через tx.setLocks() (доля --duty, периоды --pwmperiod). Переключение начинается вместе с
// инжектом и идёт до конца попытки. Что считается: сколько команд маски эталон успел отправить
// рядом с инжектом, фактическая доля руки после конца инжекта (сообщения Raw Input с движением /
// все), время доведения, счётчики эталона.
struct AssistTrial {
    int variant = 0;  // 0 - без маски, k - период g_pwmPeriodUs[k-1]
    double dist0 = 0.0, pullMs = 0.0, handMs = 0.0, totalMs = 0.0, pullMissPx = 0.0;
    bool timeout = false;
    int64_t tPullEnd = 0, tEnd = 0;
    uint64_t toggles = 0, lockCmds = 0, records = 0, merged = 0, repairs = 0;
};

inline POINT cursorClient() {
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(g_ui.hwnd, &p);
    return p;
}

inline void showTarget(int x, int y) {
    g_ui.tgtX = x;
    g_ui.tgtY = y;
    if (g_ui.hwnd) InvalidateRect(g_ui.hwnd, nullptr, FALSE);
}

template <class T>
int runAssist(int trials) {
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    if (g_pwmDuty.empty()) g_pwmDuty.push_back(50);
    if (g_pwmPeriodUs.empty()) g_pwmPeriodUs.push_back(16000);
    const int duty = std::min(std::max(g_pwmDuty[0], 0), 100);
    std::printf("[+] эталон: %s | доля руки %d %%, maxLag %u\n", tx.info().c_str(), duty, static_cast<unsigned>(tx.maxLag()));
    using makcu::LockTarget;
    static const wchar_t* const kHint = L"курсор сам едет к красному кругу, но мимо центра: доведите рукой в круг и подержите";
    say(L"ДОВОДИТЕ КУРСОР В КРАСНЫЙ КРУГ", kHint);
    sleepMs(2500.0);

    // Поле целей: область ClipCursor в координатах клиента, с отступом.
    POINT org{0, 0};
    ClientToScreen(g_ui.hwnd, &org);
    const int x0 = g_ui.clip.left - org.x + 50, x1 = g_ui.clip.right - org.x - 50;
    const int y0 = g_ui.clip.top - org.y + 50, y1 = g_ui.clip.bottom - org.y - 50;
    uint32_t rng = static_cast<uint32_t>(Q::now()) | 1u;
    auto rnd = [&rng](int lo, int hi) {  // xorshift: [lo, hi]
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return lo + static_cast<int>(rng % static_cast<uint32_t>(hi - lo + 1));
    };
    const size_t variants = 1 + (duty < 100 ? g_pwmPeriodUs.size() : 0);
    static std::vector<std::wstring> labels;
    labels.clear();
    labels.push_back(L"ДОВЕДИТЕ В КРУГ   |   рука 100 % (маски нет)");
    for (size_t i = 1; i < variants; ++i) {
        wchar_t l[96];
        swprintf(l, 96, L"ДОВЕДИТЕ В КРУГ   |   рука %d %%, период %.0f мс", duty, std::max(g_pwmPeriodUs[i - 1], 500) / 1000.0);
        labels.emplace_back(l);
    }
    std::vector<AssistTrial> rs;
    const int64_t tStart = Q::now();
    for (int n = 0; n < trials && !g_stop.load(); ++n) {
        AssistTrial r;
        r.variant = static_cast<int>(static_cast<size_t>(n) % variants);
        const POINT c0 = cursorClient();
        int gx = 0, gy = 0;
        for (int tries = 0; tries < 200; ++tries) {
            gx = rnd(x0, x1);
            gy = rnd(y0, y1);
            if (std::hypot(gx - c0.x, gy - c0.y) >= 250.0) break;
        }
        // Точка, к которой тянет инжект: в 30-45 пикс. от центра круга.
        const double ang = rnd(0, 6283) / 1000.0, off = rnd(30, 45);
        const double sx = gx + off * std::cos(ang), sy = gy + off * std::sin(ang);
        r.dist0 = std::hypot(gx - c0.x, gy - c0.y);
        say(L"", labels[static_cast<size_t>(r.variant)].c_str());
        showTarget(gx, gy);
        const auto st0 = tx.stats();
        const uint64_t merged0 = T::merged(st0), rec0 = T::records(st0);
        const int64_t period = Q::fromUs(static_cast<double>(std::max(r.variant ? g_pwmPeriodUs[static_cast<size_t>(r.variant) - 1] : 16000, 500)));
        const int64_t pass = period * duty / 100;
        const int64_t t0 = Q::now();
        int64_t due = t0, inSince = 0, tEnter = 0;
        bool maskOn = false, pulling = true;
        double fx = 0.0, fy = 0.0;
        int64_t pendX = 0, pendY = 0;
        for (;;) {
            spinUntil(due);
            due += Q::fromUs(1000.0);
            const int64_t now = Q::now();
            if (g_stop.load()) break;
            if (now - t0 > Q::fromUs(6e6)) {
                r.timeout = true;
                break;
            }
            if (r.variant) {
                const bool want = (now - t0) % period >= pass;
                if (want != maskOn) {
                    tx.setLocks({LockTarget::X, LockTarget::Y}, want);
                    maskOn = want;
                    ++r.toggles;
                }
            }
            const POINT p = cursorClient();
            if (pulling) {
                const double ex = sx - p.x, ey = sy - p.y, d = std::hypot(ex, ey);
                if (d <= 4.0 || now - t0 > Q::fromUs(2.5e6)) {
                    pulling = false;
                    r.tPullEnd = now;
                    r.pullMs = Q::toUs(now - t0) / 1000.0;
                    r.pullMissPx = d;
                } else {
                    // скорость: 5 1/с от расстояния, 120...1200 пикс/с; шаг за тик не длиннее остатка
                    const double v = std::min(std::max(5.0 * d, 120.0), 1200.0), step = std::min(v * 0.001, d);
                    fx += ex / d * step;
                    fy += ey / d * step;
                    const int64_t ix = std::llround(fx), iy = std::llround(fy);
                    fx -= static_cast<double>(ix);
                    fy -= static_cast<double>(iy);
                    pendX += ix;
                    pendY += iy;
                    if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) pendX = pendY = 0;
                }
            }
            if (std::hypot(gx - p.x, gy - p.y) <= kTgtR) {
                if (!inSince) inSince = now;
                if (now - inSince >= Q::fromUs(250000.0)) {
                    tEnter = inSince;
                    break;
                }
            } else {
                inSince = 0;
            }
        }
        r.tEnd = Q::now();
        if (pulling) {  // рука довела раньше, чем инжект доехал
            r.tPullEnd = r.tEnd;
            r.pullMs = Q::toUs(r.tEnd - t0) / 1000.0;
        }
        tx.unlockAll();
        showTarget(-1, -1);
        const int64_t uEnd = Q::now() + Q::fromUs(300000.0);
        while (tx.locksOnBoard() != 0 && Q::now() < uEnd && !g_stop.load()) Sleep(1);
        const auto st1 = tx.stats();
        r.lockCmds = st1.lockCommands - st0.lockCommands;
        r.repairs = st1.repairs - st0.repairs;
        r.records = T::records(st1) - rec0;
        r.merged = T::merged(st1) - merged0;
        r.totalMs = tEnter ? Q::toUs(tEnter - t0) / 1000.0 : Q::toUs(r.tEnd - t0) / 1000.0;
        r.handMs = tEnter > r.tPullEnd ? Q::toUs(tEnter - r.tPullEnd) / 1000.0 : 0.0;
        rs.push_back(r);
        sleepMs(500.0);
    }
    showTarget(-1, -1);
    tx.unlockAll();
    sleepMs(300.0);
    std::printf("%s\n", tx.statsLine().c_str());
    const uint16_t left = tx.locksOnBoard();
    tx.close();
    say(L"ГОТОВО");
    std::printf("блокировки на плате после прогона: %04X%s\n", left, left ? "  <-- НЕ СНЯТЫ: запустите example_line.exe --check" : " (сняты)");
    if (rs.empty()) return 1;
    Sleep(50);
    const Track tr = buildTrack(tStart, Q::now());
    printRawRate(tr);
    auto varName = [&](int v) {
        char b[48];
        if (v == 0) std::snprintf(b, sizeof(b), "рука 100 %%      ");
        else std::snprintf(b, sizeof(b), "рука %2d %%, %2.0f мс", duty, std::max(g_pwmPeriodUs[static_cast<size_t>(v) - 1], 500) / 1000.0);
        return std::string(b);
    };
    // Доля руки после конца инжекта: сообщения с движением / все (под маской отчёты пустые).
    auto handShare = [&](const AssistTrial& r, size_t& all) {
        size_t nz = 0;
        all = 0;
        for (size_t i = tr.upto(r.tPullEnd + Q::fromUs(15000.0)); i < tr.upto(r.tEnd); ++i) {
            ++all;
            if (tr.dx[i] || tr.dy[i]) ++nz;
        }
        return pct(nz, all);
    };
    std::printf("\n №  | вариант           | до круга, пикс | инжект тянул, мс (мимо точки, пикс) | доведение рукой, мс | всего, мс | "
                "переключений задано / команд маски ушло | доля руки после инжекта, %% (сообщений) | записей | склеено | повторов\n");
    for (size_t i = 0; i < rs.size(); ++i) {
        const AssistTrial& r = rs[i];
        size_t all = 0;
        const double sh = handShare(r, all);
        std::printf("%3zu | %s | %14.0f | %16.0f (%4.1f) | %19.0f | %9.0f | %10llu / %-10llu | %14.1f (%zu) | %7llu | %7llu | %llu%s\n", i,
            varName(r.variant).c_str(), r.dist0, r.pullMs, r.pullMissPx, r.handMs, r.totalMs,
            static_cast<unsigned long long>(r.toggles), static_cast<unsigned long long>(r.lockCmds), sh, all,
            static_cast<unsigned long long>(r.records), static_cast<unsigned long long>(r.merged),
            static_cast<unsigned long long>(r.repairs),
            r.timeout ? "  <-- не доведено за 6 с" : r.handMs == 0.0 ? "  <-- рука довела раньше, чем инжект доехал" : "");
    }
    auto med = [](std::vector<double> v) {
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("\nитог по вариантам (медианы; доля руки и команды - суммой)\n");
    for (size_t v = 0; v < variants; ++v) {
        std::vector<double> pull, hand, total;
        uint64_t tog = 0, cmds = 0, recs = 0, mrg = 0, rep = 0;
        size_t nz = 0, all = 0, n = 0, to = 0;
        double secs = 0.0;
        for (const AssistTrial& r : rs) {
            if (r.variant != static_cast<int>(v)) continue;
            ++n;
            to += r.timeout ? 1 : 0;
            pull.push_back(r.pullMs);
            hand.push_back(r.handMs);
            total.push_back(r.totalMs);
            tog += r.toggles;
            cmds += r.lockCmds;
            recs += r.records;
            mrg += r.merged;
            rep += r.repairs;
            secs += Q::toUs(r.tEnd - r.tPullEnd + Q::fromUs(r.pullMs * 1000.0)) / 1e6;
            for (size_t i = tr.upto(r.tPullEnd + Q::fromUs(15000.0)); i < tr.upto(r.tEnd); ++i) {
                ++all;
                if (tr.dx[i] || tr.dy[i]) ++nz;
            }
        }
        if (!n) continue;
        std::printf("  %s: попыток %zu, не доведено %zu | инжект тянул %.0f мс | доведение рукой %.0f мс | всего %.0f мс | "
                    "переключений задано %llu, команд маски ушло %llu (%.0f /с) | доля руки после инжекта %.1f %% | "
                    "записей %llu, склеено %llu (%.1f %%), повторов %llu\n",
            varName(static_cast<int>(v)).c_str(), n, to, med(pull), med(hand), med(total), static_cast<unsigned long long>(tog),
            static_cast<unsigned long long>(cmds), secs > 0.0 ? static_cast<double>(cmds) / secs : 0.0, pct(nz, all),
            static_cast<unsigned long long>(recs), static_cast<unsigned long long>(mrg), pct(mrg, recs + mrg),
            static_cast<unsigned long long>(rep));
    }
    return 0;
}

// ---- --sens: чувствительность физической мыши через эталон (tx.setSensitivity) -------------
// Рука водит кругами. Отрезок --seg мс с заданной чувствительностью, между отрезками 0.5 с при
// 100 %. Фактическая доля = сообщения Raw Input с движением / все сообщения (под маской отчёты
// идут каждую мс, но пустые), делённая на то же на свободе.
template <class T>
int runSens(int rounds) {
    if constexpr (!HasSens<typename T::Tx>::value) {
        (void)rounds;
        std::printf("[-] в эталоне этого канала setSensitivity() пока нет\n");
        return 1;
    } else {
        typename T::Tx tx;
        std::string err;
        if (!T::open(tx, err)) {
            std::printf("[-] %s\n", err.c_str());
            return 1;
        }
        if (g_pwmDuty.empty()) g_pwmDuty = {100, 75, 50, 33, 25, 10};
        std::printf("[+] эталон: %s\n", tx.info().c_str());
        static const wchar_t* const kHint =
            L"водите кругами, не останавливаясь: курсор будет идти медленнее, это замер чувствительности";
        static const wchar_t* const kFree = L"100 % - ВОДИТЕ КРУГАМИ";
        say(kFree, kHint);
        sleepMs(1500.0);
        static std::vector<std::wstring> labels;
        labels.clear();
        labels.reserve(static_cast<size_t>(rounds) * g_pwmDuty.size() + 1);
        struct Seg {
            int pct = 0;
            int64_t t0 = 0, t1 = 0;
            uint64_t toggles = 0;
        };
        std::vector<Seg> segs;
        const int64_t tStart = Q::now();
        for (int r = 0; r < rounds && !g_stop.load(); ++r) {
            for (const int d : g_pwmDuty) {
                if (g_stop.load()) break;
                sleepMs(500.0);
                Seg s;
                s.pct = std::min(std::max(d, 0), 100);
                wchar_t label[96];
                swprintf(label, 96, L"ЧУВСТВИТЕЛЬНОСТЬ %d %% - ВОДИТЕ КРУГАМИ", s.pct);
                labels.emplace_back(label);
                say(labels.back().c_str(), kHint);
                const uint64_t tog0 = tx.stats().sensToggles;
                tx.setSensitivity(s.pct);
                s.t0 = Q::now();
                sleepMs(g_pwmSegMs);
                s.t1 = Q::now();
                tx.setSensitivity(100);
                s.toggles = tx.stats().sensToggles - tog0;
                say(kFree, kHint);
                segs.push_back(s);
            }
        }
        sleepMs(500.0);
        const int64_t tEnd = Q::now();
        tx.setSensitivity(100);
        sleepMs(100.0);
        std::printf("%s\n", tx.statsLine().c_str());
        const uint16_t left = tx.locksOnBoard();
        tx.close();
        say(L"ГОТОВО");
        std::printf("блокировки на плате перед close(): %04X%s\n", left, left ? "  <-- маска осталась" : " (сняты)");
        if (segs.empty()) return 1;
        Sleep(50);
        const Track tr = buildTrack(tStart, Q::now());
        printRawRate(tr);
        struct Sum {
            size_t nz = 0, all = 0, gaps = 0, oneMs = 0;
            double maxGapMs = 0.0;
        };
        auto add = [&](Sum& s, int64_t a, int64_t z) {
            const size_t i0 = tr.upto(a), i1 = tr.upto(z);
            for (size_t i = i0; i < i1; ++i) {
                ++s.all;
                if (tr.dx[i] || tr.dy[i]) ++s.nz;
                if (i > i0) {
                    const double us = Q::toUs(tr.t[i] - tr.t[i - 1]);
                    if (us < 20000.0) {
                        ++s.gaps;
                        if (us >= 500.0 && us < 1500.0) ++s.oneMs;
                    }
                    s.maxGapMs = std::max(s.maxGapMs, us / 1000.0);
                }
            }
        };
        Sum base;
        add(base, tStart, segs.front().t0);
        for (size_t i = 0; i < segs.size(); ++i) {
            add(base, segs[i].t1 + Q::fromUs(20000.0), i + 1 < segs.size() ? segs[i + 1].t0 : tEnd);
        }
        const double baseShare = base.all ? static_cast<double>(base.nz) / static_cast<double>(base.all) : 0.0;
        std::printf("на 100 %%: сообщений с движением %zu из %zu (%.1f %%), интервалов ~1 мс %.1f %%\n", base.nz, base.all,
            100.0 * baseShare, pct(base.oneMs, base.gaps));
        if (base.nz < 500) {
            std::printf("[-] на свободе мало движения: рука не водила кругами, прогон не зачётный\n");
            return 1;
        }
        std::printf("задано %% | факт. доля %% | по отрезкам min..max | переключений маски в с | интервалов ~1 мс %% | max пауза мс | сообщений\n");
        for (const int dRaw : g_pwmDuty) {
            const int d = std::min(std::max(dRaw, 0), 100);
            Sum all;
            double lo = 1e9, hi = -1.0, secs = 0.0;
            uint64_t tog = 0;
            for (const Seg& s : segs) {
                if (s.pct != d) continue;
                const int64_t a = s.t0 + Q::fromUs(10000.0);  // первые 10 мс: значение ещё применяется
                add(all, a, s.t1);
                Sum one;
                add(one, a, s.t1);
                if (one.all >= 200) {
                    const double f = pct(one.nz, one.all) / baseShare;
                    lo = std::min(lo, f);
                    hi = std::max(hi, f);
                }
                tog += s.toggles;
                secs += Q::toUs(s.t1 - s.t0) / 1e6;
            }
            if (!all.all) continue;
            char spread[32] = "-";
            if (hi >= 0.0) std::snprintf(spread, sizeof(spread), "%.1f..%.1f", lo, hi);
            std::printf("%8d | %12.1f | %20s | %22.0f | %19.1f | %12.1f | %zu\n", d, pct(all.nz, all.all) / baseShare, spread,
                secs > 0.0 ? static_cast<double>(tog) / secs : 0.0, pct(all.oneMs, all.gaps), all.maxGapMs, all.all);
        }
        return 0;
    }
}

// ---- --stress ----------------------------------------------------------------------------
// Долгий прогон на стабильность платы: нагрузка выбранного состава, строка раз в секунду в файл
// (fflush после каждой, чтобы последняя пережила зависание) и раз в 10 с в консоль. На обрыве
// связи нагрузка снимается, стенд ждёт до 30 с, вернётся ли плата сама (V4.122 обещает
// самовосстановление USB3), и выходит.
std::string g_stressLoad = "inject";  // --load inject,sens,track,busy (через запятую; idle - ничего)
std::vector<int> g_burst;             // --burst ON,OFF: инжект порциями, мс (пусто - непрерывно)
std::string g_stressLog;              // --slog FILE (пусто - stress_ДАТА_ВРЕМЯ.log в текущей папке)
bool g_hand = false;                  // --hand: рука водит (надпись окна), иначе "не трогайте"

inline bool stressHas(const char* k) {
    return ("," + g_stressLoad + ",").find(std::string(",") + k + ",") != std::string::npos;
}

template <class T>
int runStress(double seconds) {
    const bool inj = stressHas("inject"), sens = stressHas("sens"), track = stressHas("track"), busy = stressHas("busy");
    const bool poll = stressHas("poll");      // опрос кнопок 0x55 каждый тик (1000/с) рядом с инжектом
    const bool getpos = stressHas("getpos");  // опрос позиции 0x63 каждый тик (1000/с) рядом с инжектом - как km.getpos у владельца
    using TxT = typename T::Tx;
    // Опрос вызывающим: запросов и ответов 0x55 + 0x63 (у эталона без метода - 0).
    auto pollAsk = [](const auto& s) -> uint64_t {
        uint64_t n = s.buttonPolls;
        if constexpr (HasPollPosition<TxT>::value) n += s.posAsk;
        return n;
    };
    auto pollRep = [](const auto& s) -> uint64_t {
        uint64_t n = s.buttonReplies;
        if constexpr (HasPollPosition<TxT>::value) n += s.posAnyReplies;
        return n;
    };
    typename T::Tx tx;
    std::string err;
    if (!T::open(tx, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char path[96];
    if (g_stressLog.empty()) {
        std::snprintf(path, sizeof(path), "stress_%04u%02u%02u_%02u%02u%02u.log", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
            lt.wMinute, lt.wSecond);
        g_stressLog = path;
    }
    FILE* lf = std::fopen(g_stressLog.c_str(), "a");
    auto out = [&](bool console, const char* fmt, auto... args) {
        char line[512];
        std::snprintf(line, sizeof(line), fmt, args...);
        if (console) {
            std::fputs(line, stdout);
            std::fflush(stdout);
        }
        if (lf) {
            std::fputs(line, lf);
            std::fflush(lf);
        }
    };
    auto clock = [] {
        SYSTEMTIME t;
        GetLocalTime(&t);
        static char s[16];
        std::snprintf(s, sizeof(s), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
        return std::string(s);
    };
    const int sensPct = g_withSens < 100 ? g_withSens : 50;
    out(true, "[+] эталон: %s\n", tx.info().c_str());
    out(true, "[+] --stress %.0f с, старт %s, нагрузка: %s%s%s%s%s, порции %s, рука: %s, журнал %s\n", seconds, clock().c_str(),
        inj ? "инжект 1 кГц " : "", sens ? "чувствительность " : "", track ? "слежение " : "",
        (std::string(busy ? "клики+блокировки " : "") + (poll ? "опрос-0x55-1кГц " : "") + (getpos ? "опрос-позиции-0x63-1кГц " : "")).c_str(),
        (inj || sens || track || busy || poll || getpos) ? "" : "простой", g_burst.size() >= 2 ? (std::to_string(g_burst[0]) + "/" + std::to_string(g_burst[1]) + " мс").c_str() : "нет",
        g_hand ? "водит" : "не трогать", g_stressLog.c_str());
    if (sens) {
        applySens(tx, sensPct);
        out(true, "[+] чувствительность %d %%%s\n", sensPct, HasSens<typename T::Tx>::value ? "" : " - НЕТ в этом эталоне");
    }
    if (track && !tx.setTracking(true)) out(true, "[-] setTracking(true) не прошёл\n");
    static const wchar_t* const kHintInj = L"прогон на стабильность: курсор ходит кругами; Esc - досрочно";
    static const wchar_t* const kHintIdle = L"прогон на стабильность; Esc - досрочно";
    say(g_hand ? L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ" : L"НЕ ТРОГАЙТЕ МЫШЬ", inj ? kHintInj : kHintIdle);
    sleepMs(1500.0);
    if (getpos && !HasPollPosition<TxT>::value) out(true, "[-] getpos: в этом эталоне pollPosition() нет - опроса не будет\n");
    if (getpos && track) out(true, "[!] getpos вместе с track: ответы опроса смешаются со слежением\n");
    out(true, "время    |    с | move/с | Raw/с | max пауза Raw, мс | записей | склеено | 0x63 отв./с | маска/с | опрос запр./отв. в с | связь | обрывов | окно в фокусе\n");
    const auto s0 = tx.stats();
    const uint64_t raw0 = g_rawTotal.load();
    const int64_t sx0 = g_rawSumX.load(), sy0 = g_rawSumY.load();
    const int64_t t0 = Q::now(), sec = Q::fromUs(1e6);
    int64_t due = t0, nextRep = t0 + sec, lostAt = 0;
    int64_t sentX = 0, sentY = 0, pendX = 0, pendY = 0;
    uint64_t moves = 0, movesPrev = 0, rawPrev = raw0, posPrev = s0.posReplies, togPrev = s0.sensToggles;
    uint64_t bpPrev = pollAsk(s0), brPrev = pollRep(s0);
    double phase = 0.0, fx = 0.0, fy = 0.0;
    int silentS = 0, rep = 0;
    bool rawSilentNoted = false, back = false;
    g_rawMaxGap.store(0);
    for (uint64_t tick = 0; !g_stop.load(); ++tick) {
        spinUntil(due);
        due += Q::fromUs(1000.0);
        const int64_t now = Q::now();
        if (!lostAt && now - t0 >= Q::fromUs(seconds * 1e6)) break;
        if (busy && !lostAt) {
            using makcu::LockTarget;
            if (tick % 2000 == 1000) tx.press(makcu::MouseButton::Left);
            if (tick % 2000 == 1040) tx.release(makcu::MouseButton::Left);
            if (tick % 10000 == 5000) tx.setLocks({LockTarget::X, LockTarget::Y}, true);
            if (tick % 10000 == 5500) tx.unlockAll();
        }
        if constexpr (HasPollButtons<typename T::Tx>::value) {
            if (poll && !lostAt) tx.pollButtons();
        }
        if constexpr (HasPollPosition<TxT>::value) {
            if (getpos && !lostAt) tx.pollPosition();
        }
        bool on = inj && !lostAt;
        if (on && g_burst.size() >= 2 && g_burst[0] + g_burst[1] > 0) {
            on = static_cast<int>(tick % static_cast<uint64_t>(g_burst[0] + g_burst[1])) < g_burst[0];
        }
        if (on) {
            const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
            fx += -r * w * std::sin(phase);
            fy += r * w * std::cos(phase);
            phase += w;
            const int64_t ix = std::llround(fx), iy = std::llround(fy);
            fx -= static_cast<double>(ix);
            fy -= static_cast<double>(iy);
            pendX += ix;
            pendY += iy;
            if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) {
                ++moves;
                sentX += pendX;
                sentY += pendY;
                pendX = pendY = 0;
            }
        }
        if (now < nextRep) continue;
        nextRep += sec;
        ++rep;
        const auto st = tx.stats();
        const uint64_t raw = g_rawTotal.load();
        const double gapMs = Q::toUs(g_rawMaxGap.exchange(0)) / 1000.0;
        const double el = Q::toUs(now - t0) / 1e6;
        const uint64_t mv = moves - movesPrev, rw = raw - rawPrev;
        out(rep % 10 == 0 || lostAt, "%s | %4.0f | %6llu | %5llu | %17.1f | %7llu | %7llu | %11llu | %7llu | %9llu/%-9llu | %5s | %7llu | %s\n", clock().c_str(), el,
            static_cast<unsigned long long>(mv), static_cast<unsigned long long>(rw), gapMs,
            static_cast<unsigned long long>(T::records(st) - T::records(s0)), static_cast<unsigned long long>(T::merged(st) - T::merged(s0)),
            static_cast<unsigned long long>(st.posReplies - posPrev), static_cast<unsigned long long>(st.sensToggles - togPrev),
            static_cast<unsigned long long>(pollAsk(st) - bpPrev), static_cast<unsigned long long>(pollRep(st) - brPrev),
            st.connected ? "да" : "НЕТ", static_cast<unsigned long long>(st.linkLost - s0.linkLost),
            foreground() ? "да" : "НЕТ (Raw Input прорежен)");
        // Инжект шёл всю секунду, а Raw Input молчит: мышь в ПК умерла раньше, чем транспорт заметил.
        if (inj && !lostAt && g_burst.empty() && mv > 500 && rw == 0) {
            ++silentS;
            if (!rawSilentNoted) {
                out(true, "[!] %s, %.0f с: move() идут (%llu/с), а Raw Input молчит\n", clock().c_str(), el, static_cast<unsigned long long>(mv));
                rawSilentNoted = true;
            }
        }
        movesPrev = moves;
        rawPrev = raw;
        posPrev = st.posReplies;
        togPrev = st.sensToggles;
        bpPrev = pollAsk(st);
        brPrev = pollRep(st);
        if (!lostAt && st.linkLost != s0.linkLost) {
            lostAt = now;
            out(true, "[!] ОБРЫВ СВЯЗИ %s, на %.1f с прогона (нагрузка снята, жду до 30 с, вернётся ли плата сама)\n", clock().c_str(), el);
        } else if (lostAt && st.reconnects != s0.reconnects) {
            out(true, "[+] плата вернулась сама через %.1f с после обрыва (обрыв по транспорту %.0f мс)\n",
                Q::toUs(now - lostAt) / 1e6, st.lastOutageMs);
            back = true;
            break;
        } else if (lostAt && now - lostAt > Q::fromUs(30e6)) {
            out(true, "[-] за 30 с плата не вернулась: нужна перезагрузка (переподключение) платы\n");
            break;
        }
    }
    const double total = Q::toUs(Q::now() - t0) / 1e6;
    const int64_t wx = g_rawSumX.load() - sx0, wy = g_rawSumY.load() - sy0;
    out(true, "%s\n", tx.statsLine().c_str());
    out(true, "итог %s: %.0f с, обрывов %llu%s, move() %llu, отправлено %+lld,%+lld, Windows %+lld,%+lld%s, Raw Input сообщений %llu, "
              "секунд с немым Raw Input %d\n", clock().c_str(), total, static_cast<unsigned long long>(tx.stats().linkLost - s0.linkLost),
        lostAt ? (back ? " (вернулась сама)" : " (не вернулась)") : "", static_cast<unsigned long long>(moves),
        static_cast<long long>(sentX), static_cast<long long>(sentY), static_cast<long long>(wx), static_cast<long long>(wy),
        g_hand ? " (рука водила: путь не сравнивается)" : (wx == sentX && wy == sentY ? " (совпал)" : " <-- путь не совпал"),
        static_cast<unsigned long long>(g_rawTotal.load() - raw0), silentS);
    if (track) tx.setTracking(false);
    tx.close();
    say(L"ГОТОВО");
    if (lf) std::fclose(lf);
    return lostAt ? 3 : 0;
}

// ---- разбор ключей и запуск: общее для UDP- и COM-стенда ---------------------------------
struct Args {
    double accuracyS = 0.0, trackS = 0.0, lossS = 0.0, stressS = 0.0;
    int lockN = 0, injectN = 0, refLockN = 0, trackInjectN = 0, pwmN = 0, assistN = 0, sensN = 0;
    bool any() const {
        return accuracyS > 0.0 || lockN > 0 || injectN > 0 || lossS > 0.0 || refLockN > 0 || trackS > 0.0 ||
            trackInjectN > 0 || pwmN > 0 || assistN > 0 || sensN > 0 || stressS > 0.0;
    }
};

inline void parseList(char* s, std::vector<int>& out) {
    while (*s) {
        out.push_back(std::atoi(s));
        while (*s && *s != ',') ++s;
        if (*s == ',') ++s;
    }
}

// Общие ключи. false - такого ключа нет.
inline bool parseArg(Args& a, int& i, int argc, char** argv) {
    const char* k = argv[i];
    const bool more = i + 1 < argc;
    if (std::strcmp(k, "--accuracy") == 0 && more) a.accuracyS = std::atof(argv[++i]);
    else if (std::strcmp(k, "--lock") == 0 && more) a.lockN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--inject") == 0 && more) a.injectN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--lockloss") == 0 && more) a.lossS = std::atof(argv[++i]);
    else if (std::strcmp(k, "--prepoll") == 0 && more) g_prePollMs = std::atof(argv[++i]);
    else if (std::strcmp(k, "--reflock") == 0 && more) a.refLockN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--lag") == 0 && more) parseList(argv[++i], g_maxLags);
    else if (std::strcmp(k, "--stop") == 0) g_stopCall = true;
    else if (std::strcmp(k, "--period") == 0 && more) g_tickUs = std::atof(argv[++i]);
    else if (std::strcmp(k, "--mark") == 0) g_mark = true;
    else if (std::strcmp(k, "--busy") == 0) g_busy = true;
    else if (std::strcmp(k, "--track") == 0 && more) a.trackS = std::atof(argv[++i]);
    else if (std::strcmp(k, "--trackinject") == 0 && more) a.trackInjectN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--pwm") == 0 && more) a.pwmN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--assist") == 0 && more) a.assistN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--sens") == 0 && more) a.sensN = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--withsens") == 0 && more) g_withSens = std::atoi(argv[++i]);
    else if (std::strcmp(k, "--stress") == 0 && more) a.stressS = std::atof(argv[++i]);
    else if (std::strcmp(k, "--load") == 0 && more) g_stressLoad = argv[++i];
    else if (std::strcmp(k, "--burst") == 0 && more) parseList(argv[++i], g_burst);
    else if (std::strcmp(k, "--slog") == 0 && more) g_stressLog = argv[++i];
    else if (std::strcmp(k, "--hand") == 0) g_hand = true;
    else if (std::strcmp(k, "--duty") == 0 && more) parseList(argv[++i], g_pwmDuty);
    else if (std::strcmp(k, "--pwmperiod") == 0 && more) parseList(argv[++i], g_pwmPeriodUs);
    else if (std::strcmp(k, "--oneway") == 0) g_pwmOneWay = true;
    else if (std::strcmp(k, "--both") == 0) g_pwmBoth = true;
    else if (std::strcmp(k, "--seg") == 0 && more) g_pwmSegMs = std::max(std::atof(argv[++i]), 100.0);
    else if (std::strcmp(k, "--dur") == 0 && more) {
        g_injectDurMs.clear();
        parseList(argv[++i], g_injectDurMs);
    } else if (std::strcmp(k, "--screen") == 0 && i + 2 < argc) {
        g_scrW = std::atoi(argv[++i]);
        g_scrH = std::atoi(argv[++i]);
    } else {
        return false;
    }
    return true;
}

// Окно, ожидание клика, режим. T: Tx, Board, open(tx, err), openBoard(b, err), depth(), merged(), retries().
template <class T>
int benchMain(const Args& a) {
    SetConsoleCtrlHandler(onCtrl, TRUE);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::thread ui(uiThread);
    while (!g_ui.ready.load()) Sleep(5);
    say(L"КЛИКНИТЕ В ЭТО ОКНО", L"тест начнётся по клику");
    if (!waitClick(300.0)) {
        std::printf("[-] клика в окно теста не было, тест не запускался\n");
        g_stop = true;
        ui.join();
        return 1;
    }
    int rc = 0;
    if (a.injectN > 0) {
        rc = runInject<T>(a.injectN);
    } else if (a.refLockN > 0) {
        rc = runRefLock<T>(a.refLockN);
    } else if (a.trackS > 0.0) {
        rc = runTrack<T>(a.trackS);
    } else if (a.trackInjectN > 0) {
        rc = runTrackInject<T>(a.trackInjectN);
    } else if (a.assistN > 0) {
        rc = runAssist<T>(a.assistN);
    } else if (a.sensN > 0) {
        rc = runSens<T>(a.sensN);
    } else if (a.stressS > 0.0) {
        rc = runStress<T>(a.stressS);
    } else {
        typename T::Board b;
        std::string err;
        if (!T::openBoard(b, err)) {
            std::printf("[-] %s\n", err.c_str());
            rc = 1;
        } else if (auto fw = b.ask(0x04); !fw || fw->size() < 4) {
            std::printf("[-] плата не отвечает\n");
            rc = 1;
        } else {
            std::printf("[+] прошивка %u\n", static_cast<unsigned>((*fw)[0] | (*fw)[1] << 8 | (*fw)[2] << 16 | (*fw)[3] << 24));
            rc = a.pwmN > 0 ? runPwm(b, a.pwmN) : a.accuracyS > 0.0 ? runAccuracy(b, a.accuracyS) :
                a.lossS > 0.0 ? runLockLoss(b, a.lossS) : runLock(b, a.lockN);
        }
    }
    g_cleanDone = true;
    Sleep(800);
    g_stop = true;
    ui.join();
    timeEndPeriod(1);
    return rc;
}

}  // namespace bench
