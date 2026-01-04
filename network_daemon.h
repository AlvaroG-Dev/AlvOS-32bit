#ifndef NETWORK_DAEMON_H
#define NETWORK_DAEMON_H

#include <stdbool.h>

// Función del daemon de red
void network_daemon_func(void *arg);

// Iniciar/detener el daemon
bool network_daemon_start(void);
void network_daemon_stop(void);

// Estado del daemon
bool network_daemon_is_running(void);

#endif // NETWORK_DAEMON_H
