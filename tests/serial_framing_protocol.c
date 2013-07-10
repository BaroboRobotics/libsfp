//#define SFP_CONFIG_HISTORY_BUFFER_CAPACITY SFP_SEQ_RANGE

#include "../serial_framing_protocol.h"

#include <stdio.h>
#include <stdlib.h>

#include <time.h>

#define BYTES_IN_THE_PIPE 100
#define CHANCE_OF_BIT_FLIP 0.02
#define CHANCE_OF_BYTE_DROP 0.02

sfp_t alice;
sfp_t bob;

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

void service_pipe (pipe_t *the_pipe, sfp_t *dest) {
  while (RINGBUF_SIZE(*the_pipe) > BYTES_IN_THE_PIPE) {
    sfpDeliverOctet(dest, RINGBUF_FRONT(*the_pipe));
    RINGBUF_POP_FRONT(*the_pipe);
  }
}

void flush_pipes (pipe_t *the_pipe0, sfp_t *dest0,
    pipe_t *the_pipe1, sfp_t *dest1) {
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

void alice_deliver (sfp_frame_t *frame) {
  printf("==== ALICE RECEIVED ====\n\t");
  fwrite(frame->buf, 1, frame->len, stdout);
  printf("\n\n");
  fflush(stdout);
}

void alice_write (uint8_t octet) {
#if 1
  if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
    return;
  }
#endif
  RINGBUF_PUSH_BACK(alice2bob, garble(octet));
}

void bob_deliver (sfp_frame_t *frame) {
  printf("==== BOB RECEIVED ====\n\t");
  fwrite(frame->buf, 1, frame->len, stdout);
  printf("\n\n");
  fflush(stdout);
}

void bob_write (uint8_t octet) {
#if 1
  if ((double)rand() / (double)RAND_MAX < CHANCE_OF_BYTE_DROP) {
    return;
  }
#endif
  RINGBUF_PUSH_BACK(bob2alice, garble(octet));
}

int main () {
  srand(time(NULL));
  sfpInit(&alice, alice_deliver, alice_write, NULL, NULL, NULL);
  sfpInit(&bob, bob_deliver, bob_write, NULL, NULL, NULL);

#ifdef SFP_DEBUG
  alice.debugName = "alice";
  bob.debugName = "bob";
#endif

  for (int i = 0; i < 100; i++) {
    sfp_frame_t frame;
    frame.len = snprintf(frame.buf, SFP_CONFIG_MAX_FRAME_SIZE, "Hi Bob! (%d)", i);
    sfpWriteFrame(&alice, &frame);

#if 1
    frame.len = snprintf(frame.buf, SFP_CONFIG_MAX_FRAME_SIZE, "Shut up Alice! (%d)", i);
    sfpWriteFrame(&bob, &frame);
#endif

    service_pipe(&alice2bob, &bob);
    service_pipe(&bob2alice, &alice);
  }

  printf("!!!! FLUSHING PIPES !!!!\n");
  flush_pipes(&bob2alice, &alice, &alice2bob, &bob);
}
