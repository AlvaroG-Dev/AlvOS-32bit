#!/bin/bash

# Colores para salida
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
RESET='\033[0m'

# Opciones comunes de compilaciÃ³n
GCC=i386-elf-gcc
GCC_OPTS="-m32 -nostdlib -ffreestanding -O2 -U_FORTIFY_SOURCE -fno-builtin-memcpy -fno-builtin-memset -fno-stack-protector"

# Limpiar build anterior
echo -e "${CYAN}Limpiando directorio build...${RESET}"
rm -Rf build/*
mkdir -p build/isodir/boot/grub

# FunciÃ³n para compilar y verificar
compile() {
    echo -e "${YELLOW}Compilando $1...${RESET}"
    $2
    if [ $? -ne 0 ]; then
        echo -e "${RED}Error al compilar $1${RESET}"
        exit 1
    fi
}

# Ensambladores NASM
compile "boot.asm"          "nasm -f elf32 boot.asm -o build/boot.o"
compile "gdt_flush.asm"     "nasm -f elf32 gdt.asm -o build/gdt_flush.o"
compile "idt.asm"           "nasm -f elf32 idt.asm -o build/idt_load.o"
compile "isr.asm"           "nasm -f elf32 isr.asm -o build/isr_asm.o"
compile "irq.asm"           "nasm -f elf32 irq.asm -o build/irq_asm.o"
compile "syscalls.asm"      "nasm -f elf32 syscalls.asm -o build/syscall_asm.o"

# Compilacion C
compile "kernel.c"     "$GCC $GCC_OPTS -c kernel.c -o build/kernel.o"
compile "gdt.c"        "$GCC $GCC_OPTS -c gdt.c -o build/gdt.o"
compile "idt.c"        "$GCC $GCC_OPTS -c idt.c -o build/idt.o"
compile "isr.c"        "$GCC $GCC_OPTS -c isr.c -o build/isr.o"
compile "irq.c"        "$GCC $GCC_OPTS -c irq.c -o build/irq.o"
compile "pmm.c"     "$GCC $GCC_OPTS -c pmm.c -o build/pmm.o"
compile "memory.c"     "$GCC $GCC_OPTS -c memory.c -o build/memory.o"
compile "mmu.c"        "$GCC $GCC_OPTS -c mmu.c -o build/mmu.o"
compile "vmm.c"        "$GCC $GCC_OPTS -c vmm.c -o build/vmm.o"
compile "memutils.c"   "$GCC $GCC_OPTS -c memutils.c -o build/memutils.o"
compile "string.c"     "$GCC $GCC_OPTS -c string.c -o build/string.o"
compile "cpuid.c" "$GCC $GCC_OPTS -c cpuid.c -o build/cpuid.o"
compile "math_utils.c" "$GCC $GCC_OPTS -c math_utils.c -o build/math_utils.o"
compile "keyboard.c"   "$GCC $GCC_OPTS -c keyboard.c -o build/keyboard.o"
compile "drawing.c"    "$GCC $GCC_OPTS -c drawing.c -o build/drawing.o"
compile "terminal.c"   "$GCC $GCC_OPTS -c terminal.c -o build/terminal.o"
compile "disk.c"       "$GCC $GCC_OPTS -c disk.c -o build/disk.o"
compile "disk_io_daemon.c"       "$GCC $GCC_OPTS -c disk_io_daemon.c -o build/disk_io_daemon.o"
compile "vfs.c"        "$GCC $GCC_OPTS -c vfs.c -o build/vfs.o"
compile "tmpfs.c"      "$GCC $GCC_OPTS -c tmpfs.c -o build/tmpfs.o"
compile "fat32.c"      "$GCC $GCC_OPTS -c fat32.c -o build/fat32.o"
compile "task.c"       "$GCC $GCC_OPTS -c task.c -o build/task.o"
compile "task_switch.s" "$GCC $GCC_OPTS -c task_switch.s -o build/task_switch.o"
compile "task_utils.c" "$GCC $GCC_OPTS -c task_utils.c -o build/task_utils.o"
compile "task_test.c"       "$GCC $GCC_OPTS -c task_test.c -o build/task_test.o"
compile "serial.c"     "$GCC $GCC_OPTS -c serial.c -o build/serial.o"
compile "log.c"        "$GCC $GCC_OPTS -c log.c -o build/log.o"
compile "module_loader.c" "$GCC $GCC_OPTS -c module_loader.c -o build/module_loader.o"
compile "driver_system.c" "$GCC $GCC_OPTS -c driver_system.c -o build/driver_system.o"
compile "pci.c" "$GCC $GCC_OPTS -c pci.c -o build/pci.o"
compile "acpi.c" "$GCC $GCC_OPTS -c acpi.c -o build/acpi.o"
compile "dma.c" "$GCC $GCC_OPTS -c dma.c -o build/dma.o"
compile "ahci.c" "$GCC $GCC_OPTS -c ahci.c -o build/ahci.o"
compile "sata_disk.c" "$GCC $GCC_OPTS -c sata_disk.c -o build/sata_disk.o"
compile "atapi.c" "$GCC $GCC_OPTS -c atapi.c -o build/atapi.o"
compile "usb_core.c" "$GCC $GCC_OPTS -c usb_core.c -o build/usb_core.o"
compile "usb_uhci.c" "$GCC $GCC_OPTS -c usb_uhci.c -o build/uhci.o"
compile "usb_ehci.c" "$GCC $GCC_OPTS -c usb_ehci.c -o build/ehci.o"
compile "usb_disk_wrapper.c" "$GCC $GCC_OPTS -c usb_disk_wrapper.c -o build/usb_disk_wrapper.o"
compile "usb_mass_storage.c" "$GCC $GCC_OPTS -c usb_mass_storage.c -o build/usb_mass_storage.o"
compile "usb_commands.c" "$GCC $GCC_OPTS -c usb_commands.c -o build/usb_commands.o"
compile "installer.c" "$GCC $GCC_OPTS -c installer.c -o build/installer.o"
compile "mbr.c" "$GCC $GCC_OPTS -c mbr.c -o build/mbr.o"
compile "partition.c" "$GCC $GCC_OPTS -c partition.c -o build/partition.o"
compile "partition_manager.c" "$GCC $GCC_OPTS -c partition_manager.c -o build/partition_manager.o"
compile "boot_log.c" "$GCC $GCC_OPTS -c boot_log.c -o build/boot_log.o"
compile "mouse.c" "$GCC $GCC_OPTS -c mouse.c -o build/mouse.o"
compile "apic.c" "$GCC $GCC_OPTS -c apic.c -o build/apic.o"
compile "text_editor.c" "$GCC $GCC_OPTS -c text_editor.c -o build/text_editor.o"
compile "mini_parser.c" "$GCC $GCC_OPTS -c mini_parser.c -o build/mini_parser.o"
compile "syscalls.c" "$GCC $GCC_OPTS -c syscalls.c -o build/syscalls.o"
compile "exec.c" "$GCC $GCC_OPTS -c exec.c -o build/exec.o"

# Enlazado con linker.ld
echo -e "${GREEN}Enlazando kernel.bin...${RESET}"
ld -m elf_i386 -T linker.ld -o build/kernel.bin \
    build/boot.o build/kernel.o build/gdt_flush.o build/gdt.o build/idt.o \
    build/isr.o build/idt_load.o build/isr_asm.o build/irq.o build/irq_asm.o build/vmm.o \
    build/pmm.o build/memory.o build/cpuid.o build/mmu.o build/memutils.o build/string.o \
    build/keyboard.o build/drawing.o build/math_utils.o build/terminal.o \
    build/disk.o build/disk_io_daemon.o build/task.o build/task_switch.o build/task_utils.o \
    build/task_test.o build/serial.o build/vfs.o build/tmpfs.o build/fat32.o build/log.o \
    build/module_loader.o build/driver_system.o \
    build/pci.o build/acpi.o build/dma.o build/ahci.o build/sata_disk.o \
    build/atapi.o build/usb_core.o build/usb_disk_wrapper.o \
    build/uhci.o build/usb_mass_storage.o build/ehci.o build/usb_commands.o \
    build/partition.o build/mbr.o build/installer.o build/boot_log.o \
    build/mouse.o build/partition_manager.o build/apic.o build/mini_parser.o \
    build/text_editor.o build/syscalls.o build/syscall_asm.o build/exec.o

if [ $? -ne 0 ]; then
    echo -e "${RED}Error en el enlazado con linker.ld${RESET}"
    exit 1
fi

# IMPORTANTE: Extraer solo los primeros 446 bytes
echo "Generating pre-configured GRUB boot.img..."

cp /usr/lib/grub/i386-pc/boot.img boot.img
# Verificar
BOOT_SIZE=$(stat -c%s boot.img)
echo "boot.img size: $BOOT_SIZE bytes"

# Generar core.img con módulos necesarios
grub-mkimage -O i386-pc -o core.img -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos fat

# Verificar tamaños
BOOT_SIZE=$(stat -c%s boot.img)
CORE_SIZE=$(stat -c%s core.img)

echo "boot.img size: $BOOT_SIZE bytes (must be 446)"
echo "core.img size: $CORE_SIZE bytes"

# Calcular cuántos sectores necesita core.img
CORE_SECTORS=$(( ($CORE_SIZE + 511) / 512 ))
echo "core.img needs: $CORE_SECTORS sectors (max ~62 for typical partition layout)"

if [ "$CORE_SECTORS" -gt 62 ]; then
    echo "WARNING: core.img is very large, may not fit in embedding area"
fi

# Copiar binario y archivo de GRUB
echo -e "${CYAN}Copiando kernel.bin y grub.cfg...${RESET}"
cp build/kernel.bin build/isodir/boot/
cp core.img build/isodir/boot/
cp boot.img build/isodir/boot/mbr_boot.bin
cp grub.cfg build/isodir/boot/grub/

# Crear ISO con GRUB
echo -e "${GREEN}Creando imagen ISO...${RESET}"
grub-mkrescue -o build/os.iso build/isodir -d /usr/lib/grub/i386-pc

if [ $? -ne 0 ]; then
    echo -e "${RED}Error al crear la imagen ISO${RESET}"
    exit 1
fi

echo -e "${BLUE}Creacion de ISO completadas con exito!${RESET}"

echo "Compilando programa de prueba..."

# 1. Primero, compila el objeto
i386-elf-gcc -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
    -O0 -Wall -Wextra -c hello.c -o hello.o

# 2. Luego enlázalo como binario plano
ld -m elf_i386 -Ttext 0x0 --oformat binary -e _start \
    hello.o -o hello.bin

# 3. (OPCIONAL) Verificar el binario resultante
echo "Tamaño del binario: $(stat -c%s hello.bin) bytes"
hexdump -C hello.bin | head -20

#dd if=/dev/zero of=~/osdisk/disk.img bs=1M count=1000
#parted ~/osdisk/disk.img --script mklabel msdos
#parted ~/osdisk/disk.img --script mkpart primary fat32 1MiB 100%
#sudo losetup -fP ~/osdisk/disk.img
#sudo mkfs.vfat -F 32 /dev/loop0p1 -n MYDISK
#sudo losetup -d /dev/loop0

#dd if=/dev/zero of=~/osdisk/disk.img bs=1M count=1000
#sudo mkfs.vfat -F 32 ~/osdisk/disk.img