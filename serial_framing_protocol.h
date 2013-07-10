#ifndef _SERIAL_FRAMING_PROTOCOL_H_
#define _SERIAL_FRAMING_PROTOCOL_H_

#include "net_byte_order.h"
#include "ringbuf.h"

#include <stdlib.h>

//#define SFP_DEBUG

#ifndef SFP_CONFIG_HISTORY_CAPACITY
/* Must be a power of two for use in the ring buffer. */
#define SFP_CONFIG_HISTORY_CAPACITY (1<<4)
#endif

#ifndef SFP_CONFIG_MAX_FRAME_SIZE
#define SFP_CONFIG_MAX_FRAME_SIZE 256
#endif

typedef uint8_t sfp_seq_t;
typedef sfp_seq_t sfp_header_t;
typedef uint16_t sfp_crc_t;

/* Must be kept in sync with sfp_crc_t's size. */
#define sfpByteSwapCRC netByteOrder16

typedef struct {
  uint8_t buf[SFP_CONFIG_MAX_FRAME_SIZE];
  size_t len;
} sfp_frame_t;

typedef void (*sfp_deliver_func_t) (sfp_frame_t *frame);
typedef void (*sfp_write_func_t) (uint8_t octet);
typedef void (*sfp_lock_func_t) (void *mutex);
typedef void (*sfp_unlock_func_t) (void *mutex);

typedef enum {
  SFP_ESCAPE_STATE_NORMAL,
  SFP_ESCAPE_STATE_ESCAPING
} sfp_escape_state_t;

typedef enum {
  SFP_FRAME_STATE_NEW,
  SFP_FRAME_STATE_RECEIVING
} sfp_frame_state_t;

typedef struct {
  sfp_seq_t seq;
  sfp_crc_t crc;

  RINGBUF(sfp_frame_t, SFP_CONFIG_HISTORY_CAPACITY) history;

  sfp_write_func_t write;

  sfp_lock_func_t lock;
  sfp_unlock_func_t unlock;

  void *mutex;
} sfp_transmitter_t;

typedef struct {
  sfp_seq_t seq;
  sfp_crc_t crc;

  sfp_escape_state_t escapeState;
  sfp_frame_state_t frameState;

  sfp_header_t header;
  sfp_frame_t frame;

  sfp_deliver_func_t deliver;
} sfp_receiver_t;

typedef struct {
  sfp_transmitter_t tx;
  sfp_receiver_t rx;

#ifdef SFP_DEBUG
  const char *debugName;
#endif
} sfp_t;

void sfpDeliverOctet (sfp_t *context, uint8_t octet);
void sfpWriteFrame (sfp_t *context, sfp_frame_t *frame);
void sfpInit (sfp_t *context, sfp_deliver_func_t deliver, sfp_write_func_t write,
    sfp_lock_func_t lock, sfp_unlock_func_t unlock, void *mutex);

/* SFP reserved octets */
enum {
  SFP_ESC  = 0x7d,
  SFP_FLAG = 0x7e
};

#define SFP_ESC_FLIP_BIT 0x10 // the fifth bit, like in HDLC
#define SFP_NAK_BIT 0x80
#define SFP_SEQ_RANGE SFP_NAK_BIT

#define SFP_CRC_SIZE sizeof(sfp_crc_t)

#define SFP_CRC_PRESET 0xffff   /* The initial value for the CRC, recommended
                                 * by an article in Dr. Dobb's Journal */
#define SFP_INITIAL_SEQ 0


#endif
