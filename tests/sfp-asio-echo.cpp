#include "sfp/asio/messagequeue.hpp"

#include <boost/asio.hpp>

#include <iostream>
#include <functional>

#include <cstdio>
#include <cstdlib>
#include <ctime>

int main (int argc, char** argv) {
    enum { SUCCEEDED, FAILED };
    int testResult = FAILED;

    boost::asio::io_service ioService;

    using UnixDomainSocket = boost::asio::local::stream_protocol::socket;
    sfp::asio::MessageQueue<UnixDomainSocket> alice { ioService };
    sfp::asio::MessageQueue<UnixDomainSocket> bob { ioService };

#ifdef SFP_CONFIG_DEBUG
    alice.setDebugName("alice");
    bob.setDebugName("bob");
#endif

    boost::asio::local::connect_pair(alice.stream(), bob.stream());

    boost::asio::spawn(ioService, [&] (boost::asio::yield_context yield) {
        try {
            alice.asyncHandshake(yield);
            std::cout << "alice shook hands\n";

            std::vector<uint8_t> in { 10 };
            std::vector<uint8_t> out { 10 };
            std::iota(out.begin(), out.end(), 0);
            assert(in != out);

            alice.asyncSend(boost::asio::buffer(out), yield);
            std::cout << "alice sent a message\n";

            alice.asyncReceive(boost::asio::buffer(in), yield);
            std::cout << "alice received a "
                      << (in == out ? "matching" : "NONMATCHING")
                      << " message\n";

            if (in == out) {
                testResult = SUCCEEDED;
            }

            alice.asyncShutdown(yield);
            std::cout << "alice shut down\n";
        }
        catch (boost::system::system_error& e) {
            if (boost::asio::error::operation_aborted != e.code()) {
                throw;
            }
        }
    });

    boost::asio::spawn(ioService, [&] (boost::asio::yield_context yield) {
        try {
            bob.asyncHandshake(yield);
            std::cout << "bob shook hands\n";

            std::vector<uint8_t> in { 10 };

            bob.asyncReceive(boost::asio::buffer(in), yield);
            std::cout << "bob received a message\n";

            bob.asyncSend(boost::asio::buffer(in), yield);
            std::cout << "bob echoed a message\n";

            bob.asyncShutdown(yield);
            std::cout << "bob shut down\n";
        }
        catch (boost::system::system_error& e) {
            if (boost::asio::error::operation_aborted != e.code()) {
                throw;
            }
        }
    });

    ioService.run();

    return testResult;
}
