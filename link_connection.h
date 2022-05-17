#ifndef LINK_CONNECTION_H
#define LINK_CONNECTION_H

/*
gba-link-connection - C port of https://github.com/rodri042/gba-link-connection

A Link Cable connection for Multi-player mode.

Usage:

1) Include this header in your main.c file, then instantiate a connection.

  LinkConnection conn = lc_init({
    .baud_rate = BAUD_RATE_1,
    .timeout = 3,
    .remote_timeout = 5,
    .buffer_size = 30,
    .interval = 50,
    .send_timer_id = 3,
  });

2) Add the required interrupt service routines:

  void myVBlankHandler() {
    lc_on_vblank(&conn);
  }
  void mySerialHandler() {
    lc_on_serial(&conn);
  }
  void myTimerHandler() {
    lc_on_timer(&conn);
  }
  
  irq_init(NULL);
  
  irq_add(II_VBLANK, myVBlankHandler);
  irq_add(II_SERIAL, mySerialHandler);
  irq_add(II_TIMER3, myTimerHandler);

3) Initialize the library with:
  
  lc_activate(&conn);

4) Send/read messages by using:

  lc_send(&conn, data)
  lc_is_connected(&conn)
  lc_has_message(&conn)
  lc_read_message(&conn)

`data` restrictions:
0xFFFF and 0x0 are reserved values, so don't use them
(they mean 'disconnected' and 'no data' respectively)

5) Free the buffers when done:

  lc_destroy(&conn);

*/

#include <stdlib.h>
#include <tonc_core.h>
#include <tonc_memdef.h>
#include <tonc_memmap.h>

#define LINK_MAX_PLAYERS 4
#define LINK_DISCONNECTED 0xFFFF
#define LINK_NO_DATA 0x0
#define LINK_BASE_FREQUENCY TM_FREQ_1024
#define LINK_REMOTE_TIMEOUT_OFFLINE -1
#define LINK_BIT_SLAVE 2
#define LINK_BIT_READY 3
#define LINK_BITS_PLAYER_ID 4
#define LINK_BIT_ERROR 6
#define LINK_BIT_START 7
#define LINK_BIT_MULTIPLAYER 13
#define LINK_BIT_IRQ 14
#define LINK_BIT_GENERAL_PURPOSE_LOW 14
#define LINK_BIT_GENERAL_PURPOSE_HIGH 15
#define LINK_SET_HIGH(REG, BIT) REG |= 1 << BIT
#define LINK_SET_LOW(REG, BIT) REG &= ~(1 << BIT)

/**
 * A basic std::queue<u16> replacement.
 */
typedef struct U16Queue {
  u16 *buf;
  u32 cap, len;
  u32 i, j;
} U16Queue;

typedef enum BaudRate {
  BAUD_RATE_0,  // 9600 bps
  BAUD_RATE_1,  // 38400 bps
  BAUD_RATE_2,  // 57600 bps
  BAUD_RATE_3   // 115200 bps
} BaudRate;

typedef struct LinkState {
  u8 player_count;
  u8 current_player_id;
  
  // private fields
  U16Queue incoming_messages[LINK_MAX_PLAYERS];
  U16Queue outgoing_messages;
  int timeouts[LINK_MAX_PLAYERS];
  bool irq_flag;
  u32 irq_timeout;
  bool is_locked;
} LinkState;

/**
 * The main connection type.
 */
typedef struct LinkConnection {
  LinkState state;
  
  // private fields
  BaudRate baud_rate;
  u32 timeout;
  u32 remote_timeout;
  u32 buffer_size;
  u32 interval;
  u8 send_timer_id;
  bool is_enabled;
} LinkConnection;

/**
 * Parameters for `lc_init`
 */
typedef struct LinkConnectionSettings {
  BaudRate baud_rate;    // Sets a specific baud rate.
  u32 timeout;           // Number of frames without an II_SERIAL IRQ to reset the connection.
  u32 remote_timeout;    // Number of messages with 0xFFFF to mark a player as disconnected.
  u32 buffer_size;       // Number of messages that the queues will be able to store.
  u32 interval;          // Number of 1024-cycles (61.04μs) ticks between messages (50 = 3,052ms). It's the interval of the timer chosen by `send_timer_id`.
  u8 send_timer_id;      // GBA Timer to use for sending.
} LinkConnectionSettings;


// Basic std::queue<u16> replacement
// ---------------------------------

inline static U16Queue u16q_init(u32 cap) {
  return (U16Queue) {
    .buf = malloc(cap * sizeof(u16)),
    .cap = cap,
    .len = 0,
    .i = 0,
    .j = 0,
  };
}

inline static void u16q_destroy(U16Queue *q) {
  free(q->buf);
}

inline static bool u16q_empty(U16Queue *q) {
  return q->i == q->j;
}

inline static u16 u16q_front(U16Queue *q) {
  return q->buf[q->i];
}

inline static void u16q_pop(U16Queue *q) {
  u16 res = q->buf[q->i++];
  if (q->i >= q->cap) {
    q->i = 0;
  }
  q->len--;
  return res;
}

inline static void u16q_push(U16Queue *q, u16 n) {
  q->buf[q->j++] = n;
  if (q->j >= q->cap) {
    q->j = 0;
  }
  q->len++;
}

static inline u16 LINK_QUEUE_POP(U16Queue *q) {
  if (u16q_empty(q)) {
    return LINK_NO_DATA;
  }
  u16 value = u16q_front(q);
  u16q_pop(q);
  return value;
}

static inline void LINK_QUEUE_CLEAR(U16Queue *q) {
  while (!u16q_empty(q)) {
    LINK_QUEUE_POP(q);
  }
}

// Link State
// ----------

static inline LinkState linkstate_init(u32 buffer_size) {
  LinkState self = {};
  
  // allocate message buffers:
  for (int i = 0; i < LINK_MAX_PLAYERS; i++) {
    self.incoming_messages[i] = u16q_init(buffer_size);
  }
  self.outgoing_messages = u16q_init(buffer_size);
  
  return self;
}

static inline void linkstate_destroy(LinkState *self) {
  // free message buffers:
  for (int i = 0; i < LINK_MAX_PLAYERS; i++) {
    u16q_destroy(&self->incoming_messages[i]);
  }
  u16q_destroy(&self->outgoing_messages);
}

static inline bool linkstate_is_connected(LinkState *self) {
  return self->player_count > 1 && self->current_player_id < self->player_count;
}

static inline bool linkstate_has_message(LinkState *self, u8 player_id) {
  if (player_id >= self->player_count) {
    return false;
  }
  self->is_locked = true;
  bool has_message = !u16q_empty(&(self->incoming_messages[player_id]));
  self->is_locked = false;
  return has_message;
}

static inline u16 linkstate_read_message(LinkState *self, u8 player_id) {
  self->is_locked = true;
  u16 message = LINK_QUEUE_POP(&self->incoming_messages[player_id]);
  self->is_locked = false;
  return message;
}


// Link Connection Private API
// ---------------------------

static inline bool isBitHigh(u8 bit) { return (REG_SIOCNT >> bit) & 1; }
static inline void setBitHigh(u8 bit) { LINK_SET_HIGH(REG_SIOCNT, bit); }
static inline void setBitLow(u8 bit) { LINK_SET_LOW(REG_SIOCNT, bit); }

static inline bool lc_is_ready(LinkConnection *self) { return isBitHigh(LINK_BIT_READY); }
static inline bool lc_has_error(LinkConnection *self) { return isBitHigh(LINK_BIT_ERROR); }
static inline bool lc_is_master(LinkConnection *self) { return !isBitHigh(LINK_BIT_SLAVE); }
static inline bool lc_is_sending(LinkConnection *self) { return isBitHigh(LINK_BIT_START); }
static inline bool lc_did_timeout(LinkConnection *self) { return self->state.irq_timeout >= self->timeout; }

static inline void lc_reset_state(LinkConnection *self) {
  self->state.player_count = 0;
  self->state.current_player_id = 0;
  for (u32 i = 0; i < LINK_MAX_PLAYERS; i++) {
    LINK_QUEUE_CLEAR(&self->state.incoming_messages[i]);
    self->state.timeouts[i] = LINK_REMOTE_TIMEOUT_OFFLINE;
  }
  LINK_QUEUE_CLEAR(&self->state.outgoing_messages);
  self->state.irq_flag = false;
  self->state.irq_timeout = 0;
}

static inline void lc_transfer(LinkConnection *self, u16 data) {
  REG_SIOMLT_SEND = data;

  if (lc_is_master(self))
    setBitHigh(LINK_BIT_START);
}

static inline void lc_send_pending_data(LinkConnection *self) {
  lc_transfer(self, LINK_QUEUE_POP(&self->state.outgoing_messages));
}

static inline void lc_stop_timer(LinkConnection *self) {
  REG_TM[self->send_timer_id].cnt = REG_TM[self->send_timer_id].cnt & (~TM_ENABLE);
}

static inline void lc_start_timer(LinkConnection *self) {
  REG_TM[self->send_timer_id].start = -(self->interval);
  REG_TM[self->send_timer_id].cnt = TM_ENABLE | TM_IRQ | LINK_BASE_FREQUENCY;
}

static inline void lc_stop(LinkConnection *self) {
  lc_stop_timer(self);

  LINK_SET_LOW(REG_RCNT, LINK_BIT_GENERAL_PURPOSE_LOW);
  LINK_SET_HIGH(REG_RCNT, LINK_BIT_GENERAL_PURPOSE_HIGH);
}

static inline void lc_start(LinkConnection *self) {
  lc_start_timer(self);

  LINK_SET_LOW(REG_RCNT, LINK_BIT_GENERAL_PURPOSE_HIGH);
  REG_SIOCNT = self->baud_rate;
  REG_SIOMLT_SEND = 0;
  setBitHigh(LINK_BIT_MULTIPLAYER);
  setBitHigh(LINK_BIT_IRQ);
}

static inline void lc_reset(LinkConnection *self) {
  lc_reset_state(self);
  lc_stop(self);
  lc_start(self);
}

static inline bool lc_reset_if_needed(LinkConnection *self) {
  if (!lc_is_ready(self) || lc_has_error(self)) {
    lc_reset(self);
    return true;
  }
  return false;
}

static inline void lc_push(LinkConnection *self, U16Queue *q, u16 value) {
  if (q->len >= self->buffer_size) {
    LINK_QUEUE_POP(&q);
  }
  u16q_push(q, value);
}


// Public API
// ----------

static inline LinkConnection lc_init(LinkConnectionSettings settings) {
  LinkConnection self = {
    .state = linkstate_init(settings.buffer_size),
    .baud_rate = settings.baud_rate,
    .timeout = settings.timeout,
    .remote_timeout = settings.remote_timeout,
    .buffer_size = settings.buffer_size,
    .interval = settings.interval,
    .send_timer_id = settings.send_timer_id,
  };
  lc_stop(&self);
  return self;
}

static inline void lc_destroy(LinkConnection *self) {
  lc_stop(self);
  linkstate_destroy(&self->state);
}

static inline bool lc_is_active(LinkConnection *self) {
  return self->is_enabled;
}

static inline void lc_activate(LinkConnection *self) {
  lc_reset(self);
  self->is_enabled = true;
}

static inline void lc_deactivate(LinkConnection *self) {
  self->is_enabled = false;
  lc_reset_state(self);
  lc_stop(self);
}

static inline void lc_send(LinkConnection *self, u16 data) {
  if (data == LINK_DISCONNECTED || data == LINK_NO_DATA) {
    return;
  }
  self->state.is_locked = true;
  u16q_push(&(self->state.outgoing_messages), data);
  self->state.is_locked = false;
}

static inline bool lc_is_connected(LinkConnection *self) {
  return linkstate_is_connected(&self->state);
}
static inline bool lc_has_message(LinkConnection *self, u8 player_id) {
  return linkstate_has_message(&self->state, player_id);
}
static inline u16 lc_read_message(LinkConnection *self, u8 player_id) {
  return linkstate_read_message(&self->state, player_id);
}

static inline void lc_on_vblank(LinkConnection *self) {
  if (!self->is_enabled || self->state.is_locked) {
    return;
  }
  if (!self->state.irq_flag) {
    self->state.irq_timeout++;
  }
  self->state.irq_flag = false;
}

static inline void lc_on_timer(LinkConnection *self) {
  if (!self->is_enabled || self->state.is_locked) {
    return;
  }
  if (lc_did_timeout(self)) {
    lc_reset(self);
    return;
  }
  if (lc_is_master(self) && lc_is_ready(self) && !lc_is_sending(self)) {
    lc_send_pending_data(self);
  }
}

static inline void lc_on_serial(LinkConnection *self) {
  if (!self->is_enabled || self->state.is_locked) {
    return;
  }
  
  if (lc_reset_if_needed(self)) {
    return;
  }
  
  self->state.irq_flag = true;
  self->state.irq_timeout = 0;
  
  int new_player_count = 0;
  
  for (u32 i = 0; i < LINK_MAX_PLAYERS; i++) {
    u16 data = REG_SIOMULTI[i];
    
    if (data != LINK_DISCONNECTED) {
      
      if (data != LINK_NO_DATA && i != self->state.current_player_id) {
        u16q_push(&self->state.incoming_messages[i], data);
      }
      new_player_count++;
      self->state.timeouts[i] = 0;
      
    } else if (self->state.timeouts[i] > LINK_REMOTE_TIMEOUT_OFFLINE) {
      
      self->state.timeouts[i]++;
      
      if (self->state.timeouts[i] >= (int)self->remote_timeout) {
        LINK_QUEUE_CLEAR(&self->state.incoming_messages[i]);
        self->state.timeouts[i] = LINK_REMOTE_TIMEOUT_OFFLINE;
      } else {
        new_player_count++;
      }
    }
  }
  
  self->state.player_count = new_player_count;
  self->state.current_player_id = (REG_SIOCNT & (0b11 << LINK_BITS_PLAYER_ID)) >> LINK_BITS_PLAYER_ID;
  
  if (!lc_is_master(self)) {
    lc_send_pending_data(self);
  }
}

#endif  // LINK_CONNECTION_H