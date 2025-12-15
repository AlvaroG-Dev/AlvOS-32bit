#include "gdt.h"
#include "memutils.h"

#define GDT_ENTRIES 7
#define TSS_SIZE 104

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_descriptor;

extern void gdt_flush(uint32_t);
extern void tss_flush();
extern char _stack_top;

// Stack dedicado para double fault (4KB alineado)
__attribute__((aligned(4096))) uint8_t double_fault_stack[4096];

// Stack para modo usuario
__attribute__((aligned(4096))) uint8_t user_mode_stack[4096];

struct tss_entry tss = {0};

static void set_gdt_entry(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

void gdt_init() {
    // Configurar descriptor GDT
    gdt_descriptor.limit = sizeof(struct gdt_entry) * GDT_ENTRIES - 1;
    gdt_descriptor.base = (uint32_t)&gdt;

    // Limpiar toda la GDT primero
    memset(&gdt, 0, sizeof(gdt));

    // 1. Segmento nulo (obligatorio) - ya está en 0 por memset
    set_gdt_entry(0, 0, 0, 0, 0);

    // 2. Segmento de código kernel (0x08) - DPL=0
    // Access: Present=1, DPL=00, S=1, Type=1010 (Code, Execute/Read)
    // Gran: G=1, D/B=1, L=0, AVL=0
    set_gdt_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);

    // 3. Segmento de datos kernel (0x10) - DPL=0
    // Access: Present=1, DPL=00, S=1, Type=0010 (Data, Read/Write)
    // Gran: G=1, D/B=1, L=0, AVL=0
    set_gdt_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

    // 4. Segmento de código usuario (0x18) - DPL=3 (para futuro)
    // Por ahora no se usa, pero dejamos para compatibilidad
    set_gdt_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);

    // 5. Segmento de datos usuario (0x20) - DPL=3 (para futuro)
    // Por ahora no se usa, pero dejamos para compatibilidad
    set_gdt_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);

    // 6. Configurar TSS (0x28)
    uint32_t tss_base = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    
    // Limpiar TSS completamente
    memset(&tss, 0, sizeof(tss));
    
    // Configurar stack kernel para interrupciones
    tss.esp0 = (uint32_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.ss0 = 0x10;  // Segmento de datos kernel
    
    // CRÍTICO: No configurar CS/DS/etc en el TSS
    // Estos se cargarán desde el contexto de la tarea
    // Solo necesitamos SS0 y ESP0 para cambios de privilegio
    
    tss.iomap_base = sizeof(tss);
    
    // TSS Descriptor
    // Access: Present=1, DPL=00, Type=1001 (32-bit TSS Available)
    // Gran: G=0 (byte granularity), otros=0
    set_gdt_entry(5, tss_base, tss_limit, 0x89, 0x40);
    
    // Cargar GDT
    gdt_flush((uint32_t)&gdt_descriptor);
    

    // Cargar TSS
    tss_flush();
    
    // Verificar que los selectores se cargaron correctamente
    uint16_t cs, ds, ss;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    
    if (cs != 0x08 || ds != 0x10 || ss != 0x10) {
        //terminal_printf(&main_terminal, "WARNING: Unexpected segment values!\n");
        while (1)
        {
            __asm__ volatile("hlt");
        }
        
    }
}