#ifndef CARLA_HOST_H
#define CARLA_HOST_H
#include <stddef.h>

/* Managed Carla instances for pattern-driven external plugin graphs. A project
   name is resolved under $TOMPOUCE_CARLA_ROOT or
   $XDG_CONFIG_HOME/tompouce/carla; startup and PipeWire MIDI linking run on a
   background thread so scheduler and audio work never wait for Carla. */
int carla_acquire(const char *project, const char *target,
    char *error, size_t error_size);
/* Stop every owned instance and join its background thread. */
void carla_stop_all(void);
/* Stop every owned instance and release all managed slots. */
void carla_shutdown(void);
int carla_active_count(void);
int carla_ready_count(void);

#endif
