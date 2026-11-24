#pragma once
/*
 * config.h - Packet format shared by sender, receiver and emulator.
 *
 * Wire layout of the fixed 12-byte header (all multi-byte fields in
 * network byte order / big-endian):
 *
 *   byte:  0        1        2        3        4 .. 7        8 .. 11
 *        +--------+--------+-----------------+-------------+-------------+
 *        |  type  | flags  |   payload_len   |   number    |    crc32    |
 *        |  u8    |  u8    |      u16        |    u32      |    u32      |
 *        +--------+--------+-----------------+-------------+-------------+
 *        followed by payload_len bytes of payload (0 for ACK/FIN/FIN_ACK).
 *
 *   type        : what kind of packet this is (see pkt_type_t).
 *   flags       : reserved for future use, always 0 for now.
 *   payload_len : number of payload bytes after the header.
 *   number      : DATA    -> sequence number of this packet
 *                 ACK     -> acknowledgment number
 *                 FIN     -> sequence number after the last DATA packet
 *                 FIN_ACK -> same number as the FIN it answers
 *                 Only the lower k bits are used (counted modulo 2^k),
 *                 but the field is always 32 bits wide (fixed size).
 *   crc32       : CRC-32 (IEEE 802.3) over header + payload, computed
 *                 with this field set to 0.
 *
 * The structs below describe the layout for readability. They are NOT
 * sent with memcpy/sendto directly: packet.c writes each field into a
 * byte buffer with htons/htonl so the format does not depend on the
 * compiler's padding or the machine's byte order.
 */
#include <stdint.h>

/* ---- Sizes ------------------------------------------------------------ */

#define HEADER_SIZE      12u    /* bytes on the wire, see layout above     */
#define MAX_PAYLOAD_SIZE 4096u  /* upper bound only; actual size: --payload */
#define MAX_PACKET_SIZE  (HEADER_SIZE + MAX_PAYLOAD_SIZE)

/* ---- Sequence-number width k (PDF: 3 <= k <= 8) ----------------------- */

#define SEQ_BITS_MIN 3u
#define SEQ_BITS_MAX 8u

/* Keep only the lower k bits of a number: value mod 2^k. */
static inline uint32_t seq_mod(uint32_t value, unsigned k)
{
    return value & ((1u << k) - 1u);
}

/* ---- Packet types ----------------------------------------------------- */

typedef enum {
    PKT_DATA    = 1,  /* sender -> receiver: file data                   */
    PKT_ACK     = 2,  /* receiver -> sender: acknowledgment              */
    PKT_FIN     = 3,  /* sender -> receiver: all data sent, end transfer */
    PKT_FIN_ACK = 4   /* receiver -> sender: FIN received, file written  */
} pkt_type_t;

/* ---- Header / packet (in-memory, host byte order) --------------------- */

typedef struct {
    uint8_t  type;         /* one of pkt_type_t                          */
    uint8_t  flags;        /* reserved, 0                                */
    uint16_t payload_len;  /* bytes of payload                           */
    uint32_t number;       /* seq number (DATA) or ack number (ACK)      */
    uint32_t crc32;        /* CRC-32 over header (crc32=0) + payload     */
} packet_header_t;

_Static_assert(sizeof(packet_header_t) == HEADER_SIZE,
               "packet_header_t must match the 12-byte wire header");

typedef struct {
    packet_header_t header;
    uint8_t payload[MAX_PAYLOAD_SIZE];
} rdt_packet_t;
