// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_TEST_SV2_HANDSHAKE_TEST_UTIL_H
#define BITCOIN_TEST_SV2_HANDSHAKE_TEST_UTIL_H

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <util/time.h>
#include <util/sock.h>

/**
 * Fragment-tolerant receive loop for test transports.
 *
 * Reads from a DynSock::Pipes::send pipe (bytes written by the system under test)
 * feeding each fragment into the provided ReceivedBytes functor until it returns true
 * (signalling handshake completion or full frame ready) or a timeout elapses.
 *
 * Returns total bytes consumed. Emits BOOST_TEST diagnostics with timing info.
 */
template <class Pipes, class ReceiverFn>
size_t Sv2TestAccumulateRecv(const std::shared_ptr<Pipes>& pipes, ReceiverFn&& receiver,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{2000},
                             const char* phase = "handshake2",
                             bool allow_zero_first = false)
{
    const auto start = std::chrono::steady_clock::now();
    uint8_t buf[4096];
    size_t total = 0;
    int polls = 0;
    for (;;) {
        ssize_t n = pipes->send.GetBytes(buf, sizeof(buf), 0);
        if (n == -1 && errno == EAGAIN) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                BOOST_FAIL("Sv2TestAccumulateRecv timeout in phase=" << phase << " total=" << total << " polls=" << polls);
            }
            UninterruptibleSleep(5ms);
            ++polls;
            continue;
        }
        if (n < 0) {
            BOOST_FAIL("Sv2TestAccumulateRecv unexpected negative read errno=" << errno);
        }
        if (n == 0 && total == 0) {
            if (allow_zero_first) {
                auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                BOOST_TEST_MESSAGE("Sv2TestAccumulateRecv phase=" << phase << " early_eof bytes=0 polls=" << polls << " ms=" << dur_ms);
                return 0;
            }
            BOOST_FAIL("Sv2TestAccumulateRecv zero-length first read (phase=" << phase << ")");
        }
        if (n > 0) {
            std::span<const uint8_t> frag(buf, n);
            bool done = receiver(frag);
            total += n;
            if (done) {
                auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                BOOST_TEST_MESSAGE("Sv2TestAccumulateRecv phase=" << phase << " bytes=" << total << " polls=" << polls << " ms=" << dur_ms);
                return total;
            }
        }
        if (std::chrono::steady_clock::now() - start > timeout) {
            BOOST_FAIL("Sv2TestAccumulateRecv timeout (loop end) phase=" << phase << " total=" << total);
        }
        UninterruptibleSleep(2ms);
        ++polls;
    }
}

#endif // BITCOIN_TEST_SV2_HANDSHAKE_TEST_UTIL_H
