#include "serial_framing_protocol.h"

#include <assert.h>
#include <stdio.h>

//////////////////////////////////////////////////////////////////////////////

#ifdef AVR
#include <util/crc16.h>
#else
/* Stolen from avr-libc's docs */
static uint16_t _crc_ccitt_update (uint16_t crc, uint8_t octet) {
  octet ^= crc & 0xff;
  octet ^= octet << 4;
  return ((((uint16_t)octet << 8) | ((crc >> 8) & 0xff)) ^ (uint8_t)(octet >> 4) ^ ((uint16_t)octet << 3));
}
#endif

//////////////////////////////////////////////////////////////////////////////

static int sfpIsReservedOctet (uint8_t octet);
static sfp_seq_t sfpNextSeq (sfp_seq_t seq);

static void sfpWriteFrameWithSeq (sfp_t *context, sfp_seq_t seq, sfp_frame_t *frame);
static void sfpWriteControlFrame (sfp_t *context, sfp_header_t header);
static void sfpWriteUserFrame (sfp_t *context, sfp_frame_t *frame);
static void sfpWriteNoCRC (sfp_t *context, uint8_t octet);
static void sfpWrite (sfp_t *context, uint8_t octet);
static int sfpIsTransmitterLockable (sfp_t *context);

static void sfpBufferOctet (sfp_t *context, uint8_t octet);
static void sfpHandleNAK (sfp_t *context, sfp_seq_t seq);
static void sfpSendNAK (sfp_t *context);
static void sfpHandleControlFrame (sfp_t *context);
static void sfpTryDeliverUserFrame (sfp_t *context);
static void sfpResetReceiver (sfp_t *context);
static void sfpTryDeliverFrame (sfp_t *context);

//////////////////////////////////////////////////////////////////////////////

void sfpInit (sfp_t *context, sfp_deliver_func_t deliver, sfp_write_func_t write,
    sfp_lock_func_t lock, sfp_unlock_func_t unlock, void *mutex) {
  assert(write);
  assert((lock && unlock) || (!lock && !unlock));

#ifdef SFP_DEBUG
  context->debugName = NULL;
#endif

  ////////////////////////////////////////////////////////////////////////////

  context->rx.seq = SFP_INITIAL_SEQ;

  sfpResetReceiver(context);

  context->rx.deliver = deliver;

  ////////////////////////////////////////////////////////////////////////////
  
  context->tx.crc = SFP_INITIAL_SEQ;
  context->tx.crc = SFP_CRC_PRESET;

  context->tx.write = write;
  context->tx.lock = lock;
  context->tx.unlock = unlock;
  context->tx.mutex = mutex;

  RINGBUF_INIT(context->tx.history);
}

/* Entry point for receiver. */
void sfpDeliverOctet (sfp_t *context, uint8_t octet) {
  if (SFP_FLAG == octet) {
    if (SFP_FRAME_STATE_RECEIVING == context->rx.frameState) {
      sfpTryDeliverFrame(context);
    }
    /* If we receive a FLAG while in FRAME_STATE_NEW, this means we have
     * received back-to-back FLAG octets. This is a heartbeat/keepalive, and we
     * simply ignore them. */
    sfpResetReceiver(context);
  }
  else if (SFP_ESC == octet) {
    context->rx.escapeState = SFP_ESCAPE_STATE_ESCAPING;
  }
  else {
    /* All other, non-control octets. */

    if (SFP_ESCAPE_STATE_ESCAPING == context->rx.escapeState) {
      octet ^= SFP_ESC_FLIP_BIT;
      context->rx.escapeState = SFP_ESCAPE_STATE_NORMAL;
    }

#if 0
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received data octet<0x%02x> CRC<0x%04x>\n", context->debugName,
        octet, context->rx.crc);
#endif
#endif


    if (SFP_FRAME_STATE_NEW == context->rx.frameState) {
      /* We are receiving the header. */

      context->rx.crc = _crc_ccitt_update(context->rx.crc, octet);
      context->rx.header = octet;
      context->rx.frameState = SFP_FRAME_STATE_RECEIVING;
    }
    else {
      /* We are receiving the payload. Since the CRC will be indistinguishable
       * from the rest of the payload until we receive the terminating FLAG
       * octet, we put the CRC calculation on a delay of SFP_CRC_SIZE octets. */

      if (SFP_CRC_SIZE <= context->rx.frame.len) {
        context->rx.crc = _crc_ccitt_update(context->rx.crc,
            context->rx.frame.buf[context->rx.frame.len - SFP_CRC_SIZE]);
      }

      sfpBufferOctet(context, octet);
    }
  }
}

/* Entry point for transmitter. */
void sfpWriteFrame (sfp_t *context, sfp_frame_t *frame) {
  if (sfpIsTransmitterLockable(context)) {
    context->tx.lock(context->tx.mutex);
  }

  RINGBUF_PUSH_BACK(context->tx.history, *frame);
  sfpWriteUserFrame(context, frame);

  if (sfpIsTransmitterLockable(context)) {
    context->tx.unlock(context->tx.mutex);
  }
}

//////////////////////////////////////////////////////////////////////////////

static sfp_seq_t sfpNextSeq (sfp_seq_t seq) {
  return (seq + 1) & (SFP_SEQ_RANGE - 1);
}

static void sfpResetReceiver (sfp_t *context) {
  context->rx.crc = SFP_CRC_PRESET;
  context->rx.escapeState = SFP_ESCAPE_STATE_NORMAL;
  context->rx.frameState = SFP_FRAME_STATE_NEW;
  context->rx.frame.len = 0;
}

static void sfpTryDeliverFrame (sfp_t *context) {
  if (SFP_CRC_SIZE > context->rx.frame.len) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): RX<0x%02x \"", context->debugName, context->rx.header);
    fwrite(context->rx.frame.buf, 1, context->rx.frame.len, stderr);
    fprintf(stderr, "\">\n\tEXPECTED<0x%02x (payload) 0x%04x>\n",
        context->rx.seq, context->rx.crc);
    fflush(stderr);
#endif

#ifdef SFP_SHOW_WARNINGS
    fprintf(stderr, "(sfp) WARNING: short frame received, sending NAK.\n");
#endif
    sfpSendNAK(context);
    return;
  }

  uint8_t *pcrc = &context->rx.frame.buf[context->rx.frame.len - SFP_CRC_SIZE];
  sfp_crc_t crc = sfpByteSwapCRC(*(sfp_crc_t *)pcrc);
  context->rx.frame.len -= SFP_CRC_SIZE;

#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): RX<0x%02x \"", context->debugName, context->rx.header);
  fwrite(context->rx.frame.buf, 1, context->rx.frame.len, stderr);
  fprintf(stderr, "\" 0x%04x>\n\tEXPECTED<0x%02x (payload) 0x%04x>\n",
      crc, context->rx.seq, context->rx.crc);
  fflush(stderr);
#endif

  if (0 == context->rx.frame.len) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received control frame\n", context->debugName);
#endif

    if (crc != context->rx.crc) {
#ifdef SFP_SHOW_WARNINGS
      fprintf(stderr, "(sfp) WARNING: CRC mismatch, ignoring.\n");
#endif
      return;
    }

    sfpHandleControlFrame(context);
  }
  else {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received user frame\n", context->debugName);
#endif

    if (crc != context->rx.crc) {
#ifdef SFP_SHOW_WARNINGS
      fprintf(stderr, "(sfp) WARNING: CRC mismatch, sending NAK.\n");
#endif
      sfpSendNAK(context);
      return;
    }

    sfpTryDeliverUserFrame(context);
  }
}

static void sfpTryDeliverUserFrame (sfp_t *context) {
  if ((context->rx.header & (SFP_SEQ_RANGE - 1)) == context->rx.seq) {
    /* Good frame received and accepted--deliver it.p */
    context->rx.deliver(&context->rx.frame);
    context->rx.seq = sfpNextSeq(context->rx.seq);
  }
  else {
#ifdef SFP_SHOW_WARNINGS
    fprintf(stderr, "(sfp) WARNING: out-of-order frame received, sending NAK.\n");
#endif
    sfpSendNAK(context);
  }
}

static void sfpHandleControlFrame (sfp_t *context) {
  if (SFP_NAK_BIT & context->rx.header) {
    sfp_seq_t seq = context->rx.header & (SFP_SEQ_RANGE - 1);

    if (seq == context->tx.seq) {
      /* The remote is telling us it expects the current sequence number, but
       * received something different. This is fine, and probably just means
       * that it received a frame that had to be retransmitted multiple
       * times. This is unlikely to even happen on a USB line, since the
       * bandwidth-delay product is so low. */
#ifdef SFP_DEBUG
      fprintf(stderr, "(sfp) DEBUG(%s): received NAK<%d> for current SEQ. Ignoring.\n",
          context->debugName, seq);
#endif
    }
    else {
      sfpHandleNAK(context, seq);
    }
  }
  else {
#ifdef SFP_SHOW_WARNINGS
    fprintf(stderr, "(sfp) WARNING: unknown or corrupt control frame received, ignoring: 0x%x.\n",
        context->rx.header);
#endif
  }
}

static void sfpSendNAK (sfp_t *context) {
  /* XXX The receiver must lock the transmitter before sending anything! */

  if (sfpIsTransmitterLockable(context)) {
    context->tx.lock(context->tx.mutex);
  }

  sfpWriteControlFrame(context, context->rx.seq | SFP_NAK_BIT);

  if (sfpIsTransmitterLockable(context)) {
    context->tx.unlock(context->tx.mutex);
  }
}

static void sfpHandleNAK (sfp_t *context, sfp_seq_t seq) {

  /* XXX The receiver must lock the transmitter before sending anything! */

  if (sfpIsTransmitterLockable(context)) {
    context->tx.lock(context->tx.mutex);
  }

  /* The number of frames we'll have to drop from our history ring buffer in
   * order to fast-forward to the remote's current sequence number. */
  unsigned fastforward = seq
    - (context->tx.seq - RINGBUF_SIZE(context->tx.history));

  fastforward &= (SFP_SEQ_RANGE - 1);

#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): received NAK<%d> (current SEQ<%d>). History size<%d>, fastforward<%d>.\n",
      context->debugName, seq, context->tx.seq, RINGBUF_SIZE(context->tx.history), fastforward);
  fprintf(stderr, "(sfp) DEBUG(%s): r' - (r - s) == %d - (%d - %d) == %d\n",
      context->debugName, seq, context->tx.seq, RINGBUF_SIZE(context->tx.history),
      fastforward);
#endif


  if (RINGBUF_SIZE(context->tx.history) > fastforward) {
    for (unsigned i = 0; i < fastforward; ++i) {
      RINGBUF_POP_FRONT(context->tx.history);
    }
  }
  else {
    fprintf(stderr, "(sfp) ERROR: %d outgoing frame(s) lost by history buffer underrun.\n"
        "\tTry adjusting SFP_CONFIG_HISTORY_CAPACITY.\n", SFP_SEQ_RANGE - fastforward);

    /* Even if we lost frames, the show still has to go on. Resynchronize, and
     * send what frames we have available in our history. */
  }

  /* Synchronize our remote sequence number with the NAK. */
  context->tx.seq = seq;

  size_t reTxCount = RINGBUF_SIZE(context->tx.history);

  for (size_t i = 0; i < reTxCount; ++i) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): retransmitting frame with SEQ<%d>\n",
        context->debugName, context->tx.seq);
#endif
    sfpWriteUserFrame(context, &RINGBUF_AT(context->tx.history, i));
  }

  if (sfpIsTransmitterLockable(context)) {
    context->tx.unlock(context->tx.mutex);
  }
}

static void sfpBufferOctet (sfp_t *context, uint8_t octet) {
  if (SFP_CONFIG_MAX_FRAME_SIZE <= context->rx.frame.len) {
    fprintf(stderr, "(sfp) ERROR: incoming frame(s) lost by frame buffer overrun.\n"
        "\tTry increasing SFP_CONFIG_MAX_FRAME_SIZE.\n"
        "\tThis could also be caused by a corrupt FLAG octet.\n");

    /* Until I have a better idea, just going to pretend we didn't receive
     * anything at all, and just go on with life. If this was caused by a
     * corrupt FLAG octet, then our forthcoming NAK should resynchronize
     * everything. */
    sfpResetReceiver(context);
  }
  else {
    /* Finally, the magic happens. */
    context->rx.frame.buf[context->rx.frame.len++] = octet;
  }
}

static int sfpIsTransmitterLockable (sfp_t *context) {
  return context->tx.lock && context->tx.unlock;
}

static int sfpIsReservedOctet (uint8_t octet) {
  switch (octet) {
    case SFP_ESC:
      /* fall-through */
    case SFP_FLAG:
      return 1;
    default:
      return 0;
  }
}

/* Wrapper around context->write, updating the rolling CRC and escaping
 * reserved octets as necessary. */
static void sfpWrite (sfp_t *context, uint8_t octet) {
  context->tx.crc = _crc_ccitt_update(context->tx.crc, octet);

#if 0
#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): writing data octet<0x%02x> CRC<0x%04x>\n", context->debugName,
      octet, context->tx.crc);
#endif
#endif

  sfpWriteNoCRC(context, octet);
}

static void sfpWriteNoCRC (sfp_t *context, uint8_t octet) {
  if (sfpIsReservedOctet(octet)) {
    octet ^= SFP_ESC_FLIP_BIT;
    context->tx.write(SFP_ESC);
  }
  context->tx.write(octet);
}

static void sfpWriteUserFrame (sfp_t *context, sfp_frame_t *frame) {
  sfpWriteFrameWithSeq(context, context->tx.seq, frame);
  context->tx.seq = sfpNextSeq(context->tx.seq);
}

static void sfpWriteControlFrame (sfp_t *context, sfp_header_t header) {
  sfpWriteFrameWithSeq(context, header, NULL);
}

/* Provided separately from sfpWriteframeUnrecorded so that the receiver can
 * use it to send NAKs. */
static void sfpWriteFrameWithSeq (sfp_t *context, sfp_seq_t seq, sfp_frame_t *frame) {
  context->tx.crc = SFP_CRC_PRESET;

  /* Begin frame. */
  context->tx.write(SFP_FLAG);

  sfpWrite(context, seq);

  if (frame) {
    for (size_t i = 0; i < frame->len; ++i) {
      sfpWrite(context, frame->buf[i]);
    }
  }

  sfp_crc_t crc = sfpByteSwapCRC(context->tx.crc);
  uint8_t *pcrc = (uint8_t *)&crc;

  for (size_t i = 0; i < sizeof(crc); ++i) {
    /* At first glance, this might seem bizarre. The "NoCRC" bit simply means
     * that the transmitter's rolling CRC will not be updated by the octet we
     * pass. We don't need to CRC the CRC itself. */
    sfpWriteNoCRC(context, pcrc[i]);
  }

  /* End frame. */
  context->tx.write(SFP_FLAG);
}
