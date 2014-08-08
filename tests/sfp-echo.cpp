#define SFP_CONFIG_HISTORY_BUFFER_CAPACITY SFP_SEQ_RANGE
#define BOOST_BIND_NO_PLACEHOLDERS // don't put _1, _2, etc. in global namespcae
#define SFP_CONFIG_THREADSAFE

#include "garbler.hpp"
#include "sfp/context.hpp"
#include "simulatedmedium.hpp"

#include <boost/iterator/function_input_iterator.hpp>

#include <chrono>
#include <iostream>
#include <functional>

#include <cstdio>
#include <cstdlib>
#include <ctime>

// expose _1, _2
using namespace std::placeholders;

const SimulatedMedium::Milliseconds kPropagationDelay { 10 };
const SimulatedMedium::Baud kBaud { 19200 };

int main (int argc, char** argv) {
    enum { SUCCEEDED, FAILED };
    int testResult = SUCCEEDED;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <probability-of-byte-drop> <probability-of-bit-flip>\n",
                argv[0]);
        return 1;
    }

    const double byteDropProbability = std::stod(std::string(argv[1]));
    const double bitFlipProbability = std::stod(std::string(argv[2]));

    printf("Using %f byte-drop-probability, %f bit-flip-probability\n",
            byteDropProbability, bitFlipProbability);

    using Message = std::vector<uint8_t>;
    std::mutex messagesMutex;
    std::queue<Message> sentMessages;

    sfp::Context alice {
#ifdef SFP_CONFIG_DEBUG
        std::string("alice")
#endif
    };
    sfp::Context bob {
#ifdef SFP_CONFIG_DEBUG
        std::string("bob")
#endif
    };

    SimulatedMedium aliceToBob { kPropagationDelay, kBaud, "aliceToBob" };
    SimulatedMedium bobToAlice { kPropagationDelay, kBaud, "bobToAlice" };

    printf("aliceToBob byte capacity: %u\n", aliceToBob.capacity());
    printf("bobToAlice byte capacity: %u\n", bobToAlice.capacity());

    Garbler bobsGarbler { byteDropProbability, bitFlipProbability };
    Garbler alicesGarbler { byteDropProbability, bitFlipProbability };

    alice.output.connect(BIND_MEM_CB(&Garbler::input, &alicesGarbler));
    bob.output.connect(BIND_MEM_CB(&Garbler::input, &bobsGarbler));

    alicesGarbler.output.connect(BIND_MEM_CB(&SimulatedMedium::input, &aliceToBob));
    bobsGarbler.output.connect(BIND_MEM_CB(&SimulatedMedium::input, &bobToAlice));

    aliceToBob.output.connect(BIND_MEM_CB(&sfp::Context::input, &bob));
    bobToAlice.output.connect(BIND_MEM_CB(&sfp::Context::input, &alice));

    auto aliceMessageHandler = [&] (const uint8_t* buf, size_t len) {
        if (!buf || !len) { return; }
        Message m { buf, buf + len };
        std::lock_guard<std::mutex> lock { messagesMutex };
        if (m.size() == sentMessages.front().size() &&
                std::equal(m.cbegin(), m.cend(), sentMessages.front().cbegin())) {
            printf("Alice received a matching message.\n");
        }
        else {
            testResult = FAILED;
            printf("=== Message mismatch ===\n");
            printf("Expected: ");
            for (auto b : sentMessages.front()) {
                printf("%02x ", b);
            }
            printf("\n");
            printf("Received: ");
            for (auto b : m) {
                printf("%02x ", b);
            }
            printf("\n");
            exit(1);
        }
        sentMessages.pop();
    };
    using Lambda = decltype(aliceMessageHandler);
    alice.messageReceived.connect(BIND_MEM_CB(&Lambda::operator(), &aliceMessageHandler));

    // loopback
    bob.messageReceived.connect(BIND_MEM_CB(&sfp::Context::sendMessage, &bob));

    printf("Alice connects to Bob ...");
    alice.connect();
    while (!alice.isConnected()) {
        alice.output(0x7e);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf(" success!\n");
    while (!bob.isConnected()) {
        alice.output(0x7e);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("Bob is connected, too, and they're both happy.\n");

    // Function object which returns a random byte every time it is invoked.
    struct RandomByte {
        RandomByte ()
                : randomEngine(randomDevice())
                , uniformDistribution(0, 255) { }

        using result_type = uint8_t;
        uint8_t operator() () {
            return uniformDistribution(randomEngine);
        }

    private:
        std::random_device randomDevice;
        std::default_random_engine randomEngine;
        std::uniform_int_distribution<uint8_t> uniformDistribution;
    } randomByte;

    auto randomByteIterator = [&randomByte] (int state) {
        return boost::make_function_input_iterator(randomByte, state);
    };

    printf("sending messages...\n");
    const int nMessages = 100;
    for (int i = 0; i < nMessages; ++i) {
        std::lock_guard<std::mutex> lock { messagesMutex };
        Message m { randomByteIterator(0), randomByteIterator(10) };
        sentMessages.push(m);
        alice.sendMessage(m.data(), m.size());
    }

    printf("done sending messages\n");
    while (true) {
        {
            std::lock_guard<std::mutex> lock { messagesMutex };
            if (sentMessages.empty()) { break; }
            alice.output(0x7e);
            bob.output(0x7e);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return testResult;
}
