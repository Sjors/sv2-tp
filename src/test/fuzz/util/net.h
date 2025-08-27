// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_FUZZ_UTIL_NET_H
#define BITCOIN_TEST_FUZZ_UTIL_NET_H

#include <net.h>
#include <netaddress.h>
#include <node/connection_types.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/util.h>
#include <test/util/net.h>
#include <threadsafety.h>
#include <util/sock.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

/**
 * Create a CNetAddr. It may have `addr.IsValid() == false`.
 * @param[in,out] fuzzed_data_provider Take data for the address from this, if `rand` is `nullptr`.
 * @param[in,out] rand If not nullptr, take data from it instead of from `fuzzed_data_provider`.
 * Prefer generating addresses using `fuzzed_data_provider` because it is not uniform. Only use
 * `rand` if `fuzzed_data_provider` is exhausted or its data is needed for other things.
 * @return a "random" network address.
 */
CNetAddr ConsumeNetAddr(FuzzedDataProvider& fuzzed_data_provider, FastRandomContext* rand = nullptr) noexcept;

class FuzzedSock : public Sock
{
    FuzzedDataProvider& m_fuzzed_data_provider;

    /**
     * Data to return when `MSG_PEEK` is used as a `Recv()` flag.
     * If `MSG_PEEK` is used, then our `Recv()` returns some random data as usual, but on the next
     * `Recv()` call we must return the same data, thus we remember it here.
     */
    mutable std::vector<uint8_t> m_peek_data;

    /**
     * Whether to pretend that the socket is select(2)-able. This is randomly set in the
     * constructor. It should remain constant so that repeated calls to `IsSelectable()`
     * return the same value.
     */
    const bool m_selectable;

    /**
     * Used to mock the steady clock in methods waiting for a given duration.
     */
    mutable std::chrono::milliseconds m_time;

    /**
     * Set the value of the mocked steady clock such as that many ms have passed.
     */
    void ElapseTime(std::chrono::milliseconds duration) const;

public:
    explicit FuzzedSock(FuzzedDataProvider& fuzzed_data_provider);

    ~FuzzedSock() override;

    FuzzedSock& operator=(Sock&& other) override;

    ssize_t Send(const void* data, size_t len, int flags) const override;

    ssize_t Recv(void* buf, size_t len, int flags) const override;

    int Connect(const sockaddr*, socklen_t) const override;

    int Bind(const sockaddr*, socklen_t) const override;

    int Listen(int backlog) const override;

    std::unique_ptr<Sock> Accept(sockaddr* addr, socklen_t* addr_len) const override;

    int GetSockOpt(int level, int opt_name, void* opt_val, socklen_t* opt_len) const override;

    int SetSockOpt(int level, int opt_name, const void* opt_val, socklen_t opt_len) const override;

    int GetSockName(sockaddr* name, socklen_t* name_len) const override;

    bool SetNonBlocking() const override;

    bool IsSelectable() const override;

    bool Wait(std::chrono::milliseconds timeout, Event requested, Event* occurred = nullptr) const override;

    bool WaitMany(std::chrono::milliseconds timeout, EventsPerSock& events_per_sock) const override;

    bool IsConnected(std::string& errmsg) const override;
};

[[nodiscard]] inline FuzzedSock ConsumeSock(FuzzedDataProvider& fuzzed_data_provider)
{
    return FuzzedSock{fuzzed_data_provider};
}

inline CSubNet ConsumeSubNet(FuzzedDataProvider& fuzzed_data_provider) noexcept
{
    return {ConsumeNetAddr(fuzzed_data_provider), fuzzed_data_provider.ConsumeIntegral<uint8_t>()};
}

inline CService ConsumeService(FuzzedDataProvider& fuzzed_data_provider) noexcept
{
    return {ConsumeNetAddr(fuzzed_data_provider), fuzzed_data_provider.ConsumeIntegral<uint16_t>()};
}

#endif // BITCOIN_TEST_FUZZ_UTIL_NET_H
