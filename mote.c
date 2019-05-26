#include "net/rime.h"
#include "random.h"
#include "dev/button-sensor.h"
#include "dev/serial-line.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*---------------------------------------------------------------------------*/

#define DELAY_BETWEEN_DATA      5
#define PERIOD_DATA_BY_DEFAULT  true

#define TEMP_CHANNEL_NAME       "temp"
#define MIN_RAND_TEMP           -20
#define MAX_RAND_TEMP           80

#define HUM_CHANNEL_NAME       "hum"

/*---------------------------------------------------------------------------*/
/* MT == Message Type */
#define MT_DISCOVERY            0
#define MT_STATUS               1
#define MT_DISCONNECTION        2
#define MT_DATA                 3
#define MT_BROKER_STATUS        4
#define MT_NEED_BROKER_STATUS   5

typedef struct {
  rimeaddr_t    parent_addr;
  uint16_t      parent_rssi;
  int32_t       hops_to_root;
  unsigned long broker_version;
} Mote_Status;

typedef struct {
  unsigned long version;
  bool          temp_required;
  bool          hum_required;
} Broker_Status;

typedef struct {
  int           temperature;
  int           humidity;
} Data_History;

/* Message formats */
typedef struct {
  uint8_t       type;
} Basic_Msg;

typedef struct {
  uint8_t       type; 
  Mote_Status   status;
} Mote_Status_Msg;

typedef struct {
  uint8_t       type;
  Broker_Status br_status;
} Broker_Status_Msg;

typedef struct {
  uint8_t       type;
  char*         channel_name;
  rimeaddr_t    mote_addr_from;
  int           data_value;
} Data_Msg;

/* Global variables required */
static bool             connected_to_tree = false;
static bool             periodic_data = PERIOD_DATA_BY_DEFAULT;
static uint8_t          no_news_from_parent = 0;
static Mote_Status      my_status;
static Broker_Status    broker_status;
static Data_History     my_data_history;

static void process_status_msg(const rimeaddr_t *from, Mote_Status status, uint16_t rssi);
static void send_broadcast(const void* msg, int size);
static void send_unicast(const void* msg, int size, const rimeaddr_t* to);
static void reset_status();
static void broadcast_status();
static void store_status(const rimeaddr_t *from, uint32_t hops, uint16_t rssi, bool broadcast);
static void parent_disconnection();
static void print_data_msg(Data_Msg* msg);

static bool is_the_root() {
  /* By default, the root is the mote 1.0 */
  return rimeaddr_node_addr.u8[0] == 1 && rimeaddr_node_addr.u8[1] == 0;
}
static void print_data_msg_not_ptr(Data_Msg msg)
{
  /*  Use to 'send' data to the gateway
      FORMAT : ;<mote_addr>;<channel name>;<data value> */
  printf(";%u:%u;%s;%d\n",
          msg.mote_addr_from.u8[0], 
          msg.mote_addr_from.u8[1], 
          msg.channel_name, 
          msg.data_value);
}
static void print_data_msg(Data_Msg* msg) {
  print_data_msg_not_ptr(*msg);
}

/* This function processes a mote status message */
static void process_status_msg(const rimeaddr_t *from, Mote_Status sender_status, uint16_t rssi) {
  bool is_new_parent = true; // By default, the sender is a new parent, we will 
                             // check more details further

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

/* !! This function is the place through which all received messages pass and are 
    redirected if necessary !! */
static void process_message(const rimeaddr_t *from, bool is_unicast) {
  uint8_t* message_type = (uint8_t *) packetbuf_dataptr();
  switch(*message_type) {
    case MT_DISCOVERY:
      printf("Message received from %u.%u : ask for discovery !\n",
              from->u8[0], from->u8[1]);
      if (connected_to_tree) {
        Mote_Status_Msg msg = {MT_STATUS, my_status};
        send_unicast((const void*) &msg, sizeof(msg), from);
      } else if (!connected_to_tree && is_unicast) {
        /* If it's a unicast message, it means it comes from a children */
        Basic_Msg msg = {MT_DISCONNECTION};
        send_unicast((const void*) &msg, sizeof(msg), from);
      }
      break;
    case MT_STATUS: ;
      Mote_Status_Msg* status_msg = (Mote_Status_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : status !\n", from->u8[0], from->u8[1]);
      /* We always check if the broker status of the mote is up to date : */
      if (status_msg->status.broker_version > broker_status.version) {
        /* If not, we ask for details to the status message sender */
        Basic_Msg bmsg = {MT_NEED_BROKER_STATUS};  
        send_unicast((const void*) &bmsg, sizeof(bmsg), from);
      }
      process_status_msg(from, status_msg->status, packetbuf_attr(PACKETBUF_ATTR_RSSI));
      break;
    case MT_DISCONNECTION: 
      printf("Message received from %u.%u : Disconnection !\n", from->u8[0], from->u8[1]);
      if (connected_to_tree && rimeaddr_cmp((const rimeaddr_t *) &my_status.parent_addr, from)) {
        /* If my parent is now disconnected from the tree, the mote is disconnected too */
        parent_disconnection();
      }
      break;
    case MT_DATA: ;
      Data_Msg* data_msg = (Data_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : Data !\n", from->u8[0], from->u8[1]);
      if (is_the_root()) {
        /* If the root receives some data not required by the broker, the mote does not transfer
            it to the gateway. It can happen if the sender mote wasn't informed yet about the
            last broker status version. The root is the only one able to discard the data messages
            because it always has the last broker status version. */
        if ((strcmp(data_msg->channel_name, TEMP_CHANNEL_NAME) == 0 && !broker_status.temp_required)
          || (strcmp(data_msg->channel_name, HUM_CHANNEL_NAME) == 0 && !broker_status.hum_required) ) {
          return;
        }
	    print_data_msg((Data_Msg*) data_msg);
      } else if (connected_to_tree) {
        /* Transfer the data to the mote's parent */
        send_unicast((const void*) data_msg, sizeof(*data_msg), 
                     (const rimeaddr_t*) &my_status.parent_addr);
      }
      break;
    case MT_BROKER_STATUS: ;
      Broker_Status_Msg* br_status_msg = (Broker_Status_Msg *) packetbuf_dataptr(); 
      printf("Message received from %u.%u : Broker status !\n", from->u8[0], from->u8[1]);
      /* If it is a new broker status version, update and broadcast it */
      if (br_status_msg->br_status.version > broker_status.version) {
        broker_status = br_status_msg->br_status;
        my_status.broker_version = broker_status.version;
        send_broadcast((const void *) br_status_msg, sizeof(*br_status_msg));
      }
      break;
    case MT_NEED_BROKER_STATUS:
      printf("Message received from %u.%u : Need broker status !\n", from->u8[0], from->u8[1]);
      Broker_Status_Msg rsts_msg = {MT_BROKER_STATUS, broker_status};
      send_unicast((const void*) &rsts_msg, sizeof(rsts_msg), from);
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
PROCESS(socket_listener, "Listen socket");
AUTOSTART_PROCESSES(&manage_motes_network, &data_sender, &socket_listener);

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
  my_status.broker_version = 0;
  no_news_from_parent = 0;
  broker_status.temp_required = true;
  broker_status.hum_required = true;
}
static void broadcast_status() {
  Mote_Status_Msg msg = {MT_STATUS, my_status};
  send_broadcast((const void *) &msg, sizeof(msg));
}
static void store_status(const rimeaddr_t *from, uint32_t hops, uint16_t rssi, bool broadcast) {
  rimeaddr_copy(&(my_status.parent_addr), from);
  my_status.hops_to_root = hops + 1;
  my_status.parent_rssi = rssi;
  connected_to_tree = true;
  no_news_from_parent = 0;
  /* If this is not a parent status update but a new parent,
      we inform the others (because it's not urgent, the network process
      will send a status message soon...) : */
  if (broadcast) broadcast_status();
}
static void parent_disconnection() {
  reset_status();
  Basic_Msg msg = {MT_DISCONNECTION};
  printf("/!\\ /!\\ Parent lost.\n");
  /* The mote propagates disconnection information as quickly as possible */
  send_broadcast((const void *) &msg, sizeof(msg));
}
/*---------------------------------------------------------------------------*/
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

  if (is_the_root()) {
    my_status.hops_to_root = 0;
    connected_to_tree = true;
  } 

  while(1) {
    /* If not connected, quickly broadcast a message to find a parent */
    if (!connected_to_tree) timer = 1;
    /* Else, wait for more time and broadcast the current status */
    else                    timer = 4;
    
    etimer_set(&et, CLOCK_SECOND * timer + random_rand() % (CLOCK_SECOND * timer));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    if (!connected_to_tree) {
      Basic_Msg msg = {MT_DISCOVERY};
      send_broadcast((const void*) &msg, sizeof(msg));
      continue;
    } 
    
    if (!is_the_root()) {
      /* The mote checks if it has some news from its parent 
          Remind : each time a mote receives a status message from
          its parent, it resets the variable no_news_from_parent
          at zero. */
      if (no_news_from_parent > 2) {
        parent_disconnection();
        continue;
      } else if (no_news_from_parent > 1) {
        /* It asks its parent for some news */
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
static int get_rand(int min, int max){
  unsigned short r;
  int value;
  r = random_rand();
  value = (int) r;
  value = value < 0 ? -1 * value : value;
  value = (value % (max - min + 1)) + min;
  return value;
}
static void send_new_temp_data(){
  Data_Msg dmsg;

  dmsg.type = MT_DATA;
  dmsg.channel_name = TEMP_CHANNEL_NAME;
  dmsg.mote_addr_from = rimeaddr_node_addr;
  dmsg.data_value = get_rand(MIN_RAND_TEMP, MAX_RAND_TEMP);
  
  /* If this is the "send only on data change" mode and the data hasn't
      change, send nothing */
  if (!periodic_data && dmsg.data_value == my_data_history.temperature) {
    return;
  }
  
  /* Store and send the new data */
  my_data_history.temperature = dmsg.data_value;

  /* If the mote is the root, directly send it to the gateway */
  if (is_the_root()) print_data_msg_not_ptr(dmsg);
  /* Else, send it to the mote's parent */
  else send_unicast((const void*) &dmsg, sizeof(dmsg), 
        (const rimeaddr_t*) &my_status.parent_addr);
}
static void send_new_hum_data(){
  Data_Msg dmsg;

  dmsg.type = MT_DATA;
  dmsg.channel_name = HUM_CHANNEL_NAME;
  dmsg.mote_addr_from = rimeaddr_node_addr;
  dmsg.data_value = get_rand(0, 100);

  /* If this is the "send only on data change" mode and the data hasn't
      change, send nothing */
  if (!periodic_data && dmsg.data_value == my_data_history.humidity) {
    return;
  }

  /* Store and send the new data */
  my_data_history.humidity = dmsg.data_value;

  /* If the mote is the root, directly send it to the gateway */
  if (is_the_root()) print_data_msg_not_ptr(dmsg);
  /* Else, send it to the mote's parent */
  else send_unicast((const void*) &dmsg, sizeof(dmsg), 
        (const rimeaddr_t*) &my_status.parent_addr);
}
PROCESS_THREAD(data_sender, ev, data)
{   
  static struct etimer et;
  static int    timer = DELAY_BETWEEN_DATA;
  
  PROCESS_EXITHANDLER(goto exit);
  PROCESS_BEGIN();
  SENSORS_ACTIVATE(button_sensor);
  /* Use mote's address as random seed :*/
  random_init(rimeaddr_node_addr.u8[0] + rimeaddr_node_addr.u8[1]);

  while(1) {
    etimer_set(&et, CLOCK_SECOND * timer + random_rand() % (CLOCK_SECOND * timer));
    PROCESS_WAIT_EVENT();

    /* If the button has been pressed, change the data sending mode */
    if(ev == sensors_event) {
      if(data == &button_sensor) {
        periodic_data = !periodic_data;
        printf("Data sending mode changed : %s\n", periodic_data ? "periodically" : "only on change");
      }
    }

    /* If the timer is expired and the mote is connected to the tree, send data */
    if (etimer_expired(&et) && connected_to_tree) {
      if (broker_status.temp_required) send_new_temp_data();
      if (broker_status.hum_required) send_new_hum_data();
    }
  }
 
  exit: ;
    PROCESS_END();
}
PROCESS_THREAD(socket_listener, ev, data)
{
  /* The root is the only one concerned by this process */
  if (!is_the_root()) return 0;

  PROCESS_BEGIN();

  for(;;) {
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      char *str = (char*) data;
      char *value, *subject;

      /* Split the message received : */
      value = strtok (str,":");
      subject = strtok (NULL, ":");

      /* Update the broker status */
      if (strcmp(subject, TEMP_CHANNEL_NAME) == 0) {
        broker_status.temp_required = strcmp(value, "1") == 0;
      } else if (strcmp(subject, HUM_CHANNEL_NAME) == 0) {
        broker_status.hum_required = strcmp(value, "1") == 0;
      }
      broker_status.version++;

      /* Send the new broker status */
      Broker_Status_Msg br_status_msg = {MT_BROKER_STATUS, broker_status};
      send_broadcast((const void*) &br_status_msg, sizeof(br_status_msg));
    }
  }

  PROCESS_END();
}
