#ifndef _SERIAL_FRAMING_PROTOCOL_H_
#define _SERIAL_FRAMING_PROTOCOL_H_

#include "net_byte_order.h"
#include "ringbuf.h"

#include <stdlib.h>

#define SFP_WARN
//#define SFP_DEBUG

#ifdef SFP_DEBUG
#ifndef SFP_CONFIG_MAX_DEBUG_NAME_SIZE
#define SFP_CONFIG_MAX_DEBUG_NAME_SIZE 256
#endif
#endif

#ifndef SFP_CONFIG_HISTORY_CAPACITY
/* Must be a power of two for use in the ring buffer. */
#define SFP_CONFIG_HISTORY_CAPACITY (1<<4)
#endif

#ifndef SFP_CONFIG_MAX_PACKET_SIZE
#define SFP_CONFIG_MAX_PACKET_SIZE 256
#endif

#ifndef SFP_CONFIG_WRITEBUF_SIZE
#define SFP_CONFIG_WRITEBUF_SIZE 256
#endif

typedef uint8_t SFPseq;
typedef uint8_t SFPheader;
typedef uint16_t SFPcrc;

/* Must be kept in sync with SFPcrc's size. */
#define sfpByteSwapCRC netByteOrder16

typedef struct SFPpacket {
  uint8_t buf[SFP_CONFIG_MAX_PACKET_SIZE];
  size_t len;
} SFPpacket;

typedef void (*SFPdeliverfun) (SFPpacket *packet, void *userdata);
typedef void (*SFPwrite1fun) (uint8_t octet, void *userdata);
typedef void (*SFPwritenfun) (uint8_t *octets, size_t len, void *userdata);
typedef void (*SFPlockfun) (void *userdata);
typedef void (*SFPunlockfun) (void *userdata);

typedef enum {
  SFP_ESCAPE_STATE_NORMAL,
  SFP_ESCAPE_STATE_ESCAPING
} SFPescapestate;

typedef enum {
  SFP_FRAME_STATE_NEW,
  SFP_FRAME_STATE_RECEIVING
} SFPframestate;

typedef enum {
  SFP_WRITE_ONE,
  SFP_WRITE_MULTIPLE
} SFPwritetype;

typedef struct SFPtransmitter {
  SFPseq seq;
  SFPcrc crc;

  RINGBUF(SFPpacket, SFP_CONFIG_HISTORY_CAPACITY) history;

  uint8_t writebuf[SFP_CONFIG_WRITEBUF_SIZE];
  size_t writebufn;

  SFPwrite1fun write1;
  void *write1Data;

  SFPwritenfun writen;
  void *writenData;

  SFPlockfun lock;
  void *lockData;

  SFPunlockfun unlock;
  void *unlockData;
} SFPtransmitter;

typedef struct SFPreceiver {
  SFPseq seq;
  SFPcrc crc;

  SFPescapestate escapeState;
  SFPframestate frameState;

  SFPheader header;
  SFPpacket packet;

  SFPdeliverfun deliver;
  void *deliverData;
} SFPreceiver;

typedef struct SFPcontext {
  SFPtransmitter tx;
  SFPreceiver rx;

#ifdef SFP_DEBUG
  char debugName[SFP_CONFIG_MAX_DEBUG_NAME_SIZE];
#endif
} SFPcontext;

void sfpDeliverOctet (SFPcontext *ctx, uint8_t octet);
void sfpWritePacket (SFPcontext *ctx, SFPpacket *packet);
void sfpInit (SFPcontext *ctx);
void sfpSetDeliverCallback (SFPcontext *ctx, SFPdeliverfun cbfun, void *userdata);
void sfpSetWriteCallback (SFPcontext *ctx, SFPwritetype type, void *cbfun, void *userdata);
void sfpSetLockCallback (SFPcontext *ctx, SFPlockfun cbfun, void *userdata);
void sfpSetUnlockCallback (SFPcontext *ctx, SFPunlockfun cbfun, void *userdata);

/* SFP reserved octets */
enum {
  SFP_ESC  = 0x7d,
  SFP_FLAG = 0x7e
};

#define SFP_ESC_FLIP_BIT 0x10 // the fifth bit, like in HDLC
#define SFP_NAK_BIT 0x80
#define SFP_SEQ_RANGE SFP_NAK_BIT

#define SFP_CRC_SIZE sizeof(SFPcrc)

#define SFP_CRC_PRESET 0xffff   /* The initial value for the CRC, recommended
                                 * by an article in Dr. Dobb's Journal */
#define SFP_INITIAL_SEQ 0


#endif
