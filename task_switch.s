# task_switch.s - RING 3 CORREGIDO

.section .text
.global task_switch_context
.global task_start_first
.global task_switch_to_user

# Offsets en cpu_context_t
.set CTX_EAX, 0
.set CTX_EBX, 4
.set CTX_ECX, 8
.set CTX_EDX, 12
.set CTX_ESI, 16
.set CTX_EDI, 20
.set CTX_EBP, 24
.set CTX_ESP, 28
.set CTX_EIP, 32
.set CTX_CS, 36
.set CTX_DS, 40
.set CTX_ES, 44
.set CTX_FS, 48
.set CTX_GS, 52
.set CTX_SS, 56
.set CTX_EFLAGS, 60

# ============================================================================
# void task_switch_context(cpu_context_t* old, cpu_context_t* new)
# ============================================================================
task_switch_context:
    pusha
    
    movl 36(%esp), %esi     # old_context
    movl 40(%esp), %edi     # new_context
    
    testl %esi, %esi
    jz .restore_context
    
    # Guardar contexto anterior
    movl 28(%esp), %eax
    movl %eax, CTX_EAX(%esi)
    movl 24(%esp), %eax
    movl %eax, CTX_ECX(%esi)
    movl 20(%esp), %eax
    movl %eax, CTX_EDX(%esi)
    movl 16(%esp), %eax
    movl %eax, CTX_EBX(%esi)
    movl 8(%esp), %eax
    movl %eax, CTX_EBP(%esi)
    movl 4(%esp), %eax
    movl %eax, CTX_ESI(%esi)
    movl 0(%esp), %eax
    movl %eax, CTX_EDI(%esi)
    
    leal 44(%esp), %eax
    movl %eax, CTX_ESP(%esi)
    
    movl 32(%esp), %eax
    movl %eax, CTX_EIP(%esi)
    
    xorl %eax, %eax
    movw %cs, %ax
    movl %eax, CTX_CS(%esi)
    movw %ds, %ax
    movl %eax, CTX_DS(%esi)
    movw %es, %ax
    movl %eax, CTX_ES(%esi)
    movw %fs, %ax
    movl %eax, CTX_FS(%esi)
    movw %gs, %ax
    movl %eax, CTX_GS(%esi)
    movw %ss, %ax
    movl %eax, CTX_SS(%esi)
    
    pushfl
    popl %eax
    movl %eax, CTX_EFLAGS(%esi)

.restore_context:
    testl %edi, %edi
    jz .switch_error
    
    movl CTX_ESP(%edi), %ecx
    cmpl $0x100000, %ecx
    jb .switch_error
    cmpl $0xFFFF0000, %ecx
    ja .switch_error
    
    movl CTX_DS(%edi), %eax
    movw %ax, %ds
    movl CTX_ES(%edi), %eax
    movw %ax, %es
    movl CTX_FS(%edi), %eax
    movw %ax, %fs
    movl CTX_GS(%edi), %eax
    movw %ax, %gs
    
    cli
    movl CTX_SS(%edi), %ebx
    movw %bx, %ss
    movl %ecx, %esp
    
    pushl CTX_EFLAGS(%edi)
    pushl CTX_CS(%edi)
    pushl CTX_EIP(%edi)
    
    movl CTX_EAX(%edi), %eax
    movl CTX_EBX(%edi), %ebx
    movl CTX_ECX(%edi), %ecx
    movl CTX_EDX(%edi), %edx
    movl CTX_ESI(%edi), %esi
    movl CTX_EBP(%edi), %ebp
    movl CTX_EDI(%edi), %edi
    
    iret

.switch_error:
    popa
    ret

# ============================================================================
# void task_start_first(cpu_context_t* context)
# ============================================================================
task_start_first:
    cli
    
    movl 4(%esp), %edi
    testl %edi, %edi
    jz .start_error
    
    movl CTX_DS(%edi), %eax
    movw %ax, %ds
    movl CTX_ES(%edi), %eax
    movw %ax, %es
    movl CTX_FS(%edi), %eax
    movw %ax, %fs
    movl CTX_GS(%edi), %eax
    movw %ax, %gs
    
    movl CTX_ESP(%edi), %ecx
    movl CTX_SS(%edi), %ebx
    movw %bx, %ss
    movl %ecx, %esp
    
    movl CTX_EFLAGS(%edi), %eax
    orl $0x200, %eax
    pushl %eax
    pushl CTX_CS(%edi)
    pushl CTX_EIP(%edi)
    
    movl CTX_EAX(%edi), %eax
    movl CTX_EBX(%edi), %ebx
    movl CTX_ECX(%edi), %ecx
    movl CTX_EDX(%edi), %edx
    movl CTX_ESI(%edi), %esi
    movl CTX_EBP(%edi), %ebp
    movl CTX_EDI(%edi), %edi
    
    iret

.start_error:
    cli
1:  hlt
    jmp 1b

# ============================================================================
# void task_switch_to_user(cpu_context_t* user_context)
# ============================================================================
task_switch_to_user:
    # Obtener puntero al contexto
    movl 4(%esp), %edi
    testl %edi, %edi
    jz .user_error
    
    # **CRÍTICO**: Cargar segmentos de datos ANTES del IRET
    # IRET no carga DS/ES/FS/GS, así que debemos hacerlo nosotros
    movl CTX_DS(%edi), %eax
    movw %ax, %ds
    movl CTX_ES(%edi), %eax
    movw %ax, %es
    movl CTX_FS(%edi), %eax
    movw %ax, %fs
    movl CTX_GS(%edi), %eax
    movw %ax, %gs
    
    # Preparar IRET frame
    pushl CTX_SS(%edi)      # SS de usuario (Ring 3)
    pushl CTX_ESP(%edi)     # ESP de usuario
    
    # EFLAGS con IF=1
    movl CTX_EFLAGS(%edi), %eax
    orl $0x200, %eax        # IF=1
    pushl %eax
    
    pushl CTX_CS(%edi)      # CS de usuario (Ring 3)
    pushl CTX_EIP(%edi)     # EIP de usuario
    
    # Restaurar registros generales
    movl CTX_EAX(%edi), %eax
    movl CTX_EBX(%edi), %ebx
    movl CTX_ECX(%edi), %ecx
    movl CTX_EDX(%edi), %edx
    movl CTX_ESI(%edi), %esi
    movl CTX_EBP(%edi), %ebp
    
    # EDI al final
    movl CTX_EDI(%edi), %edi
    
    # IRET hace la transición a Ring 3
    iret

.user_error:
    cli
1:  hlt
    jmp 1b