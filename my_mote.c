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
static bool connected_to_tree = false;
static Neighbour my_parent;

/*---------------------------------------------------------------------------*/
static void recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
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
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(example_broadcast_process, ev, data)
{
  static struct etimer et;
  static bool is_root = false;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
  PROCESS_EXITHANDLER(unicast_close(&uc);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);
  unicast_open(&uc, 146, &unicast_callbacks);

  /* By default, node 1.0 is the root */
  is_root = rimeaddr_node_addr.u8[0] == 1 && rimeaddr_node_addr.u8[1] == 0;

  if (is_root) {
    my_parent.hops = 0;
    connected_to_tree = true;
  } 

  while(1) {
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % (CLOCK_SECOND * 4));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
