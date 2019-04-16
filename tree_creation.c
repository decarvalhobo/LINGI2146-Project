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
#define MT_INFORMATION          0
#define MT_DATA                 1

// TODO : quid if node dead ?
// TODO : comment code

typedef struct {
  rimeaddr_t    addr;
  int32_t       hops;
  uint16_t      rssi;
} Neighbour;

/* Message format */
typedef struct {
  uint8_t       type; 
  uint32_t      hops;
} Message;

/* Global variables required */
static bool connected_to_root = false;
static Neighbour my_parent;

/*---------------------------------------------------------------------------*/
/* OPTIONAL: Sender history.
 * Detects duplicate callbacks at receiving nodes.
 * Duplicates appear when ack messages are lost. */
struct history_entry {
  struct history_entry *next;
  rimeaddr_t addr;
  uint8_t seq;
};
LIST(history_table);
MEMB(history_mem, struct history_entry, NUM_HISTORY_ENTRIES);

/*---------------------------------------------------------------------------*/
static void
recv_runicast(struct runicast_conn *c, const rimeaddr_t *from, uint8_t seqno)
{
  /* OPTIONAL: Sender history */
  struct history_entry *e = NULL;
  for(e = list_head(history_table); e != NULL; e = e->next) {
    if(rimeaddr_cmp(&e->addr, from)) {
      break;
    }
  }
  if(e == NULL) {
    /* Create new history entry */
    e = memb_alloc(&history_mem);
    if(e == NULL) {
      e = list_chop(history_table); /* Remove oldest at full history */
    }
    rimeaddr_copy(&e->addr, from);
    e->seq = seqno;
    list_push(history_table, e);
  } else {
    /* Detect duplicate callback */
    if(e->seq == seqno) {
      printf("runicast message received from %d.%d, seqno %d (DUPLICATE)\n",
	     from->u8[0], from->u8[1], seqno);
      return;
    }
    /* Update existing history entry */
    e->seq = seqno;
  }
  bool new_parent = true;
  Message *message = (Message *) packetbuf_dataptr();
  uint16_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  printf("runicast message received from %d.%d, hops : %lu\n", 
          from->u8[0], from->u8[1], message->hops);
  if(connected_to_root){
    /* We store the new parent if it is closer (hops), or if the new possible parent 
       and the current parent have the same hops number but the new one has a better 
       signal strenght */
    new_parent = (message->hops < my_parent.hops)
                  || (message->hops == my_parent.hops && rssi > my_parent.rssi);
  }
  if(new_parent){
    rimeaddr_copy(&(my_parent.addr), from);
    my_parent.hops = message->hops;
    my_parent.rssi = rssi;
    connected_to_root = true;
  }
}
static void
sent_runicast(struct runicast_conn *c, const rimeaddr_t *to, uint8_t retransmissions)
{
  printf("runicast message sent to %d.%d, retransmissions %d\n",
	 to->u8[0], to->u8[1], retransmissions);
}
static void
timedout_runicast(struct runicast_conn *c, const rimeaddr_t *to, uint8_t retransmissions)
{
  printf("runicast message timed out when sending to %d.%d, retransmissions %d\n",
	 to->u8[0], to->u8[1], retransmissions);
}
static const struct runicast_callbacks runicast_callbacks = {recv_runicast,
							     sent_runicast,
							     timedout_runicast};
static struct runicast_conn runicast;

/*---------------------------------------------------------------------------*/
PROCESS(example_broadcast_process, "Broadcast example"); // TODO
AUTOSTART_PROCESSES(&example_broadcast_process);

/*---------------------------------------------------------------------------*/
static void
broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
  Neighbour *neighbour_parent = (Neighbour *) packetbuf_dataptr();
  printf("broadcast message received from %d.%d\n",
         from->u8[0], from->u8[1]);
 // printf("current parent hops : %lu\n", my_parent.hops);
 // printf("neighbour hops      : %lu\n", neighbour_parent->hops);
  
  /* Current nodes shares its configuration if (and) : 
      - is not sending runicast
      - is connected to the tree
      - the parent hops is less (or equal) than the neighbour hops
      - is not the neighbour's parent
  */
  if(!runicast_is_transmitting(&runicast) 
      && connected_to_root
      && neighbour_parent->hops >= (my_parent.hops + 1)
      && !rimeaddr_cmp(&(neighbour_parent->addr), &rimeaddr_node_addr)) {
    Message response;

    response.type = MT_INFORMATION;
    response.hops = my_parent.hops + 1;

    packetbuf_copyfrom((const void *) &response, sizeof(response));

    /* The unicast receiver is the broadcast sender (from) : */
    printf("%u.%u: sending runicast to address %u.%u\n",
           rimeaddr_node_addr.u8[0], rimeaddr_node_addr.u8[1],
           from->u8[0], from->u8[1]);
    runicast_send(&runicast, from, MAX_RETRANSMISSIONS);
  } else {
    //printf("ELSE !\n");
    //printf("%d\n", !runicast_is_transmitting(&runicast));
    //printf("%d\n", connected_to_root);
    //printf("%d (%lu >= %lu)\n", neighbour_parent->hops >= (my_parent.hops + 1), neighbour_parent->hops, my_parent.hops + 1);
    //printf("%d\n", rimeaddr_cmp(&(neighbour_parent->addr), &rimeaddr_node_addr));
  }
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(example_broadcast_process, ev, data)
{
  static struct etimer et;
  static bool send_broadcast = false;
  static bool skip_broadcast = true;
  static bool is_root = false;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);
  runicast_open(&runicast, 144, &runicast_callbacks);

  list_init(history_table);
  memb_init(&history_mem);

  /* By default, node 1.0 is the root */
  is_root = rimeaddr_node_addr.u8[0] == 1 && rimeaddr_node_addr.u8[1] == 0;

  my_parent.addr = rimeaddr_node_addr;
  my_parent.hops = 100; // TODO fix ?
  my_parent.rssi = 0;

  if (is_root) {
    my_parent.hops = 0;
    connected_to_root = true;
  } 

  while(1) {
    send_broadcast = false;

    /* Delay 2-4 seconds */
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % (CLOCK_SECOND * 4));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    /* If not connected to root, send a broadcast message to get information
       about neighbourhood */
    if (!is_root) {
      if (!connected_to_root || !skip_broadcast) send_broadcast = true;
      else skip_broadcast = !skip_broadcast;
    } 
    if (connected_to_root) {
      printf("##### My parent is %d:%d, hops : %lu #####\n",
            my_parent.addr.u8[0], my_parent.addr.u8[1], my_parent.hops);
    }
  
    /* Send message if asked */
    if (send_broadcast) {
      packetbuf_copyfrom((const void *) &my_parent, sizeof(my_parent));
      broadcast_send(&broadcast);
      printf("broadcast message sent\n");
    }

  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
