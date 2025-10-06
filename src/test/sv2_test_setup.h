// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_TEST_SV2_TEST_SETUP_H
#define BITCOIN_TEST_SV2_TEST_SETUP_H

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>

#include <key.h>
#include <sv2/noise.h>
#include <test/util/random.h>
#include <util/fs.h>
#include <util/time.h>

/**
 * Helper to build a skew-tolerant test certificate.
 *
 * Centralizes the decision to backdate valid_from by an hour so that CI
 * environments with slight clock skew or start-up adjustments do not cause
 * flakiness in certificate validation.
 *
 * Returns the constructed Sv2SignatureNoiseMessage and populates the timing
 * output parameters for additional negative test cases (future start, expiry, etc.).
 */
inline Sv2SignatureNoiseMessage MakeSkewTolerantCertificate(const CKey& static_key,
                                                           const CKey& authority_key,
                                                           uint32_t& out_now,
                                                           uint32_t& out_valid_from,
                                                           uint32_t& out_valid_to,
                                                           uint32_t backdate_secs = 3600,
                                                           uint16_t version = 0)
{
    const auto now = GetTime<std::chrono::seconds>();
    const int64_t now_count = now.count();
    const int64_t clamped_now = std::max<int64_t>(0, now_count);
    out_now = static_cast<uint32_t>(clamped_now);

    const int64_t backdated = std::max<int64_t>(0, clamped_now - static_cast<int64_t>(backdate_secs));
    out_valid_from = static_cast<uint32_t>(backdated);
    out_valid_to = std::numeric_limits<unsigned int>::max();

    return Sv2SignatureNoiseMessage(version, out_valid_from, out_valid_to,
                                    XOnlyPubKey(static_key.GetPubKey()), authority_key);
}


class ECC_Context;

// Minimal test fixture for SV2 tests that avoids node/chainstate dependencies.
struct Sv2BasicTestingSetup {
    FastRandomContext m_rng;
    std::unique_ptr<ECC_Context> m_ecc;

    Sv2BasicTestingSetup();
    ~Sv2BasicTestingSetup();

private:
    fs::path m_tmp_root;
};

#endif // BITCOIN_TEST_SV2_TEST_SETUP_H
