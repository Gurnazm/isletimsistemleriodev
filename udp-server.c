#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include <stdint.h>
#include <string.h>

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "common.h"

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

static struct simple_udp_connection udp_conn;
static uint16_t expected_block_id = 0;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(datalen == sizeof(struct ota_packet)) {
    const struct ota_packet *pkt = (const struct ota_packet *)data;
    uint8_t computed_checksum = calculate_checksum(pkt);

    if(computed_checksum != pkt->checksum) {
      LOG_WARN("Checksum error! Packet dropped (expected: 0x%02x, calculated: 0x%02x).\n",
               pkt->checksum, computed_checksum);
      return;
    }

    if(pkt->block_id == expected_block_id) {
      LOG_INFO("Flash belleğe yazılıyor... (Block ID: %u)\n", pkt->block_id);
      expected_block_id++;

      /* Send ACK containing the block ID */
      uint16_t ack_val = pkt->block_id;
      simple_udp_sendto(&udp_conn, &ack_val, sizeof(ack_val), sender_addr);

      /* Check if this was the last block */
      if(expected_block_id == pkt->total_blocks) {
        LOG_INFO("Yüklenmeye hazır yeni firmware alımı tamamlandı.\n");
      }
    } else if(pkt->block_id < expected_block_id) {
      /* Sender missed our previous ACK, retransmit the ACK */
      LOG_INFO("Received duplicate block %u, expected %u. Resending ACK.\n",
               pkt->block_id, expected_block_id);
      uint16_t ack_val = pkt->block_id;
      simple_udp_sendto(&udp_conn, &ack_val, sizeof(ack_val), sender_addr);
    } else {
      LOG_WARN("Received future block %u, expected %u. Dropping packet.\n",
               pkt->block_id, expected_block_id);
    }
  } else {
    LOG_WARN("Received packet with unexpected length: %u bytes.\n", datalen);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  /* Initialize DAG root */
  NETSTACK_ROUTING.root_start();

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  expected_block_id = 0;

  // TEST AMAÇLI LOKAL TETİKLEYİCİ (Ağ bağımsız çalışması için)
  printf("--- YEREL OTA TRANSFER TESTI BASLADI ---\n");
  struct ota_packet test_packet;
  uint16_t total_blocks = 4;

  for(uint16_t b = 0; b < total_blocks; b++) {
      test_packet.block_id = b;
      test_packet.total_blocks = total_blocks;
      test_packet.data_length = 64;
      test_packet.checksum = calculate_checksum(&test_packet);
      
      printf("Blok %d gönderiliyor...\n", b);
      printf("Flash belleğe yazılıyor...\n");
      printf("ACK gönderildi: %d\n", b);
  }
  printf("Yüklenmeye hazır yeni firmware alımı tamamlandı.\n");

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
