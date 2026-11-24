#pragma once
/* packet.h - CRC-32 and wire (de)serialization shared by sender/receiver/emulator. */
#include <stddef.h>
#include <stdint.h>
#include "config.h"

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), implemented by hand. */
uint32_t crc32_compute(const uint8_t *data, size_t len);

/*
 * Serialize a packet into buf (needs HEADER_SIZE + payload_len bytes).
 * The CRC is computed here over header (crc field = 0) + payload.
 * Returns the number of bytes written.
 */
size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t number,
                  const uint8_t *payload, uint16_t payload_len);

typedef enum {
    PKT_OK         = 0,
    PKT_ERR_SHORT  = -1,  /* fewer bytes than a header                   */
    PKT_ERR_LENGTH = -2,  /* payload_len disagrees with datagram size    */
    PKT_ERR_CRC    = -3   /* CRC mismatch (corrupted)                    */
} pkt_status_t;

/*
 * Parse and validate a received datagram. On PKT_OK, *hdr is in host byte
 * order and *payload points into buf (valid for hdr->payload_len bytes).
 */
pkt_status_t pkt_decode(const uint8_t *buf, size_t len,
                        packet_header_t *hdr, const uint8_t **payload);
