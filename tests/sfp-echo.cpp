//#define SFP_CONFIG_HISTORY_BUFFER_CAPACITY SFP_SEQ_RANGE

//#include <sfp/sfp.hpp>

#include "simulatedmedium.hpp"

#include <chrono>
#include <iostream>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#define PROPAGATION_DELAY_IN_BYTES 50
#define SPEED_IN_MILLISECONDS_PER_BYTE 1
#define CHANCE_OF_BIT_FLIP 0.0
#define CHANCE_OF_BYTE_DROP 0.0

uint8_t garble (uint8_t octet) {
  for (int i = 0; i < 8; i++) {
    if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BIT_FLIP) {
      octet ^= (1 << i);
    }
  }

  return octet;
}

#if 0
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
    using Clock = std::chrono::system_clock;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <%%-chance-of-byte-drop> <%%-chance-of-bit-flip>\n",
                argv[0]);
        return 1;
    }

    srand(time(NULL));

    SimulatedMedium medium { SimulatedMedium::Milliseconds(100), 19200 };

    std::string hello { "Hello!" };
    auto start = Clock::now();

    medium.write(hello);
    for (int i = 0; i < hello.size(); ++i) {
        auto c = medium.read();
        std::cout << c;
    }

    auto stop = Clock::now();
    auto elapsedMs = std::chrono::duration_cast<SimulatedMedium::Milliseconds>(stop - start);
    std::cout << "\nElapsed milliseconds: " << elapsedMs.count() << std::endl;

#if 0
    sfpInit(&alice);
    sfpSetDeliverCallback(&alice, alice_deliver, NULL);
    sfpSetWriteCallback(&alice, SFP_WRITE_ONE, alice_write, NULL);

    sfpInit(&bob);
    sfpSetDeliverCallback(&bob, bob_deliver, NULL);
    sfpSetWriteCallback(&bob, SFP_WRITE_MULTIPLE, bob_write, NULL);

#ifdef SFP_DEBUG
    sfpSetDebugName(&alice, "alice");
    sfpSetDebugName(&bob, "bob");
#endif

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
