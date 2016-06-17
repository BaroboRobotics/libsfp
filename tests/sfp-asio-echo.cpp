#include <sfp/asio/messagequeue.hpp>
#include <util/log.hpp>

#include <boost/optional.hpp>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/spawn.hpp>

#include <iostream>
#include <functional>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <ctime>

int main (int argc, char** argv) {
    enum { SUCCEEDED, FAILED };
    int testResult = FAILED;

    util::log::Logger log;
    boost::asio::io_service ioService;

    using UnixDomainSocket = boost::asio::local::stream_protocol::socket;
    sfp::asio::MessageQueue<UnixDomainSocket> alice { ioService, log };
    sfp::asio::MessageQueue<UnixDomainSocket> bob { ioService, log };

#ifdef SFP_CONFIG_DEBUG
    alice.setDebugName("alice");
    bob.setDebugName("bob");
#endif

    boost::asio::local::connect_pair(alice.stream(), bob.stream());

    boost::asio::spawn(ioService, [&] (boost::asio::yield_context yield) {
        try {
            bob.asyncHandshake(yield);
            std::cout << "bob shook hands\n";

            std::vector<uint8_t> in(1024);

            auto messageSize = bob.asyncReceive(boost::asio::buffer(in), yield);
            std::cout << "bob received a message\n";

            bob.asyncSend(boost::asio::buffer(in, messageSize), yield);
            std::cout << "bob echoed a message\n";

            std::cout << "bob shut down\n";
        }
        catch (boost::system::system_error& e) {
            std::cout << "bob code threw " << e.what() << std::endl;
            bob.close();
            alice.close();
        }
    });

    boost::optional<boost::asio::io_service::work> work {
        boost::in_place(std::ref(ioService))
    };

    std::thread t { [&] () {
        boost::system::error_code ec;
        ioService.run(ec);
        std::cout << "io_service::run reports " << ec.message() << std::endl;
    }};

    try {
        alice.asyncHandshake(boost::asio::use_future).get();
        std::cout << "alice shook hands\n";

        std::vector<uint8_t> in(10);
        std::vector<uint8_t> out(10);
        std::iota(out.begin(), out.end(), 0);
        assert(in != out);

        auto sf = alice.asyncSend(boost::asio::buffer(out), boost::asio::use_future);
        auto rf = alice.asyncReceive(boost::asio::buffer(in), boost::asio::use_future);

        sf.get();
        std::cout << "alice sent a message\n";

        in.resize(rf.get());
        std::cout << "alice received a "
                  << (in == out ? "matching" : "NONMATCHING")
                  << " message\n";

        if (in == out) {
            testResult = SUCCEEDED;
        }

        std::cout << "alice shut down\n";
    }
    catch (boost::system::system_error& e) {
        std::cout << "alice code threw " << e.what() << std::endl;
        bob.close();
        alice.close();
    }

    work = boost::none;

    t.join();

    return testResult;
}
