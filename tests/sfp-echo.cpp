//#define SFP_CONFIG_HISTORY_BUFFER_CAPACITY SFP_SEQ_RANGE
#define BOOST_BIND_NO_PLACEHOLDERS // don't put _1, _2, etc. in global namespcae

#include "simulatedmedium.hpp"
#include "sfphost.hpp"

#include <boost/iterator/counting_iterator.hpp>
#include <boost/signals2.hpp>

#include <chrono>
#include <iostream>
#include <functional>

#include <cstdio>
#include <cstdlib>
#include <ctime>

const SimulatedMedium::Milliseconds kPropagationDelay { 100 };
const SimulatedMedium::Baud kBaud { 19200 };

#if 0
uint8_t garble (uint8_t octet) {
  for (int i = 0; i < 8; i++) {
    if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BIT_FLIP) {
      octet ^= (1 << i);
    }
  }

  return octet;
}

void alice_write (uint8_t octet, void *data) {
#if 1
  if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
    return;
  }
#endif
  RINGBUF_PUSH_BACK(aliceToBob, garble(octet));
}
#endif

int main (int argc, char** argv) {
    // expose _1, _2
    using namespace std::placeholders;
    using Clock = std::chrono::steady_clock;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <%%-chance-of-byte-drop> <%%-chance-of-bit-flip>\n",
                argv[0]);
        return 1;
    }

    srand(time(NULL));

    SfpHost alice { std::string("alice") };
    SfpHost bob { std::string("bob") };

    SimulatedMedium aliceToBob { kPropagationDelay, kBaud };
    SimulatedMedium bobToAlice { kPropagationDelay, kBaud };

    alice.output.connect(std::bind(&decltype(aliceToBob)::input, &aliceToBob, _1));
    bob.output.connect(std::bind(&decltype(bobToAlice)::input, &bobToAlice, _1));

    aliceToBob.output.connect(std::bind(&decltype(bob)::input, &bob, _1));
    bobToAlice.output.connect(std::bind(&decltype(alice)::input, &alice, _1));

    alice.messageReceived.connect(
        [] (uint8_t* buf, size_t len) {
            printf("=== Alice received ===\n");
            for (size_t i = 0; i < len; ++i) {
                printf("%02x ", buf[i]);
            }
            printf("\n");
        }
    );

    // loopback
    bob.messageReceived.connect(std::bind(&decltype(bob)::sendMessage, &bob, _1, _2));

    printf("Alice connects to Bob ...");
    alice.connect();
    printf(" success!\n");
    bob.waitUntilConnected();
    printf("Bob is connected, too, and they're both happy.\n");

    std::string hello { "Hello!" };
    alice.sendMessage(reinterpret_cast<const uint8_t*>(hello.c_str()), hello.size());
    std::this_thread::sleep_for(std::chrono::seconds(1));

#if 0
    using IntIterator = boost::counting_iterator<int>;

    std::string message {
        std::string("Hey guys I'm a message. :) I can contain all kinds of "
                "shit, like binary data: ") +
        std::string(IntIterator(0), IntIterator(100)) +
        std::string(", or ... well, that's all, I guess.")
    };

    for (uint8_t c : message) {
        printf("%02x ", c);
    }
    printf("\n");

    std::string hello { "Hello!" };

    auto start = Clock::now();

    aliceToBob.write(hello);
    for (int i = 0; i < hello.size(); ++i) {
        auto c = aliceToBob.read();
        std::cout << c;
    }

    auto stop = Clock::now();
    auto elapsedMs = std::chrono::duration_cast<SimulatedMedium::Milliseconds>(stop - start);
    std::cout << "\nElapsed milliseconds: " << elapsedMs.count() << std::endl;

    sfpConnect(&alice);

    for (int i = 0; i < 20; i++) {
    do {
      alice_write('\x7e', NULL);
      bob_write("\x7e", 1, NULL);
      serviceMedium(&aliceToBob, &bob);
      serviceMedium(&bobToAlice, &alice);
    } while (!sfpIsConnected(&alice) || !sfpIsConnected(&bob));

    SFPpacket packet;

    packet.len = snprintf((char *)packet.buf, SFP_CONFIG_MAX_PACKET_SIZE, "Hi Bob! (%d)", i);
    sfpWritePacket(&alice, &packet);

#if 1
    packet.len = snprintf((char *)packet.buf, SFP_CONFIG_MAX_PACKET_SIZE, "Shut up Alice! (%d)", i);
    sfpWritePacket(&bob, &packet);
#endif

    if (13 == i) {
      printf("!!!! ALICE RECONNECTING !!!!\n");
      sfpConnect(&alice);
    }
#endif
}
