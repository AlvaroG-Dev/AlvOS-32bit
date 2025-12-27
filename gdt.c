// gdt.c - GDT CON SOPORTE RING 3
#define GDT_ENTRIES 6

#include "gdt.h"
#include "kernel.h"
#include "string.h"

// TSS global
struct tss_entry tss;

// GDT global (definitions)
struct gdt_entry gdt[GDT_ENTRIES];
struct gdt_ptr gp;

// Funciones externas en assembly
extern void gdt_flush(uint32_t);
extern void tss_flush(void);

// ============================================================================
// CONFIGURAR ENTRADA DE GDT
// ============================================================================

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;

  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = (limit >> 16) & 0x0F;

  gdt[num].granularity |= gran & 0xF0;
  gdt[num].access = access;
}

// ============================================================================
// INICIALIZAR GDT
// ============================================================================

void gdt_init(void) {
  gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
  gp.base = (uint32_t)&gdt;

  // 0x00: Descriptor nulo
  gdt_set_gate(0, 0, 0, 0, 0);

  // 0x08: Segmento de código del kernel (Ring 0)
  // Base=0, Limit=4GB, Present, Ring 0, Code, Readable, 32-bit
  gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

  // 0x10: Segmento de datos del kernel (Ring 0)
  // Base=0, Limit=4GB, Present, Ring 0, Data, Writable, 32-bit
  gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

  // 0x18: Segmento de código de usuario (Ring 3)
  // Base=0, Limit=4GB, Present, Ring 3, Code, Readable, 32-bit
  // Access: 0xFA = 11111010
  //   Present=1, DPL=11 (Ring 3), Type=1010 (Code, Readable)
  gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

  // 0x20: Segmento de datos de usuario (Ring 3)
  // Base=0, Limit=4GB, Present, Ring 3, Data, Writable, 32-bit
  // Access: 0xF2 = 11110010
  //   Present=1, DPL=11 (Ring 3), Type=0010 (Data, Writable)
  gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

  // 0x28: TSS (Task State Segment)
  // Necesario para cambios de privilegio
  uint32_t tss_base = (uint32_t)&tss;
  uint32_t tss_limit = sizeof(struct tss_entry) - 1;

  // Access: 0x89 = 10001001
  //   Present=1, DPL=00 (Ring 0), Type=1001 (32-bit TSS Available)
  gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x40);

  // Inicializar TSS
  memset(&tss, 0, sizeof(struct tss_entry));

  // Configurar stack del kernel para cuando volvemos de Ring 3
  extern char _stack_top;
  tss.ss0 = 0x10; // Kernel data segment
  tss.esp0 = (uint32_t)&_stack_top;

  // IOMAP base al final del TSS (sin mapa de IO)
  tss.iomap_base = sizeof(struct tss_entry);

  // Cargar GDT
  gdt_flush((uint32_t)&gp);

  // Cargar TSS
  tss_flush();

  terminal_puts(&main_terminal, "GDT initialized with Ring 3 support\r\n");
  terminal_printf(&main_terminal, "  Kernel CS: 0x08, Kernel DS: 0x10\r\n");
  terminal_printf(&main_terminal, "  User CS:   0x1B, User DS:   0x23\r\n");
  terminal_printf(&main_terminal, "  TSS:       0x28 (base: 0x%08x)\r\n",
                  tss_base);
}

// ============================================================================
// ACTUALIZAR ESP0 EN TSS (para cambios de tarea)
// ============================================================================

void tss_set_kernel_stack(uint32_t stack) { tss.esp0 = stack; }

uint32_t tss_get_kernel_stack(void) { return tss.esp0; }