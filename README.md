# GBA Link Connection (C Port)

This is a C port of [gba-link-connection](https://github.com/rodri042/gba-link-connection) by rodri042, a library which makes it easy to add multiplayer support to Game Boy Advance homebrew games.

Notes:
* Depends on [libtonc](https://github.com/devkitPro/libtonc/).
* Uses `malloc` when creating the connection with `lc_init`, to allocate internal buffers.
* The example uses devkitARM, but it could work with any compatible toolchain.

## Usage

A Link Cable connection for Multi-player mode.

Usage:

1\) Include this header in your main.c file, then declare and initialise a connection.

```c
#include "link_connection.h"

LinkConnection conn;

// ...
  
  LinkConnectionSettings settings = {
    .baud_rate = BAUD_RATE_1,
    .timeout = 3,
    .remote_timeout = 5,
    .buffer_size = 30,
    .interval = 50,
    .send_timer_id = 3,
  };
  conn = lc_init(settings);
```

2\) Add the required interrupt service routines:

```c
void onVBlank() {
  lc_on_vblank(&conn);
}
void onSerial() {
  lc_on_serial(&conn);
}
void onTimer() {
  lc_on_timer(&conn);
}

// ...

  irq_init(NULL);
  
  irq_add(II_VBLANK, onVBlank);
  irq_add(II_SERIAL, onSerial);
  irq_add(II_TIMER3, onTimer);
```

3\) Start the library with:

```c
  lc_activate(&conn);
```

4\) Send/read messages by using:

```c
  lc_send(&conn, data)
  lc_is_connected(&conn)
  lc_has_message(&conn, player_id)
  lc_read_message(&conn, player_id)
```

Restrictions on sent data: `0xFFFF` and `0x0000` are reserved values, so don't use them (they mean 'disconnected' and 'no data' respectively).

5\) Destroy the connection to free the internal buffers:

```c
  lc_destroy(&conn);
```
