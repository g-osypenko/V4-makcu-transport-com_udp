// udp_raw_batch.cpp -- V4.097 matrix rows b) and c) over UDP, closed loop.
//
//   b) Raw 0x69, one record per datagram (dt = 0), freed one by one with
//      0x6A consume=1. One datagram in flight: every request waits for its reply
//      (moves and acks alternate). --rates caps the move rate.
//   c) 0x6C batch, N records per datagram, one receipt per datagram. Closed loop:
//      send, wait for the receipt, put the receipt's highest terminal id into X of
//      the next packet (flags bit0). --rates caps the packet rate. Each packet
//      carries the displacement accumulated since the previous one, split evenly
//      across its N records; records the board does not admit keep their
//      displacement in the carry for the next packet.
//
// 0x6C (little-endian):
//   request = 01 | flags | N | 00 | X u32 | N x [id u32 | x i16 | y i16 | dt u16]  (8 + 10N bytes)
//   receipt = [0]=1 [1]=0 all / 1 partial / 255 envelope error [2]=N [3]=admitted
//             [4]=first failing index [5]=reason [6]=freed [7]=free slots
//             [8]=terminal rows K (<=7) [9]=more [10]=queued [11]=0 [12..15]=last_id u32,
//             then K x (id u32 | state u8 | reason u8), ascending id.
//   X must be <= the highest terminal id already reported, else 255 / reason 1.
//
// Same ramp and metrics as udp_18_closed.cpp: constant speed (px/ms) to the right,
// path(live) = device dx during the send window / commanded, path(drained) = after
// the output went quiet. HID rate + intervals: WH_MOUSE_LL; path: Raw Input. Cursor moves.
//
// Build: g++ -std=c++23 -O2 udp_raw_batch.cpp -lws2_32 -lwinmm -static -o udp_raw_batch.exe
// Run:   udp_raw_batch.exe [--mode raw|batch|all] --host <board-ip> [--port 8080]
//                          [--raw-rates 125,200,300,581] [--pkt-rates 50,100,139] [--recs 7,16]
//                          [--speed 2] [--cell 2.5] [--dt 0] [--quiet-ms 400] [--drain-cap-ms 15000]

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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

using mover::Clock;

namespace {

struct Opts {
    std::string host = "192.168.1.50";  // placeholder: pass the real board IP with --host
    uint16_t port = 8080;
    std::string mode = "all";
    std::vector<double> rawRates{125, 200, 300, 581};
    std::vector<double> pktRates{50, 100, 139};
    std::vector<int> recs{7, 16};
    double speed = 2.0;
    double cellS = 2.5;
    int dt = 0;
    double quietMs = 400.0;
    double drainCapMs = 15000.0;
    double ackAgeUs = 2000.0;        // ack a Raw record no earlier than this after it was queued
    double replyTimeoutUs = 30000.0;
};

std::vector<double> parseList(const std::string& s) {
    std::vector<double> v;
    std::string tok;
    for (char c : s + ",") {
        if (c == ',') { if (!tok.empty()) v.push_back(std::atof(tok.c_str())); tok.clear(); }
        else tok += c;
    }
    return v;
}

void put16(uint8_t* p, int v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string hexOf(const std::vector<uint8_t>& v, size_t maxBytes = 64) {
    std::string s;
    char buf[4];
    for (size_t i = 0; i < v.size() && i < maxBytes; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", v[i]);
        if (i) s += ' ';
        s += buf;
    }
    if (v.size() > maxBytes) s += " ...";
    return s;
}

// Synchronous request/reply over a non-blocking socket: send one frame, spin on recv
// until the reply with the same nonce arrives. Exactly one datagram in flight.
class Udp {
public:
    bool open(const Opts& o) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(o.port);
        inet_pton(AF_INET, o.host.c_str(), &a.sin_addr);
        if (connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        u_long nb = 1;
        ioctlsocket(sock_, FIONBIO, &nb);
        return true;
    }
    void close() {
        closesocket(sock_);
        WSACleanup();
    }

    bool call(uint8_t op, const uint8_t* pl, uint16_t len, std::vector<uint8_t>& out, double timeoutUs) {
        std::vector<uint8_t> d(14 + len);
        const uint64_t seq = ++seq_;
        d[0] = 0x55;
        for (int i = 0; i < 8; ++i) d[1 + i] = static_cast<uint8_t>(seq >> (8 * i));
        d[9] = 0xDE;
        d[10] = 0xAD;
        d[11] = static_cast<uint8_t>(len);
        d[12] = static_cast<uint8_t>(len >> 8);
        d[13] = op;
        if (len) std::memcpy(d.data() + 14, pl, len);
        ::send(sock_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0);
        ++sent;
        const int64_t until = Clock::now() + Clock::fromUs(timeoutUs);
        uint8_t b[1500];
        while (Clock::now() < until) {
            const int r = recv(sock_, reinterpret_cast<char*>(b), sizeof(b), 0);
            if (r <= 0) { YieldProcessor(); continue; }
            if (r < 14 || b[0] != 0x55 || b[9] != 0xDE || b[10] != 0xAD) continue;
            uint64_t got = 0;
            for (int i = 0; i < 8; ++i) got |= static_cast<uint64_t>(b[1 + i]) << (8 * i);
            if (got != seq) { ++stale; continue; }
            const uint16_t rl = static_cast<uint16_t>(b[11] | (b[12] << 8));
            if (14 + rl > r) continue;
            out.assign(b + 14, b + 14 + rl);
            return true;
        }
        ++timeouts;
        return false;
    }

    uint64_t sent = 0, timeouts = 0, stale = 0;

private:
    SOCKET sock_ = INVALID_SOCKET;
    uint64_t seq_ = 0x6C000000;
};

struct Meta {
    uint32_t lastId = 0;
    int freeRecs = 0;
    int capacity = 0;
};

bool rawMeta(Udp& u, Meta& m) {
    uint8_t pl[5]{};
    std::vector<uint8_t> r;
    if (!u.call(0x6A, pl, 5, r, 60000) || r.size() < 8) return false;
    m.lastId = le32(r.data() + 2);
    m.freeRecs = r[6];
    m.capacity = r[7];
    return true;
}

// Free terminal records left on the board (consume=1 scanning back from last id).
// Only the newest <= capacity records can still be unfreed, so 64 ids is plenty.
int drainRecords(Udp& u) {
    Meta m;
    for (int pass = 0; pass < 6; ++pass) {
        if (!rawMeta(u, m)) return -1;
        if (m.freeRecs >= m.capacity) return m.freeRecs;
        for (uint32_t id = m.lastId, n = 0; id >= 1 && n < 64; --id, ++n) {
            uint8_t pl[5];
            put32(pl, id);
            pl[4] = 1;
            std::vector<uint8_t> r;
            u.call(0x6A, pl, 5, r, 20000);
        }
        Sleep(20);
    }
    return rawMeta(u, m) ? m.freeRecs : -1;
}

// Park the cursor at the left edge of the virtual desktop before a cell, so the +X ramp
// does not end up pinned at the right edge. SetCursorPos is not mouse input: neither the
// LL hook nor Raw Input sees it, so it does not touch the measurement.
void parkCursorLeft() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + 20;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) / 2;
    SetCursorPos(x, y);
}

long long pathX(mover::RawInputMonitor& rawin) {
    const auto dev = rawin.primary();
    return dev ? static_cast<long long>(dev->sumX) : 0;
}

// Ramp clock + HID/path snapshots shared by both modes.
struct Cell {
    mover::HookMonitor& hook;
    mover::RawInputMonitor& rawin;
    const Opts& o;
    int64_t t0 = 0, tEnd = 0, tSendEnd = 0;
    double sendS = 0, hidHz = 0, pathLive = 0, pathDrained = 0, pathAdmitted = 0, drainMs = 0;
    long long commanded = 0, sentX = 0;
    bool capped = false;
    std::array<double, 6> hist{};

    Cell(mover::HookMonitor& h, mover::RawInputMonitor& r, const Opts& opts) : hook(h), rawin(r), o(opts) {}

    void begin() {
        parkCursorLeft();
        Sleep(30);
        hook.reset();
        rawin.reset();
        t0 = Clock::now() + Clock::fromUs(2000);
        tEnd = t0 + Clock::fromUs(o.cellS * 1e6);
        while (Clock::now() < t0) YieldProcessor();
    }
    long long target(int64_t now) const {
        return static_cast<long long>(o.speed * Clock::toUs(now - t0) / 1000.0);
    }
    void endSend() {
        tSendEnd = Clock::now();
        sendS = Clock::toUs(tSendEnd - t0) / 1e6;
        commanded = target(tSendEnd);
        hidHz = hook.count() / sendS;
        const auto h = hook.histogram();
        uint64_t tot = 0;
        for (auto v : h) tot += v;
        for (size_t i = 0; i < h.size(); ++i) hist[i] = tot ? 100.0 * h[i] / tot : 0.0;
        pathLive = commanded ? static_cast<double>(pathX(rawin)) / commanded : 0.0;
    }
    // Wait until no new movement for quietMs (or the cap), then read the final path.
    void drain() {
        long long last = pathX(rawin);
        int64_t lastChange = Clock::now();
        const int64_t cap = tSendEnd + Clock::fromUs(o.drainCapMs * 1000);
        for (;;) {
            Sleep(10);
            const long long now = pathX(rawin);
            if (now != last) { last = now; lastChange = Clock::now(); }
            if (Clock::toUs(Clock::now() - lastChange) / 1000.0 >= o.quietMs) break;
            if (Clock::now() >= cap) { capped = true; break; }
        }
        drainMs = Clock::toUs(Clock::now() - tSendEnd) / 1000.0;
        pathDrained = commanded ? static_cast<double>(pathX(rawin)) / commanded : 0.0;
        pathAdmitted = sentX ? static_cast<double>(pathX(rawin)) / sentX : 0.0;  // vs what the board accepted
    }
};

struct Summary {
    std::string label;
    double hidHz, pathLive, pathDrained, pathAdmitted, drainMs, rate;
    bool capped;
};

void printHist(const Cell& c) {
    std::printf("    intervals:");
    for (size_t i = 0; i < c.hist.size(); ++i)
        std::printf("  %s %.0f%%", mover::HookMonitor::kBucketNames[i], c.hist[i]);
    std::printf("\n");
}

// ---- b) Raw 0x69 + 0x6A consume=1, one datagram in flight ----
Summary runRaw(Udp& u, Cell& c, double rate, uint32_t& nextId) {
    struct Rec { uint32_t id; int64_t t; };
    std::deque<Rec> acks;
    int inflight = 0;
    uint64_t ok = 0, ambiguous = 0, acksOk = 0, acksRej = 0, stillQ = 0, ackOther = 0;
    uint64_t rej[256]{};
    const uint64_t dg0 = u.sent, to0 = u.timeouts;
    const int64_t period = Clock::fromUs(1e6 / rate);
    const int64_t ackAge = Clock::fromUs(c.o.ackAgeUs);
    bool lastWasMove = false;

    auto doAck = [&](int64_t now) {
        Rec r = acks.front();
        acks.pop_front();
        uint8_t pl[5];
        put32(pl, r.id);
        pl[4] = 1;
        std::vector<uint8_t> rep;
        if (!u.call(0x6A, pl, 5, rep, c.o.replyTimeoutUs) || rep.size() < 6) {
            acks.push_front(Rec{r.id, now});
            return;
        }
        const uint8_t st = rep[0], rs = rep[1];
        if (st == 2) { ++acksOk; --inflight; }
        else if (st == 3) { ++acksRej; --inflight; }
        else if (st == 255 && rs == 6) { ++stillQ; acks.push_front(Rec{r.id, Clock::now()}); }
        else { ++ackOther; --inflight; }
    };

    c.begin();
    int64_t nextDue = c.t0;
    while (Clock::now() < c.tEnd) {
        const int64_t now = Clock::now();
        const bool wantMove = now >= nextDue && inflight < 16;
        const bool wantAck = !acks.empty() && now - acks.front().t >= ackAge;
        if (wantMove && (!wantAck || !lastWasMove)) {
            const long long carry = c.target(now) - c.sentX;
            nextDue += period;
            if (now - nextDue > period) nextDue = now;
            lastWasMove = true;
            if (carry == 0) continue;
            const uint32_t id = nextId;
            uint8_t pl[10];
            put32(pl, id);
            put16(pl + 4, static_cast<int>(carry));
            put16(pl + 6, 0);
            put16(pl + 8, c.o.dt);
            std::vector<uint8_t> rep;
            if (u.call(0x69, pl, 10, rep, c.o.replyTimeoutUs) && rep.size() >= 6) {
                if (rep[0] == 1) {
                    ++ok; ++inflight; ++nextId;
                    c.sentX += carry;
                    acks.push_back(Rec{id, Clock::now()});
                } else {
                    ++rej[rep[1]];
                    if (rep[1] == 2) nextId += 1000;  // id already used: move ahead
                }
            } else {
                // No reply: ask whether the board knows this id (consume=0).
                uint8_t q[5];
                put32(q, id);
                q[4] = 0;
                std::vector<uint8_t> qr;
                const bool known = u.call(0x6A, q, 5, qr, c.o.replyTimeoutUs) && qr.size() >= 6 &&
                                   (qr[0] == 1 || qr[0] == 2 || qr[0] == 3 || (qr[0] == 255 && qr[1] == 6));
                ++ambiguous;
                ++nextId;
                if (known) {
                    ++ok; ++inflight;
                    c.sentX += carry;
                    acks.push_back(Rec{id, Clock::now()});
                }
            }
        } else if (wantAck) {
            doAck(now);
            lastWasMove = false;
        } else {
            YieldProcessor();
        }
    }
    c.endSend();
    const uint64_t moves = ok;
    const uint64_t dgrams = u.sent - dg0;
    // Free every record we queued (they keep going out to USB regardless of acks).
    const int64_t ackCap = Clock::now() + Clock::fromUs(3e6);
    while (!acks.empty() && Clock::now() < ackCap) {
        const int64_t now = Clock::now();
        if (now - acks.front().t >= ackAge) doAck(now);
        else YieldProcessor();
    }
    c.drain();

    char label[64];
    std::snprintf(label, sizeof(label), "b) 0x69 @%4.0f/s", rate);
    std::printf("  %s: %llu moves (%.0f/s), %llu datagrams (%.0f/s), HID %.0f Hz, path live %.3f, drained %.3f%s (%.0f ms)\n",
        label, (unsigned long long)moves, moves / c.sendS, (unsigned long long)dgrams, dgrams / c.sendS,
        c.hidHz, c.pathLive, c.pathDrained, c.capped ? " [CAP]" : "", c.drainMs);
    std::printf("    acks: submitted %llu, rejected %llu, still-queued retries %llu, other %llu | no-reply %llu (resolved by query) | unsent carry %lld px\n",
        (unsigned long long)acksOk, (unsigned long long)acksRej, (unsigned long long)stillQ,
        (unsigned long long)ackOther, (unsigned long long)ambiguous, c.commanded - c.sentX);
    for (int r = 0; r < 256; ++r)
        if (rej[r]) std::printf("    0x69 rejected, reason %d: %llu\n", r, (unsigned long long)rej[r]);
    if (u.timeouts - to0) std::printf("    reply timeouts: %llu\n", (unsigned long long)(u.timeouts - to0));
    printHist(c);
    return Summary{label, c.hidHz, c.pathLive, c.pathDrained, c.pathAdmitted, c.drainMs, moves / c.sendS, c.capped};
}

// ---- c) 0x6C batch, one receipt per packet ----
Summary runBatch(Udp& u, Cell& c, int n, double pktRate, uint32_t& nextId, uint32_t& maxTerm, bool& haveX) {
    uint64_t pkts = 0, receipts = 0, res0 = 0, res1 = 0, res255 = 0, admittedSum = 0, nSum = 0, freedSum = 0;
    uint64_t freeSum = 0, kSum = 0, moreCnt = 0, queuedSum = 0, idMismatch = 0, xRejects = 0, noReply = 0;
    uint64_t termSubmitted = 0, termRejected = 0;
    int freeMin = 255;
    uint64_t ffReason[256]{};
    std::vector<uint8_t> firstReceipt, firstPartial;
    const uint64_t dg0 = u.sent;
    const int64_t period = Clock::fromUs(1e6 / pktRate);

    c.begin();
    int64_t nextDue = c.t0;
    std::vector<uint8_t> pl(8 + 10 * n);
    std::vector<int> xs(n);
    while (Clock::now() < c.tEnd) {
        const int64_t now = Clock::now();
        if (now < nextDue) { YieldProcessor(); continue; }
        nextDue += period;
        if (now - nextDue > period) nextDue = now;

        const long long carry = std::max<long long>(0, c.target(now) - c.sentX);
        const uint32_t firstId = nextId;
        pl[0] = 0x01;
        pl[1] = haveX ? 0x01 : 0x00;
        pl[2] = static_cast<uint8_t>(n);
        pl[3] = 0x00;
        put32(pl.data() + 4, haveX ? maxTerm : 0);
        for (int i = 0; i < n; ++i) {
            xs[i] = static_cast<int>(carry / n + (i < carry % n ? 1 : 0));
            uint8_t* rec = pl.data() + 8 + 10 * i;
            put32(rec, firstId + static_cast<uint32_t>(i));
            put16(rec + 4, xs[i]);
            put16(rec + 6, 0);
            put16(rec + 8, c.o.dt);
        }
        ++pkts;
        nSum += n;
        std::vector<uint8_t> r;
        if (!u.call(0x6C, pl.data(), static_cast<uint16_t>(pl.size()), r, c.o.replyTimeoutUs)) {
            // Lost: learn from the board how far the ids got.
            ++noReply;
            Meta m;
            if (rawMeta(u, m) && m.lastId >= firstId) {
                const uint32_t adm = std::min<uint32_t>(m.lastId - firstId + 1, static_cast<uint32_t>(n));
                for (uint32_t i = 0; i < adm; ++i) c.sentX += xs[i];
                admittedSum += adm;
                nextId = m.lastId + 1;
            }
            continue;
        }
        if (r.size() < 16 || r[0] != 1) {
            std::printf("    unexpected 0x6C reply (%zu B): %s\n", r.size(), hexOf(r).c_str());
            ++res255;
            continue;
        }
        ++receipts;
        if (firstReceipt.empty()) firstReceipt = r;
        const uint8_t result = r[1], adm = r[3], ff = r[4], rsn = r[5], freed = r[6], fs = r[7];
        const uint8_t k = r[8], more = r[9], queued = r[10];
        const uint32_t lastId = le32(r.data() + 12);
        if (result == 0) ++res0;
        else if (result == 1) { ++res1; ++ffReason[rsn]; if (firstPartial.empty()) firstPartial = r; }
        else { ++res255; ++ffReason[rsn]; if (rsn == 1) { ++xRejects; haveX = false; } }
        if (result != 255) {
            admittedSum += adm;
            for (int i = 0; i < adm && i < n; ++i) c.sentX += xs[i];
            nextId = firstId + adm;
            if (adm && lastId != nextId - 1) {
                ++idMismatch;
                nextId = std::max(nextId, lastId + 1);
            }
        }
        (void)ff;
        freedSum += freed;
        freeSum += fs;
        freeMin = std::min<int>(freeMin, fs);
        kSum += k;
        queuedSum += queued;
        if (more) ++moreCnt;
        for (int i = 0; i < k && 16 + 6 * i + 6 <= static_cast<int>(r.size()); ++i) {
            const uint8_t* row = r.data() + 16 + 6 * i;
            const uint32_t id = le32(row);
            if (row[4] == 2) ++termSubmitted;
            else if (row[4] == 3) ++termRejected;
            if (id > maxTerm) maxTerm = id;
            haveX = true;
        }
    }
    c.endSend();
    const uint64_t dgrams = u.sent - dg0;
    c.drain();

    char label[64];
    std::snprintf(label, sizeof(label), "c) 0x6C N=%-2d @%3.0f pkt/s", n, pktRate);
    const double rc = receipts ? static_cast<double>(receipts) : 1.0;
    std::printf("  %s: %llu packets (%.0f/s), %llu records admitted (%.0f moves/s), HID %.0f Hz, path live %.3f, drained %.3f%s (%.0f ms)\n",
        label, (unsigned long long)pkts, pkts / c.sendS, (unsigned long long)admittedSum, admittedSum / c.sendS,
        c.hidHz, c.pathLive, c.pathDrained, c.capped ? " [CAP]" : "", c.drainMs);
    std::printf("    receipts %llu: result 0 (all) %llu, 1 (partial) %llu, 255 %llu | admitted/packet %.2f of %d | free slots min %d avg %.1f\n",
        (unsigned long long)receipts, (unsigned long long)res0, (unsigned long long)res1, (unsigned long long)res255,
        admittedSum / rc, n, freeMin == 255 ? -1 : freeMin, freeSum / rc);
    std::printf("    freed/packet %.2f | terminal rows K/packet %.2f (submitted %llu, rejected %llu) | 'more' set %.0f%% | queued avg %.1f\n",
        freedSum / rc, kSum / rc, (unsigned long long)termSubmitted, (unsigned long long)termRejected,
        100.0 * moreCnt / rc, queuedSum / rc);
    for (int r = 0; r < 256; ++r)
        if (ffReason[r]) std::printf("    first-failure reason %d: %llu packets\n", r, (unsigned long long)ffReason[r]);
    std::printf("    no receipt %llu | X rejected (255/1) %llu | last_id mismatch %llu | datagrams %llu | unsent carry %lld px\n",
        (unsigned long long)noReply, (unsigned long long)xRejects, (unsigned long long)idMismatch,
        (unsigned long long)dgrams, c.commanded - c.sentX);
    if (!firstReceipt.empty()) std::printf("    first receipt:   %s\n", hexOf(firstReceipt).c_str());
    if (!firstPartial.empty()) std::printf("    first partial:   %s\n", hexOf(firstPartial).c_str());
    printHist(c);
    return Summary{label, c.hidHz, c.pathLive, c.pathDrained, c.pathAdmitted, c.drainMs, admittedSum / c.sendS, c.capped};
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--host") o.host = next();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (a == "--mode") o.mode = next();
        else if (a == "--raw-rates") o.rawRates = parseList(next());
        else if (a == "--pkt-rates") o.pktRates = parseList(next());
        else if (a == "--recs") { o.recs.clear(); for (double v : parseList(next())) o.recs.push_back(static_cast<int>(v)); }
        else if (a == "--speed") o.speed = std::atof(next().c_str());
        else if (a == "--cell") o.cellS = std::atof(next().c_str());
        else if (a == "--dt") o.dt = std::atoi(next().c_str());
        else if (a == "--quiet-ms") o.quietMs = std::atof(next().c_str());
        else if (a == "--drain-cap-ms") o.drainCapMs = std::atof(next().c_str());
    }
    const bool doRaw = o.mode == "raw" || o.mode == "all";
    const bool doBatch = o.mode == "batch" || o.mode == "all";

    mover::tuneProcessForTiming();
    Udp u;
    if (!u.open(o)) { std::printf("socket failed\n"); return 1; }

    std::vector<uint8_t> rep;
    std::string interp = "no reply";
    if (u.call(0x1F, nullptr, 0, rep, 60000)) interp = hexOf(rep);
    Meta m;
    if (!rawMeta(u, m)) {
        std::printf("no Raw-meta reply from %s:%u\n", o.host.c_str(), o.port);
        u.close();
        mover::restoreProcessTiming();
        return 1;
    }
    std::printf("========================================================================\n");
    std::printf(" MAKCU  Raw 0x69 / batch 0x6C  closed loop over UDP\n");
    std::printf(" host %s:%u   speed %.2f px/ms   cell %.1f s   ramp +X   dt %d\n", o.host.c_str(), o.port, o.speed, o.cellS, o.dt);
    std::printf(" interpolation (0x1F GET): %s\n", interp.c_str());
    std::printf(" board: last_accepted_id %u, free Raw records %d/%d\n", m.lastId, m.freeRecs, m.capacity);
    if (m.freeRecs < m.capacity) std::printf(" freeing leftover records -> free %d\n", drainRecords(u));
    std::printf("========================================================================\n");

    POINT cursor0{};
    GetCursorPos(&cursor0);
    mover::HookMonitor hook;
    mover::RawInputMonitor rawin;
    if (!hook.start() || !rawin.start()) { std::printf("monitor start failed\n"); u.close(); return 1; }
    Sleep(150);
    mover::tuneTimingThread(-1);

    uint32_t nextId = 0;
    uint32_t maxTerm = 0;
    bool haveX = false;
    auto resync = [&] {
        Meta mm;
        if (rawMeta(u, mm)) nextId = std::max(nextId, mm.lastId + 1);
    };
    std::vector<Summary> rows;

    if (doRaw) {
        std::printf("\n b) Raw 0x69, one record per datagram, closed loop\n");
        for (double rate : o.rawRates) {
            resync();
            Cell c(hook, rawin, o);
            rows.push_back(runRaw(u, c, rate, nextId));
            drainRecords(u);
        }
    }
    if (doBatch) {
        for (int n : o.recs) {
            std::printf("\n c) 0x6C batch, N=%d records per packet, closed loop (receipt -> X)\n", n);
            for (double rate : o.pktRates) {
                resync();
                Cell c(hook, rawin, o);
                rows.push_back(runBatch(u, c, n, rate, nextId, maxTerm, haveX));
                drainRecords(u);
            }
        }
    }

    const auto dev = rawin.primary();
    hook.stop();
    rawin.stop();
    SetCursorPos(cursor0.x, cursor0.y);
    Meta end;
    const bool haveEnd = rawMeta(u, end);

    std::printf("\n------------------------------------------------------------------------\n");
    std::printf(" device for path: %s\n", dev ? dev->name.c_str() : "(none seen)");
    std::printf(" %-26s  moves/s  HID Hz  path(live)  path(drained)  delivered/admitted  drain ms\n", "row");
    for (const auto& r : rows)
        std::printf(" %-26s  %7.0f  %6.0f  %10.3f  %10.3f%s  %18.3f  %8.0f\n", r.label.c_str(), r.rate, r.hidHz,
                    r.pathLive, r.pathDrained, r.capped ? " CAP" : "    ", r.pathAdmitted, r.drainMs);
    if (haveEnd) std::printf(" board after run: last_accepted_id %u, free Raw records %d/%d\n", end.lastId, end.freeRecs, end.capacity);
    std::printf("========================================================================\n");

    u.close();
    mover::restoreProcessTiming();
    return 0;
}
