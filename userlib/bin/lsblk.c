#include "syscalls.h"

int _strlen(const char *s) {
    int i = 0;
    while(s[i]) i++;
    return i;
}

void _strcat(char *dest, const char *src) {
    while(*dest) dest++;
    while(*src) {
        *dest++ = *src++;
    }
    *dest = 0;
}

int main(int argc, char **argv) {
    char cmd[256];
    cmd[0] = 0;
    
    // El nombre del comando origen (basename)
    _strcat(cmd, "lsblk");
    
    // Agregar argumentos originales, ignorando argv[0] que es la ruta al binario
    for (int i = 1; i < argc; i++) {
        _strcat(cmd, " ");
        _strcat(cmd, argv[i]);
    }
    
    // En AlvOS este syscall (0x47) ejecutara la logica pesada de Ring 0 en el Kernel
    int res;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(res)
        : "a"(0x47), "b"(cmd)
    );
    
    return res;
}
