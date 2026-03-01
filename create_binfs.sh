#!/bin/bash

# Script para crear una imagen FAT32 con binarios de usuario
# Esta imagen se montará en /bin al arrancar el sistema

set -e

BINFS_SIZE_MB=50
BINFS_IMG="build/binfs.img"
BINFS_MOUNT="build/binfs_mount"
BIN_DIR="userlib/bin"

# Colores
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
RESET='\033[0m'

echo -e "${CYAN}Creando imagen FAT32 para /bin...${RESET}"

# Crear directorio de montaje temporal
mkdir -p "$BINFS_MOUNT"
mkdir -p "$BIN_DIR"

# Crear imagen vacía
echo -e "${YELLOW}Creando imagen de ${BINFS_SIZE_MB}MB...${RESET}"
dd if=/dev/zero of="$BINFS_IMG" bs=1M count=$BINFS_SIZE_MB 2>/dev/null

# Formatear como FAT32
echo -e "${YELLOW}Formateando como FAT32...${RESET}"
mkfs.vfat -F 32 -n "ALVOSBIN" "$BINFS_IMG" >/dev/null 2>&1 || {
    echo "Error: mkfs.vfat no encontrado. Instala dosfstools."
    exit 1
}

# Compilar binarios primero si existe Makefile
if [ -f "userlib/Makefile" ]; then
    echo -e "${YELLOW}Compilando binarios de usuario...${RESET}"
    (cd userlib && make binaries 2>/dev/null || echo "Advertencia: No se pudieron compilar todos los binarios")
fi

# Copiar archivos a la imagen: primero con mtools (no requiere sudo), luego con mount
COPIED=0
if [ -d "$BIN_DIR" ] && [ -n "$(ls -A $BIN_DIR/*.elf $BIN_DIR/*.tga 2>/dev/null)" ]; then
    if command -v mcopy >/dev/null 2>&1; then
        echo -e "${YELLOW}Copiando archivos con mtools...${RESET}"
        # Copiar binarios ELF (sin extensión)
        for elf in "$BIN_DIR"/*.elf; do
            [ -f "$elf" ] || continue
            name=$(basename "$elf" .elf)
            if mcopy -i "$BINFS_IMG" "$elf" "::$name" 2>/dev/null; then
                echo "  Copiado (elf): $name"
                COPIED=1
            fi
        done
        # Copiar imágenes TGA (con extensión)
        for tga in "$BIN_DIR"/*.tga; do
            [ -f "$tga" ] || continue
            name=$(basename "$tga")
            if mcopy -i "$BINFS_IMG" "$tga" "::$name" 2>/dev/null; then
                echo "  Copiado (tga): $name"
                COPIED=1
            fi
        done
        # Copiar también symlinks.alv si existe
        if [ -f "$BIN_DIR/symlinks.alv" ]; then
             mcopy -i "$BINFS_IMG" "$BIN_DIR/symlinks.alv" "::symlinks.alv" 2>/dev/null && echo "  Copiado: symlinks.alv"
        fi
        [ "$COPIED" = 1 ] && echo -e "${GREEN}Archivos añadidos a la imagen (mtools).${RESET}"
    fi
    if [ "$COPIED" = 0 ] && sudo mount -o loop "$BINFS_IMG" "$BINFS_MOUNT" 2>/dev/null; then
        echo -e "${YELLOW}Copiando archivos montando imagen (sudo)...${RESET}"
        for elf in "$BIN_DIR"/*.elf; do
            [ -f "$elf" ] || continue
            name=$(basename "$elf" .elf)
            sudo cp "$elf" "$BINFS_MOUNT/$name" 2>/dev/null && echo "  Copiado: $name"
        done
        for tga in "$BIN_DIR"/*.tga; do
            [ -f "$tga" ] || continue
            name=$(basename "$tga")
            sudo cp "$tga" "$BINFS_MOUNT/$name" 2>/dev/null && echo "  Copiado: $name"
        done
        sudo umount "$BINFS_MOUNT" 2>/dev/null || true
        COPIED=1
    fi
    if [ "$COPIED" = 0 ]; then
        echo -e "${YELLOW}Instala mtools (apt install mtools) o ejecuta con sudo para incluir archivos en /bin.${RESET}"
    fi
else
    echo -e "${YELLOW}No hay archivos .elf o .tga en $BIN_DIR, imagen vacía.${RESET}"
fi

rmdir "$BINFS_MOUNT" 2>/dev/null || true

echo -e "${GREEN}Imagen creada: $BINFS_IMG${RESET}"
echo -e "${CYAN}Tamaño: $(du -h "$BINFS_IMG" | cut -f1)${RESET}"
