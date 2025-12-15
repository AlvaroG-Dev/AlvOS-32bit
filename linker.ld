ENTRY(start)

SECTIONS {
    . = 0x00100000;

    .text : {
        *(.multiboot2)
        *(.text*)
    }
    /* NUEVA sección para page_directory y page_tables */
    .page_tables : {
        *(.page_tables)
    }
    /* Fuerza alineación a 4KB */
    . = ALIGN(4096);
    
    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
        _edata = .;
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    _end = .;

    /* Stack del kernel */
    _stack_bottom = .;
    . = ALIGN(4K);
    . += 8K;
    _stack_top = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}