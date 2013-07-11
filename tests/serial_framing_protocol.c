//#define SFP_CONFIG_HISTORY_BUFFER_CAPACITY SFP_SEQ_RANGE

#include "../serial_framing_protocol.h"

#include <stdio.h>
#include <stdlib.h>

#include <time.h>

#define BYTES_IN_THE_PIPE 100
#define CHANCE_OF_BIT_FLIP 0.02
#define CHANCE_OF_BYTE_DROP 0.02

SFPcontext alice;
SFPcontext bob;

typedef RINGBUF(uint8_t, (1<<15)) pipe_t;

pipe_t alice2bob;
pipe_t bob2alice;

uint8_t garble (uint8_t octet) {
  for (int i = 0; i < 8; i++) {
    if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BIT_FLIP) {
      octet ^= (1 << i);
    }
  }

  return octet;
}

void service_pipe (pipe_t *the_pipe, SFPcontext *dest) {
  while (RINGBUF_SIZE(*the_pipe) > BYTES_IN_THE_PIPE) {
    sfpDeliverOctet(dest, RINGBUF_FRONT(*the_pipe));
    RINGBUF_POP_FRONT(*the_pipe);
  }
}

void flush_pipes (pipe_t *the_pipe0, SFPcontext *dest0,
    pipe_t *the_pipe1, SFPcontext *dest1) {
  while (!RINGBUF_EMPTY(*the_pipe0) || !RINGBUF_EMPTY(*the_pipe1)) {
    if (!RINGBUF_EMPTY(*the_pipe0)) {
      sfpDeliverOctet(dest0, RINGBUF_FRONT(*the_pipe0));
      RINGBUF_POP_FRONT(*the_pipe0);
    }

    if (!RINGBUF_EMPTY(*the_pipe1)) {
      sfpDeliverOctet(dest1, RINGBUF_FRONT(*the_pipe1));
      RINGBUF_POP_FRONT(*the_pipe1);
    }
  }
}

void alice_deliver (SFPpacket *packet, void *data) {
  printf("==== ALICE RECEIVED ====\n\t");
  fwrite(packet->buf, 1, packet->len, stdout);
  printf("\n\n");
  fflush(stdout);
}

void alice_write (uint8_t octet, void *data) {
#if 1
  if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
    return;
  }
#endif
  RINGBUF_PUSH_BACK(alice2bob, garble(octet));
}

void bob_deliver (SFPpacket *packet, void *data) {
  printf("==== BOB RECEIVED ====\n\t");
  fwrite(packet->buf, 1, packet->len, stdout);
  printf("\n\n");
  fflush(stdout);
}

void bob_write (uint8_t *octet, size_t len, void *data) {
  for (size_t i = 0; i < len; ++i) {
#if 1
    if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
      continue;
    }
#endif
    RINGBUF_PUSH_BACK(bob2alice, garble(octet[i]));
  }
}

int main () {
  srand(time(NULL));

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

  for (int i = 0; i < 100; i++) {
    SFPpacket packet;
    packet.len = snprintf(packet.buf, SFP_CONFIG_MAX_PACKET_SIZE, "Hi Bob! (%d)", i);
    sfpWritePacket(&alice, &packet);

#if 1
    packet.len = snprintf(packet.buf, SFP_CONFIG_MAX_PACKET_SIZE, "Shut up Alice! (%d)", i);
    sfpWritePacket(&bob, &packet);
#endif

    service_pipe(&alice2bob, &bob);
    service_pipe(&bob2alice, &alice);
  }

  printf("!!!! FLUSHING PIPES !!!!\n");
  flush_pipes(&bob2alice, &alice, &alice2bob, &bob);
}
