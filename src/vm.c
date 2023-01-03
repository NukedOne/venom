#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "math.h"
#include "compiler.h"
#include "vm.h"
#include "object.h"
#include "util.h"

void init_vm(VM *vm) {
    memset(vm, 0, sizeof(VM));
}

void free_vm(VM* vm) {
    /* Free the globals table and its strings. */
    table_free(&vm->globals);
    table_free(&vm->struct_blueprints); 
}

static inline void push(VM *vm, Object obj) {
    vm->stack[vm->tos++] = obj;
}

static inline Object pop(VM *vm) {
    return vm->stack[--vm->tos];
}

#define BINARY_OP(op, wrapper) \
do { \
    /* Operands are already on the stack. */ \
    Object b = pop(vm); \
    Object a = pop(vm); \
    Object obj = wrapper(TO_DOUBLE(a) op TO_DOUBLE(b)); \
    push(vm, obj); \
} while (0)

#define READ_UINT8() (*++(*ip))

#define READ_INT16() \
    /* ip points to one of the jump instructions and there \
     * is a 2-byte operand (offset) that comes after the jump \
     * instruction. We want to increment the ip so it points \
     * to the last of the two operands, and construct a 16-bit \
     * offset from the two bytes. Then ip is incremented in \
     * the loop again so it points to the next instruction \
     * (as opposed to pointing somewhere in the middle). */ \
    (*ip += 2, \
    (int16_t)(((*ip)[-1] << 8) | (*ip)[0]))

#define PRINT_STACK() \
do { \
    printf("stack: ["); \
    for (size_t i = 0; i < vm->tos; i++) { \
        PRINT_OBJECT(vm->stack[i]); \
        printf(", "); \
    } \
    printf("]\n"); \
} while(0)

#define RUNTIME_ERROR(...) \
do { \
    fprintf(stderr, "runtime error: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    return 1; \
} while (0)

static inline int handle_op_print(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    Object object = pop(vm);
#ifdef venom_debug
    printf("dbg print :: ");
#endif
    PRINT_OBJECT(object);
    printf("\n");
    OBJECT_DECREF(object);
    return 0;
}

static inline int handle_op_const(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* At this point, ip points to OP_CONST.
    * Since this is a 2-byte instruction with an
    * immediate operand (the index of the double
    * constant in the constant pool), we want to
    * increment the ip to point to the index of
    * the constant in the constant pool that comes
    * after the opcode, and push the constant on
    * the stack. */
    uint8_t index = READ_UINT8();
    Object obj = AS_DOUBLE(chunk->cp[index]);
    push(vm, obj);
    return 0;
}

static inline int handle_op_get_global(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* At this point, ip points to OP_GET_GLOBAL.
    * Since this is a 2-byte instruction with an
    * immediate operand (the index of the name of
    * the looked up variable in the string constant
    * pool), we want to increment the ip so it points
    * to the /index/ of the string in the string
    * constant pool that comes after the opcode.
    * We then look up the variable and push its value
    * on the stack. If we can't find the variable,
    * we bail out. */
    uint8_t name_index = READ_UINT8();
    Object *obj = table_get(&vm->globals, chunk->sp[name_index]);
    if (obj == NULL) {
        RUNTIME_ERROR(
            "Variable '%s' is not defined",
            chunk->sp[name_index]
        );
    }
    push(vm, *obj);
    OBJECT_INCREF(*obj);
    return 0;
}

static inline int handle_op_set_global(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* At this point, ip points to OP_SET_GLOBAL.
    * This is a single-byte instruction that expects
    * two things to already be on the stack: the index
    * of the variable name in the string constant pool,
    * and the value of the double constant that the name
    * refers to. We pop these two and add the variable
    * to the globals table. */
    uint8_t name_index = READ_UINT8();
    Object constant = pop(vm);
    table_insert(&vm->globals, chunk->sp[name_index], constant);
    return 0;
}

static inline int handle_op_str(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* At this point, ip points to OP_CONST.
    * Since this is a 2-byte instruction with an
    * immediate operand (the index of the double
    * constant in the constant pool), we want to
    * increment the ip to point to the index of
    * the constant in the constant pool that comes
    * after the opcode, and push the constant on
    * the stack. */
    uint8_t index = READ_UINT8();
    Object obj = AS_STR(chunk->sp[index]);
    push(vm, obj);
    return 0;
}

static inline int handle_op_deep_set(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t index = READ_UINT8();
    Object obj = pop(vm);
    int fp = vm->fp_stack[vm->fp_count-1];
    vm->stack[fp+index] = obj;
    OBJECT_DECREF(obj);
    return 0;
}

static inline int handle_op_deep_get(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t index = READ_UINT8();
    int fp = vm->fp_stack[vm->fp_count-1];
    Object obj = vm->stack[fp+index];
    push(vm, obj);
    OBJECT_INCREF(obj);
    return 0;
}

static inline int handle_op_getattr(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t property_name_index = READ_UINT8();
    Object obj = pop(vm);
    Object *property = table_get(TO_STRUCT(obj).properties, chunk->sp[property_name_index]);
    if (property == NULL) {
        RUNTIME_ERROR(
            "Property '%s' is not defined on object '%s'",
            chunk->sp[property_name_index],
            TO_STRUCT(obj).name
        );
    }
    push(vm, *property);
    OBJECT_DECREF(obj);
    OBJECT_INCREF(*property);
    return 0;
}

static inline int handle_op_setattr(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t property_name_index = READ_UINT8();
    Object value = pop(vm);
    Object structobj = pop(vm);
    table_insert(TO_STRUCT(structobj).properties, chunk->sp[property_name_index], value);
    push(vm, structobj);
    return 0;
}

static inline int handle_op_add(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(+, AS_DOUBLE);
    return 0;
}

static inline int handle_op_sub(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(-, AS_DOUBLE);
    return 0;
}

static inline int handle_op_mul(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(*, AS_DOUBLE);
    return 0;
}

static inline int handle_op_div(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(/, AS_DOUBLE);
    return 0;
}

static inline int handle_op_mod(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    Object b = pop(vm);
    Object a = pop(vm);
    Object obj = AS_DOUBLE(fmod(TO_DOUBLE(a), TO_DOUBLE(b)));
    push(vm, obj);
    return 0;
}

static inline bool check_equality(VM *vm, Object *a, Object *b) {
    if (a->type != b->type) {
        return false;
    }
    if (IS_NUM(*a) && IS_NUM(*b)) {
        return TO_DOUBLE(*a) == TO_DOUBLE(*b);
    } else if (IS_STRING(*a) && IS_STRING(*b)) {
        return strcmp(TO_STR(*a), TO_STR(*b)) == 0;
    } else if (IS_BOOL(*a) && IS_BOOL(*b)) {
        return TO_BOOL(*a) == TO_BOOL(*b);      
    } else if (IS_HEAP(*a) && IS_HEAP(*b)) {
        Object *blueprint = table_get(&vm->struct_blueprints, TO_STRUCT(*a).name);
        for (size_t i = 0; i < TO_STRUCT_BLUEPRINT(*blueprint).properties.count; i++) {
            char *prop = TO_STRUCT_BLUEPRINT(*blueprint).properties.data[i];
            if (!check_equality(
                vm,
                table_get(TO_STRUCT(*a).properties, prop),
                table_get(TO_STRUCT(*b).properties, prop)
            )) {
                return false;
            }
        }
        return true;
    }
    assert(0);
}

static inline int handle_op_gt(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(>, AS_BOOL);
    return 0;
}

static inline int handle_op_lt(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    BINARY_OP(<, AS_BOOL);
    return 0;
}

static inline char *obj_typename(ObjectType type) {
    switch (type) {
        case OBJ_NUMBER: return "number";
        case OBJ_STRING: return "string";
        case OBJ_BOOLEAN: return "boolean";
        case OBJ_NULL: return "null";
        case OBJ_HEAP: return "heap";
        default: return "not implemented";
    }
}

static inline int handle_op_eq(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    Object b = pop(vm);
    Object a = pop(vm);
    OBJECT_DECREF(a);
    OBJECT_DECREF(b);
    if (a.type != b.type) {
        RUNTIME_ERROR(
            "Comparing objects of different types: '%s' vs '%s'.",
            obj_typename(a.type),
            obj_typename(b.type)
        );
    }
    push(vm, AS_BOOL(check_equality(vm, &a, &b)));
    return 0;
}

static inline int handle_op_jz(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* Jump if zero. */
    int16_t offset = READ_INT16();
    Object obj = pop(vm);
    if (!TO_BOOL(obj)) {
        *ip += offset;
    }
    return 0;
}

static inline int handle_op_jmp(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    int16_t offset = READ_INT16();
    *ip += offset;
    return 0;
}

static inline int handle_op_negate(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    Object original = pop(vm);
    Object negated = AS_DOUBLE(-TO_DOUBLE(original));
    push(vm, negated);
    return 0;
}

static inline int handle_op_not(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    Object obj = pop(vm);
    push(vm, AS_BOOL(TO_BOOL(obj) ^ 1));
    return 0;
}

static inline int handle_op_func(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* At this point, ip points to OP_FUNC. 
     * After the opcode, there is the index
     * of the function's name in the string
     * constant pool, followed by the number
     * of function parameters. */
    uint8_t funcname_index = READ_UINT8();
    uint8_t paramcount = READ_UINT8();

    /* After the number of parameters, there
     * is one more byte: the location of the
     * function in the bytecode. */ 
    uint8_t location = READ_UINT8();

    /* We make the function object... */
    char *funcname = chunk->sp[funcname_index];
    Function func = {
        .location = location,
        .name = funcname,
        .paramcount = paramcount,
    };

    Object funcobj = AS_FUNC(func);

    /* ...and insert it into the 'vm->globals' table. */
    table_insert(&vm->globals, funcname, funcobj);
    return 0;
}

static inline int handle_op_invoke(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* We first read the index of the function name and the argcount. */
    uint8_t funcname = READ_UINT8();
    uint8_t argcount = READ_UINT8();

    /* Then, we look it up from the globals table. */ 
    Object *funcobj = table_get(&vm->globals, chunk->sp[funcname]);
    if (funcobj == NULL) {
        /* Runtime error if the function is not defined. */
        RUNTIME_ERROR(
            "Variable '%s' is not defined",
            chunk->sp[funcname]
        );
    }

    /* If the number of arguments the function was called with 
        + does not match the number of parameters the function was
        * declared to accept, raise a runtime error. */
    if (argcount != funcobj->as.func.paramcount) {
        RUNTIME_ERROR(
            "Function '%s' requires '%d' arguments.",
            chunk->sp[funcname], argcount
        );
    }

    vm->fp_count++;
                    
    /* We modify ip so that it points to one instruction
     * just before the code we're invoking. */
    *ip = &chunk->code.data[TO_FUNC(*funcobj).location-1];
    return 0;
}

static inline int handle_op_ip(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    int16_t offset = READ_INT16();
    Object ip_obj = AS_POINTER(*(ip)+offset);
    push(vm, ip_obj);
    vm->fp_stack[vm->fp_count] = vm->tos;
    return 0;
}

static inline int handle_op_ret(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    /* By the time we encounter OP_RET, the return
     * value is located on the stack. Beneath it are
     * the function arguments, followed by the return
     * address. */
    Object returnvalue = pop(vm);

    /* We pop the last frame pointer off the frame pointer stack. */
    int fp = vm->fp_stack[--vm->fp_count];

    /* Then, we clean up everything between the top of the stack
     * and the frame pointer we popped in the previous step. */

    /* After the arguments comes the return address which we'll
     * use to modify the instruction pointer ip and return to the
     * caller. */
    Object returnaddr = pop(vm);

    /* Then, we push the return value back on the stack.  */
    push(vm, returnvalue);
    OBJECT_DECREF(returnvalue);

    /* Finally, we modify the instruction pointer. */
    *ip = returnaddr.as.ptr;
    return 0;
}

static inline int handle_op_true(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    push(vm, AS_BOOL(true));
    return 0;
}

static inline int handle_op_null(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    push(vm, AS_NULL());
    return 0;
}

static inline int handle_op_struct(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t struct_name = READ_UINT8();
    uint8_t property_count = READ_UINT8();

    StructBlueprint sb = { 
        .name = chunk->sp[struct_name],
        .propertycount = property_count,
    };

    for (int i = 0; i < property_count; i++) {
        uint8_t property_name = READ_UINT8();
        dynarray_insert(&sb.properties, chunk->sp[property_name]);
    }

    table_insert(
        &vm->struct_blueprints,
        chunk->sp[struct_name],
        AS_STRUCT_BLUEPRINT(sb)
    );
    return 0;
}

static inline int handle_op_struct_init(VM *vm, BytecodeChunk *chunk, uint8_t **ip) {
    uint8_t structname = READ_UINT8();

    Object *blueprint = table_get(&vm->struct_blueprints, chunk->sp[structname]);
    if (blueprint == NULL) {
        RUNTIME_ERROR(
            "Struct '%s' is not defined",
            chunk->sp[structname]
        );
    }

    uint8_t propertycount = READ_UINT8();
    if (propertycount != blueprint->as.struct_blueprint.propertycount) {
        RUNTIME_ERROR(
            "Incorrect property count for struct '%s'",
            TO_STRUCT_BLUEPRINT(*blueprint).name
        );
    }

    Struct s = {
        .name = TO_STRUCT_BLUEPRINT(*blueprint).name,
        .propertycount = TO_STRUCT_BLUEPRINT(*blueprint).propertycount,
        .properties = malloc(sizeof(Table)),
    };

    memset(s.properties, 0, sizeof(Table));

    Object structobj = AS_STRUCT(s);

    HeapObject heapobj = {
        .refcount = 1,
        .obj = structobj,
    };

    push(vm, AS_HEAP(ALLOC(heapobj)));
    return 0;
}

typedef int (*HandlerFn)(VM *vm, BytecodeChunk *chunk, uint8_t **ip);
typedef struct {
    HandlerFn fn;
    char *opcode;
} Handler;

Handler dispatcher[] = {
    [OP_PRINT] = { .fn = handle_op_print, .opcode = "OP_PRINT" },
    [OP_CONST] = { .fn = handle_op_const, .opcode = "OP_CONST" },
    [OP_GET_GLOBAL] = { .fn = handle_op_get_global, .opcode = "OP_GET_GLOBAL" },
    [OP_SET_GLOBAL] = { .fn = handle_op_set_global, .opcode = "OP_SET_GLOBAL" },
    [OP_STR] = { .fn = handle_op_str, .opcode = "OP_STR" },
    [OP_DEEP_GET] = { .fn = handle_op_deep_get, .opcode = "OP_DEEP_GET" },
    [OP_DEEP_SET] = { .fn = handle_op_deep_set, .opcode = "OP_DEEP_SET" },
    [OP_GETATTR] = { .fn = handle_op_getattr, .opcode = "OP_GETATTR" },
    [OP_SETATTR] = { .fn = handle_op_setattr, .opcode = "OP_SETATTR" },
    [OP_ADD] = { .fn = handle_op_add, .opcode = "OP_ADD" },
    [OP_SUB] = { .fn = handle_op_sub, .opcode = "OP_SUB" },
    [OP_MUL] = { .fn = handle_op_mul, .opcode = "OP_MUL" },
    [OP_DIV] = { .fn = handle_op_div, .opcode = "OP_DIV" },
    [OP_MOD] = { .fn = handle_op_mod, .opcode = "OP_MOD" },
    [OP_GT] = { .fn = handle_op_gt, .opcode = "OP_GT" },
    [OP_LT] = { .fn = handle_op_lt, .opcode = "OP_LT" },
    [OP_EQ] = { .fn = handle_op_eq, .opcode = "OP_EQ" },
    [OP_JZ] = { .fn = handle_op_jz, .opcode = "OP_JZ" },
    [OP_JMP] = { .fn = handle_op_jmp, .opcode = "OP_JMP" },
    [OP_NEGATE] = { .fn = handle_op_negate, .opcode = "OP_NEGATE" },
    [OP_NOT] = { .fn = handle_op_not, .opcode = "OP_NOT" },
    [OP_FUNC] = { .fn = handle_op_func, .opcode = "OP_FUNC" },
    [OP_INVOKE] = { .fn = handle_op_invoke, .opcode = "OP_INVOKE" },
    [OP_RET] = { .fn = handle_op_ret, .opcode = "OP_RET" },
    [OP_TRUE] = { .fn = handle_op_true, .opcode = "OP_TRUE" },
    [OP_NULL] = { .fn = handle_op_null, .opcode = "OP_NULL" },
    [OP_STRUCT] = { .fn = handle_op_struct, .opcode = "OP_STRUCT" },
    [OP_STRUCT_INIT] = { .fn = handle_op_struct_init, .opcode = "OP_STRUCT_INIT" },
    [OP_IP] = { .fn = handle_op_ip, .opcode = "OP_IP" },
};

void print_current_instruction(uint8_t *ip) {
    printf("current instruction: %s\n", dispatcher[*ip].opcode);
}

int run(VM *vm, BytecodeChunk *chunk) {
#ifdef venom_debug
    disassemble(chunk);
#endif

    for (
        uint8_t *ip = chunk->code.data;
        ip < &chunk->code.data[chunk->code.count];  /* ip < addr of just beyond the last instruction */
        ip++
    ) {
#ifdef venom_debug
        print_current_instruction(ip);
#endif
        int status = dispatcher[*ip].fn(vm, chunk, &ip);
        if (status != 0) return status;
#ifdef venom_debug
        PRINT_STACK();
#endif
    }

    return 0;
}