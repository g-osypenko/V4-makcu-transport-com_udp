// Combo probe: interpolator 0x18 + Raw 0x69 in the free report slots, over UDP.
// A fixed datagram grid (one datagram per --gap-us) cycles through a slot pattern:
//   M = 0x18 MOVE with the accumulated delta (interpolator: ~4 reports per command)
//   R = 0x69 RAW_MOVE with the accumulated delta (one report; waits for older 0x18 debt)
//   A = 0x6A consume=1 for the oldest Raw record sent >= 3 ms ago
//   - = no datagram
// Motion: ramp to the right at --speed counts/ms. HID rate and intervals: WH_MOUSE_LL hook;
// path: Raw Input sum of the MAKCU device vs the sum sent. Cursor moves.
//
// Build: g++ -std=c++23 -O2 -I ..\..\comport_way udp_combo.cpp -lws2_32 -lwinmm -static -o udp_combo.exe
// Run:   udp_combo.exe [--pattern MARRMAAR] [--gap-us 2000] [--speed 2] [--duration 5] [--host IP]
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include "mover_common.h"
#include "hook_monitor.h"
#include "rawinput_monitor.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

using mover::Clock;

namespace {

struct Opts {
    std::string host = "192.168.50.175";
    uint16_t port = 8080;
    std::string pattern = "MARRMAAR";
    double gapUs = 2000.0;
    double speed = 2.0;
    double durationS = 5.0;
};

enum class Kind : uint8_t { Move18, Raw69, Ack6A, Query };

struct Pending {
    Kind kind;
    uint32_t id;
    int64_t t;
};

struct RawRec {
    uint32_t id;
    int64_t sentT;  // when 0x69 went out (or last ack attempt)
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

    // Returns the nonce. Reply handling happens on the RX thread.
    uint64_t send(uint8_t op, const uint8_t* pl, uint16_t len, Kind kind, uint32_t id) {
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
        std::memcpy(d + n, pl, len);
        n += len;
        if (kind != Kind::Move18) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_[seq] = Pending{kind, id, Clock::now()};
        }
        ::send(sock_, reinterpret_cast<const char*>(d), static_cast<int>(n), 0);
        ++sent_;
        return seq;
    }

    // Blocking query (only outside the timed loop).
    bool query(uint8_t op, const uint8_t* pl, uint16_t len, std::vector<uint8_t>& out) {
        const uint64_t seq = send(op, pl, len, Kind::Query, 0);
        const int64_t until = Clock::now() + Clock::fromUs(60000);
        while (Clock::now() < until) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = queryReplies_.find(seq);
                if (it != queryReplies_.end()) {
                    out = it->second;
                    queryReplies_.erase(it);
                    return true;
                }
            }
            Sleep(1);
        }
        return false;
    }

    // Raw bookkeeping, shared with the scheduler.
    std::mutex mu_;
    std::deque<RawRec> ackQueue_;                  // records to acknowledge, oldest first
    std::unordered_map<uint32_t, int> recState_;   // id -> 0 sent, 1 queued, 2 freed(submitted), 3 freed(rejected)
    uint64_t raw69Accepted = 0, raw69Errors = 0, acksOk = 0, acksRejected = 0, acksStillQueued = 0,
             acksUnknown = 0, lastErrReason = 0;
    uint64_t rawReason[256]{}, ackReason[256]{};
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
            const uint8_t* res = b + 14;
            if (14 + len > r) continue;
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_.find(seq);
            if (it == pending_.end()) continue;
            const Pending p = it->second;
            pending_.erase(it);
            if (p.kind == Kind::Query) {
                queryReplies_[seq] = std::vector<uint8_t>(res, res + len);
                continue;
            }
            if (len < 6) continue;
            const uint8_t state = res[0], reason = res[1];
            if (p.kind == Kind::Raw69) {
                if (state == 1) {
                    ++raw69Accepted;
                    recState_[p.id] = 1;
                } else {
                    ++raw69Errors;
                    ++rawReason[reason];
                    lastErrReason = reason;
                    recState_[p.id] = 3;
                    for (auto q = ackQueue_.begin(); q != ackQueue_.end(); ++q)
                        if (q->id == p.id) { ackQueue_.erase(q); break; }
                }
            } else if (p.kind == Kind::Ack6A) {
                if (state == 2) { ++acksOk; recState_[p.id] = 2; }
                else if (state == 3) { ++acksRejected; recState_[p.id] = 3; }
                else if (state == 255 && reason == 6) {  // still queued: try again later
                    ++acksStillQueued;
                    ackQueue_.push_front(RawRec{p.id, Clock::now()});
                } else {
                    ++acksUnknown;
                    ++ackReason[reason];
                    lastErrReason = reason;
                    recState_[p.id] = 3;
                }
            }
        }
    }

    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread rx_;
    std::atomic<uint64_t> seq_{0x5000000};
    std::unordered_map<uint64_t, Pending> pending_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> queryReplies_;
};

void put16(uint8_t* p, int v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }

bool rawMeta(Link& l, uint32_t& lastId, int& freeRecs) {
    uint8_t pl[5]{};
    std::vector<uint8_t> r;
    if (!l.query(0x6A, pl, 5, r) || r.size() != 8) return false;
    lastId = static_cast<uint32_t>(r[2] | (r[3] << 8) | (r[4] << 16) | (static_cast<uint32_t>(r[5]) << 24));
    freeRecs = r[6];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--pattern") o.pattern = next();
        else if (a == "--gap-us") o.gapUs = std::atof(next().c_str());
        else if (a == "--speed") o.speed = std::atof(next().c_str());
        else if (a == "--duration") o.durationS = std::atof(next().c_str());
        else if (a == "--host") o.host = next();
    }
    mover::tuneProcessForTiming();
    Link link;
    if (!link.open(o)) { std::printf("socket failed\n"); return 1; }
    uint32_t lastId = 0;
    int freeRecs = 0;
    if (!rawMeta(link, lastId, freeRecs)) { std::printf("no reply from board\n"); return 1; }
    std::printf("board: last_accepted_id %u, free records %d/16\n", lastId, freeRecs);
    if (freeRecs < 12) { std::printf("too few free Raw records, aborting\n"); return 1; }

    mover::HookMonitor hook;
    mover::RawInputMonitor rawin;
    if (!hook.start() || !rawin.start()) { std::printf("monitor start failed\n"); return 1; }
    Sleep(150);

    int slotsM = 0, slotsR = 0, slotsA = 0;
    for (char c : o.pattern) { slotsM += c == 'M'; slotsR += c == 'R'; slotsA += c == 'A'; }
    const double cycleMs = o.pattern.size() * o.gapUs / 1000.0;
    std::printf("pattern %s, gap %.0f us (%.0f datagrams/s max), cycle %.1f ms: %d x 0x18, %d x 0x69, %d x 0x6A\n",
        o.pattern.c_str(), o.gapUs, 1e6 / o.gapUs, cycleMs, slotsM, slotsR, slotsA);
    std::printf("ramp %.2f counts/ms for %.1f s. Cursor moves.\n", o.speed, o.durationS);

    mover::tuneTimingThread(-1);
    uint32_t nextId = lastId + 1;
    int64_t sentX = 0;
    uint64_t n18 = 0, n69 = 0, n6A = 0, ackSkipped = 0, rawSkippedZero = 0;
    const int64_t gap = Clock::fromUs(o.gapUs);
    const int64_t ackAge = Clock::fromUs(3000);
    const uint64_t total = static_cast<uint64_t>(o.durationS * 1e6 / o.gapUs);

    hook.reset();
    rawin.reset();
    const int64_t t0 = Clock::now() + Clock::fromUs(2000);
    for (uint64_t k = 0; k < total; ++k) {
        const int64_t due = t0 + static_cast<int64_t>(k) * gap;
        while (Clock::now() < due) YieldProcessor();
        const double tMs = Clock::toUs(Clock::now() - t0) / 1000.0;
        const int64_t target = static_cast<int64_t>(o.speed * tMs);  // floor, ramp to the right
        const int64_t carry = target - sentX;
        const char role = o.pattern[k % o.pattern.size()];
        if (role == 'M') {
            if (carry != 0) {
                uint8_t pl[4];
                put16(pl, static_cast<int>(carry));
                put16(pl + 2, 0);
                link.send(0x18, pl, 4, Kind::Move18, 0);
                sentX += carry;
                ++n18;
            }
        } else if (role == 'R') {
            if (carry == 0) { ++rawSkippedZero; continue; }
            uint8_t pl[10];
            const uint32_t id = nextId++;
            put32(pl, id);
            put16(pl + 4, static_cast<int>(carry));
            put16(pl + 6, 0);
            put16(pl + 8, 0);
            {
                std::lock_guard<std::mutex> lk(link.mu_);
                link.recState_[id] = 0;
                link.ackQueue_.push_back(RawRec{id, Clock::now()});
            }
            link.send(0x69, pl, 10, Kind::Raw69, id);
            sentX += carry;
            ++n69;
        } else if (role == 'A') {
            uint32_t id = 0;
            {
                std::lock_guard<std::mutex> lk(link.mu_);
                if (!link.ackQueue_.empty() && Clock::now() - link.ackQueue_.front().sentT >= ackAge) {
                    id = link.ackQueue_.front().id;
                    link.ackQueue_.pop_front();
                }
            }
            if (!id) { ++ackSkipped; continue; }
            uint8_t pl[5];
            put32(pl, id);
            pl[4] = 1;
            link.send(0x6A, pl, 5, Kind::Ack6A, id);
            ++n6A;
        }
    }
    const double runS = Clock::toUs(Clock::now() - t0) / 1e6;
    const uint64_t hid = hook.count();
    const auto hist = hook.histogram();
    Sleep(120);  // let the board finish the last interpolation steps and Raw records

    // Cleanup: acknowledge every Raw record still on the board.
    const int64_t until = Clock::now() + Clock::fromUs(600000);
    while (Clock::now() < until) {
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> lk(link.mu_);
            if (!link.ackQueue_.empty()) { id = link.ackQueue_.front().id; link.ackQueue_.pop_front(); }
        }
        if (!id) {
            bool open = false;
            std::lock_guard<std::mutex> lk(link.mu_);
            for (auto& [rid, st] : link.recState_) if (st < 2) { link.ackQueue_.push_back(RawRec{rid, 0}); open = true; }
            if (!open) break;
            continue;
        }
        uint8_t pl[5];
        put32(pl, id);
        pl[4] = 1;
        link.send(0x6A, pl, 5, Kind::Ack6A, id);
        Sleep(4);
    }
    Sleep(60);
    const auto dev = rawin.primary();
    hook.stop();
    rawin.stop();

    std::printf("\nsent: 0x18 %llu, 0x69 %llu, 0x6A %llu (A slots skipped %llu, R slots with zero delta %llu)\n",
        (unsigned long long)n18, (unsigned long long)n69, (unsigned long long)n6A,
        (unsigned long long)ackSkipped, (unsigned long long)rawSkippedZero);
    std::printf("datagrams %.0f/s over %.2f s\n", link.sent_.load() / runS, runS);
    {
        std::lock_guard<std::mutex> lk(link.mu_);
        std::printf("raw: accepted %llu, 0x69 errors %llu (last reason %llu), acks ok %llu, rejected %llu, still queued %llu, unknown %llu\n",
            (unsigned long long)link.raw69Accepted, (unsigned long long)link.raw69Errors,
            (unsigned long long)link.lastErrReason, (unsigned long long)link.acksOk,
            (unsigned long long)link.acksRejected, (unsigned long long)link.acksStillQueued,
            (unsigned long long)link.acksUnknown);
        for (int r = 0; r < 256; ++r)
            if (link.rawReason[r]) std::printf("  0x69 reason %d: %llu\n", r, (unsigned long long)link.rawReason[r]);
        for (int r = 0; r < 256; ++r)
            if (link.ackReason[r]) std::printf("  0x6A reason %d: %llu\n", r, (unsigned long long)link.ackReason[r]);
    }
    uint64_t histTotal = 0;
    for (auto h : hist) histTotal += h;
    std::printf("HID (LL hook): %.0f Hz | intervals:", hid / runS);
    for (size_t i = 0; i < hist.size(); ++i)
        std::printf("  %s %.1f%%", mover::HookMonitor::kBucketNames[i], histTotal ? 100.0 * hist[i] / histTotal : 0.0);
    std::printf("\n");
    if (dev) {
        std::printf("path (%s): sent %lld, received %lld -> %s (ratio %.3f)\n", dev->name.c_str(),
            (long long)sentX, (long long)dev->sumX, dev->sumX == sentX ? "MATCH" : "MISMATCH",
            sentX ? double(dev->sumX) / double(sentX) : 0.0);
    } else {
        std::printf("path: no Raw Input from the device\n");
    }
    uint32_t endLast = 0;
    int endFree = 0;
    if (rawMeta(link, endLast, endFree)) std::printf("board after cleanup: free records %d/16\n", endFree);
    link.close();
    mover::restoreProcessTiming();
    return 0;
}
