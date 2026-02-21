# Makefile para compilar aplicaciones de usuario con Newlib en AlvOS

CC = i386-elf-gcc
AS = nasm
LD = i386-elf-ld

# Flags de compilación
# -ffreestanding: Necesario ya que no usamos el entorno de host
# -nostdlib: No usamos las bibliotecas estándar por defecto, las enlazamos manualmente o vía newlib
CFLAGS = -m32 -ffreestanding -O2 -I. -Iinclude
ASFLAGS = -f elf32

# Objetos de soporte (Stubs y Entry Point)
SUPPORT_OBJS = crt0.o syscalls.o

all: libalvos.a

crt0.o: crt0.asm
	$(AS) $(ASFLAGS) crt0.asm -o crt0.o

syscalls.o: syscalls.c
	$(CC) $(CFLAGS) -c syscalls.c -o syscalls.o

# Creamos una biblioteca estática para facilitar el enlazado
libalvos.a: $(SUPPORT_OBJS)
	i386-elf-ar rcs libalvos.a $(SUPPORT_OBJS)

# Programa de prueba de la portación Newlib (modo usuario)
newlib_test.elf: crt0.o syscalls.o newlib_test.o
	$(LD) -m elf_i386 -o newlib_test.elf crt0.o newlib_test.o syscalls.o

newlib_test.o: newlib_test.c
	$(CC) $(CFLAGS) -c newlib_test.c -o newlib_test.o

# Compilar programas en bin/
BIN_DIR = bin
BIN_SOURCES = $(wildcard $(BIN_DIR)/*.c)
BIN_PROGRAMS = $(patsubst $(BIN_DIR)/%.c, $(BIN_DIR)/%.elf, $(BIN_SOURCES))

$(BIN_DIR)/%.elf: $(BIN_DIR)/%.c crt0.o syscalls.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BIN_DIR)/$*.o
	$(LD) -m elf_i386 -o $@ crt0.o $(BIN_DIR)/$*.o syscalls.o
	@echo "Compilado: $@"

# Compilar todos los programas
binaries: $(BIN_PROGRAMS)

clean:
	rm -f *.o *.a newlib_test.elf
	rm -f $(BIN_DIR)/*.o $(BIN_DIR)/*.elf
