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
  if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
    return;
  }
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

    using IntIterator = boost::counting_iterator<int>;

    // TODO: keep a queue of sent messages. Add messages to this queue and call
    // alice.sendMessage() at the same time. Alice's messageReceived handler
    // shall pop messages off the queue and compare them with messages that she
    // sent.
}
