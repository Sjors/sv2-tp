// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/process.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <stdexcept>

BOOST_AUTO_TEST_SUITE(ipc_process_tests)

// Test command line parsing in ipc::Process::checkSpawned().
BOOST_AUTO_TEST_CASE(check_spawned_test)
{
    std::unique_ptr<ipc::Process> process{ipc::MakeProcess()};
    char arg0[]{"bitcoin-node"};
    char arg_spawn[]{"-ipcchild"};
    char arg_invalid[]{"invalid"};
    char arg_other[]{"-ipcbind=unix"};

    // no -ipcchild arg.
    {
        char* argv[]{arg0};
        BOOST_CHECK(!process->checkSpawned(1, argv));
    }
    {
        char* argv[]{arg0, arg_other};
        BOOST_CHECK(!process->checkSpawned(2, argv));
    }
    {
        char* argv[]{arg0, arg_other, arg_invalid};
        BOOST_CHECK(!process->checkSpawned(3, argv));
    }
    // -ipcchild without a value.
    {
        char* argv[]{arg0, arg_spawn};
        BOOST_CHECK(!process->checkSpawned(2, argv));
    }
    // -ipcchild combined with other arguments.
    {
        char* argv[]{arg0, arg_spawn, arg_invalid, arg_other};
        BOOST_CHECK(!process->checkSpawned(4, argv));
    }
    // -ipcchild with a value that is not a valid way of connecting to the
    // parent process.
    {
        char* argv[]{arg0, arg_spawn, arg_invalid};
        BOOST_CHECK_THROW(process->checkSpawned(3, argv), std::runtime_error);
    }
}

BOOST_AUTO_TEST_SUITE_END()
