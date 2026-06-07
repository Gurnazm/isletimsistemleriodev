#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "sys/node-id.h"
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "firmware_data.h"
#include "common.h"

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define TIMEOUT_INTERVAL   (2 * CLOCK_SECOND)
#define SEND_INTERVAL      (10 * CLOCK_SECOND)

static struct simple_udp_connection udp_conn;

static uint16_t current_block_id = 0;
static bool waiting_for_ack = false;
static uint16_t total_blocks = 0;
static bool ota_started = false;
static uip_ipaddr_t dest_ipaddr;

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
/*---------------------------------------------------------------------------*/
static void
send_block(uint16_t block_id)
{
  struct ota_packet pkt;
  uint32_t offset = (uint32_t)block_id * BLOCK_SIZE;
  uint32_t len = firmware_data_len - offset;
  if(len > BLOCK_SIZE) {
    len = BLOCK_SIZE;
  }

  pkt.block_id = block_id;
  pkt.total_blocks = total_blocks;
  pkt.data_length = (uint8_t)len;

  memset(pkt.payload, 0, BLOCK_SIZE);
  memcpy(pkt.payload, &firmware_data[offset], len);

  pkt.checksum = calculate_checksum(&pkt);

  LOG_INFO("Sending block %u/%u (len: %u, checksum: 0x%02x) to ",
           block_id, total_blocks, (unsigned int)len, pkt.checksum);
  LOG_INFO_6ADDR(&dest_ipaddr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dest_ipaddr);
}
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

  if(node_id == 2) {
    if(datalen == sizeof(uint16_t)) {
      uint16_t ack_block_id;
      memcpy(&ack_block_id, data, sizeof(ack_block_id));
      if(waiting_for_ack && ack_block_id == current_block_id) {
        LOG_INFO("ACK received for block %u\n", ack_block_id);
        current_block_id++;
        waiting_for_ack = false;
        process_poll(&udp_client_process);
      } else {
        LOG_INFO("Received ACK %u, but expected %u or not waiting for ACK\n", ack_block_id, current_block_id);
      }
    } else {
      LOG_INFO("Client received unexpected response of length %u\n", datalen);
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;

  PROCESS_BEGIN();

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  total_blocks = (firmware_data_len + BLOCK_SIZE - 1) / BLOCK_SIZE;
  current_block_id = 0;
  waiting_for_ack = false;
  ota_started = false;

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer) || ev == PROCESS_EVENT_POLL);

    if(node_id == 2) {
      if(!ota_started) {
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
          LOG_INFO("Root is reachable. Starting OTA transmission of %u blocks.\n", total_blocks);
          ota_started = true;
          send_block(current_block_id);
          waiting_for_ack = true;
          etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
        } else {
          LOG_INFO("Not reachable yet\n");
          etimer_set(&periodic_timer, 5 * CLOCK_SECOND);
        }
      } else {
        if(current_block_id < total_blocks) {
          if(!waiting_for_ack) {
            send_block(current_block_id);
            waiting_for_ack = true;
            etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
          } else {
            if(etimer_expired(&periodic_timer)) {
              LOG_INFO("Timeout: Retransmitting block %u\n", current_block_id);
              send_block(current_block_id);
              etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
            }
          }
        } else {
          LOG_INFO("OTA transmission completed. All %u blocks sent.\n", total_blocks);
          etimer_stop(&periodic_timer);
        }
      }
    } else {
      // For Node 3 or routing nodes, just sleep to save cycles
      etimer_set(&periodic_timer, 60 * CLOCK_SECOND);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
