;-----------------------------------------------------------
; Archivo: boot.asm
; Versión NASM del código original (.S)
;-----------------------------------------------------------

; Definiciones de macros y constantes

%ifdef HAVE_ASM_USCORE
  %define EXT_C(sym) _##sym
%else
  %define EXT_C(sym) sym
%endif

%define STACK_SIZE        0x4000

; Para el AOUT_KLUDGE: (en ELF se usa 0)
%define AOUT_KLUDGE 0


; Definiciones obtenidas de multiboot2.h (ajusta según tus cabeceras)
%define MULTIBOOT2_HEADER_MAGIC         0xe85250d6
%define GRUB_MULTIBOOT_ARCHITECTURE_I386  0
%define MULTIBOOT_HEADER_TAG_ADDRESS      2
%define MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS 3
%define MULTIBOOT_HEADER_TAG_FRAMEBUFFER   5
%define MULTIBOOT_HEADER_TAG_END           0
%define MULTIBOOT_HEADER_TAG_OPTIONAL      0

;-----------------------------------------------------------
; Sección de código
;-----------------------------------------------------------
section .text
  global start, _start
  extern _edata
  extern _end
  extern EXT_C(cmain)
  extern EXT_C(printf)

start:
_start:
    jmp multiboot_entry

; Alineamos a 8 bytes (para 64 bits)
align 8

;-----------------------------------------------------------
; Encabezado Multiboot2
;-----------------------------------------------------------
multiboot_header:
    ; magic
    dd MULTIBOOT2_HEADER_MAGIC
    ; ISA: i386
    dd GRUB_MULTIBOOT_ARCHITECTURE_I386
    ; Longitud total del header (desde multiboot_header hasta multiboot_header_end)
    dd multiboot_header_end - multiboot_header
    ; Checksum: la suma de (magic + arch + header length + checksum) debe ser 0.
    dd -(MULTIBOOT2_HEADER_MAGIC + GRUB_MULTIBOOT_ARCHITECTURE_I386 + (multiboot_header_end - multiboot_header))

%ifndef __ELF__
address_tag_start:
    dw MULTIBOOT_HEADER_TAG_ADDRESS
    dw MULTIBOOT_HEADER_TAG_OPTIONAL
    dd address_tag_end - address_tag_start
    dd multiboot_header      ; header_addr
    dd _start                ; load_addr
    dd _edata                ; load_end_addr
    dd _end                  ; bss_end_addr
address_tag_end:

entry_address_tag_start:
    dw MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS
    dw MULTIBOOT_HEADER_TAG_OPTIONAL
    dd entry_address_tag_end - entry_address_tag_start
    dd multiboot_entry       ; entry_addr
    dd 0                    ; Padding for alignment
entry_address_tag_end:
%endif

framebuffer_tag_start:
    dw MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw MULTIBOOT_HEADER_TAG_OPTIONAL
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 0                  ; Width
    dd 0                   ; Height
    dd 32                    ; BPP
    dd 0                     ; Padding for alignment
framebuffer_tag_end:
    dw MULTIBOOT_HEADER_TAG_END
    dw 0
    dd 8
multiboot_header_end:

;-----------------------------------------------------------
; Punto de entrada: multiboot_entry
;-----------------------------------------------------------
multiboot_entry:
    ; Inicializa el puntero de pila
    extern _stack_top
    mov esp, _stack_top

    ; Resetea EFLAGS
    push dword 0
    popf

    ; Empuja el puntero a la estructura Multiboot (en EBX)
    push ebx
    ; Empuja el valor mágico (en EAX)
    push eax

    ; Llama a la función principal en C (cmain)
    call EXT_C(cmain)

loop:
    hlt
    jmp loop


;-----------------------------------------------------------
; Sección de datos sin inicializar (pila)
;-----------------------------------------------------------