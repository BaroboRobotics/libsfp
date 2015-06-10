#ifndef _LIBSFP_SERIAL_FRAMING_PROTOCOL_H_
#define _LIBSFP_SERIAL_FRAMING_PROTOCOL_H_

#include "sfp/config.h"
#include "util/potringbuffer.hpp"

#if defined(SFP_CONFIG_DEBUG) || defined(SFP_CONFIG_WARN) || defined(SFP_CONFIG_ERROR)
#include <boost/log/sources/logger.hpp>
#endif

#include <stdlib.h>
#include <stdint.h>

#ifdef SFP_CONFIG_DEBUG
#ifndef SFP_CONFIG_MAX_DEBUG_NAME_SIZE
#define SFP_CONFIG_MAX_DEBUG_NAME_SIZE 256
#endif
#endif

#ifndef SFP_CONFIG_HISTORY_CAPACITY
/* Must be a power of two for use in the ring buffer. */
#define SFP_CONFIG_HISTORY_CAPACITY 16
#endif

#ifndef SFP_CONFIG_MAX_PACKET_SIZE
#define SFP_CONFIG_MAX_PACKET_SIZE 256
#endif

#ifndef SFP_CONFIG_WRITEBUF_SIZE
#define SFP_CONFIG_WRITEBUF_SIZE 512
#endif

typedef uint8_t SFPseq;
typedef uint8_t SFPheader;
typedef uint16_t SFPcrc;

/* SFP reserved octets */
enum {
  SFP_ESC  = 0x7d,
  SFP_FLAG = 0x7e
};

#define SFP_ESC_FLIP_BIT (1<<5) // bit 5, like in HDLC

#define SFP_CRC_SIZE sizeof(SFPcrc)
#define SFP_CRC_PRESET 0xffff   /* The initial value for the CRC, recommended
                                 * by an article in Dr. Dobb's Journal */

#define SFP_CRC_GOOD 0xf0b8     /* A CRC updated over its bitwise complement,
                                 * least significant byte first, results in
                                 * this value. */

/* Header format:
 *
 * ccss ssss
 *
 * where cc are the control bits (the frame type), and ss ssss are the
 * sequence number bits. */

#define SFP_FIRST_SEQ_BIT 0
#define SFP_NUM_SEQ_BITS 6
#define SFP_FIRST_CONTROL_BIT SFP_NUM_SEQ_BITS
#define SFP_NUM_CONTROL_BITS 2

#define SFP_SEQ_RANGE (1 << SFP_NUM_SEQ_BITS)
#define SFP_INITIAL_SEQ 0

typedef enum {
  SFP_FRAME_USR = 0,
  SFP_FRAME_RTX,
  SFP_FRAME_NAK,
  SFP_FRAME_SYN
} SFPframetype;

enum {
  SFP_SEQ_SYN0 = 0,
  SFP_SEQ_SYN1,
  SFP_SEQ_SYN2,
  SFP_SEQ_SYN_DIS
};

typedef struct SFPpacket {
  uint8_t buf[SFP_CONFIG_MAX_PACKET_SIZE];
  size_t len;
} SFPpacket;

typedef void (*SFPdeliverfun) (uint8_t* buf, size_t len, void *userdata);
typedef int (*SFPwrite1fun) (uint8_t octet, size_t *outlen, void *userdata);
typedef int (*SFPwritenfun) (uint8_t *octets, size_t len, size_t *outlen, void *userdata);
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

typedef enum {
  SFP_CONNECT_STATE_DISCONNECTED,
  SFP_CONNECT_STATE_SENT_SYN0,
  SFP_CONNECT_STATE_SENT_SYN1,
  SFP_CONNECT_STATE_CONNECTED
} SFPconnectstate;

typedef struct SFPtransmitter {
  SFPseq seq;
  SFPcrc crc;

  util::PotRingbuffer<SFPpacket, SFP_CONFIG_HISTORY_CAPACITY> history;

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

  SFPconnectstate connectState;

#ifdef SFP_CONFIG_DEBUG
  char debugName[SFP_CONFIG_MAX_DEBUG_NAME_SIZE];
#endif
#if defined(SFP_CONFIG_DEBUG) || defined(SFP_CONFIG_ERROR) || defined(SFP_CONFIG_WARN)
  boost::log::sources::logger log;
#endif
} SFPcontext;

#ifdef __cplusplus
extern "C" {
#endif

/* Return 1 on packet available, 0 on unavailable, -1 on error. */
int sfpDeliverOctet (SFPcontext *ctx, uint8_t octet, uint8_t *buf, size_t len, size_t *outlen);
int sfpWritePacket (SFPcontext *ctx, const uint8_t *buf, size_t len, size_t *outlen);
void sfpConnect (SFPcontext *ctx);
int sfpIsConnected (SFPcontext *ctx);

size_t sfpGetSizeof (void);
void sfpInit (SFPcontext *ctx);

void sfpSetDeliverCallback (SFPcontext *ctx, SFPdeliverfun cbfun, void *userdata);
void sfpSetWriteCallback (SFPcontext *ctx, SFPwritetype type, void *cbfun, void *userdata);
void sfpSetLockCallback (SFPcontext *ctx, SFPlockfun cbfun, void *userdata);
void sfpSetUnlockCallback (SFPcontext *ctx, SFPunlockfun cbfun, void *userdata);

#ifdef SFP_CONFIG_DEBUG
void sfpSetDebugName (SFPcontext *ctx, const char *name);
#endif

#ifdef __cplusplus
}
#endif
#endif
