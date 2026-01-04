/* hello.c - Versión Limpia y PIC para AlvOS
   - Optimizada para la nueva gestión de FDs del kernel.
   - 100% Position Independent Code.
   - Sin "escudo" de descriptores (el kernel ya lo hace).
*/

__asm__(".code32\n"
        ".text\n"
        ".global _start\n"
        "_start:\n"
        "    # 1. Obtener base real para PIC\n"
        "    call .Lget_pc\n"
        ".Lget_pc:\n"
        "    pop %ebp\n"
        "    sub $.Lget_pc, %ebp\n"

        "    pushal\n"
        "    sub $1100, %esp\n" // Buffer de red en [esp]

        "    # 2. DNS -> google.com\n"
        "    lea msg_dns(%ebp), %ecx\n"
        "    mov $msg_dns_len, %edx\n"
        "    call .Lprint_term\n"
        "    \n"
        "    lea host_str(%ebp), %ebx\n"
        "    lea 1024(%esp), %ecx\n" // Espacio para la IP
        "    mov $0x45, %eax\n"
        "    int $0x80\n"
        "    test %eax, %eax\n"
        "    jnz .Ldns_err\n"

        "    # 3. CONNECT -> Puerto 80\n"
        "    lea msg_conn(%ebp), %ecx\n"
        "    mov $msg_conn_len, %edx\n"
        "    call .Lprint_term\n"
        "    \n"
        "    lea 1024(%esp), %ebx\n"
        "    mov $80, %ecx\n"
        "    mov $0x42, %eax\n"
        "    int $0x80\n"
        "    cmp $0, %eax\n"
        "    jl .Lconn_err\n"
        "    mov %eax, %esi\n" // ESI = socket descriptor (será >= 3)

        "    # 4. OPEN -> /home/goog.txt (WRONLY=2 | CREAT=8 = 10)\n"
        "    lea file_str(%ebp), %ebx\n"
        "    mov $10, %ecx\n"
        "    mov $0x07, %eax\n"
        "    int $0x80\n"
        "    cmp $0, %eax\n"
        "    jl .Lfile_err\n"
        "    mov %eax, %edi\n" // EDI = archivo (será >= 4)

        "    # 5. SEND -> Request HTTP\n"
        "    mov %esi, %ebx\n" // sock
        "    lea req_str(%ebp), %ecx\n"
        "    mov $(req_str_end - req_str), %edx\n"
        "    mov $0x43, %eax\n"
        "    int $0x80\n"

        "    # 6. RECV/WRITE LOOP\n"
        "    lea msg_recv(%ebp), %ecx\n"
        "    mov $msg_recv_len, %edx\n"
        "    call .Lprint_term\n"

        ".Lloop:\n"
        "    mov %esi, %ebx\n" // socket
        "    mov %esp, %ecx\n" // buffer
        "    mov $1024, %edx\n"
        "    mov $0x44, %eax\n"
        "    int $0x80\n"
        "    cmp $0, %eax\n"
        "    jle .Ldone\n"

        "    # Escribir en DISCO (EDI)\n"
        "    mov %eax, %edx\n" // n_bytes
        "    mov %edi, %ebx\n" // file_fd
        "    mov %esp, %ecx\n" // buffer
        "    mov $0x01, %eax\n"
        "    int $0x80\n"

        "    # Feedback en PANTALLA\n"
        "    mov $1, %ebx\n" // stdout
        "    lea dot_str(%ebp), %ecx\n"
        "    mov $1, %edx\n"
        "    mov $0x01, %eax\n"
        "    int $0x80\n"
        "    jmp .Lloop\n"

        ".Ldone:\n"
        "    # 7. Cerrar y Salir\n"
        "    mov %edi, %ebx\n"
        "    mov $0x08, %eax\n"
        "    int $0x80\n"
        "    mov %esi, %ebx\n"
        "    mov $0x08, %eax\n"
        "    int $0x80\n"
        "    lea msg_ok(%ebp), %ecx\n"
        "    mov $msg_ok_len, %edx\n"
        "    call .Lprint_term\n"
        "    jmp .Lexit\n"

        ".Ldns_err:  lea err_dns(%ebp), %ecx; mov $err_dns_len, %edx; call "
        ".Lprint_term; jmp .Lexit\n"
        ".Lconn_err: lea err_conn(%ebp), %ecx; mov $err_conn_len, %edx; call "
        ".Lprint_term; jmp .Lexit\n"
        ".Lfile_err: lea err_file(%ebp), %ecx; mov $err_file_len, %edx; call "
        ".Lprint_term; jmp .Lexit\n"

        ".Lexit:\n"
        "    add $1100, %esp\n"
        "    popal\n"
        "    xor %ebx, %ebx\n"
        "    mov $0x00, %eax\n"
        "    int $0x80\n"

        "    # Auxiliar: Print a terminal (fd 1)\n"
        ".Lprint_term:\n"
        "    pushal\n"
        "    mov $1, %ebx\n"
        "    mov $0x01, %eax\n"
        "    int $0x80\n"
        "    popal\n"
        "    ret\n"

        "host_str: .asciz \"google.com\"\n"
        "file_str: .asciz \"/home/goog.txt\"\n"
        "dot_str:  .ascii \".\"\n"
        "msg_dns:  .ascii \"[HTTP] Resolving...\\r\\n\"\n"
        "msg_dns_len = . - msg_dns\n"
        "msg_conn: .ascii \"[HTTP] Connecting...\\r\\n\"\n"
        "msg_conn_len = . - msg_conn\n"
        "msg_recv: .ascii \"[HTTP] Receiving: \"\n"
        "msg_recv_len = . - msg_recv\n"
        "msg_ok:   .ascii \"\\r\\n[HTTP] Saved to /home/goog.txt\\r\\n\"\n"
        "msg_ok_len = . - msg_ok\n"
        "err_dns:  .ascii \"[ERR] DNS\\r\\n\"\n"
        "err_dns_len = . - err_dns\n"
        "err_conn: .ascii \"[ERR] Connection\\r\\n\"\n"
        "err_conn_len = . - err_conn\n"
        "err_file: .ascii \"[ERR] File Create\\r\\n\"\n"
        "err_file_len = . - err_file\n"
        "req_str:  .ascii \"GET /index.html HTTP/1.0\\r\\nHost: "
        "google.com\\r\\nConnection: close\\r\\n\\r\\n\"\n"
        "req_str_end:\n");