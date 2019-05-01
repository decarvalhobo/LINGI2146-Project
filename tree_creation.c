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
#define MT_PARENT_INFO          0
#define MT_STATUS               1
#define MT_DISCONNECTED         2
#define MT_DATA                 3

// TODO : quid if node dead ?
// TODO : comment code

typedef struct {
  rimeaddr_t    addr;
  int32_t       hops;
  uint16_t      rssi;
} Neighbour;

/* Message formats */
typedef struct {
  uint8_t       type; 
  uint32_t      hops;
} Status_Msg;

typedef struct {
  uint8_t       type;
  Neighbour     parent;
} Parent_Info_Msg;

/* Global variables required */
static int no_parent_news = 0;
static bool connected_to_tree = false;
static Neighbour my_parent;

static void lost_parent();

/*---------------------------------------------------------------------------*/
static void
process_status_msg(Status_Msg* message, const rimeaddr_t *from) {
  bool new_parent = true;
  uint16_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);

  if (my_parent.addr.u8[0] == from->u8[0] && my_parent.addr.u8[1] == from->u8[1]) {
    /* If the message comes from the current parent, we store its new status */
    my_parent.hops = message->hops;
    my_parent.rssi = rssi;
    no_parent_news = 0;
    connected_to_tree = true;
    return;
  }
  
  if(connected_to_tree){
    /* We store the new parent if it is closer (hops), or if the new possible parent 
       and the current parent have the same hops number but the new one has a better 
       signal strenght */
    new_parent = (message->hops < my_parent.hops)
                  || (message->hops == my_parent.hops && rssi > my_parent.rssi);
  }

  if(new_parent){
    /* We store the new parent */
    rimeaddr_copy(&(my_parent.addr), from);
    my_parent.hops = message->hops;
    my_parent.rssi = rssi;
    connected_to_tree = true;
    no_parent_news = 0;
  }

}

static void recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
  uint8_t* message_type = (uint8_t *) packetbuf_dataptr();

  switch(*message_type) {
    case MT_DISCONNECTED:
      printf("unicast message received : NODE DISCONNECTED !\n");
      //TODO
      break;

    case MT_PARENT_INFO:
    case MT_STATUS: ; 
      printf("unicast message received : status\n");
      Status_Msg* message = (Status_Msg *) packetbuf_dataptr();
      process_status_msg(message, from);
      break;
      
    default:
      printf("unicast message received : UNKNOWN TYPE : %d\n", *message_type);
      break;
  }
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};
static struct unicast_conn uc;
/*---------------------------------------------------------------------------*/
PROCESS(example_broadcast_process, "Broadcast example"); // TODO
AUTOSTART_PROCESSES(&example_broadcast_process);

/*---------------------------------------------------------------------------*/
static void process_parent_info_msg(Neighbour* neighbour_parent, const rimeaddr_t *from) {
  printf("broadcast message received from %d.%d\n",
         from->u8[0], from->u8[1]);
  
  /* Current node shares its configuration if (and) :
    - is connected to the tree
    - the parent hops is less (or equal) than the neighbour hops
  */
  if(connected_to_tree 
      && neighbour_parent->hops >= (my_parent.hops + 1)) {
    Status_Msg response;

    response.type = MT_STATUS;
    response.hops = my_parent.hops + 1;

    packetbuf_copyfrom((const void *) &response, sizeof(response));

    /* The unicast receiver is the broadcast sender (from) : */
    printf("%u.%u: sending unicast to address %u.%u\n",
           rimeaddr_node_addr.u8[0], rimeaddr_node_addr.u8[1],
           from->u8[0], from->u8[1]);
    unicast_send(&uc, from);
  }
}

static void
broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
  uint8_t* message_type = (uint8_t *) packetbuf_dataptr();

  switch(*message_type) {
    case MT_DISCONNECTED:
      printf("broadcast message received : node disconnected !\n");
      if (rimeaddr_cmp(from, (const rimeaddr_t *) &my_parent.addr)) {
        lost_parent();
        printf("My parent is disconnected...\n");
      } 
      break;

    case MT_PARENT_INFO: ; 
      Parent_Info_Msg* message = (Parent_Info_Msg *) packetbuf_dataptr();
      process_parent_info_msg(&(message->parent), from);
      break;
      
    default:
      printf("broadcast message received : UNKNOWN TYPE : %d\n", *message_type);
      break;
  }
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
static void reset_my_parent() {
  my_parent.addr = rimeaddr_node_addr;
  my_parent.hops = 100; // TODO fix ?
  my_parent.rssi = 0;
}

static void lost_parent() {
  connected_to_tree = false;
  no_parent_news = 0;
  reset_my_parent();
  printf("NO MORE PARENT!\n");
  uint8_t type = MT_DISCONNECTED;
  packetbuf_copyfrom((const void *) &type, sizeof(type));
  broadcast_send(&broadcast);
  printf("broadcast message sent : Disconnected\n");
}


PROCESS_THREAD(example_broadcast_process, ev, data)
{
  static struct etimer et;
  static bool send_broadcast = false;
  static bool skip_broadcast = true;
  static bool is_root = false;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
  PROCESS_EXITHANDLER(unicast_close(&uc);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);
  unicast_open(&uc, 146, &unicast_callbacks);

  reset_my_parent();

  /* By default, node 1.0 is the root */
  is_root = rimeaddr_node_addr.u8[0] == 1 && rimeaddr_node_addr.u8[1] == 0;

  if (is_root) {
    my_parent.hops = 0;
    connected_to_tree = true;
  } 

  while(1) {
    send_broadcast = false;

    /* Delay 2-4 seconds */
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % (CLOCK_SECOND * 4));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    /* If not root, send a broadcast message to get information
       about neighbourhood */
    if (!is_root) {
      /* Send broadcast message as faster as possible if not yet connected
         to the tree */
      if (!connected_to_tree || !skip_broadcast) send_broadcast = true;
      /* Else, skip one iteration before sending a new broadcast message */
      else skip_broadcast = !skip_broadcast;
    } 

    if (!is_root && connected_to_tree) {
      no_parent_news++;
      if (no_parent_news > 2) {
        lost_parent();
      } else {
        printf("##### My parent is %d:%d, hops : %lu #####\n",
              my_parent.addr.u8[0], my_parent.addr.u8[1], my_parent.hops);
      }
    }

    if (no_parent_news > 1) {
      Parent_Info_Msg message;
      message.type = MT_PARENT_INFO;
      message.parent = my_parent;

      packetbuf_copyfrom((const void *) &message, sizeof(message));
      unicast_send(&uc, (const rimeaddr_t *) &my_parent.addr);
      printf("unicast message sent to parent : need status update !\n");
    }
  
    /* Send message if asked */
    if (send_broadcast) {
      Parent_Info_Msg message;
      message.type = MT_PARENT_INFO;
      message.parent = my_parent;

      packetbuf_copyfrom((const void *) &message, sizeof(message));
      broadcast_send(&broadcast);
      printf("broadcast message sent\n");
    }

  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
