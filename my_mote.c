#include "contiki.h"
#include "net/rime.h"
#include "random.h"

#include "dev/button-sensor.h"

#include "dev/leds.h"

#include <stdio.h>

#define MAX_RETRANSMISSIONS 4
#define NUM_HISTORY_ENTRIES 4

/*---------------------------------------------------------------------------*/
#include <string.h> // for strncpy
#include <stdbool.h>
#include <stdint.h>

/* MT == Message Type */
#define MT_DISCOVERY    0
#define MT_STATUS       1

typedef struct {
  rimeaddr_t    parent_addr;
  uint16_t      parent_rssi;
  int32_t       hops_to_root;
} Status;

/* Message formats */
typedef struct {
  uint8_t       type;
} Discovery_Msg;

typedef struct {
  uint8_t       type; 
  uint32_t      hops_to_root;
} Status_Msg;

/* Global variables required */
static bool connected_to_tree = false;
static Status my_status;


static void send_broadcast(const void* msg, int size);
static void send_unicast(const void* msg, int size, const rimeaddr_t* to);
static void reset_status();

static void process_status_msg(const rimeaddr_t *from, uint32_t hops, uint16_t rssi) {
  if (!connected_to_tree) {
    my_status.parent_addr = *from;
    my_status.parent_rssi = rssi;
    my_status.hops_to_root = hops;
    connected_to_tree = true;
    return;
  }
  // TODO : compare if better parent
}

static void process_message(const rimeaddr_t *from) {
  uint8_t* message_type = (uint8_t *) packetbuf_dataptr();
  switch(*message_type) {
    case MT_DISCOVERY:
      printf("Message received from %u.%u : ask for discovery !\n",
              from->u8[0], from->u8[1]);
      if (connected_to_tree) {
        Status_Msg msg;
        msg.type = MT_STATUS;
        msg.hops_to_root = my_status.hops_to_root;
        send_unicast((const void*) &msg, sizeof(msg), from);
      }
      break;
    case MT_STATUS:
      ; 
      Status_Msg* msg = (Status_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : status ! hops : %d\n",
              from->u8[0], from->u8[1], (int) msg->hops_to_root);
      process_status_msg(from, msg->hops_to_root, packetbuf_attr(PACKETBUF_ATTR_RSSI));
      break;
    default:
      printf("Message received from %u.%u : UNKOWN TYPE\n",
              from->u8[0], from->u8[1]);
      break;
  }
}

/*---------------------------------------------------------------------------*/
static void recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
  process_message(from);
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};
static struct unicast_conn uc;
/*---------------------------------------------------------------------------*/

PROCESS(example_broadcast_process, "Broadcast example"); // TODO
AUTOSTART_PROCESSES(&example_broadcast_process);

/*---------------------------------------------------------------------------*/
static void
broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
  process_message(from);
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
static void send_broadcast(const void* msg, int size){
  packetbuf_copyfrom(msg, size);
  broadcast_send(&broadcast);
}
static void send_unicast(const void* msg, int size, const rimeaddr_t* to){
  packetbuf_copyfrom(msg, size);
  unicast_send(&uc, to);
}
static void reset_status() {
  connected_to_tree = false;
  my_status.parent_addr = rimeaddr_null;
  my_status.hops_to_root = 100; // TODO fix ?
  my_status.parent_rssi = 0;
}

PROCESS_THREAD(example_broadcast_process, ev, data)
{
  static struct etimer et;
  static int    timer = 4;
  static bool   is_root = false;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
  PROCESS_EXITHANDLER(unicast_close(&uc);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);
  unicast_open(&uc, 146, &unicast_callbacks);

  reset_status();

  /* By default, node 1.0 is the root */
  is_root = rimeaddr_node_addr.u8[0] == 1 && rimeaddr_node_addr.u8[1] == 0;

  if (is_root) {
    my_status.hops_to_root = 0;
    connected_to_tree = true;
  } 

  while(1) {
    if (!connected_to_tree) timer = 1;
    else                    timer = 4;
    
    etimer_set(&et, CLOCK_SECOND * timer + random_rand() % (CLOCK_SECOND * timer));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    if (!connected_to_tree) {
      Discovery_Msg msg;
      msg.type = MT_DISCOVERY;
      send_broadcast((const void*) &msg, sizeof(msg));
      continue;
    } else {
      printf("Connected to tree, parent : %u.%u\n",
            my_status.parent_addr.u8[0], my_status.parent_addr.u8[1]);
    }

  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
