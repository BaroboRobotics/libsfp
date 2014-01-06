#ifndef _NET_BYTE_ORDER_H_
#define _NET_BYTE_ORDER_H_

#ifdef _MSC_VER
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

uint16_t netByteOrder16 (uint16_t);
uint32_t netByteOrder32 (uint32_t);
uint64_t netByteOrder64 (uint64_t);

#endif
