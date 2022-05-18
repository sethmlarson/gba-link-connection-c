#include <stdio.h>
#include <tonc.h>
#include "../../link_connection.h"

LinkConnection conn;

void onVBlank() {
  lc_on_vblank(&conn);
}
void onSerial() {
  lc_on_serial(&conn);
}
void onTimer() {
  lc_on_timer(&conn);
}

int main(void) {
  
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
  tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
  
  // (1) Create a LinkConnection instance
  LinkConnectionSettings settings = {
    .baud_rate = BAUD_RATE_1,
    .timeout = 3,
    .remote_timeout = 5,
    .buffer_len = 30,
    .interval = 50,
    .send_timer_id = 3,
  };
  conn = lc_init(settings);
  
  irq_init(NULL);
  
  // (2) Add the interrupt service routines
  irq_add(II_VBLANK, onVBlank);
  irq_add(II_SERIAL, onSerial);
  irq_add(II_TIMER3, onTimer);
  
  // (3) Initialize the library
  lc_activate(&conn);
  
  
  u16 data[LINK_MAX_PLAYERS] = {};
  char str[128] = {'\0'};
  
  while (1) {
    tte_erase_screen();
    tte_set_pos(0, 0);
    
    // (4) Send/read messages
    u16 keys = ~REG_KEYS & KEY_ANY;
    u16 message = keys + 1;
    lc_send(&conn, message);
    
    if (lc_is_connected(&conn)) {
      
      sprintf(str, "Players: %d\n", conn.state.player_count);
      tte_write(str);
      
      for (int id = 0; id < conn.state.player_count; id++) {
        
        while (lc_has_message(&conn, id)) {
          data[id] = lc_read_message(&conn, id) - 1;
        }
        
        sprintf(str, "Players %d: %d\n", id, data[id]);
        tte_write(str);
        
      }
      
      sprintf(str, "Sent: %d\nSelf pID: %d\n", message, conn.state.current_player_id);
      tte_write(str);
      
    } else {
      
      sprintf(str, "Waiting...\n");
      tte_write("Waiting...");
      
    }
    
    VBlankIntrWait();
    
  }
  
}
