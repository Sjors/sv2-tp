// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/capnp/common-types.h>
#include <ipc/capnp/common.capnp.h>
#include <ipc/serialized.h>
#include <primitives/block.h>

#include <boost/test/unit_test.hpp>
#include <capnp/message.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace {
struct DataOutput {
    ipc::capnp::messages::BlockRef::Builder field;
    auto init(size_t size) { return field.initHash(size); }
    [[maybe_unused]] auto get() { return field.getHash(); }
    [[maybe_unused]] void setHas() {}
};

struct IpcFixture {
    mp::EventLoop loop{"serialized-test", [](mp::LogMessage) {}};
    mp::EventLoopRef loop_ref{loop};
    decltype(loop.m_io_context.provider->newTwoWayPipe()) pipe{loop.m_io_context.provider->newTwoWayPipe()};
    std::optional<mp::Connection> connection{std::in_place, loop, kj::mv(pipe.ends[0])};
    mp::InvokeContext context{*connection};
    ~IpcFixture()
    {
        loop.m_task_set->add(kj::evalLater([&] {
            connection.reset();
            pipe.ends[1] = nullptr;
            loop_ref.reset();
        }));
        loop.loop();
    }
};

template <typename Payload>
auto& PayloadBytes(Payload& value)
{
    return value;
}

template <typename T>
auto& PayloadBytes(ipc::Serialized<T>& value)
{
    return value.bytes;
}

template <typename Payload>
void CheckOwnedRoundTrip(mp::InvokeContext& context)
{
    Payload original;
    auto& original_bytes = PayloadBytes(original);
    original_bytes = {0x01, 0x02, 0x00, 0xff};
    Payload copy;
    {
        capnp::MallocMessageBuilder message;
        auto field = message.initRoot<ipc::capnp::messages::BlockRef>();
        mp::BuildField(mp::TypeList<Payload>{}, context, DataOutput{field}, original);
        auto data = field.asReader().getHash();
        mp::ReadField(mp::TypeList<Payload>{}, context, mp::ValueField{data}, mp::ReadDestUpdate{copy});
        BOOST_CHECK(PayloadBytes(copy) == original_bytes);
        field.getHash()[0] = 0x42;
    }
    BOOST_CHECK(PayloadBytes(copy) == original_bytes);
}

template <typename Payload>
void CheckEmptyRoundTrip(mp::InvokeContext& context)
{
    capnp::MallocMessageBuilder message;
    auto field = message.initRoot<ipc::capnp::messages::BlockRef>();
    Payload empty;
    mp::BuildField(mp::TypeList<Payload>{}, context, DataOutput{field}, empty);
    auto data = field.asReader().getHash();
    Payload copy;
    PayloadBytes(copy) = {1};
    mp::ReadField(mp::TypeList<Payload>{}, context, mp::ValueField{data}, mp::ReadDestUpdate{copy});
    BOOST_CHECK(PayloadBytes(copy).empty());
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(ipc_serialized_tests, IpcFixture)

BOOST_AUTO_TEST_CASE(owned_roundtrip)
{
    CheckOwnedRoundTrip<std::vector<unsigned char>>(context);
    // Deliberately invalid block bytes: retaining them must not decode a block.
    CheckOwnedRoundTrip<ipc::Serialized<CBlock>>(context);
}

BOOST_AUTO_TEST_CASE(empty_roundtrip)
{
    CheckEmptyRoundTrip<std::vector<unsigned char>>(context);
    CheckEmptyRoundTrip<ipc::Serialized<CBlock>>(context);
}

BOOST_AUTO_TEST_SUITE_END()
