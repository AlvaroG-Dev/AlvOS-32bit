#include "mini_parser.h"
#include "terminal.h"
#include "memory.h"
#include "string.h"
#include "vfs.h"
#include "task.h"

// Tabla de mnemónicos
static const char* opcode_names[] = {
    "nop", "mov", "add", "sub", "mul", "div", "cmp",
    "jmp", "je", "jne", "jg", "jl", "call", "ret",
    "push", "pop", "print", "print_int", "print_str",
    "read_int", "read_str", "exit", "sleep", "yield",
    "open", "read", "write", "close", "seek"
};

// Declaración externa de la terminal principal
extern Terminal main_terminal;

// ========================================================================
// FUNCIONES DE CREACIÓN Y EJECUCIÓN
// ========================================================================

task_t* mini_parser_create_task(const char* filename, const char* task_name) {
    terminal_printf(&main_terminal, "[MINIPARSER] Loading program: %s\r\n", filename);
    
    // Cargar programa
    mini_program_t* program = (mini_program_t*)kernel_malloc(sizeof(mini_program_t));
    if (!program) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Failed to allocate program\r\n");
        return NULL;
    }
    
    memset(program, 0, sizeof(mini_program_t));
    
    if (mini_parser_load_file(filename, program) != 0) {
        terminal_printf(&main_terminal, "[MINIPARSER] ERROR: Failed to load file %s\r\n", filename);
        kernel_free(program);
        return NULL;
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Program loaded: %u instructions\r\n", 
                   program->instruction_count);
    
    // Crear tarea
    task_t* task = task_create(task_name, mini_program_task_wrapper, program, TASK_PRIORITY_NORMAL);
    if (!task) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Failed to create task\r\n");
        mini_parser_cleanup(program);
        kernel_free(program);
        return NULL;
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Task created: %s (ID: %u)\r\n", 
                   task_name, task->task_id);
    
    return task;
}

void mini_program_task_wrapper(void* arg) {
    mini_program_t* program = (mini_program_t*)arg;
    
    if (!program) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: NULL program in wrapper\r\n");
        return;
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Executing program '%s'\r\n", 
                   CURRENT_TASK() ? CURRENT_TASK()->name : "unknown");
    
    // Ejecutar programa
    int result = mini_parser_execute(program);
    
    // Mostrar resultado
    terminal_printf(&main_terminal, "[MINIPARSER] Program '%s' exited with code: %d\r\n", 
                   CURRENT_TASK() ? CURRENT_TASK()->name : "unknown", result);
    
    // Pequeña pausa para asegurar que el output se muestre
    task_sleep(100);
    
    // Limpiar
    mini_parser_cleanup(program);
    kernel_free(program);
    
    task_exit(result);
}

// ========================================================================
// CARGA Y PARSING
// ========================================================================

int mini_parser_load_file(const char* filename, mini_program_t* program) {
    if (!filename || !program) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Invalid parameters to load_file\r\n");
        return -1;
    }
    
    // Abrir archivo
    int fd = vfs_open(filename, VFS_O_RDONLY);
    if (fd < 0) {
        terminal_printf(&main_terminal, "[MINIPARSER] ERROR: Cannot open file %s\r\n", filename);
        return -1;
    }
    
    // Leer contenido
    char* file_buffer = (char*)kernel_malloc(MAX_PROGRAM_SIZE);
    if (!file_buffer) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Cannot allocate buffer\r\n");
        vfs_close(fd);
        return -1;
    }
    
    int bytes_read = vfs_read(fd, file_buffer, MAX_PROGRAM_SIZE - 1);
    vfs_close(fd);
    
    if (bytes_read <= 0) {
        terminal_printf(&main_terminal, "[MINIPARSER] ERROR: Failed to read file (bytes: %d)\r\n", bytes_read);
        kernel_free(file_buffer);
        return -1;
    }
    
    file_buffer[bytes_read] = '\0';
    
    terminal_printf(&main_terminal, "[MINIPARSER] File loaded: %d bytes\r\n", bytes_read);
    
    // Parsear programa
    int result = mini_parser_parse_source(file_buffer, program);
    
    kernel_free(file_buffer);
    return result;
}

int mini_parser_parse_source(const char* source, mini_program_t* program) {
    if (!source || !program) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Invalid parameters to parse_source\r\n");
        return -1;
    }
    
    char line[MAX_LINE_LENGTH];
    const char* src_ptr = source;
    uint32_t line_num = 0;
    
    // Primera pasada: contar instrucciones y procesar labels
    program->instruction_count = 0;
    program->label_count = 0;
    
    terminal_puts(&main_terminal, "[MINIPARSER] First pass: counting instructions\r\n");
    
    while (*src_ptr && mini_parser_read_line(&src_ptr, line, sizeof(line))) {
        line_num++;
        mini_parser_trim_whitespace(line);
        
        // Saltar líneas vacías y comentarios
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
            continue;
        }
        
        // Procesar labels
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            mini_parser_trim_whitespace(line);
            
            if (program->label_count < MAX_LABELS) {
                strncpy(program->labels[program->label_count].name, 
                       line, sizeof(program->labels[0].name) - 1);
                program->labels[program->label_count].name[sizeof(program->labels[0].name) - 1] = '\0';
                program->labels[program->label_count].instruction_index = program->instruction_count;
                
                terminal_printf(&main_terminal, "[MINIPARSER]   Label: %s -> %u\r\n",
                               program->labels[program->label_count].name,
                               program->instruction_count);
                
                program->label_count++;
            }
            continue;
        }
        
        program->instruction_count++;
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Found %u instructions, %u labels\r\n",
                   program->instruction_count, program->label_count);
    
    if (program->instruction_count == 0) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: No instructions found\r\n");
        return -1;
    }
    
    // Asignar memoria para instrucciones
    program->instructions = (instruction_t*)kernel_malloc(
        program->instruction_count * sizeof(instruction_t));
    if (!program->instructions) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Cannot allocate instructions\r\n");
        return -1;
    }
    
    memset(program->instructions, 0, 
           program->instruction_count * sizeof(instruction_t));
    
    // Segunda pasada: parsear instrucciones
    terminal_puts(&main_terminal, "[MINIPARSER] Second pass: parsing instructions\r\n");
    
    src_ptr = source;
    line_num = 0;
    uint32_t instr_index = 0;
    
    while (*src_ptr && instr_index < program->instruction_count) {
        if (!mini_parser_read_line(&src_ptr, line, sizeof(line))) {
            break;
        }
        
        line_num++;
        mini_parser_trim_whitespace(line);
        
        // Saltar líneas vacías, comentarios y labels
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#' || strchr(line, ':')) {
            continue;
        }
        
        // Parsear instrucción
        if (mini_parser_parse_instruction(line, &program->instructions[instr_index]) != 0) {
            terminal_printf(&main_terminal, "[MINIPARSER] ERROR: Failed to parse line %u: %s\r\n",
                           line_num, line);
            kernel_free(program->instructions);
            program->instructions = NULL;
            return -1;
        }
        
        program->instructions[instr_index].line_number = line_num;
        program->instructions[instr_index].original_line = kernel_malloc(strlen(line) + 1);
        if (program->instructions[instr_index].original_line) {
            strcpy(program->instructions[instr_index].original_line, line);
        }
        
        instr_index++;
    }
    
    program->instruction_count = instr_index;
    
    terminal_printf(&main_terminal, "[MINIPARSER] Parsed %u instructions successfully\r\n", 
                   instr_index);
    
    // Inicializar memoria y registros
    program->memory_size = 64 * 1024; // 64KB
    program->memory = (uint8_t*)kernel_malloc(program->memory_size);
    program->stack_size = MAX_STACK_SIZE;
    program->stack = (uint8_t*)kernel_malloc(program->stack_size);
    
    if (!program->memory || !program->stack) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Cannot allocate memory/stack\r\n");
        if (program->memory) kernel_free(program->memory);
        if (program->stack) kernel_free(program->stack);
        kernel_free(program->instructions);
        return -1;
    }
    
    memset(program->memory, 0, program->memory_size);
    memset(program->stack, 0, program->stack_size);
    memset(program->registers, 0, sizeof(program->registers));
    
    // Inicializar registros
    program->registers[REG_SP] = program->stack_size - 4;
    program->registers[REG_PC] = 0;
    program->registers[REG_FLAGS] = 0;
    
    // Inicializar archivos
    for (int i = 0; i < 16; i++) {
        program->open_files[i] = -1;
    }
    
    program->running = 1;
    program->output_pos = 0;
    
    terminal_puts(&main_terminal, "[MINIPARSER] Program initialized successfully\r\n");
    
    return 0;
}

int mini_parser_parse_instruction(const char* line, instruction_t* instr) {
    if (!line || !instr) {
        return -1;
    }
    
    char line_copy[MAX_LINE_LENGTH];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';
    
    // Tokenizar respetando strings entre comillas
    char* tokens[8];
    int token_count = 0;
    char* token = line_copy;
    char* end = line_copy;
    int in_string = 0;
    
    while (*end && token_count < 8) {
        if (*end == '"') {
            in_string = !in_string; // Toggle string state
        }
        
        // Si no estamos en un string y encontramos un delimitador, o si es el final
        if ((!in_string && (*end == ' ' || *end == '\t' || *end == ',')) || *end == '\0') {
            if (token != end) {
                // Terminar el token actual
                if (*end != '\0') {
                    *end = '\0';
                }
                
                // Solo agregar tokens no vacíos
                if (*token != '\0') {
                    tokens[token_count++] = token;
                }
                
                // Mover al siguiente token
                token = end + 1;
            } else {
                token++;
            }
        }
        end++;
    }
    
    // Agregar el último token si existe
    if (token != end && token_count < 8 && *token != '\0') {
        tokens[token_count++] = token;
    }
    
    if (token_count == 0) {
        return -1;
    }
    
    // Encontrar opcode
    instr->opcode = OP_NOP;
    bool found = false;
    
    for (int i = 0; i < sizeof(opcode_names) / sizeof(opcode_names[0]); i++) {
        if (strcmp(tokens[0], opcode_names[i]) == 0) {
            instr->opcode = (opcode_t)i;
            found = true;
            break;
        }
    }
    
    if (!found) {
        terminal_printf(&main_terminal, "[MINIPARSER] Unknown opcode: %s\r\n", tokens[0]);
        return -1;
    }
    
    // Parsear operandos
    for (int i = 1; i < token_count && i - 1 < 3; i++) {
        if (mini_parser_parse_operand(tokens[i], &instr->operands[i - 1]) != 0) {
            terminal_printf(&main_terminal, "[MINIPARSER] Failed to parse operand: %s\r\n", tokens[i]);
            return -1;
        }
    }
    
    return 0;
}

int mini_parser_parse_operand(const char* token, operand_t* operand) {
    if (!token || !operand) {
        return -1;
    }
    
    memset(operand, 0, sizeof(operand_t));
    
    // Registro
    if (token[0] == 'r' || token[0] == 'R') {
        if (strlen(token) == 2) {
            switch (token[1]) {
                case 'a': case 'A': operand->reg = REG_A; break;
                case 'b': case 'B': operand->reg = REG_B; break;
                case 'c': case 'C': operand->reg = REG_C; break;
                case 'd': case 'D': operand->reg = REG_D; break;
                case 's': case 'S': operand->reg = REG_SP; break;
                case 'p': case 'P': operand->reg = REG_PC; break;
                case 'f': case 'F': operand->reg = REG_FLAGS; break;
                default: return -1;
            }
            operand->type = OP_TYPE_REG;
            return 0;
        }
    }
    
    // String literal (verificar primero para capturar strings completos)
    if (token[0] == '"') {
        operand->type = OP_TYPE_STRING;
        
        // Calcular longitud del string sin las comillas
        const char* str_start = token + 1;
        size_t token_len = strlen(token);
        
        // Si el token termina con comilla, removerla
        if (token_len > 1 && token[token_len - 1] == '"') {
            size_t str_len = token_len - 2; // Sin las dos comillas
            operand->string_literal = kernel_malloc(str_len + 1);
            if (operand->string_literal) {
                strncpy(operand->string_literal, str_start, str_len);
                operand->string_literal[str_len] = '\0';
            }
        } else {
            // String sin cerrar - tomar todo el token después de la primera comilla
            size_t str_len = token_len - 1;
            operand->string_literal = kernel_malloc(str_len + 1);
            if (operand->string_literal) {
                strcpy(operand->string_literal, str_start);
            }
        }
        return 0;
    }
    
    // Número inmediato (incluyendo hexadecimal)
    if ((token[0] >= '0' && token[0] <= '9') || token[0] == '-' || 
        (token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))) {
        operand->type = OP_TYPE_IMM;
        
        if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
            // Hexadecimal
            operand->imm_value = 0;
            for (int i = 2; token[i]; i++) {
                operand->imm_value *= 16;
                if (token[i] >= '0' && token[i] <= '9') {
                    operand->imm_value += token[i] - '0';
                } else if (token[i] >= 'a' && token[i] <= 'f') {
                    operand->imm_value += token[i] - 'a' + 10;
                } else if (token[i] >= 'A' && token[i] <= 'F') {
                    operand->imm_value += token[i] - 'A' + 10;
                }
            }
        } else {
            operand->imm_value = atoi(token);
        }
        return 0;
    }
    
    // Label (cualquier otra cosa que empiece con letra)
    if ((token[0] >= 'a' && token[0] <= 'z') || 
        (token[0] >= 'A' && token[0] <= 'Z') ||
        token[0] == '_') {
        operand->type = OP_TYPE_LABEL;
        operand->label_name = kernel_malloc(strlen(token) + 1);
        if (operand->label_name) {
            strcpy(operand->label_name, token);
        }
        return 0;
    }
    
    return -1;
}

// ========================================================================
// EJECUCIÓN
// ========================================================================

int mini_parser_execute(mini_program_t* program) {
    if (!program || !program->instructions) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Invalid program\r\n");
        return -1;
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Starting execution (%u instructions)\r\n", 
                   program->instruction_count);
    
    program->running = 1;
    program->exit_code = 0;
    uint32_t instruction_count = 0;
    
    while (program->running && 
           program->registers[REG_PC] < program->instruction_count) {
        
        uint32_t pc = program->registers[REG_PC];
        instruction_t* instr = &program->instructions[pc];
        
        // Debug cada 10 instrucciones
        if (instruction_count % 10 == 0 && instruction_count > 0) {
            terminal_printf(&main_terminal, "[MINIPARSER] Executed %u instructions\r\n", 
                           instruction_count);
        }
        
        // Ejecutar instrucción
        if (mini_parser_execute_instruction(program, instr) != 0) {
            terminal_printf(&main_terminal, "[MINIPARSER] Runtime error at line %u: %s\r\n", 
                           instr->line_number, 
                           instr->original_line ? instr->original_line : "unknown");
            program->running = 0;
            program->exit_code = -1;
            break;
        }
        
        instruction_count++;
        
        // Incrementar PC solo si no fue modificado por la instrucción
        if (program->registers[REG_PC] == pc) {
            program->registers[REG_PC]++;
        }
        
        // Yield cada 100 instrucciones para no monopolizar CPU
        if (instruction_count % 100 == 0) {
            task_yield();
        }
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] Execution completed: %u instructions executed\r\n", 
                   instruction_count);
    
    return program->exit_code;
}

int mini_parser_execute_instruction(mini_program_t* program, instruction_t* instr) {
    if (!program || !instr) {
        return -1;
    }
    
    switch (instr->opcode) {
        case OP_NOP:
            break;
            
        case OP_MOV:
            return mini_parser_op_mov(program, instr);
            
        case OP_ADD:
            return mini_parser_op_add(program, instr);
            
        case OP_SUB:
            return mini_parser_op_sub(program, instr);
            
        case OP_MUL:
            return mini_parser_op_mul(program, instr);
            
        case OP_DIV:
            return mini_parser_op_div(program, instr);
            
        case OP_CMP:
            return mini_parser_op_cmp(program, instr);
            
        case OP_JMP:
            return mini_parser_op_jmp(program, instr);
            
        case OP_JE:
            return mini_parser_op_je(program, instr);
            
        case OP_JNE:
            return mini_parser_op_jne(program, instr);
            
        case OP_JG:
            return mini_parser_op_jg(program, instr);
            
        case OP_JL:
            return mini_parser_op_jl(program, instr);
            
        case OP_CALL:
            return mini_parser_op_call(program, instr);
            
        case OP_RET:
            return mini_parser_op_ret(program, instr);
            
        case OP_PUSH:
            return mini_parser_op_push(program, instr);
            
        case OP_POP:
            return mini_parser_op_pop(program, instr);
            
        case OP_PRINT:
            return mini_parser_op_print(program, instr);
            
        case OP_PRINT_INT:
            return mini_parser_op_print_int(program, instr);
            
        case OP_PRINT_STR:
            return mini_parser_op_print_str(program, instr);
            
        case OP_EXIT:
            return mini_parser_op_exit(program, instr);
            
        case OP_SLEEP:
            return mini_parser_op_sleep(program, instr);
            
        case OP_YIELD:
            task_yield();
            break;
            
        default:
            terminal_printf(&main_terminal, "[MINIPARSER] Unsupported instruction: %s\r\n", 
                           opcode_names[instr->opcode]);
            return -1;
    }
    
    return 0;
}

// ========================================================================
// IMPLEMENTACIONES DE INSTRUCCIONES
// ========================================================================

int mini_parser_op_mov(mini_program_t* program, instruction_t* instr) {
    int32_t value = mini_parser_get_operand_value(program, &instr->operands[1]);
    return mini_parser_set_operand_value(program, &instr->operands[0], value);
}

int mini_parser_op_add(mini_program_t* program, instruction_t* instr) {
    int32_t a = mini_parser_get_operand_value(program, &instr->operands[0]);
    int32_t b = mini_parser_get_operand_value(program, &instr->operands[1]);
    int32_t result = a + b;
    
    // Actualizar flags
    program->registers[REG_FLAGS] = 0;
    if (result == 0) program->registers[REG_FLAGS] |= FLAG_ZERO;
    if (result < 0) program->registers[REG_FLAGS] |= FLAG_SIGN;
    
    return mini_parser_set_operand_value(program, &instr->operands[0], result);
}

int mini_parser_op_sub(mini_program_t* program, instruction_t* instr) {
    int32_t a = mini_parser_get_operand_value(program, &instr->operands[0]);
    int32_t b = mini_parser_get_operand_value(program, &instr->operands[1]);
    int32_t result = a - b;
    
    // Actualizar flags
    program->registers[REG_FLAGS] = 0;
    if (result == 0) program->registers[REG_FLAGS] |= FLAG_ZERO;
    if (result < 0) program->registers[REG_FLAGS] |= FLAG_SIGN;
    
    return mini_parser_set_operand_value(program, &instr->operands[0], result);
}

int mini_parser_op_mul(mini_program_t* program, instruction_t* instr) {
    int32_t a = mini_parser_get_operand_value(program, &instr->operands[0]);
    int32_t b = mini_parser_get_operand_value(program, &instr->operands[1]);
    return mini_parser_set_operand_value(program, &instr->operands[0], a * b);
}

int mini_parser_op_div(mini_program_t* program, instruction_t* instr) {
    int32_t a = mini_parser_get_operand_value(program, &instr->operands[0]);
    int32_t b = mini_parser_get_operand_value(program, &instr->operands[1]);
    
    if (b == 0) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Division by zero!\r\n");
        return -1;
    }
    
    return mini_parser_set_operand_value(program, &instr->operands[0], a / b);
}

int mini_parser_op_cmp(mini_program_t* program, instruction_t* instr) {
    int32_t a = mini_parser_get_operand_value(program, &instr->operands[0]);
    int32_t b = mini_parser_get_operand_value(program, &instr->operands[1]);
    int32_t result = a - b;
    
    // Actualizar flags según el resultado
    program->registers[REG_FLAGS] = 0;
    if (result == 0) program->registers[REG_FLAGS] |= FLAG_ZERO;
    if (result < 0) program->registers[REG_FLAGS] |= FLAG_SIGN;
    if (a < b) program->registers[REG_FLAGS] |= FLAG_CARRY;
    
    return 0;
}

int mini_parser_op_jmp(mini_program_t* program, instruction_t* instr) {
    uint32_t target = mini_parser_resolve_label(program, &instr->operands[0]);
    if (target == (uint32_t)-1) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Invalid jump target\r\n");
        return -1;
    }
    program->registers[REG_PC] = target;
    return 0;
}

int mini_parser_op_je(mini_program_t* program, instruction_t* instr) {
    if (program->registers[REG_FLAGS] & FLAG_ZERO) {
        return mini_parser_op_jmp(program, instr);
    }
    return 0;
}

int mini_parser_op_jne(mini_program_t* program, instruction_t* instr) {
    if (!(program->registers[REG_FLAGS] & FLAG_ZERO)) {
        return mini_parser_op_jmp(program, instr);
    }
    return 0;
}

int mini_parser_op_jg(mini_program_t* program, instruction_t* instr) {
    // Jump if greater: !(ZERO) && !(SIGN)
    uint32_t flags = program->registers[REG_FLAGS];
    if (!(flags & FLAG_ZERO) && !(flags & FLAG_SIGN)) {
        return mini_parser_op_jmp(program, instr);
    }
    return 0;
}

int mini_parser_op_jl(mini_program_t* program, instruction_t* instr) {
    // Jump if less: SIGN is set (negative result)
    if (program->registers[REG_FLAGS] & FLAG_SIGN) {
        return mini_parser_op_jmp(program, instr);
    }
    return 0;
}

int mini_parser_op_call(mini_program_t* program, instruction_t* instr) {
    // Verificar stack overflow
    if (program->registers[REG_SP] < 4) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Stack overflow\r\n");
        return -1;
    }
    
    // Push return address
    program->registers[REG_SP] -= 4;
    *(uint32_t*)(program->stack + program->registers[REG_SP]) = program->registers[REG_PC] + 1;
    
    // Jump to target
    return mini_parser_op_jmp(program, instr);
}

int mini_parser_op_ret(mini_program_t* program, instruction_t* instr) {
    (void)instr;
    
    // Verificar stack underflow
    if (program->registers[REG_SP] >= program->stack_size - 4) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Stack underflow\r\n");
        return -1;
    }
    
    // Pop return address
    program->registers[REG_PC] = *(uint32_t*)(program->stack + program->registers[REG_SP]);
    program->registers[REG_SP] += 4;
    return 0;
}

int mini_parser_op_push(mini_program_t* program, instruction_t* instr) {
    if (program->registers[REG_SP] < 4) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Stack overflow\r\n");
        return -1;
    }
    
    int32_t value = mini_parser_get_operand_value(program, &instr->operands[0]);
    program->registers[REG_SP] -= 4;
    *(int32_t*)(program->stack + program->registers[REG_SP]) = value;
    return 0;
}

int mini_parser_op_pop(mini_program_t* program, instruction_t* instr) {
    if (program->registers[REG_SP] >= program->stack_size - 4) {
        terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Stack underflow\r\n");
        return -1;
    }
    
    int32_t value = *(int32_t*)(program->stack + program->registers[REG_SP]);
    program->registers[REG_SP] += 4;
    return mini_parser_set_operand_value(program, &instr->operands[0], value);
}

int mini_parser_op_print(mini_program_t* program, instruction_t* instr) {
    for (int i = 0; i < 3; i++) {
        if (instr->operands[i].type == OP_TYPE_NONE) break;
        
        int32_t value = mini_parser_get_operand_value(program, &instr->operands[i]);
        terminal_printf(&main_terminal, "%d ", value);
    }
    terminal_puts(&main_terminal, "\r\n");
    return 0;
}

int mini_parser_op_print_int(mini_program_t* program, instruction_t* instr) {
    int32_t value = mini_parser_get_operand_value(program, &instr->operands[0]);
    terminal_printf(&main_terminal, "%d\r\n", value);
    return 0;
}

int mini_parser_op_print_str(mini_program_t* program, instruction_t* instr) {
    if (instr->operands[0].type == OP_TYPE_STRING && instr->operands[0].string_literal) {
        terminal_printf(&main_terminal, "%s\r\n", instr->operands[0].string_literal);
    } else if (instr->operands[0].type == OP_TYPE_REG) {
        // Si es un registro, interpretar como dirección de memoria
        uint32_t addr = program->registers[instr->operands[0].reg];
        if (addr < program->memory_size) {
            terminal_printf(&main_terminal, "%s\r\n", (char*)(program->memory + addr));
        }
    }
    return 0;
}

int mini_parser_op_exit(mini_program_t* program, instruction_t* instr) {
    program->exit_code = mini_parser_get_operand_value(program, &instr->operands[0]);
    program->running = 0;
    terminal_printf(&main_terminal, "[MINIPARSER] Program exit with code: %d\r\n", 
                   program->exit_code);
    return 0;
}

int mini_parser_op_sleep(mini_program_t* program, instruction_t* instr) {
    uint32_t ms = mini_parser_get_operand_value(program, &instr->operands[0]);
    task_sleep(ms);
    return 0;
}

// ========================================================================
// FUNCIONES AUXILIARES
// ========================================================================

int32_t mini_parser_get_operand_value(mini_program_t* program, operand_t* op) {
    switch (op->type) {
        case OP_TYPE_IMM:
            return op->imm_value;
            
        case OP_TYPE_REG:
            if (op->reg < REG_COUNT) {
                return program->registers[op->reg];
            }
            return 0;
            
        case OP_TYPE_MEM:
            if (op->mem_addr + 4 <= program->memory_size) {
                return *(int32_t*)(program->memory + op->mem_addr);
            }
            return 0;
            
        case OP_TYPE_STRING:
            // Retornar la dirección del string (no implementado completamente)
            return (int32_t)op->string_literal;
            
        default:
            return 0;
    }
}

int mini_parser_set_operand_value(mini_program_t* program, operand_t* op, int32_t value) {
    switch (op->type) {
        case OP_TYPE_REG:
            if (op->reg < REG_COUNT) {
                program->registers[op->reg] = value;
                return 0;
            }
            return -1;
            
        case OP_TYPE_MEM:
            if (op->mem_addr + 4 <= program->memory_size) {
                *(int32_t*)(program->memory + op->mem_addr) = value;
                return 0;
            }
            return -1;
            
        default:
            terminal_puts(&main_terminal, "[MINIPARSER] ERROR: Cannot write to this operand type\r\n");
            return -1;
    }
}

uint32_t mini_parser_resolve_label(mini_program_t* program, operand_t* op) {
    // Si es un inmediato, usarlo directamente
    if (op->type == OP_TYPE_IMM) {
        return op->imm_value;
    }
    
    // Si es un label, buscarlo
    if (op->type != OP_TYPE_LABEL || !op->label_name) {
        return (uint32_t)-1;
    }
    
    for (uint32_t i = 0; i < program->label_count; i++) {
        if (strcmp(program->labels[i].name, op->label_name) == 0) {
            return program->labels[i].instruction_index;
        }
    }
    
    terminal_printf(&main_terminal, "[MINIPARSER] ERROR: Label not found: %s\r\n", 
                   op->label_name);
    return (uint32_t)-1;
}

// ========================================================================
// LIMPIEZA Y UTILIDADES
// ========================================================================

void mini_parser_cleanup(mini_program_t* program) {
    if (!program) return;
    
    if (program->instructions) {
        for (uint32_t i = 0; i < program->instruction_count; i++) {
            if (program->instructions[i].original_line) {
                kernel_free(program->instructions[i].original_line);
            }
            
            for (int j = 0; j < 3; j++) {
                if (program->instructions[i].operands[j].type == OP_TYPE_STRING &&
                    program->instructions[i].operands[j].string_literal) {
                    kernel_free(program->instructions[i].operands[j].string_literal);
                } else if (program->instructions[i].operands[j].type == OP_TYPE_LABEL &&
                          program->instructions[i].operands[j].label_name) {
                    kernel_free(program->instructions[i].operands[j].label_name);
                }
            }
        }
        kernel_free(program->instructions);
    }
    
    if (program->memory) kernel_free(program->memory);
    if (program->stack) kernel_free(program->stack);
    
    // Cerrar archivos abiertos
    for (int i = 0; i < 16; i++) {
        if (program->open_files[i] >= 0) {
            vfs_close(program->open_files[i]);
        }
    }
}

int mini_parser_read_line(const char** src_ptr, char* line, size_t max_len) {
    if (!src_ptr || !*src_ptr || !line) return 0;
    
    const char* start = *src_ptr;
    const char* end = start;
    
    // Encontrar fin de línea
    while (*end && *end != '\n' && *end != '\r') {
        end++;
    }
    
    // Calcular longitud
    size_t len = end - start;
    if (len >= max_len) {
        len = max_len - 1;
    }
    
    // Copiar línea
    if (len > 0) {
        strncpy(line, start, len);
    }
    line[len] = '\0';
    
    // Avanzar puntero (saltar \r\n o \n)
    *src_ptr = end;
    if (**src_ptr == '\r') (*src_ptr)++;
    if (**src_ptr == '\n') (*src_ptr)++;
    
    return len > 0 || *end != '\0';
}

void mini_parser_trim_whitespace(char* str) {
    if (!str) return;
    
    // Trim leading space
    char* start = str;
    while (*start == ' ' || *start == '\t') start++;
    
    // Si todo es whitespace
    if (*start == '\0') {
        *str = '\0';
        return;
    }
    
    // Mover al inicio
    if (start != str) {
        char* dst = str;
        while (*start) {
            *dst++ = *start++;
        }
        *dst = '\0';
    }
    
    // Trim trailing space
    char* end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
}

// ========================================================================
// FUNCIONES DE DEPURACIÓN
// ========================================================================

void mini_parser_disassemble(mini_program_t* program) {
    if (!program || !program->instructions) {
        terminal_puts(&main_terminal, "[MINIPARSER] No program to disassemble\r\n");
        return;
    }
    
    terminal_puts(&main_terminal, "\r\n=== Program Disassembly ===\r\n");
    terminal_printf(&main_terminal, "Instructions: %u\r\n", program->instruction_count);
    terminal_printf(&main_terminal, "Labels: %u\r\n\r\n", program->label_count);
    
    for (uint32_t i = 0; i < program->instruction_count; i++) {
        instruction_t* instr = &program->instructions[i];
        
        // Mostrar labels
        for (uint32_t j = 0; j < program->label_count; j++) {
            if (program->labels[j].instruction_index == i) {
                terminal_printf(&main_terminal, "%s:\r\n", program->labels[j].name);
            }
        }
        
        terminal_printf(&main_terminal, "  %04u: %s", 
                       i, opcode_names[instr->opcode]);
        
        // Mostrar operandos
        for (int j = 0; j < 3; j++) {
            operand_t* op = &instr->operands[j];
            if (op->type == OP_TYPE_NONE) break;
            
            terminal_puts(&main_terminal, " ");
            
            switch (op->type) {
                case OP_TYPE_REG:
                    terminal_printf(&main_terminal, "r%c", 
                                   "abcdspf"[op->reg]);
                    break;
                case OP_TYPE_IMM:
                    terminal_printf(&main_terminal, "%d", op->imm_value);
                    break;
                case OP_TYPE_STRING:
                    terminal_printf(&main_terminal, "\"%s\"", 
                                   op->string_literal ? op->string_literal : "");
                    break;
                case OP_TYPE_LABEL:
                    terminal_printf(&main_terminal, "%s", 
                                   op->label_name ? op->label_name : "?");
                    break;
                case OP_TYPE_MEM:
                    terminal_printf(&main_terminal, "[0x%x]", op->mem_addr);
                    break;
                default:
                    terminal_puts(&main_terminal, "?");
            }
            
            if (j < 2 && instr->operands[j + 1].type != OP_TYPE_NONE) {
                terminal_puts(&main_terminal, ",");
            }
        }
        
        terminal_puts(&main_terminal, "\r\n");
    }
    
    terminal_puts(&main_terminal, "\r\n");
}

void mini_parser_dump_registers(mini_program_t* program) {
    if (!program) {
        terminal_puts(&main_terminal, "[MINIPARSER] No program\r\n");
        return;
    }
    
    terminal_puts(&main_terminal, "\r\n=== Register Dump ===\r\n");
    terminal_printf(&main_terminal, "RA: 0x%08x (%d)\r\n", 
                   program->registers[REG_A], program->registers[REG_A]);
    terminal_printf(&main_terminal, "RB: 0x%08x (%d)\r\n", 
                   program->registers[REG_B], program->registers[REG_B]);
    terminal_printf(&main_terminal, "RC: 0x%08x (%d)\r\n", 
                   program->registers[REG_C], program->registers[REG_C]);
    terminal_printf(&main_terminal, "RD: 0x%08x (%d)\r\n", 
                   program->registers[REG_D], program->registers[REG_D]);
    terminal_printf(&main_terminal, "SP: 0x%08x\r\n", program->registers[REG_SP]);
    terminal_printf(&main_terminal, "PC: 0x%08x\r\n", program->registers[REG_PC]);
    terminal_printf(&main_terminal, "FLAGS: 0x%08x ", program->registers[REG_FLAGS]);
    
    if (program->registers[REG_FLAGS] & FLAG_ZERO) terminal_puts(&main_terminal, "Z");
    if (program->registers[REG_FLAGS] & FLAG_CARRY) terminal_puts(&main_terminal, "C");
    if (program->registers[REG_FLAGS] & FLAG_SIGN) terminal_puts(&main_terminal, "S");
    
    terminal_puts(&main_terminal, "\r\n\r\n");
}