ENTRY(_start)

SECTIONS
{
    /* Comenzamos en 0 para PIE, ld lo ajustará */
    . = 0x0;

    .text : {
        build/crt0.o(.init)
        *(.init)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
