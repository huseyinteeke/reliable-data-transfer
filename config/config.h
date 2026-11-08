
#pragma once
#include <stdint.h>

#define MAX_PAYLOAD_SIZE 1024


typedef struct {
    uint32_t seq_num;                    
    uint32_t ack_num;      
    uint16_t flags;       
    uint16_t payload_len;
    uint16_t checksum;    
} packet_header_t;

typedef struct {
    packet_header_t header;
    char payload[MAX_PAYLOAD_SIZE];
} rdt_packet_t;


