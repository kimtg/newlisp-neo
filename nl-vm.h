/* nl-vm.h - Bytecode Virtual Machine header for newLISP */

#ifndef NL_VM_H
#define NL_VM_H

#include <stdint.h>

/* Bytecode Opcodes */
typedef enum {
    OP_NOP = 0,
    OP_NIL,
    OP_TRUE,
    OP_CONST,           /* operand: uint16_t const_idx */
    OP_POP,
    OP_DUP,
    
    OP_LOAD_LOCAL,      /* operand: uint16_t slot */
    OP_STORE_LOCAL,     /* operand: uint16_t slot */
    
    OP_LOAD_GLOBAL,     /* operand: uint16_t sym_idx */
    OP_STORE_GLOBAL,    /* operand: uint16_t sym_idx */
    OP_LOAD_SELF,       /* push current lambda cell */
    
    OP_JUMP,            /* operand: int16_t offset */
    OP_JUMP_IF_NIL,     /* operand: int16_t offset */
    OP_JUMP_IF_NOT_NIL, /* operand: int16_t offset */
    OP_RET,
    
    OP_LT,              /* < */
    OP_GT,              /* > */
    OP_LE,              /* <= */
    OP_GE,              /* >= */
    OP_EQ,              /* = */
    OP_NE,              /* != */
    
    OP_ADD,             /* + */
    OP_SUB,             /* - */
    OP_MUL,             /* * */
    OP_DIV,             /* / */
    OP_MOD,             /* % */
    OP_NEG,             /* unary - */
    
    OP_CALL,            /* operand: uint8_t argc */
    OP_CALL_SELF,       /* operand: uint8_t argc */
    
    OP_LOAD_LOCAL_0,
    OP_LOAD_LOCAL_1,
    OP_LOAD_LOCAL_2,
    OP_LOAD_LOCAL_3,
    OP_STORE_LOCAL_0,
    OP_STORE_LOCAL_1,
    OP_STORE_LOCAL_2,
    OP_STORE_LOCAL_3,
    OP_CONST_0,
    OP_CONST_1,
    OP_CONST_2,
    OP_SUB_1,
    OP_SUB_2,
    OP_ADD_1
} VM_OPCODE;

/* Forward declarations */
#ifndef NEWLISP_H
#include "newlisp.h"
#endif

#define BYTECODE_MAGIC 0xBEEC0DE0

/* Bytecode Object */
typedef struct BYTECODE_OBJ {
    uint32_t magic;
    uint8_t * code;
    int code_size;
    int code_capacity;
    CELL * * constants;
    int num_constants;
    int constants_capacity;
    SYMBOL * * symbols;
    int num_symbols;
    int symbols_capacity;
    int num_params;
    int num_locals;
    int ref_count;
    CELL * orig_ast;
} BYTECODE_OBJ;

/* VM Stack Frame */
typedef struct VM_FRAME {
    BYTECODE_OBJ * bytecode;
    const uint8_t * ip;
    int fp;
    int argc;
    CELL * self_cell;
} VM_FRAME;

/* VM Global State */
extern CELL * * vm_stack;
extern int vm_sp;
extern VM_FRAME * vm_frames;
extern int vm_frame_count;

/* Functions */
void initBytecodeVM(void);
BYTECODE_OBJ * compileLambda(CELL * lambda, SYMBOL * selfSymbol);
CELL * executeBytecode(CELL * lambdaCell, CELL * args, SYMBOL * newContext);
void freeBytecodeObj(BYTECODE_OBJ * bc);
void gcEvacuateVMRoots(void);

#endif /* NL_VM_H */
