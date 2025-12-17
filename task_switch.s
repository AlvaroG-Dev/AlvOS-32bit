.section .text
.global task_switch_context
.global task_start_first

# Estructura cpu_context_t offsets
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

# void task_switch_context(cpu_context_t* old_context, cpu_context_t* new_context)
task_switch_context:
    # Al entrar: ESP apunta a [ret_addr][old_ctx][new_ctx]
    
    # PASO 1: Guardar TODOS los registros con PUSHA
    pusha                   # Stack: [regs][ret][old][new]
    
    # PASO 2: Obtener punteros (ahora están desplazados +32 bytes)
    movl 36(%esp), %esi     # ESI = old_context (32 + 4)
    movl 40(%esp), %edi     # EDI = new_context (32 + 8)
    
    # PASO 3: Verificar old_context
    testl %esi, %esi
    jz .restore_context
    
    # ============================================
    # GUARDAR CONTEXTO EN OLD_CONTEXT
    # ============================================
    
    # Guardar registros desde el PUSHA
    movl 28(%esp), %eax     # EAX original
    movl %eax, CTX_EAX(%esi)
    
    movl 24(%esp), %eax     # ECX original
    movl %eax, CTX_ECX(%esi)
    
    movl 20(%esp), %eax     # EDX original
    movl %eax, CTX_EDX(%esi)
    
    movl 16(%esp), %eax     # EBX original
    movl %eax, CTX_EBX(%esi)
    
    movl 8(%esp), %eax      # EBP original
    movl %eax, CTX_EBP(%esi)
    
    movl 4(%esp), %eax      # ESI original
    movl %eax, CTX_ESI(%esi)
    
    movl 0(%esp), %eax      # EDI original
    movl %eax, CTX_EDI(%esi)
    
    # ESP: debe apuntar a ANTES del PUSHA y del CALL
    leal 44(%esp), %eax     # 32 (pusha) + 4 (ret) + 8 (args)
    movl %eax, CTX_ESP(%esi)
    
    # EIP: dirección de retorno
    movl 32(%esp), %eax     # Return address
    movl %eax, CTX_EIP(%esi)
    
    # Guardar segmentos
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
    
    # Guardar EFLAGS
    pushfl
    popl %eax
    movl %eax, CTX_EFLAGS(%esi)

.restore_context:
    testl %edi, %edi
    jz .switch_error
    
    # ✅ FIX: Verificar ESP válido antes de restaurar
    movl CTX_ESP(%edi), %ecx
    cmpl $0x100000, %ecx    # ESP debe ser >= 1MB
    jb .switch_error
    cmpl $0xFFFF0000, %ecx  # ESP debe ser < 4GB-64K
    ja .switch_error
    
    # 1. Restaurar segmentos
    movl CTX_DS(%edi), %eax
    movw %ax, %ds
    movl CTX_ES(%edi), %eax
    movw %ax, %es
    movl CTX_FS(%edi), %eax
    movw %ax, %fs
    movl CTX_GS(%edi), %eax
    movw %ax, %gs
    
    # 2. ✅ CRÍTICO: Cambiar SS:ESP atómicamente
    cli                         # Asegurar que no hay interrupciones
    movl CTX_SS(%edi), %ebx
    movw %bx, %ss
    movl %ecx, %esp             # ECX = CTX_ESP que validamos
    
    # 3. Preparar IRET frame
    pushl CTX_EFLAGS(%edi)
    pushl CTX_CS(%edi)
    pushl CTX_EIP(%edi)
    
    # 4. Restaurar registros
    movl CTX_EAX(%edi), %eax
    movl CTX_EBX(%edi), %ebx
    movl CTX_ECX(%edi), %ecx
    movl CTX_EDX(%edi), %edx
    movl CTX_ESI(%edi), %esi
    movl CTX_EBP(%edi), %ebp
    movl CTX_EDI(%edi), %edi
    
    # 5. ✅ IRET restaura CS:EIP y habilita interrupciones
    iret

.switch_error:
    popa
    ret

# void task_start_first(cpu_context_t* context)
task_start_first:
    cli
    
    movl 4(%esp), %edi      # EDI = context
    testl %edi, %edi
    jz .start_error
    
    # 1. Restaurar segmentos de datos
    movl CTX_DS(%edi), %eax
    movw %ax, %ds
    movl CTX_ES(%edi), %eax
    movw %ax, %es
    movl CTX_FS(%edi), %eax
    movw %ax, %fs
    movl CTX_GS(%edi), %eax
    movw %ax, %gs
    
    # 2. ✅ CORRECCIÓN: Restaurar SS:ESP de manera segura
    movl CTX_ESP(%edi), %ecx
    movl CTX_SS(%edi), %ebx
    movw %bx, %ss
    movl %ecx, %esp
    
    # 3. Preparar IRET: EFLAGS, CS, EIP
    movl CTX_EFLAGS(%edi), %eax
    orl $0x200, %eax        # Asegurar IF=1
    pushl %eax
    pushl CTX_CS(%edi)
    pushl CTX_EIP(%edi)
    
    # 4. Restaurar registros
    movl CTX_EAX(%edi), %eax
    movl CTX_EBX(%edi), %ebx
    movl CTX_ECX(%edi), %ecx
    movl CTX_EDX(%edi), %edx
    movl CTX_ESI(%edi), %esi
    movl CTX_EBP(%edi), %ebp
    
    # 5. EDI al final
    movl CTX_EDI(%edi), %edi
    
    # 6. Saltar con IRET (habilita interrupciones)
    iret

.start_error:
    cli
1:  hlt
    jmp 1b