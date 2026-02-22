import os

commands = [
    "mmap", "clear", "top", "stack-debug", "modules", "apic", 
    "net-init", "ifconfig", "ip", "arp", "ping", "net-stats", 
    "net-diag", "net-loopback", "lookup", "wget", "net-help", 
    "setfg", "setbg", "lspci", "acpi", "reboot", "suspend", 
    "heap", "ticks", "heaptest", "async_read", "async_write", 
    "defrag", "defrag_stats", "disk", "lsblk", "format", 
    "cpuinfo", "cpufreq", "tasks", "task_state", "tstats", 
    "tbuffer", "kill", "yield", "sleep", "scheduler", 
    "start_scheduler", "stop_scheduler", "task_health", 
    "sync-test", "itest", "install", "list_programs", "help_tasks",
    "mounts", "write_test", "read_test", "cd"
]

template = """#include "syscalls.h"

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
    _strcat(cmd, "%%CMD%%");
    
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
"""

for cmd in commands:
    path = f"bin/{cmd}.c"
    # Skip if already exists as a native app like clear.c
    if not os.path.exists(path):
        with open(path, "w") as f:
            f.write(template.replace("%%CMD%%", cmd))
