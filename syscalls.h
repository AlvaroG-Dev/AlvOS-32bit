#ifndef SYSCALLS_H
#define SYSCALLS_H

#include "isr.h"
#include <stdint.h>

// Números de syscall (POSIX-like)
#define SYSCALL_EXIT 0x00
#define SYSCALL_WRITE 0x01
#define SYSCALL_READ 0x02
#define SYSCALL_GETPID 0x03
#define SYSCALL_YIELD 0x04
#define SYSCALL_SLEEP 0x05
#define SYSCALL_GETTIME 0x06
#define SYSCALL_OPEN 0x07
#define SYSCALL_CLOSE 0x08
#define SYSCALL_GETCWD 0x09
#define SYSCALL_CHDIR 0x0A
#define SYSCALL_STAT 0x0B
#define SYSCALL_FORK 0x0C
#define SYSCALL_EXECVE 0x0D
#define SYSCALL_READKEY 0x0E       // Leer tecla del teclado
#define SYSCALL_KEY_AVAILABLE 0x0F // Verificar si hay teclas disponibles
#define SYSCALL_GETC 0x10          // Obtener caracter (similar a getchar)
#define SYSCALL_GETS 0x11          // Obtener string (similar a gets)
#define SYSCALL_KBHIT 0x12         // Verificar si hay tecla disponible
#define SYSCALL_KBFLUSH 0x13       // Limpiar buffer del teclado
#define SYSCALL_MKDIR 0x14         // Crear directorio
#define SYSCALL_RMDIR 0x15         // Eliminar directorio
#define SYSCALL_UNLINK 0x16        // Eliminar archivo
#define SYSCALL_SEEK 0x17          // Mover puntero de archivo
#define SYSCALL_TELL 0x18          // Obtener posición en archivo
#define SYSCALL_IOCTL 0x19         // Control de dispositivo
#define SYSCALL_GETPPID 0x1A       // Obtener PID del padre
#define SYSCALL_GETUID 0x1B        // Obtener UID
#define SYSCALL_GETGID 0x1C        // Obtener GID
#define SYSCALL_DUP 0x1D           // Duplicar descriptor de archivo
#define SYSCALL_DUP2 0x1E          // Duplicar descriptor con número específico
#define SYSCALL_PIPE 0x1F          // Crear pipe
#define SYSCALL_WAITPID 0x20       // Esperar proceso hijo
#define SYSCALL_BRK 0x21           // Control de heap
#define SYSCALL_SBRK 0x22          // Cambiar tamaño del heap
#define SYSCALL_MMAP 0x23          // Mapear memoria
#define SYSCALL_MUNMAP 0x24        // Desmapear memoria
#define SYSCALL_GETDENTS 0x25      // Leer directorio
#define SYSCALL_FSTAT 0x26         // Estadísticas de archivo abierto
#define SYSCALL_FSYNC 0x27         // Sincronizar archivo
#define SYSCALL_TRUNCATE 0x28      // Truncar archivo
#define SYSCALL_ACCESS 0x29        // Verificar acceso a archivo
#define SYSCALL_CHMOD 0x2A         // Cambiar permisos
#define SYSCALL_CHOWN 0x2B         // Cambiar propietario
#define SYSCALL_UMASK 0x2C         // Cambiar máscara de permisos
#define SYSCALL_GETRUSAGE 0x2D     // Obtener uso de recursos
#define SYSCALL_TIMES 0x2E         // Obtener tiempos del proceso
#define SYSCALL_UNAME 0x2F         // Obtener información del sistema
#define SYSCALL_SYSCONF 0x30       // Obtener configuración del sistema
#define SYSCALL_GETPGRP 0x31       // Obtener grupo de proceso
#define SYSCALL_SETPGID 0x32       // Establecer grupo de proceso
#define SYSCALL_SETSID 0x33        // Crear nueva sesión
#define SYSCALL_GETSID 0x34        // Obtener ID de sesión
#define SYSCALL_MOUNT 0x35         // Montar filesystem
#define SYSCALL_UMOUNT 0x36        // Desmontar filesystem
#define SYSCALL_LSEEK 0x37         // Mover puntero de archivo (alias)
#define SYSCALL_LINK 0x38          // Crear enlace físico
#define SYSCALL_SYMLINK 0x39       // Crear enlace simbólico
#define SYSCALL_READLINK 0x3A      // Leer enlace simbólico
#define SYSCALL_RENAME 0x3B        // Renombrar archivo
#define SYSCALL_FCHDIR 0x3C        // Cambiar directorio por FD
#define SYSCALL_FCHMOD 0x3D        // Cambiar permisos de archivo abierto
#define SYSCALL_FCHOWN 0x3E        // Cambiar propietario de archivo abierto
#define SYSCALL_UTIME 0x3F         // Cambiar tiempos de acceso/modificación
#define SYSCALL_SYNC 0x40          // Sincronizar filesystem
#define SYSCALL_SOCKET 0x41        // Crear socket
#define SYSCALL_CONNECT 0x42       // Conectar socket
#define SYSCALL_SEND 0x43          // Enviar por socket
#define SYSCALL_RECV 0x44          // Recibir de socket
#define SYSCALL_DNS_RESOLVE 0x45   // Resolver host DNS

// ✅ Definir códigos de error (versión simplificada)
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EIO 5
#define ENXIO 6
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ENOLCK 37
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EWOULDBLOCK EAGAIN
#define ENOMSG 42
#define EIDRM 43
#define ECHRNG 44
#define EL2NSYNC 45
#define EL3HLT 46
#define EL3RST 47
#define ELNRNG 48
#define EUNATCH 49
#define ENOCSI 50
#define EL2HLT 51
#define EBADE 52
#define EBADR 53
#define EXFULL 54
#define ENOANO 55
#define EBADRQC 56
#define EBADSLT 57
#define EDEADLOCK EDEADLK
#define EBFONT 59
#define ENOSTR 60
#define ENODATA 61
#define ETIME 62
#define ENOSR 63
#define ENONET 64
#define ENOPKG 65
#define EREMOTE 66
#define ENOLINK 67
#define EADV 68
#define ESRMNT 69
#define ECOMM 70
#define EPROTO 71
#define EMULTIHOP 72
#define EDOTDOT 73
#define EBADMSG 74
#define EOVERFLOW 75
#define ENOTUNIQ 76
#define EBADFD 77
#define EREMCHG 78
#define ELIBACC 79
#define ELIBBAD 80
#define ELIBSCN 81
#define ELIBMAX 82
#define ELIBEXEC 83
#define EILSEQ 84
#define ERESTART 85
#define ESTRPIPE 86
#define EUSERS 87
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP 95
#define EPFNOSUPPORT 96
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ESHUTDOWN 108
#define ETOOMANYREFS 109
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTDOWN 112
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ESTALE 116
#define EUCLEAN 117
#define ENOTNAM 118
#define ENAVAIL 119
#define EISNAM 120
#define EREMOTEIO 121
#define EDQUOT 122
#define ENOMEDIUM 123
#define EMEDIUMTYPE 124
#define ECANCELED 125
#define ENOKEY 126
#define EKEYEXPIRED 127
#define EKEYREVOKED 128
#define EKEYREJECTED 129
#define EOWNERDEAD 130
#define ENOTRECOVERABLE 131
#define ERFKILL 132
#define EHWPOISON 133

// Estructuras adicionales para syscalls
typedef struct {
  uint32_t st_dev;     // Device ID
  uint32_t st_ino;     // Inode number
  uint16_t st_mode;    // File type and mode
  uint16_t st_nlink;   // Number of hard links
  uint16_t st_uid;     // User ID of owner
  uint16_t st_gid;     // Group ID of owner
  uint32_t st_rdev;    // Device ID (if special file)
  uint32_t st_size;    // Total size, in bytes
  uint32_t st_blksize; // Block size for filesystem I/O
  uint32_t st_blocks;  // Number of 512-byte blocks allocated
  uint32_t st_atime;   // Time of last access
  uint32_t st_mtime;   // Time of last modification
  uint32_t st_ctime;   // Time of last status change
} vfs_stat_t;

typedef struct {
  char sysname[65];    // Operating system name
  char nodename[65];   // Name within network
  char release[65];    // Operating system release
  char version[65];    // Operating system version
  char machine[65];    // Hardware identifier
  char domainname[65]; // Network domain
} uname_t;

// Prototipos básicos
void syscall_init(void);
void syscall_handler(struct regs *r);

// Funciones de utilidad para syscalls
bool validate_user_pointer(uint32_t ptr, uint32_t size);
int copy_from_user(void *kernel_dst, uint32_t user_src, size_t size);
int copy_to_user(uint32_t user_dst, void *kernel_src, size_t size);
int copy_string_from_user(char *kernel_dst, uint32_t user_src, size_t max_len);
int copy_string_to_user(uint32_t user_dst, const char *kernel_src,
                        size_t max_len);

#endif // SYSCALLS_H