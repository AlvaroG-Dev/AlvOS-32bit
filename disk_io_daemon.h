// ========================================================================
// disk_io_daemon.h
// ========================================================================

#ifndef DISK_IO_DAEMON_H
#define DISK_IO_DAEMON_H

#include "disk.h"

// Inicializar el daemon
void disk_io_daemon_init(void);

// Función principal del daemon (no llamar directamente)
void disk_io_daemon(void* arg);

// API pública para I/O asíncrono
disk_err_t async_disk_read(disk_t* disk, uint64_t lba, uint32_t count, void* buffer);
disk_err_t async_disk_write(disk_t* disk, uint64_t lba, uint32_t count, const void* buffer);
disk_err_t async_disk_flush(disk_t* disk);

void cmd_async_read_test(void);
void cmd_async_write_test(void);

#endif // DISK_IO_DAEMON_H
