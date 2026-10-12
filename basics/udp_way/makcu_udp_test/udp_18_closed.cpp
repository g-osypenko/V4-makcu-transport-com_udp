// udp_18_closed.cpp -- settle the "0x18 loses path over UDP" question the way
// the firmware engineers asked (closed loop, one command in flight), but built
// to tell apart their hypothesis from ours.
//
//   Their hypothesis : 0x18 path loss is an ADMISSION drop -- datagrams land in
//                       the ~1.72 ms busy window and are discarded. Send one
//                       0x18 at a time (closed loop) and the path is exact up to
//                       ~581 cmd/s. Nothing is lost, only (maybe) delayed:
//                       "exact totals after the tail drains".
//   Our hypothesis   : the interpolator makes ~4 HID reports per 0x18 and the
//                       UDP output tops out ~556 reports/s, so once
//                       cmd/s * reports/cmd > ~556 the extra steps are LOST even
//                       when the datagrams are spaced far wider than 1.72 ms
//                       (no contention possible).
//
// How this probe separates them:
//   * One 0x18 in flight at a time. "spacing" = gap between datagrams. The board
//     cycle is ~1.72 ms, so every row with spacing >= ~1.72 ms is CONTENTION-FREE
//     by construction -- an admission drop cannot happen there.
//   * path(live)    = movement that arrived during the send window / movement sent.
//   * path(drained) = total movement after the output backlog fully drained.
//        == 1.000 -> steps were only DELAYED, interp carried them   [engineers right]
//        <  1.000 -> steps were LOST                                 [we are right]
//   * Phase 0 also checks, as a fact, whether 0x18 even produces a UDP reply --
//     if it does not, a literal "wait for the previous reply" loop is impossible
//     and spacing-paced one-in-flight is the faithful closed loop.
//
// Transport: UDP 8080, RAW wrapper (0x55 + 8-byte nonce + DE AD + len + op + payload),
// no encryption -- identical framing to udp_combo.cpp. HID rate + interval histogram
// via WH_MOUSE_LL hook; path via Raw Input sum of the MAKCU device. Cursor moves.
//
// Build: g++ -std=c++23 -O2 udp_18_closed.cpp -lws2_32 -lwinmm -static -o udp_18_closed.exe
// Run:   udp_18_closed.exe --host <board-ip> [--port 8080]
//                          [--targets 125,150,200,300,450,581] [--speed 2]
//                          [--cell 2.5] [--quiet-ms 400] [--drain-cap-ms 15000]

// Windows 10+ APIs (process/thread power-throttling control) must precede <windows.h>.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include "mover_common.h"
#include "hook_monitor.h"
#include "rawinput_monitor.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

using mover::Clock;

namespace {

struct Opts {
    std::string host = "192.168.1.50";  // placeholder: pass the real board IP with --host
    uint16_t port = 8080;
    std::vector<double> targets{125, 150, 200, 300, 450, 581};
    double speed = 2.0;           // px/ms, ramp to the right
    double cellS = 2.5;           // send window per target
    double quietMs = 400.0;       // "backlog drained" = no new movement for this long
    double drainCapMs = 15000.0;  // give up waiting for the drain after this
};

class Link {
public:
    bool open(const Opts& o) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        DWORD tmo = 50;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(o.port);
        inet_pton(AF_INET, o.host.c_str(), &a.sin_addr);
        if (connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        running_ = true;
        rx_ = std::thread([this] { rxMain(); });
        return true;
    }
    void close() {
        running_ = false;
        if (rx_.joinable()) rx_.join();
        closesocket(sock_);
        WSACleanup();
    }

    // Fire-and-forget send. If track18 is set, remember the nonce so the RX thread
    // can count how many 0x18 actually came back (admission count, when 0x18 replies).
    void send(uint8_t op, const uint8_t* pl, uint16_t len, bool track18 = false) {
        uint8_t d[64];
        size_t n = 0;
        const uint64_t seq = ++seq_;
        d[n++] = 0x55;
        for (int i = 0; i < 8; ++i) d[n++] = static_cast<uint8_t>(seq >> (8 * i));
        d[n++] = 0xDE;
        d[n++] = 0xAD;
        d[n++] = static_cast<uint8_t>(len);
        d[n++] = static_cast<uint8_t>(len >> 8);
        d[n++] = op;
        if (len) std::memcpy(d + n, pl, len);
        n += len;
        if (track18) {
            std::lock_guard<std::mutex> lk(mu_);
            pending18_.insert(seq);
        }
        ::send(sock_, reinterpret_cast<const char*>(d), static_cast<int>(n), 0);
        ++sent_;
    }

    // Blocking request/reply (only outside the timed loop). Returns the payload.
    bool query(uint8_t op, const uint8_t* pl, uint16_t len, std::vector<uint8_t>& out, int timeoutMs = 60) {
        uint8_t d[64];
        size_t n = 0;
        const uint64_t seq = ++seq_;
        d[n++] = 0x55;
        for (int i = 0; i < 8; ++i) d[n++] = static_cast<uint8_t>(seq >> (8 * i));
        d[n++] = 0xDE;
        d[n++] = 0xAD;
        d[n++] = static_cast<uint8_t>(len);
        d[n++] = static_cast<uint8_t>(len >> 8);
        d[n++] = op;
        if (len) std::memcpy(d + n, pl, len);
        n += len;
        {
            std::lock_guard<std::mutex> lk(mu_);
            wantQuery_ = seq;
            queryHit_ = false;
        }
        ::send(sock_, reinterpret_cast<const char*>(d), static_cast<int>(n), 0);
        ++sent_;
        const int64_t until = Clock::now() + Clock::fromUs(timeoutMs * 1000);
        while (Clock::now() < until) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queryHit_) { out = queryReply_; wantQuery_ = 0; return true; }
            }
            Sleep(1);
        }
        std::lock_guard<std::mutex> lk(mu_);
        wantQuery_ = 0;
        return false;
    }

    // Per-cell 0x18 admission counters.
    void beginCell() {
        std::lock_guard<std::mutex> lk(mu_);
        pending18_.clear();
        replies18_ = 0;
    }
    uint64_t replies18() {
        std::lock_guard<std::mutex> lk(mu_);
        return replies18_;
    }

    std::atomic<uint64_t> sent_{0};

private:
    void rxMain() {
        uint8_t b[512];
        while (running_) {
            const int r = recv(sock_, reinterpret_cast<char*>(b), sizeof(b), 0);
            if (r < 14 || b[0] != 0x55 || b[9] != 0xDE || b[10] != 0xAD) continue;
            uint64_t seq = 0;
            for (int i = 0; i < 8; ++i) seq |= static_cast<uint64_t>(b[1 + i]) << (8 * i);
            const uint16_t len = static_cast<uint16_t>(b[11] | (b[12] << 8));
            if (14 + len > r) continue;
            std::lock_guard<std::mutex> lk(mu_);
            if (wantQuery_ && seq == wantQuery_) {
                queryReply_.assign(b + 14, b + 14 + len);
                queryHit_ = true;
                continue;
            }
            auto it = pending18_.find(seq);
            if (it != pending18_.end()) {
                pending18_.erase(it);
                ++replies18_;
            }
        }
    }

    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread rx_;
    std::atomic<uint64_t> seq_{0x1800000};
    std::mutex mu_;
    std::unordered_set<uint64_t> pending18_;
    uint64_t replies18_ = 0;
    uint64_t wantQuery_ = 0;
    bool queryHit_ = false;
    std::vector<uint8_t> queryReply_;
};

void put16(uint8_t* p, int v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }

std::string hexOf(const std::vector<uint8_t>& v) {
    std::string s;
    char buf[4];
    for (size_t i = 0; i < v.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", v[i]);
        s += buf;
        if (i + 1 < v.size()) s += ' ';
    }
    return s;
}

// Park the cursor at the left edge of the virtual desktop before a cell, so the +X ramp
// does not end up pinned at the right edge. SetCursorPos is not mouse input: neither the
// LL hook nor Raw Input sees it, so it does not touch the measurement.
void parkCursorLeft() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + 20;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) / 2;
    SetCursorPos(x, y);
}

// Read the current Raw-Input X sum for the MAKCU device (0 if not seen yet).
long long pathX(mover::RawInputMonitor& rawin) {
    const auto dev = rawin.primary();
    return dev ? static_cast<long long>(dev->sumX) : 0;
}

struct Row {
    double target, spacingMs, cmdS, dgramS, admitPct, hidHz, repPerCmd, pathLive, drainMs, pathDrain;
    long long sent18, residual;
    bool contentionFree, drainCapped, hasAdmit;
    std::array<double, 6> hist{};
};

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--host") o.host = next();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (a == "--speed") o.speed = std::atof(next().c_str());
        else if (a == "--cell") o.cellS = std::atof(next().c_str());
        else if (a == "--quiet-ms") o.quietMs = std::atof(next().c_str());
        else if (a == "--drain-cap-ms") o.drainCapMs = std::atof(next().c_str());
        else if (a == "--targets") {
            o.targets.clear();
            std::string s = next(), tok;
            for (char c : s) {
                if (c == ',') { if (!tok.empty()) o.targets.push_back(std::atof(tok.c_str())), tok.clear(); }
                else tok += c;
            }
            if (!tok.empty()) o.targets.push_back(std::atof(tok.c_str()));
        }
    }

    mover::tuneProcessForTiming();
    Link link;
    if (!link.open(o)) { std::printf("socket failed\n"); return 1; }

    // ---- Phase 0: board liveness, interpolation setting, does 0x18 reply? ----
    std::vector<uint8_t> rep;
    std::string interpHex = "no reply";
    if (link.query(0x1F, nullptr, 0, rep)) interpHex = hexOf(rep);

    std::vector<uint8_t> meta;
    uint8_t zero5[5]{};
    if (!link.query(0x6A, zero5, 5, meta) || meta.size() < 7) {
        std::printf("no Raw-meta reply from the board (is it reachable at %s:%u?)\n", o.host.c_str(), o.port);
        link.close();
        mover::restoreProcessTiming();
        return 1;
    }
    const uint32_t lastId = static_cast<uint32_t>(meta[2] | (meta[3] << 8) | (meta[4] << 16) | (static_cast<uint32_t>(meta[5]) << 24));
    const int freeRecs = meta[6];

    // Does 0x18 answer over UDP? Send a handful, well spaced so none contend, and
    // count replies via the per-cell tracker.
    link.beginCell();
    for (int i = 0; i < 20; ++i) {
        uint8_t pl[4];
        put16(pl, (i & 1) ? -1 : 1);  // alternate +/-1: a real 0x18 move, net ~0 drift
        put16(pl + 2, 0);
        link.send(0x18, pl, 4, /*track18=*/true);
        Sleep(6);
    }
    Sleep(50);
    const uint64_t replied18 = link.replies18();
    const bool op18Replies = replied18 > 0;

    std::printf("========================================================================\n");
    std::printf(" MAKCU  0x18 MOVE  closed-loop / contention-free sweep over UDP\n");
    std::printf(" host %s:%u   speed %.2f px/ms   cell %.1f s   ramp +X\n", o.host.c_str(), o.port, o.speed, o.cellS);
    std::printf(" interpolation (0x1F GET): %s\n", interpHex.c_str());
    std::printf(" board: last_accepted_id %u, free Raw records %d/16\n", lastId, freeRecs);
    std::printf("------------------------------------------------------------------------\n");
    std::printf(" 0x18 UDP reply: %llu/20 frames replied  =>  %s\n",
                (unsigned long long)replied18,
                op18Replies ? "0x18 IS answered (true reply-clock possible)"
                            : "0x18 is FIRE-AND-FORGET (no reply; paced one-in-flight by spacing)");
    std::printf("------------------------------------------------------------------------\n");
    std::printf(" One 0x18 in flight at a time. Board cycle ~1.72 ms, so any row with\n");
    std::printf(" spacing >= 1.72 ms is CONTENTION-FREE: an admission drop cannot happen.\n\n");

    POINT cursor0{};
    GetCursorPos(&cursor0);
    mover::HookMonitor hook;
    mover::RawInputMonitor rawin;
    if (!hook.start() || !rawin.start()) { std::printf("monitor start failed\n"); link.close(); return 1; }
    Sleep(150);
    mover::tuneTimingThread(-1);

    std::vector<Row> rows;
    for (double target : o.targets) {
        if (target <= 0) continue;
        const int64_t gap = Clock::fromUs(1e6 / target);
        const double spacingMs = 1000.0 / target;
        const uint64_t nSlots = static_cast<uint64_t>(o.cellS * target);

        parkCursorLeft();
        Sleep(30);
        link.beginCell();
        hook.reset();
        rawin.reset();
        const long long baseX = pathX(rawin);
        const uint64_t baseSent = link.sent_.load();

        int64_t sentX = 0;
        const int64_t t0 = Clock::now() + Clock::fromUs(2000);
        for (uint64_t k = 0; k < nSlots; ++k) {
            const int64_t due = t0 + static_cast<int64_t>(k) * gap;
            while (Clock::now() < due) YieldProcessor();
            const double tMs = Clock::toUs(Clock::now() - t0) / 1000.0;
            const int64_t tgt = static_cast<int64_t>(o.speed * tMs);
            const int64_t carry = tgt - sentX;
            if (carry == 0) continue;
            uint8_t pl[4];
            put16(pl, static_cast<int>(carry));
            put16(pl + 2, 0);
            link.send(0x18, pl, 4, op18Replies);
            sentX += carry;
        }
        const double sendS = Clock::toUs(Clock::now() - t0) / 1e6;
        const int64_t tSendEnd = Clock::now();
        const uint64_t hidLive = hook.count();
        const auto hist = hook.histogram();
        const long long sumLive = pathX(rawin) - baseX;
        const uint64_t dgrams = link.sent_.load() - baseSent;

        // ---- Drain: wait until no new movement for quietMs, or give up at the cap ----
        long long lastSum = pathX(rawin);
        int64_t lastChange = Clock::now();
        const int64_t drainCap = tSendEnd + Clock::fromUs(o.drainCapMs * 1000);
        bool capped = false;
        for (;;) {
            Sleep(10);
            const long long now = pathX(rawin);
            if (now != lastSum) { lastSum = now; lastChange = Clock::now(); }
            if (Clock::toUs(Clock::now() - lastChange) / 1000.0 >= o.quietMs) break;
            if (Clock::now() >= drainCap) { capped = true; break; }
        }
        const long long sumDrain = pathX(rawin) - baseX;
        const double drainMs = Clock::toUs(Clock::now() - tSendEnd) / 1000.0;
        const uint64_t hidDrain = hook.count();

        Row rrow{};
        rrow.target = target;
        rrow.spacingMs = spacingMs;
        rrow.sent18 = static_cast<long long>(dgrams);  // this cell sends only 0x18
        rrow.cmdS = dgrams / sendS;
        rrow.dgramS = dgrams / sendS;
        rrow.admitPct = op18Replies && dgrams ? 100.0 * link.replies18() / dgrams : 0.0;
        rrow.hasAdmit = op18Replies;
        rrow.hidHz = hidLive / sendS;
        rrow.repPerCmd = dgrams ? static_cast<double>(hidDrain) / dgrams : 0.0;
        rrow.pathLive = sentX ? static_cast<double>(sumLive) / sentX : 0.0;
        rrow.pathDrain = sentX ? static_cast<double>(sumDrain) / sentX : 0.0;
        rrow.residual = sentX - sumDrain;
        rrow.drainMs = drainMs;
        rrow.drainCapped = capped;
        rrow.contentionFree = spacingMs >= 1.72;
        uint64_t histTotal = 0;
        for (auto h : hist) histTotal += h;
        for (size_t i = 0; i < hist.size() && i < rrow.hist.size(); ++i)
            rrow.hist[i] = histTotal ? 100.0 * hist[i] / histTotal : 0.0;
        rows.push_back(rrow);

        std::printf("  %4.0f c/s done: sent %lld x 0x18, HID %.0f Hz, path live %.3f, drained %.3f%s (%.0f ms)\n",
                    target, rrow.sent18, rrow.hidHz, rrow.pathLive, rrow.pathDrain,
                    capped ? " [CAP]" : "", drainMs);
    }

    const auto devName = rawin.primary();
    hook.stop();
    rawin.stop();
    SetCursorPos(cursor0.x, cursor0.y);

    // ---- Report table ----
    std::printf("\n");
    std::printf(" device for path: %s\n", devName ? devName->name.c_str() : "(none seen)");
    std::printf(" target  spacing  sent    cmd/s   admit   HID Hz  rep/cmd  path    drain    path\n");
    std::printf("  c/s     (ms)    0x18            %%(adm)  (live)  (final)  (live)  (ms)     (drained)\n");
    std::printf(" ------  -------  ------  ------  ------  ------  -------  ------  -------  ---------\n");
    for (const auto& r : rows) {
        char admit[8];
        if (r.hasAdmit) std::snprintf(admit, sizeof(admit), "%5.1f", r.admitPct);
        else std::snprintf(admit, sizeof(admit), "  n/a");
        char drained[16];
        if (r.drainCapped) std::snprintf(drained, sizeof(drained), "%.3f CAP", r.pathDrain);
        else std::snprintf(drained, sizeof(drained), "%.3f", r.pathDrain);
        std::printf(" %5.0f%s  %6.2f  %6lld  %6.0f  %6s  %6.0f  %7.2f  %6.3f  %7.0f  %s\n",
                    r.target, r.contentionFree ? " " : "!", r.spacingMs, r.sent18, r.cmdS,
                    admit, r.hidHz, r.repPerCmd, r.pathLive, r.drainMs, drained);
    }
    std::printf("   ( ! = spacing < 1.72 ms: busy-window contention is possible on this row )\n\n");

    std::printf(" interval histogram (LL hook), %% per bucket:\n");
    for (const auto& r : rows) {
        std::printf("  %4.0f c/s:", r.target);
        for (size_t i = 0; i < r.hist.size(); ++i)
            std::printf("  %s %.0f%%", mover::HookMonitor::kBucketNames[i], r.hist[i]);
        std::printf("\n");
    }

    std::printf("\n Reading:\n");
    std::printf("  path(live)    = movement during the send window / movement sent\n");
    std::printf("  path(drained) = total movement after the output backlog drained / sent\n");
    std::printf("     == 1.000  -> steps only DELAYED, interp carried them   [engineers right]\n");
    std::printf("     <  1.000  -> steps LOST                                 [our hypothesis]\n");
    std::printf("  A contention-free row (no !) that still loses path = the loss is output-bound\n");
    std::printf("  (~4 reports/cmd * cmd/s > ~556 reports/s), not a busy-window admission drop.\n");
    std::printf("  Reference (bench): Raw 0x69 reply-clocked over UDP = ~283 moves/s, path exact.\n");
    std::printf("========================================================================\n");

    link.close();
    mover::restoreProcessTiming();
    return 0;
}
