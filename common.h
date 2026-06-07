#ifndef COMMON_H_
#define COMMON_H_

#include <stdint.h>
#include <stddef.h>

#define BLOCK_SIZE 64

struct ota_packet {
  uint16_t block_id;
  uint16_t total_blocks;
  uint8_t data_length;
  uint8_t payload[BLOCK_SIZE];
  uint8_t checksum;
} __attribute__((packed));

static inline uint8_t
calculate_checksum(const struct ota_packet *pkt)
{
  const uint8_t *ptr = (const uint8_t *)pkt;
  uint8_t xor_val = 0;
  size_t i;
  for(i = 0; i < offsetof(struct ota_packet, checksum); i++) {
    xor_val ^= ptr[i];
  }
  return xor_val;
}

#endif /* COMMON_H_ */
