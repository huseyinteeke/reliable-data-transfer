/* packet.c - CRC-32 and wire (de)serialization. See config.h for the layout. */
#include <arpa/inet.h>
#include <string.h>
#include "packet.h"

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t crc32_compute(const uint8_t *data, size_t len)
{
    if (!crc_ready)
        crc_init();
    return crc_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t number,
                  const uint8_t *payload, uint16_t payload_len)
{
    uint16_t len_n = htons(payload_len);
    uint32_t num_n = htonl(number);
    uint32_t zero  = 0;

    buf[0] = type;
    buf[1] = 0; /* flags */
    memcpy(buf + 2, &len_n, 2);
    memcpy(buf + 4, &num_n, 4);
    memcpy(buf + 8, &zero, 4);
    if (payload_len > 0)
        memcpy(buf + HEADER_SIZE, payload, payload_len);

    size_t total = HEADER_SIZE + (size_t)payload_len;
    uint32_t crc_n = htonl(crc32_compute(buf, total));
    memcpy(buf + 8, &crc_n, 4);
    return total;
}

pkt_status_t pkt_decode(const uint8_t *buf, size_t len,
                        packet_header_t *hdr, const uint8_t **payload)
{
    if (len < HEADER_SIZE)
        return PKT_ERR_SHORT;

    uint16_t len_n;
    uint32_t num_n, crc_n;
    memcpy(&len_n, buf + 2, 2);
    memcpy(&num_n, buf + 4, 4);
    memcpy(&crc_n, buf + 8, 4);

    hdr->type        = buf[0];
    hdr->flags       = buf[1];
    hdr->payload_len = ntohs(len_n);
    hdr->number      = ntohl(num_n);
    hdr->crc32       = ntohl(crc_n);

    if (len != HEADER_SIZE + (size_t)hdr->payload_len ||
        hdr->payload_len > MAX_PAYLOAD_SIZE)
        return PKT_ERR_LENGTH;

    /* Recompute the CRC with the crc field zeroed. */
    uint8_t tmp[MAX_PACKET_SIZE];
    memcpy(tmp, buf, len);
    memset(tmp + 8, 0, 4);
    if (crc32_compute(tmp, len) != hdr->crc32)
        return PKT_ERR_CRC;

    *payload = buf + HEADER_SIZE;
    return PKT_OK;
}
