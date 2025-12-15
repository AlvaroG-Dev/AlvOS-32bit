#ifndef MINI_PARSER_H
#define MINI_PARSER_H

#include "task.h"
#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

#define MAX_PROGRAM_SIZE    (64 * 1024)  // 64KB máximo por programa
#define MAX_LABELS          256
#define MAX_LINE_LENGTH     256
#define MAX_STACK_SIZE      (8 * 1024)   // 8KB stack para programas

// Tipos de instrucciones soportadas
typedef enum {
    OP_NOP = 0,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_CMP,
    OP_JMP,
    OP_JE,
    OP_JNE,
    OP_JG,
    OP_JL,
    OP_CALL,
    OP_RET,
    OP_PUSH,
    OP_POP,
    OP_PRINT,
    OP_PRINT_INT,
    OP_PRINT_STR,
    OP_READ_INT,
    OP_READ_STR,
    OP_EXIT,
    OP_SLEEP,
    OP_YIELD,
    OP_OPEN,
    OP_READ,
    OP_WRITE,
    OP_CLOSE,
    OP_SEEK
} opcode_t;

// Tipos de operandos
typedef enum {
    OP_TYPE_NONE = 0,
    OP_TYPE_REG,
    OP_TYPE_IMM,
    OP_TYPE_MEM,
    OP_TYPE_LABEL,
    OP_TYPE_STRING
} operand_type_t;

// Registros virtuales
typedef enum {
    REG_A = 0,
    REG_B,
    REG_C,
    REG_D,
    REG_SP,
    REG_PC,
    REG_FLAGS,
    REG_COUNT
} register_t;

// Flags
#define FLAG_ZERO       (1 << 0)
#define FLAG_CARRY      (1 << 1)
#define FLAG_SIGN       (1 << 2)

// Estructura de operando
typedef struct {
    operand_type_t type;
    union {
        int32_t imm_value;        // Valor inmediato
        register_t reg;           // Registro
        uint32_t mem_addr;        // Dirección de memoria
        char* string_literal;     // Literal de string
        char* label_name;         // Nombre de label
    };
} operand_t;

// Estructura de instrucción
typedef struct {
    opcode_t opcode;
    operand_t operands[3];        // Hasta 3 operandos
    uint32_t line_number;
    char* original_line;
} instruction_t;

// Estructura del programa
typedef struct {
    instruction_t* instructions;
    uint32_t instruction_count;
    uint32_t data_size;
    uint8_t* data_section;
    
    // Tabla de labels
    struct {
        char name[64];
        uint32_t instruction_index;
    } labels[MAX_LABELS];
    uint32_t label_count;
    
    // Memoria del programa
    uint8_t* memory;
    uint32_t memory_size;
    
    // Registros virtuales
    int32_t registers[REG_COUNT];
    
    // Stack
    uint8_t* stack;
    uint32_t stack_size;
    
    // Estado
    uint8_t running;
    int32_t exit_code;
    
    // Archivos abiertos
    int open_files[16];
    
    // Entrada/salida
    char output_buffer[1024];
    uint32_t output_pos;
    
} mini_program_t;

// Funciones públicas
int mini_parser_load_file(const char* filename, mini_program_t* program);
int mini_parser_execute(mini_program_t* program);
void mini_parser_cleanup(mini_program_t* program);
task_t* mini_parser_create_task(const char* filename, const char* task_name);
void mini_program_task_wrapper(void* arg);

// Funciones de parsing
int mini_parser_parse_source(const char* source, mini_program_t* program);
int mini_parser_parse_instruction(const char* line, instruction_t* instr);
int mini_parser_parse_operand(const char* token, operand_t* operand);
int mini_parser_read_line(const char** src_ptr, char* line, size_t max_len);
void mini_parser_trim_whitespace(char* str);

// Funciones de ejecución
int mini_parser_execute_instruction(mini_program_t* program, instruction_t* instr);
int32_t mini_parser_get_operand_value(mini_program_t* program, operand_t* op);
int mini_parser_set_operand_value(mini_program_t* program, operand_t* op, int32_t value);
uint32_t mini_parser_resolve_label(mini_program_t* program, operand_t* op);

// Implementaciones de instrucciones
int mini_parser_op_mov(mini_program_t* program, instruction_t* instr);
int mini_parser_op_add(mini_program_t* program, instruction_t* instr);
int mini_parser_op_sub(mini_program_t* program, instruction_t* instr);
int mini_parser_op_mul(mini_program_t* program, instruction_t* instr);
int mini_parser_op_div(mini_program_t* program, instruction_t* instr);
int mini_parser_op_cmp(mini_program_t* program, instruction_t* instr);
int mini_parser_op_jmp(mini_program_t* program, instruction_t* instr);
int mini_parser_op_je(mini_program_t* program, instruction_t* instr);
int mini_parser_op_jne(mini_program_t* program, instruction_t* instr);
int mini_parser_op_call(mini_program_t* program, instruction_t* instr);
int mini_parser_op_ret(mini_program_t* program, instruction_t* instr);
int mini_parser_op_push(mini_program_t* program, instruction_t* instr);
int mini_parser_op_pop(mini_program_t* program, instruction_t* instr);
int mini_parser_op_print(mini_program_t* program, instruction_t* instr);
int mini_parser_op_print_int(mini_program_t* program, instruction_t* instr);
int mini_parser_op_print_str(mini_program_t* program, instruction_t* instr);
int mini_parser_op_exit(mini_program_t* program, instruction_t* instr);
int mini_parser_op_sleep(mini_program_t* program, instruction_t* instr);
int mini_parser_op_jg(mini_program_t* program, instruction_t* instr);  // NUEVO
int mini_parser_op_jl(mini_program_t* program, instruction_t* instr);  // NUEVO

// Funciones de depuración
void mini_parser_disassemble(mini_program_t* program);
void mini_parser_dump_registers(mini_program_t* program);

#endif