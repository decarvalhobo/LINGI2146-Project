#include "contiki.h"
#include "net/rime.h"
#include "random.h"

#include <stdlib.h>
#include <stdio.h>

#include "dev/button-sensor.h"

#include "dev/leds.h"

#include <stdio.h>

#define MAX_RETRANSMISSIONS 4
#define NUM_HISTORY_ENTRIES 4

/*---------------------------------------------------------------------------*/
#include <string.h> // for strncpy
#include <stdbool.h>
#include <stdint.h>

#define TEMP_CHANNEL_NAME       "temp"
#define MIN_RAND_TEMP           -20
#define MAX_RAND_TEMP           80


/* MT == Message Type */
#define MT_DISCOVERY            0
#define MT_STATUS               1
#define MT_DISCONNECTION        2
#define MT_DATA                 3

typedef struct {
  rimeaddr_t    parent_addr;
  uint16_t      parent_rssi;
  int32_t       hops_to_root;
} Status;

/* Message formats */
typedef struct {
  uint8_t       type;
} Basic_Msg;

typedef struct {
  uint8_t       type; 
  Status        status;
} Status_Msg;

typedef struct {
    uint8_t     type;
    char*       channel_name;
    rimeaddr_t  mote_addr_from;
    int         data_value;
} Data_Msg;

/* Global variables required */
static bool     connected_to_tree = false;
static bool     is_root = false;
static Status   my_status;
static uint8_t  no_news_from_parent = 0;

static void process_status_msg(const rimeaddr_t *from, Status status, uint16_t rssi);
static void send_broadcast(const void* msg, int size);
static void send_unicast(const void* msg, int size, const rimeaddr_t* to);
static void reset_status();
static void broadcast_status();
static void store_status(const rimeaddr_t *from, uint32_t hops, uint16_t rssi, bool broadcast);
static void parent_disconnection();
static void print_data_msg(Data_Msg* msg);

static void print_data_msg(Data_Msg* msg)
{
  /* FORMAT : ;<mote_addr>;<channel name>;<data value> */
  printf(";%u:%u;%s;%d\n",
          msg->mote_addr_from.u8[0], 
          msg->mote_addr_from.u8[1], 
          msg->channel_name, 
          msg->data_value);
}
static void process_status_msg(const rimeaddr_t *from, Status sender_status, uint16_t rssi) {
  bool is_new_parent = true;

  if (rimeaddr_cmp((const rimeaddr_t*) &rimeaddr_node_addr, 
        (const rimeaddr_t*) &sender_status.parent_addr)) {
    /* If the current mote is the parent of the status message sender,
       ignore it (so they can't be each other's parent) */
        return;
  }

  if (rimeaddr_cmp(from, (const rimeaddr_t *) &my_status.parent_addr)) {
    /* If the message comes from the current parent, we store its new status */
    store_status(from, sender_status.hops_to_root, rssi, false);
    return;
  }

  if (connected_to_tree) {
    /* We store the new parent if it is closer (hops), or if the new possible parent 
       and the current parent have the same hops number but the new one has a better 
       signal strenght */
    is_new_parent = ((sender_status.hops_to_root + 1) < my_status.hops_to_root)
                    || ((sender_status.hops_to_root + 1) == my_status.hops_to_root 
                         && rssi > my_status.parent_rssi);
  }

  if (is_new_parent) {
    /* We store the new parent */
    store_status(from, sender_status.hops_to_root, rssi, true);
  }
}

static void process_message(const rimeaddr_t *from, bool is_unicast) {
  uint8_t* message_type = (uint8_t *) packetbuf_dataptr();
  switch(*message_type) {
    case MT_DISCOVERY:
      printf("Message received from %u.%u : ask for discovery !\n",
              from->u8[0], from->u8[1]);
      if (connected_to_tree) {
        Status_Msg msg = {MT_STATUS, my_status};
        send_unicast((const void*) &msg, sizeof(msg), from);
      } else if (!connected_to_tree && is_unicast) {
        Basic_Msg msg = {MT_DISCONNECTION};
        send_unicast((const void*) &msg, sizeof(msg), from);
      }
      break;
    case MT_STATUS: ;
      Status_Msg* status_msg = (Status_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : status !\n", from->u8[0], from->u8[1]);
      process_status_msg(from, status_msg->status, packetbuf_attr(PACKETBUF_ATTR_RSSI));
      break;
    case MT_DISCONNECTION: 
      printf("Message received from %u.%u : Disconnection !\n", from->u8[0], from->u8[1]);
      if (connected_to_tree && rimeaddr_cmp((const rimeaddr_t *) &my_status.parent_addr, from)) {
        parent_disconnection();
      }
      break;
    case MT_DATA: ;
      Data_Msg* data_msg = (Data_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : Data !\n", from->u8[0], from->u8[1]);
      if (is_root) {
	    print_data_msg((Data_Msg*) data_msg);
      } else if (connected_to_tree) {
        send_unicast((const void*) data_msg, sizeof(*data_msg), 
                     (const rimeaddr_t*) &my_status.parent_addr);
      }
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
  process_message(from, true);
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};
static struct unicast_conn uc;
/*---------------------------------------------------------------------------*/

PROCESS(manage_motes_network, "Manage the motes network");
PROCESS(data_sender, "Send data");
AUTOSTART_PROCESSES(&manage_motes_network, &data_sender);

/*---------------------------------------------------------------------------*/
static void
broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
  process_message(from, false);
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
static void send_broadcast(const void* msg, int size){
  printf("Send broadcast message\n");
  packetbuf_copyfrom(msg, size);
  broadcast_send(&broadcast);
}
static void send_unicast(const void* msg, int size, const rimeaddr_t* to){
  printf("Send unicast message to %u.%u\n", to->u8[0], to->u8[1]);//
  packetbuf_copyfrom(msg, size);
  unicast_send(&uc, to);
}
static void reset_status() {
  connected_to_tree = false;
  my_status.parent_addr = rimeaddr_null;
  my_status.hops_to_root = INT32_MAX;
  my_status.parent_rssi = 0;
  no_news_from_parent = 0;
}
static void broadcast_status() {
  Status_Msg msg = {MT_STATUS, my_status};
  send_broadcast((const void *) &msg, sizeof(msg));
}
static void store_status(const rimeaddr_t *from, uint32_t hops, uint16_t rssi, bool broadcast) {
  rimeaddr_copy(&(my_status.parent_addr), from);
  my_status.hops_to_root = hops + 1;
  my_status.parent_rssi = rssi;
  connected_to_tree = true;
  no_news_from_parent = 0;
  if (broadcast) broadcast_status();
}
static void parent_disconnection() {
  reset_status();
  Basic_Msg msg = {MT_DISCONNECTION};
  printf("/!\\ /!\\ Parent lost.\n");
  send_broadcast((const void *) &msg, sizeof(msg));
}
PROCESS_THREAD(manage_motes_network, ev, data)
{
  static struct etimer et;
  static int    timer = 4;

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
      Basic_Msg msg = {MT_DISCOVERY};
      send_broadcast((const void*) &msg, sizeof(msg));
      continue;
    } 
    
    if (!is_root) {
      if (no_news_from_parent > 2) {
        parent_disconnection();
        continue;
      } else if (no_news_from_parent > 1) {
        Basic_Msg msg = {MT_DISCOVERY};
        send_unicast((const void*) &msg, sizeof(msg), (const rimeaddr_t*) &my_status.parent_addr);
      }
      no_news_from_parent++;
    }
	
    printf("### Connected to tree, parent : %u.%u ###\n",
          my_status.parent_addr.u8[0], my_status.parent_addr.u8[1]);
    broadcast_status();

  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
static int get_rand_temp(){
  unsigned short r;
  int value;
  r = random_rand();
  value = (int) r;
  value = value < 0 ? -1 * value : value;
  value = (value % ((MAX_RAND_TEMP) - (MIN_RAND_TEMP) + 1)) + (MIN_RAND_TEMP);
  return value;
}
static void send_new_temp_data(){
  Data_Msg dmsg;

  dmsg.type = MT_DATA;
  dmsg.channel_name = TEMP_CHANNEL_NAME;
  dmsg.mote_addr_from = rimeaddr_node_addr;
  dmsg.data_value = get_rand_temp();

  send_unicast((const void*) &dmsg, sizeof(dmsg), (const rimeaddr_t*) &my_status.parent_addr);
}
PROCESS_THREAD(data_sender, ev, data)
{   
  static struct etimer et;
  static int    timer = 5;

  PROCESS_EXITHANDLER(goto exit);
  PROCESS_BEGIN();

  random_init(rimeaddr_node_addr.u8[0] + rimeaddr_node_addr.u8[1]);

  while(1) {
    etimer_set(&et, CLOCK_SECOND * timer + random_rand() % (CLOCK_SECOND * timer));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    if (!connected_to_tree) continue;   // skip the loop, wait for tree connection
    else if (is_root) goto exit;        // the root doesn't create any data (TODO: really ?)

    send_new_temp_data();
  }
 
  exit: ;
    PROCESS_END();
}
