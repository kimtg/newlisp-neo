/* nl-vm.c - Bytecode Virtual Machine and Compiler for newLISP */

#include "newlisp.h"
#include "nl-vm.h"
#include "protos.h"

#define VM_INITIAL_STACK_SIZE  (256 * 1024)
#define VM_INITIAL_FRAMES_SIZE (64 * 1024)

CELL * * vm_stack = NULL;
int vm_sp = 0;
static int vm_stack_capacity = 0;

VM_FRAME * vm_frames = NULL;
int vm_frame_count = 0;
static int vm_frames_capacity = 0;

void initBytecodeVM(void)
{
    if (vm_stack == NULL)
    {
        vm_stack_capacity = VM_INITIAL_STACK_SIZE;
        vm_stack = (CELL * *)calloc(vm_stack_capacity, sizeof(CELL *));
        vm_sp = 0;
    }
    if (vm_frames == NULL)
    {
        vm_frames_capacity = VM_INITIAL_FRAMES_SIZE;
        vm_frames = (VM_FRAME *)calloc(vm_frames_capacity, sizeof(VM_FRAME));
        vm_frame_count = 0;
    }
}

void gcEvacuateVMRoots(void)
{
    if (vm_stack != NULL)
    {
        for (int i = 0; i < vm_sp; i++)
        {
            if (vm_stack[i] != NULL && isInGen0(vm_stack[i]))
            {
                vm_stack[i] = gcEvacuate(vm_stack[i]);
            }
        }
    }
    if (vm_frames != NULL)
    {
        for (int f = 0; f < vm_frame_count; f++)
        {
            if (vm_frames[f].self_cell != NULL && isInGen0(vm_frames[f].self_cell))
            {
                vm_frames[f].self_cell = gcEvacuate(vm_frames[f].self_cell);
            }
        }
    }
}

void freeBytecodeObj(BYTECODE_OBJ * bc)
{
    if (bc == NULL) return;
    if (bc->magic != BYTECODE_MAGIC) return;
    if (--bc->ref_count > 0) return;
    bc->magic = 0;
    if (bc->constants != NULL)
    {
        for (int i = 0; i < bc->num_constants; i++)
        {
            if (bc->constants[i] != NULL && bc->constants[i] != nilCell && bc->constants[i] != trueCell)
                deleteList(bc->constants[i]);
        }
        free(bc->constants);
    }
    if (bc->code != NULL) free(bc->code);
    if (bc->symbols != NULL) free(bc->symbols);
    free(bc);
}

/* -------------------- Compiler ----------------------- */

#define MAX_LOCALS 256

typedef struct {
    SYMBOL * locals[MAX_LOCALS];
    int num_locals;
    int num_params;
    int max_locals;
    SYMBOL * self_symbol;
    
    uint8_t * code;
    int code_size;
    int code_capacity;
    
    CELL * * constants;
    int num_constants;
    int constants_capacity;
    
    SYMBOL * * symbols;
    int num_symbols;
    int symbols_capacity;
    
    int failed;
} Compiler;

static void cleanupCompiler(Compiler * c)
{
    if (c->code != NULL) free(c->code);
    if (c->constants != NULL)
    {
        for (int i = 0; i < c->num_constants; i++)
        {
            if (c->constants[i] != NULL && c->constants[i] != nilCell && c->constants[i] != trueCell)
                deleteList(c->constants[i]);
        }
        free(c->constants);
    }
    if (c->symbols != NULL) free(c->symbols);
}

static CELL * copyToGen1(CELL * cell)
{
    if (cell == NULL || cell == nilCell || cell == trueCell)
        return cell;

    CELL * newCell = allocGen1Cell(cell->type);
    newCell->aux = cell->aux;
    newCell->contents = cell->contents;
    newCell->next = nilCell;

    if (isEnvelope(cell->type))
    {
        if (cell->type == CELL_ARRAY)
        {
            newCell->contents = (UINT)copyArray(cell);
        }
        else
        {
            if (cell->contents != (UINT)nilCell && cell->contents != 0)
            {
                CELL * origList = (CELL *)cell->contents;
                CELL * newList = copyToGen1(origList);
                newCell->contents = (UINT)newList;
                CELL * last = newList;
                while (origList->next != nilCell && origList->next != NULL)
                {
                    origList = origList->next;
                    last->next = copyToGen1(origList);
                    last = last->next;
                }
                last->next = nilCell;
                newCell->aux = (UINT)last;
            }
        }
    }
    else if (cell->type == CELL_STRING)
    {
        newCell->contents = (UINT)allocMemory((UINT)cell->aux);
        memcpy((void *)newCell->contents, (void *)cell->contents, (UINT)cell->aux);
    }
    else if (cell->type == CELL_DYN_SYMBOL)
    {
        size_t len = strlen((char *)cell->contents);
        newCell->contents = (UINT)allocMemory(len + 1);
        memcpy((char *)newCell->contents, (char *)cell->contents, len + 1);
    }
#ifdef BIGINT
    else if (cell->type == CELL_BIGINT)
    {
        size_t len = strlen((char *)cell->contents);
        newCell->contents = (UINT)allocMemory(len + 1);
        memcpy((char *)newCell->contents, (char *)cell->contents, len + 1);
    }
#endif

    return newCell;
}

static void emitByte(Compiler * c, uint8_t b)
{
    if (c->failed) return;
    if (c->code_size >= c->code_capacity)
    {
        int new_cap = (c->code_capacity == 0) ? 64 : c->code_capacity * 2;
        uint8_t * new_code = (uint8_t *)realloc(c->code, new_cap);
        if (!new_code) { c->failed = 1; return; }
        c->code = new_code;
        c->code_capacity = new_cap;
    }
    c->code[c->code_size++] = b;
}

static void emitOp(Compiler * c, VM_OPCODE op)
{
    emitByte(c, (uint8_t)op);
}

static void emitUint16(Compiler * c, uint16_t val)
{
    emitByte(c, (uint8_t)(val & 0xFF));
    emitByte(c, (uint8_t)((val >> 8) & 0xFF));
}

static int emitJump(Compiler * c, VM_OPCODE op)
{
    emitOp(c, op);
    int pos = c->code_size;
    emitUint16(c, 0);
    return pos;
}

static void patchJump(Compiler * c, int jump_pos)
{
    if (c->failed) return;
    int16_t offset = (int16_t)(c->code_size - (jump_pos + 2));
    c->code[jump_pos] = (uint8_t)(offset & 0xFF);
    c->code[jump_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

static uint16_t addConstant(Compiler * c, CELL * val)
{
    if (c->failed) return 0;
    CELL * permanentVal = copyToGen1(val);

    for (int i = 0; i < c->num_constants; i++)
    {
        if (compareCells(c->constants[i], permanentVal) == 0)
        {
            if (permanentVal != nilCell && permanentVal != trueCell)
                deleteList(permanentVal);
            return (uint16_t)i;
        }
    }
    if (c->num_constants >= c->constants_capacity)
    {
        int new_cap = (c->constants_capacity == 0) ? 16 : c->constants_capacity * 2;
        CELL * * new_consts = (CELL * *)realloc(c->constants, new_cap * sizeof(CELL *));
        if (!new_consts) { c->failed = 1; return 0; }
        c->constants = new_consts;
        c->constants_capacity = new_cap;
    }
    uint16_t idx = (uint16_t)c->num_constants++;
    c->constants[idx] = permanentVal;
    return idx;
}

static uint16_t addSymbol(Compiler * c, SYMBOL * sym)
{
    if (c->failed) return 0;
    for (int i = 0; i < c->num_symbols; i++)
    {
        if (c->symbols[i] == sym)
            return (uint16_t)i;
    }
    if (c->num_symbols >= c->symbols_capacity)
    {
        int new_cap = (c->symbols_capacity == 0) ? 16 : c->symbols_capacity * 2;
        SYMBOL * * new_syms = (SYMBOL * *)realloc(c->symbols, new_cap * sizeof(SYMBOL *));
        if (!new_syms) { c->failed = 1; return 0; }
        c->symbols = new_syms;
        c->symbols_capacity = new_cap;
    }
    uint16_t idx = (uint16_t)c->num_symbols++;
    c->symbols[idx] = sym;
    return idx;
}

static int addLocal(Compiler * c, SYMBOL * sym)
{
    if (c->num_locals >= MAX_LOCALS) { c->failed = 1; return 0; }
    int slot = c->num_locals++;
    c->locals[slot] = sym;
    if (c->num_locals > c->max_locals)
        c->max_locals = c->num_locals;
    return slot;
}

static int findLocal(Compiler * c, SYMBOL * sym)
{
    for (int i = c->num_locals - 1; i >= 0; i--)
    {
        if (c->locals[i] == sym)
            return i;
    }
    return -1;
}

static int isSpecialForm(SYMBOL * sym)
{
    if (sym == NULL) return 0;
    if (sym->flags & (SYMBOL_MACRO | SYMBOL_DESTRUCTIVE)) return 1;
    if (sym->contents != 0 && sym->contents != (UINT)nilCell)
    {
        CELL * sc = (CELL *)sym->contents;
        if (sc->type == CELL_FEXPR) return 1;
    }
    const char * name = sym->name;
    if (!name) return 0;
    if (strcmp(name, "letn") == 0 ||
        strcmp(name, "letex") == 0 ||
        strcmp(name, "cond") == 0 || strcmp(name, "case") == 0 ||
        strcmp(name, "catch") == 0 || strcmp(name, "throw") == 0 ||
        strcmp(name, "throw-error") == 0 ||
        strcmp(name, "collect") == 0 || strcmp(name, "amb") == 0 ||
        strcmp(name, "dolist") == 0 ||
        strcmp(name, "dotree") == 0 || strcmp(name, "dostring") == 0 ||
        strcmp(name, "for") == 0 || strcmp(name, "for-all") == 0 ||
        strcmp(name, "do-until") == 0 || strcmp(name, "do-while") == 0 ||
        strcmp(name, "doargs") == 0 ||
        strcmp(name, "setf") == 0 ||
        strcmp(name, "define") == 0 || strcmp(name, "define-macro") == 0 ||
        strcmp(name, "def-new") == 0 || strcmp(name, "new") == 0 ||
        strcmp(name, "quote") == 0 || strcmp(name, "time") == 0 ||
        strcmp(name, "trace") == 0 || strcmp(name, "trace-highlight") == 0 ||
        strcmp(name, "expand") == 0 || strcmp(name, "lambda") == 0 ||
        strcmp(name, "fn") == 0 || strcmp(name, "lambda-macro") == 0 ||
        strcmp(name, "fn-macro") == 0 || strcmp(name, "args") == 0 ||
        strcmp(name, "self") == 0 || strcmp(name, "env") == 0 ||
        strcmp(name, "context") == 0 || strcmp(name, "eval") == 0 ||
        strcmp(name, "and") == 0 || strcmp(name, "or") == 0)
    {
        return 1;
    }
    return 0;
}

static void compileExpr(Compiler * c, CELL * expr);

static void compileExpr(Compiler * c, CELL * expr)
{
    if (c->failed || expr == NULL || expr == nilCell)
    {
        emitOp(c, OP_NIL);
        return;
    }

    switch (expr->type)
    {
        case CELL_NIL:
            emitOp(c, OP_NIL);
            return;

        case CELL_TRUE:
            emitOp(c, OP_TRUE);
            return;

        case CELL_LONG:
        {
            INT val = (INT)expr->contents;
            if (val == 0) emitOp(c, OP_CONST_0);
            else if (val == 1) emitOp(c, OP_CONST_1);
            else if (val == 2) emitOp(c, OP_CONST_2);
            else
            {
                uint16_t idx = addConstant(c, expr);
                emitOp(c, OP_CONST);
                emitUint16(c, idx);
            }
            return;
        }

        case CELL_FLOAT:
        case CELL_STRING:
        {
            uint16_t idx = addConstant(c, expr);
            emitOp(c, OP_CONST);
            emitUint16(c, idx);
            return;
        }

        case CELL_QUOTE:
        {
            uint16_t idx = addConstant(c, (CELL *)expr->contents);
            emitOp(c, OP_CONST);
            emitUint16(c, idx);
            return;
        }

        case CELL_SYMBOL:
        {
            SYMBOL * sym = (SYMBOL *)expr->contents;
            int slot = findLocal(c, sym);
            if (slot >= 0)
            {
                if (slot < 4)
                    emitOp(c, (VM_OPCODE)(OP_LOAD_LOCAL_0 + slot));
                else
                {
                    emitOp(c, OP_LOAD_LOCAL);
                    emitUint16(c, (uint16_t)slot);
                }
            }
            else if (sym == c->self_symbol && c->self_symbol != NULL)
            {
                emitOp(c, OP_LOAD_SELF);
            }
            else
            {
                uint16_t sidx = addSymbol(c, sym);
                emitOp(c, OP_LOAD_GLOBAL);
                emitUint16(c, sidx);
            }
            return;
        }

        case CELL_EXPRESSION:
        {
            CELL * head = (CELL *)expr->contents;
            if (head == nilCell || head == NULL)
            {
                emitOp(c, OP_NIL);
                return;
            }

            if (head->type == CELL_SYMBOL)
            {
                SYMBOL * hsym = (SYMBOL *)head->contents;
                const char * name = hsym->name;

                /* Special form: (if cond then-expr [else-expr]) */
                if (strcmp(name, "if") == 0)
                {
                    CELL * cond = head->next;
                    if (cond == nilCell) { c->failed = 1; return; }
                    CELL * then_expr = cond->next;
                    if (then_expr == nilCell) { c->failed = 1; return; }
                    CELL * else_expr = then_expr->next;

                    compileExpr(c, cond);
                    int else_jump = emitJump(c, OP_JUMP_IF_NIL);
                    compileExpr(c, then_expr);

                    if (else_expr != nilCell)
                    {
                        int end_jump = emitJump(c, OP_JUMP);
                        patchJump(c, else_jump);
                        compileExpr(c, else_expr);
                        patchJump(c, end_jump);
                    }
                    else
                    {
                        int end_jump = emitJump(c, OP_JUMP);
                        patchJump(c, else_jump);
                        emitOp(c, OP_NIL);
                        patchJump(c, end_jump);
                    }
                    return;
                }

                /* Special form: (when cond body...) */
                if (strcmp(name, "when") == 0)
                {
                    CELL * cond = head->next;
                    if (cond == nilCell) { c->failed = 1; return; }
                    CELL * body = cond->next;

                    compileExpr(c, cond);
                    int else_jump = emitJump(c, OP_JUMP_IF_NIL);

                    if (body == nilCell)
                    {
                        emitOp(c, OP_NIL);
                    }
                    else
                    {
                        while (body != nilCell)
                        {
                            compileExpr(c, body);
                            if (body->next != nilCell)
                                emitOp(c, OP_POP);
                            body = body->next;
                        }
                    }
                    int end_jump = emitJump(c, OP_JUMP);
                    patchJump(c, else_jump);
                    emitOp(c, OP_NIL);
                    patchJump(c, end_jump);
                    return;
                }

                /* Special form: (begin expr...) */
                if (strcmp(name, "begin") == 0)
                {
                    CELL * body = head->next;
                    if (body == nilCell)
                    {
                        emitOp(c, OP_NIL);
                    }
                    else
                    {
                        while (body != nilCell)
                        {
                            compileExpr(c, body);
                            if (body->next != nilCell)
                                emitOp(c, OP_POP);
                            body = body->next;
                        }
                    }
                    return;
                }

                /* Special form: (while cond body...) */
                if (strcmp(name, "while") == 0)
                {
                    CELL * cond = head->next;
                    if (cond == nilCell) { c->failed = 1; return; }
                    CELL * body = cond->next;

                    int loop_start = c->code_size;
                    compileExpr(c, cond);
                    int exit_jump = emitJump(c, OP_JUMP_IF_NIL);

                    while (body != nilCell)
                    {
                        compileExpr(c, body);
                        emitOp(c, OP_POP);
                        body = body->next;
                    }

                    int back_jump = emitJump(c, OP_JUMP);
                    int16_t rel = (int16_t)(loop_start - (back_jump + 2));
                    c->code[back_jump] = (uint8_t)(rel & 0xFF);
                    c->code[back_jump + 1] = (uint8_t)((rel >> 8) & 0xFF);

                    patchJump(c, exit_jump);
                    emitOp(c, OP_NIL);
                    return;
                }

                /* Special form: (let (var1 val1 ...) body...) or (let ((var1 val1) ...) body...) */
                if (strcmp(name, "let") == 0)
                {
                    CELL * bindings = head->next;
                    if (bindings == nilCell) { c->failed = 1; return; }
                    CELL * body = bindings->next;
                    int old_locals = c->num_locals;

                    if (bindings->type == CELL_EXPRESSION)
                    {
                        CELL * b = (CELL *)bindings->contents;
                        while (b != nilCell && b != NULL)
                        {
                            SYMBOL * var_sym = NULL;
                            CELL * init_val = nilCell;

                            if (b->type == CELL_EXPRESSION)
                            {
                                CELL * pair = (CELL *)b->contents;
                                if (pair == nilCell || pair->type != CELL_SYMBOL) { c->failed = 1; return; }
                                var_sym = (SYMBOL *)pair->contents;
                                if (pair->next != nilCell)
                                    init_val = pair->next;
                                b = b->next;
                            }
                            else if (b->type == CELL_SYMBOL)
                            {
                                var_sym = (SYMBOL *)b->contents;
                                b = b->next;
                                if (b != nilCell)
                                {
                                    init_val = b;
                                    b = b->next;
                                }
                            }
                            else
                            {
                                c->failed = 1;
                                return;
                            }

                            int slot = addLocal(c, var_sym);
                            compileExpr(c, init_val);
                            if (slot < 4)
                                emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + slot));
                            else
                            {
                                emitOp(c, OP_STORE_LOCAL);
                                emitUint16(c, (uint16_t)slot);
                            }
                        }
                    }
                    else
                    {
                        c->failed = 1;
                        return;
                    }

                    if (body == nilCell)
                        emitOp(c, OP_NIL);
                    else
                    {
                        while (body != nilCell)
                        {
                            compileExpr(c, body);
                            if (body->next != nilCell)
                                emitOp(c, OP_POP);
                            body = body->next;
                        }
                    }

                    c->num_locals = old_locals;
                    return;
                }

                /* Special form: (local (v1 v2 ...) body...) */
                if (strcmp(name, "local") == 0)
                {
                    CELL * vars = head->next;
                    if (vars == nilCell || vars->type != CELL_EXPRESSION) { c->failed = 1; return; }
                    CELL * body = vars->next;
                    int old_locals = c->num_locals;

                    CELL * v = (CELL *)vars->contents;
                    while (v != nilCell && v != NULL)
                    {
                        if (v->type != CELL_SYMBOL) { c->failed = 1; return; }
                        SYMBOL * var_sym = (SYMBOL *)v->contents;
                        int slot = addLocal(c, var_sym);
                        emitOp(c, OP_NIL);
                        if (slot < 4)
                            emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + slot));
                        else
                        {
                            emitOp(c, OP_STORE_LOCAL);
                            emitUint16(c, (uint16_t)slot);
                        }
                        v = v->next;
                    }

                    if (body == nilCell)
                        emitOp(c, OP_NIL);
                    else
                    {
                        while (body != nilCell)
                        {
                            compileExpr(c, body);
                            if (body->next != nilCell)
                                emitOp(c, OP_POP);
                            body = body->next;
                        }
                    }

                    c->num_locals = old_locals;
                    return;
                }

                /* Special form: (dotimes (var count) body...) */
                if (strcmp(name, "dotimes") == 0)
                {
                    CELL * header = head->next;
                    if (header == nilCell || header->type != CELL_EXPRESSION) { c->failed = 1; return; }
                    CELL * pair = (CELL *)header->contents;
                    if (pair == nilCell || pair->type != CELL_SYMBOL) { c->failed = 1; return; }
                    SYMBOL * var_sym = (SYMBOL *)pair->contents;
                    CELL * count_expr = pair->next;
                    if (count_expr == nilCell) { c->failed = 1; return; }
                    CELL * body = header->next;

                    int old_locals = c->num_locals;

                    int count_slot = addLocal(c, NULL);
                    compileExpr(c, count_expr);
                    if (count_slot < 4) emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + count_slot));
                    else { emitOp(c, OP_STORE_LOCAL); emitUint16(c, (uint16_t)count_slot); }

                    int var_slot = addLocal(c, var_sym);
                    emitOp(c, OP_CONST_0);
                    if (var_slot < 4) emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + var_slot));
                    else { emitOp(c, OP_STORE_LOCAL); emitUint16(c, (uint16_t)var_slot); }

                    int loop_start = c->code_size;
                    if (var_slot < 4) emitOp(c, (VM_OPCODE)(OP_LOAD_LOCAL_0 + var_slot));
                    else { emitOp(c, OP_LOAD_LOCAL); emitUint16(c, (uint16_t)var_slot); }

                    if (count_slot < 4) emitOp(c, (VM_OPCODE)(OP_LOAD_LOCAL_0 + count_slot));
                    else { emitOp(c, OP_LOAD_LOCAL); emitUint16(c, (uint16_t)count_slot); }

                    emitOp(c, OP_LT);
                    int exit_jump = emitJump(c, OP_JUMP_IF_NIL);

                    if (body == nilCell)
                    {
                        emitOp(c, OP_NIL);
                        emitOp(c, OP_POP);
                    }
                    else
                    {
                        while (body != nilCell)
                        {
                            compileExpr(c, body);
                            emitOp(c, OP_POP);
                            body = body->next;
                        }
                    }

                    if (var_slot < 4) emitOp(c, (VM_OPCODE)(OP_LOAD_LOCAL_0 + var_slot));
                    else { emitOp(c, OP_LOAD_LOCAL); emitUint16(c, (uint16_t)var_slot); }
                    emitOp(c, OP_ADD_1);
                    if (var_slot < 4) emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + var_slot));
                    else { emitOp(c, OP_STORE_LOCAL); emitUint16(c, (uint16_t)var_slot); }

                    int back_jump = emitJump(c, OP_JUMP);
                    int16_t rel = (int16_t)(loop_start - (back_jump + 2));
                    c->code[back_jump] = (uint8_t)(rel & 0xFF);
                    c->code[back_jump + 1] = (uint8_t)((rel >> 8) & 0xFF);

                    patchJump(c, exit_jump);
                    emitOp(c, OP_NIL);

                    c->num_locals = old_locals;
                    return;
                }

                /* Assignment: (setq var val ...) or (set 'var val ...) */
                if (strcmp(name, "setq") == 0 || strcmp(name, "set") == 0)
                {
                    int is_setq = (strcmp(name, "setq") == 0);
                    CELL * pair = head->next;
                    if (pair == nilCell) { c->failed = 1; return; }

                    while (pair != nilCell && pair != NULL)
                    {
                        SYMBOL * sym = NULL;
                        if (is_setq)
                        {
                            if (pair->type != CELL_SYMBOL) { c->failed = 1; return; }
                            sym = (SYMBOL *)pair->contents;
                        }
                        else
                        {
                            if (pair->type == CELL_QUOTE)
                            {
                                CELL * q = (CELL *)pair->contents;
                                if (q->type != CELL_SYMBOL) { c->failed = 1; return; }
                                sym = (SYMBOL *)q->contents;
                            }
                            else
                            {
                                c->failed = 1;
                                return;
                            }
                        }
                        pair = pair->next;
                        if (pair == nilCell) { c->failed = 1; return; }
                        CELL * val_expr = pair;
                        pair = pair->next;

                        int slot = findLocal(c, sym);
                        compileExpr(c, val_expr);
                        emitOp(c, OP_DUP);
                        if (slot >= 0)
                        {
                            if (slot < 4)
                                emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + slot));
                            else
                            {
                                emitOp(c, OP_STORE_LOCAL);
                                emitUint16(c, (uint16_t)slot);
                            }
                        }
                        else
                        {
                            uint16_t sidx = addSymbol(c, sym);
                            emitOp(c, OP_STORE_GLOBAL);
                            emitUint16(c, sidx);
                        }
                        if (pair != nilCell)
                            emitOp(c, OP_POP);
                    }
                    return;
                }

                /* Increment / Decrement: (inc var [delta]) or (dec var [delta]) */
                if (strcmp(name, "inc") == 0 || strcmp(name, "dec") == 0)
                {
                    int is_inc = (strcmp(name, "inc") == 0);
                    CELL * arg1 = head->next;
                    if (arg1 == nilCell) { c->failed = 1; return; }
                    SYMBOL * sym = NULL;
                    if (arg1->type == CELL_SYMBOL)
                        sym = (SYMBOL *)arg1->contents;
                    else if (arg1->type == CELL_QUOTE && ((CELL *)arg1->contents)->type == CELL_SYMBOL)
                        sym = (SYMBOL *)((CELL *)arg1->contents)->contents;
                    else
                    {
                        c->failed = 1;
                        return;
                    }

                    CELL * delta = arg1->next;
                    int slot = findLocal(c, sym);
                    if (slot >= 0)
                    {
                        if (slot < 4)
                            emitOp(c, (VM_OPCODE)(OP_LOAD_LOCAL_0 + slot));
                        else
                        {
                            emitOp(c, OP_LOAD_LOCAL);
                            emitUint16(c, (uint16_t)slot);
                        }

                        if (delta == nilCell)
                        {
                            emitOp(c, is_inc ? OP_ADD_1 : OP_SUB_1);
                        }
                        else
                        {
                            compileExpr(c, delta);
                            emitOp(c, is_inc ? OP_ADD : OP_SUB);
                        }

                        emitOp(c, OP_DUP);
                        if (slot < 4)
                            emitOp(c, (VM_OPCODE)(OP_STORE_LOCAL_0 + slot));
                        else
                        {
                            emitOp(c, OP_STORE_LOCAL);
                            emitUint16(c, (uint16_t)slot);
                        }
                        return;
                    }
                    else
                    {
                        uint16_t sidx = addSymbol(c, sym);
                        emitOp(c, OP_LOAD_GLOBAL);
                        emitUint16(c, sidx);
                        if (delta == nilCell)
                        {
                            emitOp(c, is_inc ? OP_ADD_1 : OP_SUB_1);
                        }
                        else
                        {
                            compileExpr(c, delta);
                            emitOp(c, is_inc ? OP_ADD : OP_SUB);
                        }
                        emitOp(c, OP_DUP);
                        emitOp(c, OP_STORE_GLOBAL);
                        emitUint16(c, sidx);
                        return;
                    }
                }

                /* Comparison operators */
                if (strcmp(name, "<") == 0 || strcmp(name, ">") == 0 ||
                    strcmp(name, "<=") == 0 || strcmp(name, ">=") == 0 ||
                    strcmp(name, "=") == 0 || strcmp(name, "!=") == 0)
                {
                    CELL * arg1 = head->next;
                    if (arg1 == nilCell) { c->failed = 1; return; }
                    CELL * arg2 = arg1->next;
                    if (arg2 == nilCell || arg2->next != nilCell) { c->failed = 1; return; }

                    compileExpr(c, arg1);
                    compileExpr(c, arg2);

                    if (strcmp(name, "<") == 0) emitOp(c, OP_LT);
                    else if (strcmp(name, ">") == 0) emitOp(c, OP_GT);
                    else if (strcmp(name, "<=") == 0) emitOp(c, OP_LE);
                    else if (strcmp(name, ">=") == 0) emitOp(c, OP_GE);
                    else if (strcmp(name, "=") == 0) emitOp(c, OP_EQ);
                    else if (strcmp(name, "!=") == 0) emitOp(c, OP_NE);
                    return;
                }

                /* Arithmetic operator: + */
                if (strcmp(name, "+") == 0)
                {
                    CELL * arg = head->next;
                    if (arg == nilCell)
                    {
                        emitOp(c, OP_CONST_0);
                        return;
                    }
                    if (arg->next != nilCell && ((CELL *)arg->next)->next == nilCell)
                    {
                        CELL * arg2 = (CELL *)arg->next;
                        if (arg2->type == CELL_LONG && (INT)arg2->contents == 1)
                        {
                            compileExpr(c, arg);
                            emitOp(c, OP_ADD_1);
                            return;
                        }
                        if (arg->type == CELL_LONG && (INT)arg->contents == 1)
                        {
                            compileExpr(c, arg2);
                            emitOp(c, OP_ADD_1);
                            return;
                        }
                    }
                    compileExpr(c, arg);
                    if (arg->next == nilCell) return;
                    while ((arg = (CELL *)arg->next) != nilCell)
                    {
                        compileExpr(c, arg);
                        emitOp(c, OP_ADD);
                    }
                    return;
                }

                /* Arithmetic operator: - */
                if (strcmp(name, "-") == 0)
                {
                    CELL * arg = head->next;
                    if (arg == nilCell) { c->failed = 1; return; }
                    if (arg->next != nilCell && ((CELL *)arg->next)->next == nilCell)
                    {
                        CELL * arg2 = (CELL *)arg->next;
                        if (arg2->type == CELL_LONG && (INT)arg2->contents == 1)
                        {
                            compileExpr(c, arg);
                            emitOp(c, OP_SUB_1);
                            return;
                        }
                        if (arg2->type == CELL_LONG && (INT)arg2->contents == 2)
                        {
                            compileExpr(c, arg);
                            emitOp(c, OP_SUB_2);
                            return;
                        }
                    }
                    compileExpr(c, arg);
                    if (arg->next == nilCell)
                    {
                        emitOp(c, OP_NEG);
                        return;
                    }
                    while ((arg = (CELL *)arg->next) != nilCell)
                    {
                        compileExpr(c, arg);
                        emitOp(c, OP_SUB);
                    }
                    return;
                }

                /* Arithmetic operator: * */
                if (strcmp(name, "*") == 0)
                {
                    CELL * arg = head->next;
                    if (arg == nilCell)
                    {
                        emitOp(c, OP_CONST_1);
                        return;
                    }
                    compileExpr(c, arg);
                    if (arg->next == nilCell) return;
                    while ((arg = arg->next) != nilCell)
                    {
                        compileExpr(c, arg);
                        emitOp(c, OP_MUL);
                    }
                    return;
                }

                /* Arithmetic operator: / */
                if (strcmp(name, "/") == 0)
                {
                    CELL * arg = head->next;
                    if (arg == nilCell || arg->next == nilCell) { c->failed = 1; return; }
                    compileExpr(c, arg);
                    while ((arg = arg->next) != nilCell)
                    {
                        compileExpr(c, arg);
                        emitOp(c, OP_DIV);
                    }
                    return;
                }

                /* Self-recursive call */
                if (hsym == c->self_symbol && c->self_symbol != NULL)
                {
                    CELL * arg = head->next;
                    int argc = 0;
                    while (arg != nilCell)
                    {
                        compileExpr(c, arg);
                        argc++;
                        arg = arg->next;
                    }
                    emitOp(c, OP_CALL_SELF);
                    emitByte(c, (uint8_t)argc);
                    return;
                }
                if (isSpecialForm(hsym))
                {
                    c->failed = 1;
                    return;
                }
            }

            /* General function invocation */
            compileExpr(c, head);
            CELL * arg = head->next;
            int argc = 0;
            while (arg != nilCell)
            {
                compileExpr(c, arg);
                argc++;
                arg = arg->next;
            }
            emitOp(c, OP_CALL);
            emitByte(c, (uint8_t)argc);
            return;
        }

        default:
            c->failed = 1;
            return;
    }
}

BYTECODE_OBJ * compileLambda(CELL * lambda, SYMBOL * selfSymbol)
{
    if (lambda == NULL || lambda->type != CELL_LAMBDA) return NULL;
    CELL * contents = (CELL *)lambda->contents;
    if (contents == NULL || contents == nilCell || contents->type != CELL_EXPRESSION)
        return NULL;

    Compiler c;
    memset(&c, 0, sizeof(Compiler));
    c.self_symbol = selfSymbol;

    /* 1. Parse parameters */
    CELL * paramList = (CELL *)contents->contents;
    while (paramList != nilCell && paramList != NULL)
    {
        if (paramList->type == CELL_SYMBOL)
        {
            SYMBOL * sym = (SYMBOL *)paramList->contents;
            if (strcmp(sym->name, ",") == 0)
                return NULL; /* Dynamic or local parameters: fallback to tree walker */
            if (c.num_locals >= MAX_LOCALS) return NULL;
            c.locals[c.num_locals++] = sym;
        }
        else
        {
            return NULL; /* Dynamic or default parameters: fallback to tree walker */
        }
        paramList = paramList->next;
    }
    c.num_params = c.num_locals;
    c.max_locals = c.num_locals;

    /* 2. Compile body expressions */
    CELL * body = contents->next;
    if (body == NULL || body == nilCell)
    {
        emitOp(&c, OP_NIL);
        emitOp(&c, OP_RET);
    }
    else
    {
        while (body != nilCell && body != NULL)
        {
            compileExpr(&c, body);
            if (c.failed)
            {
                cleanupCompiler(&c);
                return NULL;
            }
            if (body->next != nilCell)
            {
                emitOp(&c, OP_POP);
            }
            body = body->next;
        }
        emitOp(&c, OP_RET);
    }

    if (c.failed)
    {
        cleanupCompiler(&c);
        return NULL;
    }

    /* 3. Create BYTECODE_OBJ */
    BYTECODE_OBJ * bc = (BYTECODE_OBJ *)calloc(1, sizeof(BYTECODE_OBJ));
    bc->magic = BYTECODE_MAGIC;
    bc->code = c.code;
    bc->code_size = c.code_size;
    bc->code_capacity = c.code_capacity;
    bc->constants = c.constants;
    bc->num_constants = c.num_constants;
    bc->symbols = c.symbols;
    bc->num_symbols = c.num_symbols;
    bc->num_params = c.num_params;
    bc->num_locals = c.max_locals;
    bc->ref_count = 1;
    bc->orig_ast = lambda;

    return bc;
}

/* -------------------- Execution Engine ----------------------- */

#define VM_CHECK_STACK(needed) do { \
    if (__builtin_expect(vm_sp + (needed) >= vm_stack_capacity, 0)) { \
        vm_stack_capacity *= 2; \
        vm_stack = (CELL * *)realloc(vm_stack, vm_stack_capacity * sizeof(CELL *)); \
    } \
} while(0)

#define VM_CHECK_FRAMES() do { \
    if (__builtin_expect(vm_frame_count >= vm_frames_capacity, 0)) { \
        vm_frames_capacity *= 2; \
        vm_frames = (VM_FRAME *)realloc(vm_frames, vm_frames_capacity * sizeof(VM_FRAME)); \
    } \
} while(0)

#if defined(__GNUC__)
#define USE_COMPUTED_GOTO 1
#else
#define USE_COMPUTED_GOTO 0
#endif

CELL * executeBytecode(CELL * lambdaCell, CELL * args, SYMBOL * newContext)
{
    if (__builtin_expect(vm_stack == NULL, 0)) initBytecodeVM();

    BYTECODE_OBJ * bc = (BYTECODE_OBJ *)lambdaCell->aux;
    if (bc == NULL) return nilCell;

    int start_sp = vm_sp;
    int argc = 0;
    UINT * resultIdxSave = resultStackIdx;
    SYMBOL * contextSave = currentContext;
    if (newContext) currentContext = newContext;

    /* Evaluate incoming arguments */
    while (args != nilCell && args != NULL)
    {
        CELL * evaluated;
        if (args->type == CELL_SYMBOL)
        {
            evaluated = copyCell((CELL *)((SYMBOL *)args->contents)->contents);
        }
        else
        {
            evaluated = copyCell(evaluateExpression(args));
        }
        VM_CHECK_STACK(1);
        vm_stack[vm_sp++] = evaluated;
        argc++;
        args = args->next;
    }

    /* Cleanup any temporary evaluation leftovers */
    while (resultStackIdx > resultIdxSave)
        deleteList(popResult());

    /* Pad missing parameters and allocate local variables */
    while (argc < bc->num_params)
    {
        VM_CHECK_STACK(1);
        vm_stack[vm_sp++] = nilCell;
        argc++;
    }
    while (vm_sp < start_sp + bc->num_locals)
    {
        VM_CHECK_STACK(1);
        vm_stack[vm_sp++] = nilCell;
    }

    int base_frame = vm_frame_count;
    VM_CHECK_FRAMES();
    vm_frames[vm_frame_count++] = (VM_FRAME){
        .bytecode = bc,
        .ip = bc->code,
        .fp = start_sp,
        .argc = argc,
        .self_cell = lambdaCell
    };

    register const uint8_t * ip = bc->code;
    register BYTECODE_OBJ * current_bc = bc;
    register CELL * current_self = lambdaCell;
    register int fp = start_sp;

#if USE_COMPUTED_GOTO
    static const void * const dispatch_table[] = {
        [OP_NOP] = &&DO_OP_NOP,
        [OP_NIL] = &&DO_OP_NIL,
        [OP_TRUE] = &&DO_OP_TRUE,
        [OP_CONST] = &&DO_OP_CONST,
        [OP_POP] = &&DO_OP_POP,
        [OP_DUP] = &&DO_OP_DUP,
        [OP_LOAD_LOCAL] = &&DO_OP_LOAD_LOCAL,
        [OP_STORE_LOCAL] = &&DO_OP_STORE_LOCAL,
        [OP_LOAD_GLOBAL] = &&DO_OP_LOAD_GLOBAL,
        [OP_STORE_GLOBAL] = &&DO_OP_STORE_GLOBAL,
        [OP_LOAD_SELF] = &&DO_OP_LOAD_SELF,
        [OP_JUMP] = &&DO_OP_JUMP,
        [OP_JUMP_IF_NIL] = &&DO_OP_JUMP_IF_NIL,
        [OP_JUMP_IF_NOT_NIL] = &&DO_OP_JUMP_IF_NOT_NIL,
        [OP_RET] = &&DO_OP_RET,
        [OP_LT] = &&DO_OP_LT,
        [OP_GT] = &&DO_OP_GT,
        [OP_LE] = &&DO_OP_LE,
        [OP_GE] = &&DO_OP_GE,
        [OP_EQ] = &&DO_OP_EQ,
        [OP_NE] = &&DO_OP_NE,
        [OP_ADD] = &&DO_OP_ADD,
        [OP_SUB] = &&DO_OP_SUB,
        [OP_MUL] = &&DO_OP_MUL,
        [OP_DIV] = &&DO_OP_DIV,
        [OP_MOD] = &&DO_OP_MOD,
        [OP_NEG] = &&DO_OP_NEG,
        [OP_CALL] = &&DO_OP_CALL,
        [OP_CALL_SELF] = &&DO_OP_CALL_SELF,
        [OP_LOAD_LOCAL_0] = &&DO_OP_LOAD_LOCAL_0,
        [OP_LOAD_LOCAL_1] = &&DO_OP_LOAD_LOCAL_1,
        [OP_LOAD_LOCAL_2] = &&DO_OP_LOAD_LOCAL_2,
        [OP_LOAD_LOCAL_3] = &&DO_OP_LOAD_LOCAL_3,
        [OP_STORE_LOCAL_0] = &&DO_OP_STORE_LOCAL_0,
        [OP_STORE_LOCAL_1] = &&DO_OP_STORE_LOCAL_1,
        [OP_STORE_LOCAL_2] = &&DO_OP_STORE_LOCAL_2,
        [OP_STORE_LOCAL_3] = &&DO_OP_STORE_LOCAL_3,
        [OP_CONST_0] = &&DO_OP_CONST_0,
        [OP_CONST_1] = &&DO_OP_CONST_1,
        [OP_CONST_2] = &&DO_OP_CONST_2,
        [OP_SUB_1] = &&DO_OP_SUB_1,
        [OP_SUB_2] = &&DO_OP_SUB_2,
        [OP_ADD_1] = &&DO_OP_ADD_1,
    };
    #define DISPATCH() goto *dispatch_table[*ip++]
    DISPATCH();
#else
    #define DISPATCH() break
    while (1)
    {
        uint8_t op = *ip++;
        switch (op)
        {
#endif

#if USE_COMPUTED_GOTO
            DO_OP_NOP:
#else
            case OP_NOP:
#endif
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_NIL:
#else
            case OP_NIL:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = nilCell;
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_TRUE:
#else
            case OP_TRUE:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = trueCell;
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_CONST:
#else
            case OP_CONST:
#endif
            {
                uint16_t idx = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = current_bc->constants[idx];
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_CONST_0:
#else
            case OP_CONST_0:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = stuffInteger(0);
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_CONST_1:
#else
            case OP_CONST_1:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = stuffInteger(1);
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_CONST_2:
#else
            case OP_CONST_2:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = stuffInteger(2);
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_POP:
#else
            case OP_POP:
#endif
                --vm_sp;
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_DUP:
#else
            case OP_DUP:
#endif
            {
                CELL * c = vm_stack[vm_sp - 1];
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = c;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_LOCAL:
#else
            case OP_LOAD_LOCAL:
#endif
            {
                uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = vm_stack[fp + slot];
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_LOCAL_0:
#else
            case OP_LOAD_LOCAL_0:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = vm_stack[fp + 0];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_LOCAL_1:
#else
            case OP_LOAD_LOCAL_1:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = vm_stack[fp + 1];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_LOCAL_2:
#else
            case OP_LOAD_LOCAL_2:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = vm_stack[fp + 2];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_LOCAL_3:
#else
            case OP_LOAD_LOCAL_3:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = vm_stack[fp + 3];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_STORE_LOCAL:
#else
            case OP_STORE_LOCAL:
#endif
            {
                uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                vm_stack[fp + slot] = vm_stack[--vm_sp];
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_STORE_LOCAL_0:
#else
            case OP_STORE_LOCAL_0:
#endif
                vm_stack[fp + 0] = vm_stack[--vm_sp];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_STORE_LOCAL_1:
#else
            case OP_STORE_LOCAL_1:
#endif
                vm_stack[fp + 1] = vm_stack[--vm_sp];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_STORE_LOCAL_2:
#else
            case OP_STORE_LOCAL_2:
#endif
                vm_stack[fp + 2] = vm_stack[--vm_sp];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_STORE_LOCAL_3:
#else
            case OP_STORE_LOCAL_3:
#endif
                vm_stack[fp + 3] = vm_stack[--vm_sp];
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_GLOBAL:
#else
            case OP_LOAD_GLOBAL:
#endif
            {
                uint16_t idx = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                SYMBOL * s = current_bc->symbols[idx];
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = (CELL *)s->contents;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_STORE_GLOBAL:
#else
            case OP_STORE_GLOBAL:
#endif
            {
                uint16_t idx = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                SYMBOL * s = current_bc->symbols[idx];
                CELL * val = vm_stack[--vm_sp];
                CELL * c = copyCell(val);
                c = gcEvacuate(c);
                deleteList((CELL *)s->contents);
                s->contents = (UINT)c;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_LOAD_SELF:
#else
            case OP_LOAD_SELF:
#endif
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = current_self;
                DISPATCH();

#if USE_COMPUTED_GOTO
            DO_OP_JUMP:
#else
            case OP_JUMP:
#endif
            {
                int16_t offset = (int16_t)(ip[0] | (ip[1] << 8));
                ip += 2 + offset;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_JUMP_IF_NIL:
#else
            case OP_JUMP_IF_NIL:
#endif
            {
                int16_t offset = (int16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                CELL * cond = vm_stack[--vm_sp];
                if (cond == nilCell || cond->type == CELL_NIL)
                {
                    ip += offset;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_JUMP_IF_NOT_NIL:
#else
            case OP_JUMP_IF_NOT_NIL:
#endif
            {
                int16_t offset = (int16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                CELL * cond = vm_stack[--vm_sp];
                if (cond != nilCell && cond->type != CELL_NIL)
                {
                    ip += offset;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_LT:
#else
            case OP_LT:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents < (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) < 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_LE:
#else
            case OP_LE:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents <= (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) <= 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_GT:
#else
            case OP_GT:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents > (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) > 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_GE:
#else
            case OP_GE:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents >= (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) >= 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_EQ:
#else
            case OP_EQ:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents == (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) == 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_NE:
#else
            case OP_NE:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = ((INT)a->contents != (INT)b->contents) ? trueCell : nilCell;
                }
                else
                {
                    vm_stack[vm_sp++] = (compareCells(a, b) != 0) ? trueCell : nilCell;
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_ADD:
#else
            case OP_ADD:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = stuffInteger((INT)a->contents + (INT)b->contents);
                }
                else
                {
                    CELL dummyA, dummyB;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    dummyB.type = b->type; dummyB.contents = b->contents; dummyB.aux = b->aux; dummyB.next = nilCell;
                    vm_stack[vm_sp++] = p_add(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_ADD_1:
#else
            case OP_ADD_1:
#endif
            {
                CELL * a = vm_stack[vm_sp - 1];
                if (__builtin_expect(a->type == CELL_LONG, 1))
                {
                    vm_stack[vm_sp - 1] = stuffInteger((INT)a->contents + 1);
                }
                else
                {
                    CELL dummyB;
                    dummyB.type = CELL_LONG; dummyB.contents = 1; dummyB.aux = (UINT)nilCell; dummyB.next = nilCell;
                    CELL dummyA;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    vm_stack[vm_sp - 1] = p_add(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_SUB:
#else
            case OP_SUB:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = stuffInteger((INT)a->contents - (INT)b->contents);
                }
                else
                {
                    CELL dummyA, dummyB;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    dummyB.type = b->type; dummyB.contents = b->contents; dummyB.aux = b->aux; dummyB.next = nilCell;
                    vm_stack[vm_sp++] = p_subtract(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_SUB_1:
#else
            case OP_SUB_1:
#endif
            {
                CELL * a = vm_stack[vm_sp - 1];
                if (__builtin_expect(a->type == CELL_LONG, 1))
                {
                    vm_stack[vm_sp - 1] = stuffInteger((INT)a->contents - 1);
                }
                else
                {
                    CELL dummyB;
                    dummyB.type = CELL_LONG; dummyB.contents = 1; dummyB.aux = (UINT)nilCell; dummyB.next = nilCell;
                    CELL dummyA;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    vm_stack[vm_sp - 1] = p_subtract(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_SUB_2:
#else
            case OP_SUB_2:
#endif
            {
                CELL * a = vm_stack[vm_sp - 1];
                if (__builtin_expect(a->type == CELL_LONG, 1))
                {
                    vm_stack[vm_sp - 1] = stuffInteger((INT)a->contents - 2);
                }
                else
                {
                    CELL dummyB;
                    dummyB.type = CELL_LONG; dummyB.contents = 2; dummyB.aux = (UINT)nilCell; dummyB.next = nilCell;
                    CELL dummyA;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    vm_stack[vm_sp - 1] = p_subtract(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_MUL:
#else
            case OP_MUL:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    vm_stack[vm_sp++] = stuffInteger((INT)a->contents * (INT)b->contents);
                }
                else
                {
                    CELL dummyA, dummyB;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    dummyB.type = b->type; dummyB.contents = b->contents; dummyB.aux = b->aux; dummyB.next = nilCell;
                    vm_stack[vm_sp++] = p_multiply(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_DIV:
#else
            case OP_DIV:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    if ((INT)b->contents == 0)
                        return errorProc(ERR_MATH);
                    vm_stack[vm_sp++] = stuffInteger((INT)a->contents / (INT)b->contents);
                }
                else
                {
                    CELL dummyA, dummyB;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    dummyB.type = b->type; dummyB.contents = b->contents; dummyB.aux = b->aux; dummyB.next = nilCell;
                    vm_stack[vm_sp++] = p_divide(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_MOD:
#else
            case OP_MOD:
#endif
            {
                CELL * b = vm_stack[--vm_sp];
                CELL * a = vm_stack[--vm_sp];
                if (a->type == CELL_LONG && b->type == CELL_LONG)
                {
                    if ((INT)b->contents == 0)
                        return errorProc(ERR_MATH);
                    vm_stack[vm_sp++] = stuffInteger((INT)a->contents % (INT)b->contents);
                }
                else
                {
                    CELL dummyA, dummyB;
                    dummyA.type = a->type; dummyA.contents = a->contents; dummyA.aux = a->aux; dummyA.next = &dummyB;
                    dummyB.type = b->type; dummyB.contents = b->contents; dummyB.aux = b->aux; dummyB.next = nilCell;
                    vm_stack[vm_sp++] = p_modulo(&dummyA);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_NEG:
#else
            case OP_NEG:
#endif
            {
                CELL * a = vm_stack[vm_sp - 1];
                if (a->type == CELL_LONG)
                {
                    vm_stack[vm_sp - 1] = stuffInteger(-(INT)a->contents);
                }
                else
                {
                    CELL dummy;
                    dummy.type = a->type; dummy.contents = a->contents; dummy.aux = a->aux; dummy.next = nilCell;
                    vm_stack[vm_sp - 1] = p_subtract(&dummy);
                }
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_CALL_SELF:
#else
            case OP_CALL_SELF:
#endif
            {
                uint8_t call_argc = *ip++;
                while (call_argc < current_bc->num_params)
                {
                    VM_CHECK_STACK(1);
                    vm_stack[vm_sp++] = nilCell;
                    call_argc++;
                }
                int new_fp = vm_sp - call_argc;
                while (vm_sp < new_fp + current_bc->num_locals)
                {
                    VM_CHECK_STACK(1);
                    vm_stack[vm_sp++] = nilCell;
                }
                vm_frames[vm_frame_count - 1].ip = ip;
                VM_CHECK_FRAMES();
                vm_frames[vm_frame_count++] = (VM_FRAME){
                    .bytecode = current_bc,
                    .ip = current_bc->code,
                    .fp = new_fp,
                    .argc = call_argc,
                    .self_cell = current_self
                };
                fp = new_fp;
                ip = current_bc->code;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_CALL:
#else
            case OP_CALL:
#endif
            {
                uint8_t call_argc = *ip++;
                int fn_idx = vm_sp - 1 - call_argc;
                CELL * target_fn = vm_stack[fn_idx];

                if (target_fn->type == CELL_LAMBDA)
                {
                    BYTECODE_OBJ * target_bc = (target_fn->aux != 0 && target_fn->aux != (UINT)nilCell) ? (BYTECODE_OBJ *)target_fn->aux : NULL;
                    if (target_bc != NULL && target_bc->magic != BYTECODE_MAGIC)
                        target_bc = NULL;
                    if (target_bc == NULL)
                    {
                        target_bc = compileLambda(target_fn, NULL);
                        if (target_bc != NULL) target_fn->aux = (UINT)target_bc;
                    }

                    if (target_bc != NULL)
                    {
                        for (int i = 0; i < call_argc; i++)
                        {
                            vm_stack[fn_idx + i] = vm_stack[fn_idx + 1 + i];
                        }
                        --vm_sp;
                        while (call_argc < target_bc->num_params)
                        {
                            VM_CHECK_STACK(1);
                            vm_stack[vm_sp++] = nilCell;
                            call_argc++;
                        }
                        while (vm_sp < fn_idx + target_bc->num_locals)
                        {
                            VM_CHECK_STACK(1);
                            vm_stack[vm_sp++] = nilCell;
                        }
                        vm_frames[vm_frame_count - 1].ip = ip;
                        VM_CHECK_FRAMES();
                        vm_frames[vm_frame_count++] = (VM_FRAME){
                            .bytecode = target_bc,
                            .ip = target_bc->code,
                            .fp = fn_idx,
                            .argc = call_argc,
                            .self_cell = target_fn
                        };
                        current_bc = target_bc;
                        current_self = target_fn;
                        fp = fn_idx;
                        ip = target_bc->code;
                        DISPATCH();
                    }
                }

                /* Fallback to tree-walking lambda or primitive call */
                CELL * argList = nilCell;
                CELL * tail = NULL;
                for (int i = 0; i < call_argc; i++)
                {
                    CELL * arg_val = vm_stack[fn_idx + 1 + i];
                    CELL * item = makeCell(CELL_QUOTE, (UINT)arg_val);

                    if (argList == nilCell)
                        argList = tail = item;
                    else
                    {
                        tail->next = item;
                        tail = item;
                    }
                }
                pushResult(argList);

                CELL * res;
                if (target_fn->type == CELL_PRIMITIVE)
                {
                    CELL * (*pfunc)(CELL *) = (CELL *(*)(CELL *))target_fn->contents;
                    res = pfunc(argList);
                }
                else if (target_fn->type == CELL_LAMBDA)
                {
                    res = evaluateLambda((CELL *)target_fn->contents, argList, currentContext);
                }
                else if (target_fn->type == CELL_EXPRESSION)
                {
                    res = implicitIndexList(target_fn, argList);
                }
                else if (target_fn->type == CELL_ARRAY)
                {
                    res = implicitIndexArray(target_fn, argList);
                }
                else if (target_fn->type == CELL_STRING)
                {
                    res = implicitIndexString(target_fn, argList);
                }
                else if (isNumber(target_fn->type))
                {
                    res = implicitNrestSlice(target_fn, argList);
                }
                else
                {
                    res = nilCell;
                }

                popResult();
                CELL * cur = argList;
                while (cur != nilCell && cur != NULL)
                {
                    if (cur->type == CELL_QUOTE)
                        cur->contents = (UINT)nilCell;
                    cur = cur->next;
                }
                deleteList(argList);

                vm_sp = fn_idx;
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = res;
                DISPATCH();
            }

#if USE_COMPUTED_GOTO
            DO_OP_RET:
#else
            case OP_RET:
#endif
            {
                CELL * ret_val = vm_stack[--vm_sp];
                --vm_frame_count;
                if (vm_frame_count == base_frame)
                {
                    CELL * final_ret = copyCell(ret_val);
                    vm_sp = start_sp;
                    currentContext = contextSave;
                    symbolCheck = NULL;
                    stringCell = NULL;
                    return final_ret;
                }
                VM_FRAME * caller = &vm_frames[vm_frame_count - 1];
                vm_sp = fp;
                VM_CHECK_STACK(1);
                vm_stack[vm_sp++] = ret_val;
                current_bc = caller->bytecode;
                current_self = caller->self_cell;
                fp = caller->fp;
                ip = caller->ip;
                DISPATCH();
            }

#if !USE_COMPUTED_GOTO
            default:
                return nilCell;
        }
    }
#endif
}
