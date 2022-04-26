#ifndef venom_vm_h
#define venom_vm_h

#define STACK_MAX 255

#include "compiler.h"
#include "dynarray.h"
#include "table.h"

typedef DynArray(double) Double_DynArray;
typedef DynArray(char *) String_DynArray;

typedef struct {
    double stack[STACK_MAX];
    size_t tos;  /* top of stack */
    Double_DynArray cp;  /* constant pool */
    String_DynArray sp;  /* string constant pool */
    Table globals;
} VM;

typedef struct BytecodeChunk BytecodeChunk;

void init_vm(VM *vm);
void free_vm(VM *vm);
void run(VM *vm, BytecodeChunk *chunk);

#endif