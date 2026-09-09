#include <boost/test/unit_test.hpp>
#include <sv2/messages.h>
#include <test/sv2_connman_tester.h>
#include <test/sv2_test_setup.h>

BOOST_FIXTURE_TEST_SUITE(sv2_connman_tests, Sv2BasicTestingSetup)

BOOST_AUTO_TEST_CASE(client_tests)
{
    ConnTester tester{};

    BOOST_REQUIRE(!tester.IsConnected());
    tester.handshake();
    BOOST_REQUIRE(!tester.IsFullyConnected());

    // After the handshake the remote peer must send a SetupConnection message to the
    // Template Provider.

    // An empty SetupConnection message should cause disconnection
    node::Sv2NetMsg sv2_msg{node::Sv2MsgType::SETUP_CONNECTION, {}};
    tester.RemoteToLocalMsg(sv2_msg);
    // Consume potential disconnect bytes via tolerant reader (expecting closure: helper would timeout if bytes keep coming)
    // Fall back to legacy single read; if bytes present they should form a complete frame immediately.
    auto first = tester.LocalToRemoteBytes();
    BOOST_REQUIRE_EQUAL(first, 0);

    BOOST_REQUIRE(!tester.IsConnected());

    BOOST_TEST_MESSAGE("Reconnect after empty message");

    // Reconnect
    tester.handshake();
    BOOST_TEST_MESSAGE("Handshake done, send SetupConnectionMsg");

    node::Sv2NetMsg setup{tester.SetupConnectionMsg()};
    tester.RemoteToLocalMsg(setup);
    // SetupConnection.Success is 6 bytes
    BOOST_REQUIRE_EQUAL(tester.LocalToRemoteBytes(), SV2_HEADER_ENCRYPTED_SIZE + 6 + Poly1305::TAGLEN);
    BOOST_REQUIRE(tester.IsFullyConnected());

    std::vector<uint8_t> coinbase_output_max_additional_size_bytes{
        0x01, 0x00, 0x00, 0x00
    };
    node::Sv2NetMsg msg{node::Sv2MsgType::COINBASE_OUTPUT_CONSTRAINTS, std::move(coinbase_output_max_additional_size_bytes)};
    // No reply expected, not yet implemented
    tester.RemoteToLocalMsg(msg);
}

// The network layer never drops a SubmitSolution, even when the client
// skipped CoinbaseOutputConstraints.
BOOST_AUTO_TEST_CASE(submit_solution_forwarded)
{
    Sv2LogCapture logs;
    ConnTester tester{};

    tester.handshake();
    node::Sv2NetMsg setup{tester.SetupConnectionMsg()};
    tester.RemoteToLocalMsg(setup);
    BOOST_REQUIRE_EQUAL(tester.LocalToRemoteBytes(), SV2_HEADER_ENCRYPTED_SIZE + 6 + Poly1305::TAGLEN);
    BOOST_REQUIRE(tester.IsFullyConnected());

    BOOST_TEST_MESSAGE("SubmitSolution without CoinbaseOutputConstraints is still forwarded");
    node::Sv2NetMsg premature_solution{TestSubmitSolutionMsg()};
    tester.RemoteToLocalMsg(premature_solution);
    BOOST_REQUIRE(tester.WaitForCount(tester.m_submit_solution_count, 1));
    BOOST_REQUIRE(tester.IsConnected());
    BOOST_REQUIRE(logs.WaitFor("Received SubmitSolution before SetupConnection and CoinbaseOutputConstraints"));

    std::vector<uint8_t> coinbase_output_max_additional_size_bytes{
        0x01, 0x00, 0x00, 0x00
    };
    node::Sv2NetMsg constraints{node::Sv2MsgType::COINBASE_OUTPUT_CONSTRAINTS, std::move(coinbase_output_max_additional_size_bytes)};
    tester.RemoteToLocalMsg(constraints);

    node::Sv2NetMsg solution{TestSubmitSolutionMsg()};
    tester.RemoteToLocalMsg(solution);
    BOOST_REQUIRE(tester.WaitForCount(tester.m_submit_solution_count, 2));
    BOOST_REQUIRE(tester.IsConnected());
}

BOOST_AUTO_TEST_SUITE_END()
