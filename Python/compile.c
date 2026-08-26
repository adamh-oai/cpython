/*
 * This file compiles an abstract syntax tree (AST) into Python bytecode.
 *
 * The primary entry point is _PyAST_Compile(), which returns a
 * PyCodeObject.  The compiler makes several passes to build the code
 * object:
 *   1. Checks for future statements.  See future.c
 *   2. Builds a symbol table.  See symtable.c.
 *   3. Generate an instruction sequence. See compiler_mod() in this file, which
 *      calls functions from codegen.c.
 *   4. Generate a control flow graph and run optimizations on it.  See flowgraph.c.
 *   5. Assemble the basic blocks into final code.  See optimize_and_assemble() in
 *      this file, and assembler.c.
 *
 */

#include "Python.h"
#include "opcode.h"
#include "pycore_ast.h"           // PyAST_Check()
#include "pycore_code.h"
#include "pycore_compile.h"
#include "pycore_flowgraph.h"     // _PyCfg_FromInstructionSequence()
#include "pycore_opcode_metadata.h" // Native opcode stack effects and jump flags
#include "pycore_pystate.h"       // _Py_GetConfig()
#include "pycore_runtime.h"       // _Py_ID()
#include "pycore_setobject.h"     // _PySet_NextEntry()
#include "pycore_stats.h"
#include "pycore_unicodeobject.h" // _PyUnicode_EqualToASCIIString()

#include "cpython/code.h"

#include <stdbool.h>


#undef SUCCESS
#undef ERROR
#define SUCCESS 0
#define ERROR -1

#define RETURN_IF_ERROR(X)  \
    do {                    \
        if ((X) == -1) {    \
            return ERROR;   \
        }                   \
    } while (0)

typedef _Py_SourceLocation location;
typedef _PyJumpTargetLabel jump_target_label;
typedef _PyInstructionSequence instr_sequence;
typedef struct _PyCfgBuilder cfg_builder;
typedef _PyCompile_FBlockInfo fblockinfo;
typedef enum _PyCompile_FBlockType fblocktype;

typedef struct _PySoacBindingCollector soac_binding_collector;

/* The following items change on entry and exit of code blocks.
   They must be saved and restored when returning to a block.
*/
struct compiler_unit {
    PySTEntryObject *u_ste;

    int u_scope_type;

    PyObject *u_private;            /* for private name mangling */
    PyObject *u_static_attributes;  /* for class: attributes accessed via self.X */
    PyObject *u_deferred_annotations; /* AnnAssign nodes deferred to the end of compilation */
    PyObject *u_conditional_annotation_indices;  /* indices of annotations that are conditionally executed (or -1 for unconditional annotations) */
    long u_next_conditional_annotation_index;  /* index of the next conditional annotation */

    instr_sequence *u_instr_sequence; /* codegen output */
    instr_sequence *u_stashed_instr_sequence; /* temporarily stashed parent instruction sequence */

    int u_nfblocks;
    int u_in_inlined_comp;
    int u_in_conditional_block;

    _PyCompile_FBlockInfo u_fblock[CO_MAXBLOCKS];

    _PyCompile_CodeUnitMetadata u_metadata;
};

/* This struct captures the global state of a compilation.

The u pointer points to the current compilation unit, while units
for enclosing blocks are stored in c_stack.     The u and c_stack are
managed by _PyCompile_EnterScope() and _PyCompile_ExitScope().

Note that we don't track recursion levels during compilation - the
task of detecting and rejecting excessive levels of nesting is
handled by the symbol analysis pass.

*/

typedef struct _PyCompiler {
    PyObject *c_filename;
    struct symtable *c_st;
    _PyFutureFeatures c_future;  /* module's __future__ */
    PyCompilerFlags c_flags;

    int c_optimize;              /* optimization level */
    int c_interactive;           /* true if in interactive mode */
    PyObject *c_const_cache;     /* Python dict holding all constants,
                                    including names tuple */
    struct compiler_unit *u;     /* compiler state for current block */
    PyObject *c_stack;           /* Python list holding compiler_unit ptrs */

    bool c_save_nested_seqs;     /* if true, construct recursive instruction sequences
                                  * (including instructions for nested code objects)
                                  */
    int c_disable_warning;
    PyObject *c_module;
    soac_binding_collector *c_soac_bindings;
} compiler;

/* This collector exists only for CompileVerifiedSourceDetails. It observes
 * native compiler decisions, not executable bytecode. All references belong
 * to this compilation and disappear after the immutable result is built. */
#define SOAC_VECTOR(T) struct { T *items; Py_ssize_t count, capacity; }

typedef struct {
    int is_deref;
    int raw_index;
    int final_index;
    Py_ssize_t entry_owner;
} soac_binding_slot;

typedef struct {
    Py_ssize_t slot;
    Py_ssize_t region;
    int kind;
    Py_ssize_t representative;
    Py_ssize_t final_id;
} soac_binding_owner;

typedef struct {
    int phase;
    Py_ssize_t owner;
    int role;
    Py_ssize_t operand;
} soac_binding_init;

typedef struct {
    int role;
    Py_ssize_t slot;
    Py_ssize_t owner;
} soac_binding_entry_op;

typedef struct {
    location loc;
    Py_ssize_t parent;
    PySTEntryObject *origin;  /* Borrowed original AST/symtable identity. */
    Py_ssize_t representative;
    Py_ssize_t final_id;
    expr_ty expression;
    location outer_loc;
    int kind;
    int is_async;
    int previous_binding_role;
    int previous_binding_generator;
    SOAC_VECTOR(soac_binding_entry_op) entry_ops;
} soac_binding_region;

typedef struct {
    PyCodeObject *child;  /* Borrowed from another collector-owned unit. */
    location loc;
    int free_ordinal;
    Py_ssize_t slot;
    Py_ssize_t region;
    int emit;
} soac_binding_capture;

typedef struct {
    int role;
    Py_ssize_t slot;
} soac_binding_export;

typedef struct {
    location loc;
    int context;
    int mode;
    Py_ssize_t slot;
    expr_ty origin;  /* Borrowed from this compilation's original AST. */
    Py_ssize_t region;
    int emit;
} soac_binding_access;

/* One opt-in origin registry. The two existing instruction lanes carry IDs
 * into it; no pointer, registry index or native block label is serialized. */
enum soac_origin_family {
    SOAC_ORIGIN_STORE = Py_SOAC_OPERATION_STORE,
    SOAC_ORIGIN_CALL = Py_SOAC_OPERATION_CALL,
    SOAC_ORIGIN_PATTERN_LEAF,  /* transient constituent, not a physical Store */
    SOAC_ORIGIN_CALL_PREPARATION,
};

typedef struct {
    const void *owner;
    location owner_loc;
    int owner_kind;
    int item;
    int entry;
    const void *transfer;
    location transfer_loc;
    int payload;
    Py_ssize_t parent;  /* -1 authenticated empty; -2 unavailable */
} soac_emission_context;

typedef struct {
    const void *origin;
    location loc;
    int kind;
    int context;
    int phase;
    int family;
    int initial_opcode;
    int initial_slot;
    Py_ssize_t emission_context;
    PyObject *detail;          /* immutable pointer-free binding/index detail */
    PyObject *pattern_leaves;  /* transient constituent origin IDs only */
    PyCodeObject *child;       /* borrowed collector-owned actual native code */
    int channel;
    int preloaded;
    int positional_kind;
    int keyword_kind;
    int keyword_constant;
    PyObject *positional_entries;
    PyObject *keyword_entries;
    PyObject *keyword_groups;
    PyObject *keyword_names;  /* Actual emitter tuple, not a source reconstruction. */
    int alternative;
    uint32_t call_owner;       /* preparation -> actual source Call origin */
} soac_reference_origin;

typedef struct {
    int ordinal;
    int opcode;
    int oparg;
    _PySoacReadOrigins origins;
    Py_ssize_t opcode_offset;
    Py_ssize_t first_byte;
    Py_ssize_t end_byte;
} soac_reference_instruction;

typedef struct {
    Py_ssize_t region;
    int role;
    int generator;
    uint32_t origin;
} soac_scope_source_binding;

typedef struct _PySoacCodeUnitBindings {
    struct _PySoacCodeUnitBindings *parent;
    PyCodeObject *code;
    PySTEntryObject *origin;
    int scope_kind;
    int symtable_kind;
    location loc;
    int cell_count;
    int free_count;
    Py_ssize_t final_id;
    Py_ssize_t active_region;
    Py_ssize_t final_owner_count;
    Py_ssize_t final_region_count;
    SOAC_VECTOR(soac_binding_slot) slots;
    SOAC_VECTOR(soac_binding_owner) owners;
    SOAC_VECTOR(soac_binding_init) initializers;
    SOAC_VECTOR(soac_binding_region) regions;
    SOAC_VECTOR(soac_binding_capture) captures;
    SOAC_VECTOR(soac_binding_export) exports;
    SOAC_VECTOR(soac_binding_access) accesses;
    int scope_binding_role;
    int scope_binding_generator;
    SOAC_VECTOR(soac_scope_source_binding) scope_source_bindings;
    int reference_instruction_count;
    Py_ssize_t active_emission_context;
    SOAC_VECTOR(soac_emission_context) emission_contexts;
    int assembled_instruction_count;
    Py_ssize_t assembled_reference_cursor;
    Py_ssize_t assembled_code_size;
    SOAC_VECTOR(soac_reference_origin) reference_origins;
    SOAC_VECTOR(soac_reference_instruction) reference_instructions;
} soac_code_bindings;

static soac_reference_origin *soac_operation_at(soac_code_bindings *, uint32_t);
struct _PySoacBindingCollector {
    SOAC_VECTOR(soac_code_bindings *) units;
};

static int
soac_binding_error(const char *message)
{
    PyErr_Format(PyExc_SystemError, "native class bindings: %s", message);
    return ERROR;
}

static int
soac_binding_append(void **items, Py_ssize_t *count, Py_ssize_t *capacity,
                    size_t item_size, const void *item)
{
    if (*count == *capacity) {
        size_t old = (size_t)*capacity;
        size_t next = old == 0 ? 8 : old * 2;
        if (next < old || next > (size_t)PY_SSIZE_T_MAX / item_size) {
            PyErr_NoMemory();
            return ERROR;
        }
        void *grown = PyMem_Realloc(*items, next * item_size);
        if (grown == NULL) {
            PyErr_NoMemory();
            return ERROR;
        }
        *items = grown;
        *capacity = (Py_ssize_t)next;
    }
    memcpy((char *)*items + (size_t)*count * item_size, item, item_size);
    ++*count;
    return SUCCESS;
}

#define SOAC_PUSH(V, ITEM) \
    soac_binding_append((void **)&(V).items, &(V).count, &(V).capacity, \
                        sizeof(*(V).items), &(ITEM))

static void
soac_binding_collector_free(soac_binding_collector *collector)
{
    if (collector == NULL) {
        return;
    }
    PyObject *saved_error = PyErr_GetRaisedException();
    for (Py_ssize_t i = 0; i < collector->units.count; i++) {
        soac_code_bindings *unit = collector->units.items[i];
        for (Py_ssize_t j = 0; j < unit->regions.count; j++) {
            soac_binding_region *region = &unit->regions.items[j];
            PyMem_Free(region->entry_ops.items);
        }
        PyMem_Free(unit->slots.items);
        PyMem_Free(unit->owners.items);
        PyMem_Free(unit->scope_source_bindings.items);
        PyMem_Free(unit->initializers.items);
        PyMem_Free(unit->regions.items);
        PyMem_Free(unit->captures.items);
        PyMem_Free(unit->exports.items);
        PyMem_Free(unit->accesses.items);
        for (Py_ssize_t j = 0; j < unit->reference_origins.count; j++) {
            soac_reference_origin *origin = &unit->reference_origins.items[j];
            Py_XDECREF(origin->detail);
            Py_XDECREF(origin->pattern_leaves);
            Py_XDECREF(origin->positional_entries);
            Py_XDECREF(origin->keyword_entries);
            Py_XDECREF(origin->keyword_groups);
            Py_XDECREF(origin->keyword_names);
        }
        PyMem_Free(unit->emission_contexts.items);
        PyMem_Free(unit->reference_origins.items);
        PyMem_Free(unit->reference_instructions.items);
        Py_XDECREF(unit->code);
        PyMem_Free(unit);
    }
    PyMem_Free(collector->units.items);
    PyMem_Free(collector);
    PyErr_SetRaisedException(saved_error);
}

static int
soac_scope_wire_kind(int scope)
{
    switch (scope) {
        case COMPILE_SCOPE_MODULE: return Py_SOAC_SCOPE_MODULE;
        case COMPILE_SCOPE_CLASS: return Py_SOAC_SCOPE_CLASS;
        case COMPILE_SCOPE_FUNCTION: return Py_SOAC_SCOPE_FUNCTION;
        case COMPILE_SCOPE_ASYNC_FUNCTION: return Py_SOAC_SCOPE_ASYNC_FUNCTION;
        case COMPILE_SCOPE_LAMBDA: return Py_SOAC_SCOPE_LAMBDA;
        case COMPILE_SCOPE_COMPREHENSION: return Py_SOAC_SCOPE_COMPREHENSION;
        case COMPILE_SCOPE_ANNOTATIONS: return Py_SOAC_SCOPE_ANNOTATIONS;
    }
    return soac_binding_error("unknown compiler scope kind");
}

static int
soac_symtable_wire_kind(_Py_block_ty kind)
{
    switch (kind) {
        case FunctionBlock: return Py_SOAC_SYMTABLE_FUNCTION;
        case ClassBlock: return Py_SOAC_SYMTABLE_CLASS;
        case ModuleBlock: return Py_SOAC_SYMTABLE_MODULE;
        case AnnotationBlock: return Py_SOAC_SYMTABLE_ANNOTATION;
        case TypeAliasBlock: return Py_SOAC_SYMTABLE_TYPE_ALIAS;
        case TypeParametersBlock: return Py_SOAC_SYMTABLE_TYPE_PARAMETERS;
        case TypeVariableBlock: return Py_SOAC_SYMTABLE_TYPE_VARIABLE;
    }
    return soac_binding_error("unknown symbol-table kind");
}

static int
soac_add_owner(soac_code_bindings *unit, Py_ssize_t slot, Py_ssize_t region,
               int kind, Py_ssize_t *owner_id)
{
    *owner_id = unit->owners.count;
    soac_binding_owner owner = {slot, region, kind, unit->owners.count, -1};
    RETURN_IF_ERROR(SOAC_PUSH(unit->owners, owner));
    return SUCCESS;
}

static int
soac_binding_slot_for(soac_code_bindings *unit, int is_deref, int raw_index,
                      Py_ssize_t *slot_id)
{
    for (Py_ssize_t i = 0; i < unit->slots.count; i++) {
        soac_binding_slot *slot = &unit->slots.items[i];
        if (slot->is_deref == is_deref && slot->raw_index == raw_index) {
            *slot_id = i;
            return SUCCESS;
        }
    }
    if (raw_index < 0 || (is_deref &&
            raw_index >= unit->cell_count + unit->free_count)) {
        return soac_binding_error("binding operand outside native layout");
    }
    *slot_id = unit->slots.count;
    soac_binding_slot slot = {is_deref, raw_index, -1, -1};
    RETURN_IF_ERROR(SOAC_PUSH(unit->slots, slot));
    Py_ssize_t owner;
    RETURN_IF_ERROR(soac_add_owner(unit, *slot_id, -1, Py_SOAC_CLASS_OWNER_ENTRY, &owner));
    /* Seed facts are serialized from the native successful-binding layout. */
    unit->slots.items[*slot_id].entry_owner = owner;
    return SUCCESS;
}

static int
soac_local_binding_slot(_PyCompile_CodeUnitMetadata *umd, int local_index,
                        Py_ssize_t *slot_id)
{
    PyObject *name, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(umd->u_varnames, &pos, &name, &value)) {
        int index = PyLong_AsInt(value);
        if (index == -1 && PyErr_Occurred()) {
            return ERROR;
        }
        if (index != local_index) {
            continue;
        }
        /* A LOCAL/CELL pair is one slot. An equal-spelling FREE is not. */
        PyObject *cell_index = PyDict_GetItemWithError(umd->u_cellvars, name);
        if (cell_index != NULL) {
            index = PyLong_AsInt(cell_index);
            if (index == -1 && PyErr_Occurred()) {
                return ERROR;
            }
            return soac_binding_slot_for(umd->u_soac_bindings, 1, index, slot_id);
        }
        if (PyErr_Occurred()) {
            return ERROR;
        }
        return soac_binding_slot_for(umd->u_soac_bindings, 0, local_index, slot_id);
    }
    return soac_binding_error("local operand has no native variable row");
}

static int
soac_register_code_unit(compiler *c, struct compiler_unit *u)
{
    u->u_metadata.u_soac_bindings = NULL;
    if (c->c_soac_bindings == NULL) {
        return SUCCESS;
    }
    soac_code_bindings *unit = PyMem_Calloc(1, sizeof(*unit));
    if (unit == NULL) {
        PyErr_NoMemory();
        return ERROR;
    }
    unit->parent = c->u == NULL ? NULL : c->u->u_metadata.u_soac_bindings;
    unit->origin = u->u_ste;
    unit->final_id = -1;
    unit->active_region = -1;
    unit->scope_binding_role = unit->scope_binding_generator = -1;
    unit->reference_instruction_count = -1;
    unit->active_emission_context = -1;
    unit->assembled_code_size = -1;
    unit->scope_kind = soac_scope_wire_kind(u->u_scope_type);
    unit->symtable_kind = soac_symtable_wire_kind(u->u_ste->ste_type);
    unit->loc = u->u_ste->ste_loc;
    if (unit->scope_kind < 0 || unit->symtable_kind < 0 ||
        SOAC_PUSH(c->c_soac_bindings->units, unit) < 0) {
        PyMem_Free(unit);
        return ERROR;
    }
    u->u_metadata.u_soac_bindings = unit;
    unit->cell_count = (int)PyDict_GET_SIZE(u->u_metadata.u_cellvars);
    unit->free_count = (int)PyDict_GET_SIZE(u->u_metadata.u_freevars);
    {
        for (int i = 0; i < unit->cell_count + unit->free_count; i++) {
            Py_ssize_t slot;
            RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, i, &slot));
        }
    }
    return SUCCESS;
}

/* Some native annotation helpers append implicit FREE entries while emitting
 * their bodies.  The scope-entry dictionary is not yet the final layout.
 * Complete only the metadata from the finished native maps, before the CFG
 * inserts COPY_FREE_VARS/MAKE_CELL and fixes their actual operands. */
static int
soac_complete_binding_layout(_PyCompile_CodeUnitMetadata *umd)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    Py_ssize_t ncellvars = PyDict_GET_SIZE(umd->u_cellvars);
    Py_ssize_t nfreevars = PyDict_GET_SIZE(umd->u_freevars);
    if (ncellvars != unit->cell_count || nfreevars < unit->free_count ||
        nfreevars > INT_MAX - unit->cell_count) {
        return soac_binding_error("final native cell/free layout changed its existing prefix");
    }
    if (nfreevars == unit->free_count) {
        return SUCCESS;
    }

    /* Native FREE indices follow the unchanged CELL prefix.  Do not renumber
     * native operands, merge same-spelling CELL/FREE rows, or infer new slots
     * from source names.  Existing symbolic slots and owners stay untouched. */
    Py_ssize_t pos = 0;
    PyObject *value;
    int next_index = unit->cell_count;
    while (PyDict_Next(umd->u_freevars, &pos, NULL, &value)) {
        int index = PyLong_AsInt(value);
        if (index == -1 && PyErr_Occurred()) {
            return ERROR;
        }
        if (index != next_index++) {
            return soac_binding_error("final native FREE indices are not contiguous");
        }
    }
    int first_new = unit->cell_count + unit->free_count;
    unit->free_count = (int)nfreevars;
    for (int index = first_new; index < next_index; index++) {
        Py_ssize_t slot;
        RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, index, &slot));
    }
    return SUCCESS;
}

static soac_code_bindings *
soac_current_class(compiler *c)
{
    if (c->c_soac_bindings == NULL || c->u == NULL ||
        c->u->u_scope_type != COMPILE_SCOPE_CLASS) {
        return NULL;
    }
    return c->u->u_metadata.u_soac_bindings;
}

int
_PyCompile_SoacEnterComprehension(compiler *c, expr_ty expression,
                                 PySTEntryObject *original)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (expression == NULL || original == NULL || !original->ste_comp_inlined) {
        return soac_binding_error("eager region lacks its original native scope");
    }
    asdl_comprehension_seq *generators;
    int kind;
    switch (expression->kind) {
        case ListComp_kind:
            kind = Py_SOAC_EAGER_LIST;
            generators = expression->v.ListComp.generators;
            break;
        case SetComp_kind:
            kind = Py_SOAC_EAGER_SET;
            generators = expression->v.SetComp.generators;
            break;
        case DictComp_kind:
            kind = Py_SOAC_EAGER_DICT;
            generators = expression->v.DictComp.generators;
            break;
        default:
            return soac_binding_error("non-eager expression entered a scope recipe");
    }
    if (asdl_seq_LEN(generators) == 0) {
        return soac_binding_error("eager region has no original generator");
    }
    comprehension_ty first = asdl_seq_GET(generators, 0);
    soac_binding_region region = {
        .loc = SRC_LOCATION_FROM_AST(expression), .parent = unit->active_region, .origin = original,
        .representative = unit->regions.count, .final_id = -1,
        .expression = expression, .outer_loc = SRC_LOCATION_FROM_AST(first->iter), .kind = kind,
        .is_async = original->ste_coroutine,
        .previous_binding_role = unit->scope_binding_role,
        .previous_binding_generator = unit->scope_binding_generator,
    };
    Py_ssize_t id = unit->regions.count;
    RETURN_IF_ERROR(SOAC_PUSH(unit->regions, region));
    unit->active_region = id;
    unit->scope_binding_role = unit->scope_binding_generator = -1;
    return SUCCESS;
}

int
_PyCompile_SoacSaveLocal(compiler *c, int local_index)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (unit->active_region < 0) {
        return soac_binding_error("comprehension save outside its actual entry prefix");
    }
    Py_ssize_t slot_id;
    RETURN_IF_ERROR(soac_local_binding_slot(&c->u->u_metadata, local_index, &slot_id));
    soac_binding_region *region = &unit->regions.items[unit->active_region];
    Py_ssize_t owner;
    RETURN_IF_ERROR(soac_add_owner(unit, slot_id, unit->active_region,
                                  Py_SOAC_CLASS_OWNER_SAVED_SLOT, &owner));
    soac_binding_entry_op op = {Py_SOAC_CLASS_OP_SAVE_CLEAR, slot_id, owner};
    RETURN_IF_ERROR(SOAC_PUSH(region->entry_ops, op));
    return SUCCESS;
}

int
_PyCompile_SoacMakeCell(compiler *c, int deref_index)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (unit->active_region < 0) {
        return soac_binding_error("replacement cell outside its actual entry prefix");
    }
    Py_ssize_t slot_id;
    RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, deref_index, &slot_id));
    soac_binding_region *region = &unit->regions.items[unit->active_region];
    Py_ssize_t owner;
    RETURN_IF_ERROR(soac_add_owner(unit, slot_id, unit->active_region,
                                  Py_SOAC_CLASS_OWNER_FRESH_CELL, &owner));
    soac_binding_entry_op op = {Py_SOAC_CLASS_OP_MAKE_CELL, slot_id, owner};
    RETURN_IF_ERROR(SOAC_PUSH(region->entry_ops, op));
    return SUCCESS;
}

int
_PyCompile_SoacLeaveComprehension(compiler *c)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (unit->active_region < 0) {
        return soac_binding_error("unbalanced comprehension ownership region");
    }
    soac_binding_region *region = &unit->regions.items[unit->active_region];
    unit->scope_binding_role = region->previous_binding_role;
    unit->scope_binding_generator = region->previous_binding_generator;
    unit->active_region = region->parent;
    return SUCCESS;
}

int
_PyCompile_SoacCapture(compiler *c, PyCodeObject *child, location loc,
                       int free_ordinal, int deref_index)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    Py_ssize_t slot;
    RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, deref_index, &slot));
    soac_binding_capture capture = {child, loc, free_ordinal, slot, unit->active_region, 1};
    RETURN_IF_ERROR(SOAC_PUSH(unit->captures, capture));
    return SUCCESS;
}

int
_PyCompile_SoacClassInitializer(compiler *c, PyObject *name, int role)
{
    soac_code_bindings *unit = soac_current_class(c);
    if (unit == NULL) {
        return SUCCESS;
    }
    int index = _PyCompile_LookupCellvar(c, name);
    RETURN_IF_ERROR(index);
    Py_ssize_t slot;
    RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, index, &slot));
    if (role != Py_SOAC_CLASS_INIT_NAMESPACE &&
        role != Py_SOAC_CLASS_INIT_CONDITIONAL_SET) {
        return soac_binding_error("invalid class header initialization role");
    }
    soac_binding_init init = {Py_SOAC_CLASS_PHASE_HEADER,
        unit->slots.items[slot].entry_owner, role, -1};
    RETURN_IF_ERROR(SOAC_PUSH(unit->initializers, init));
    return SUCCESS;
}

int
_PyCompile_SoacClassExport(compiler *c, int deref_index, int role)
{
    soac_code_bindings *unit = soac_current_class(c);
    if (unit == NULL) {
        return SUCCESS;
    }
    Py_ssize_t slot;
    RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, deref_index, &slot));
    soac_binding_export export = {role, slot};
    RETURN_IF_ERROR(SOAC_PUSH(unit->exports, export));
    return SUCCESS;
}

int
_PyCompile_SoacNameAccess(compiler *c, location loc, expr_ty original, expr_context_ty context,
                          int mode, int native_index)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    int wire_context;
    switch (context) {
        case Load: wire_context = Py_SOAC_CLASS_ACCESS_LOAD; break;
        case Store: wire_context = Py_SOAC_CLASS_ACCESS_STORE; break;
        case Del: wire_context = Py_SOAC_CLASS_ACCESS_DEL; break;
        default: return soac_binding_error("unknown original Name context");
    }
    if (mode < Py_SOAC_CLASS_ACCESS_RAW_SLOT ||
        mode > Py_SOAC_CLASS_ACCESS_NAMESPACE_OR_CELL ||
        (mode == Py_SOAC_CLASS_ACCESS_NAMESPACE_OR_CELL && context != Load)) {
        return soac_binding_error("invalid original Name access mode");
    }
    Py_ssize_t slot;
    if (mode == Py_SOAC_CLASS_ACCESS_RAW_SLOT) {
        RETURN_IF_ERROR(soac_local_binding_slot(&c->u->u_metadata, native_index, &slot));
    }
    else {
        RETURN_IF_ERROR(soac_binding_slot_for(unit, 1, native_index, &slot));
    }
    if (original == NULL || original->kind != Name_kind) {
        return soac_binding_error("Name access lacks an original AST Name");
    }
    soac_binding_access access = {
        loc, wire_context, mode, slot, original, unit->active_region, 1,
    };
    RETURN_IF_ERROR(SOAC_PUSH(unit->accesses, access));
    return SUCCESS;
}

int
_PyCompile_SoacFixSlots(_PyCompile_CodeUnitMetadata *umd,
                       const int *fixed, int noffsets)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    /* Native prepare_localsplus is the sole authority for deref remapping. */
    for (Py_ssize_t i = 0; i < unit->reference_origins.count; i++) {
        soac_reference_origin *origin = &unit->reference_origins.items[i];
        if (origin->family == SOAC_ORIGIN_STORE &&
            (origin->initial_opcode == STORE_DEREF || origin->initial_opcode == DELETE_DEREF)) {
            if (origin->initial_slot < 0 || origin->initial_slot >= noffsets) {
                return soac_binding_error("source cell Store has no native slot remapping");
            }
            origin->initial_slot = fixed[origin->initial_slot];
        }
    }
    if (noffsets != unit->cell_count + unit->free_count || unit->active_region >= 0) {
        return soac_binding_error("native cell layout or region state changed unexpectedly");
    }
    int nlocals = (int)PyDict_GET_SIZE(umd->u_varnames);
    for (int i = 0; i < nlocals; i++) {
        Py_ssize_t slot;
        RETURN_IF_ERROR(soac_local_binding_slot(umd, i, &slot));
    }
    for (Py_ssize_t i = 0; i < unit->slots.count; i++) {
        soac_binding_slot *slot = &unit->slots.items[i];
        slot->final_index = slot->is_deref ? fixed[slot->raw_index] : slot->raw_index;
    }
    return SUCCESS;
}


static PyObject *
soac_optional_id(Py_ssize_t id)
{
    return id < 0 ? Py_NewRef(Py_None) : PyLong_FromSsize_t(id);
}

static PyObject *
soac_source_span(location loc)
{
    if (loc.lineno < 1 || loc.end_lineno < 1 ||
        loc.col_offset < 0 || loc.end_col_offset < 0) {
        return Py_NewRef(Py_None);
    }
    if (loc.end_lineno < loc.lineno ||
        (loc.end_lineno == loc.lineno && loc.end_col_offset < loc.col_offset)) {
        soac_binding_error("invalid original source span");
        return NULL;
    }
    return Py_BuildValue("(iiii)", loc.lineno, loc.col_offset,
                         loc.end_lineno, loc.end_col_offset);
}

static int
soac_append_owned(PyObject *list, PyObject *item)
{
    if (item == NULL) {
        return ERROR;
    }
    int result = PyList_Append(list, item);
    Py_DECREF(item);
    return result;
}

static soac_code_bindings *
soac_unit_for_code(soac_binding_collector *collector, PyCodeObject *code)
{
    for (Py_ssize_t i = 0; i < collector->units.count; i++) {
        if (collector->units.items[i]->code == code) {
            return collector->units.items[i];
        }
    }
    soac_binding_error("final code tree has no originating compiler unit");
    return NULL;
}

static int
soac_same_original_scope_chain(soac_code_bindings *left, soac_code_bindings *right)
{
    while (left != right) {
        if (left == NULL || right == NULL || left->origin == NULL ||
            left->origin != right->origin || left->scope_kind != right->scope_kind ||
            left->symtable_kind != right->symtable_kind) {
            return 0;
        }
        left = left->parent;
        right = right->parent;
    }
    return 1;
}

static int soac_same_final_references(soac_code_bindings *, soac_code_bindings *);

static int
soac_rebind_code_constant(compiler *c, PyCodeObject *original, PyObject *key)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL || key == (PyObject *)original) {
        return SUCCESS;
    }
    if (!PyCode_Check(key)) {
        return soac_binding_error("code constant canonicalized to a non-code key");
    }
    soac_code_bindings *before = soac_unit_for_code(c->c_soac_bindings, original);
    soac_code_bindings *after = soac_unit_for_code(c->c_soac_bindings, (PyCodeObject *)key);
    if (before == NULL || after == NULL) {
        return ERROR;
    }
    int same_references = soac_same_final_references(before, after);
    if (same_references < 0) {
        return ERROR;
    }
    if (before->origin != after->origin || before->parent != unit ||
        !soac_same_original_scope_chain(after->parent, unit) ||
        before->scope_kind != after->scope_kind ||
        before->symtable_kind != after->symtable_kind ||
        !same_references) {
        return soac_binding_error("canonical code constant has ambiguous original scope");
    }
    /* This is the actual key selected by native AddConst, not a reconstructed
     * bytecode/equality guess. Every originating symtable/AST scope and its
     * parent chain must be identical. An outer finally can re-emit the parent
     * ClassDef too: its second unit may disappear when that parent's AddConst
     * selects the first code object. No unit is reparented here; final tree
     * collection still requires each retained code's exact parent unit. */
    for (Py_ssize_t i = 0; i < unit->captures.count; i++) {
        if (unit->captures.items[i].child == original) {
            unit->captures.items[i].child = (PyCodeObject *)key;
        }
    }
    for (Py_ssize_t i = 0; i < unit->reference_origins.count; i++) {
        soac_reference_origin *origin = &unit->reference_origins.items[i];
        if (origin->family == SOAC_ORIGIN_CALL && origin->child == original) {
            origin->child = (PyCodeObject *)key;
        }
    }
    return SUCCESS;
}

static int
soac_collect_final_tree(soac_binding_collector *collector, PyCodeObject *code,
                        soac_code_bindings *parent, PyObject *nodes)
{
    if (Py_EnterRecursiveCall(" while collecting native class bindings")) {
        return ERROR;
    }
    int result = ERROR;
    soac_code_bindings *unit = soac_unit_for_code(collector, code);
    if (unit == NULL) {
        goto done;
    }
    if (unit->final_id >= 0 || unit->parent != parent) {
        soac_binding_error("ambiguous or reparented final native code tree");
        goto done;
    }
    unit->final_id = PyList_GET_SIZE(nodes);
    PyObject *parent_id = soac_optional_id(parent == NULL ? -1 : parent->final_id);
    PyObject *span = soac_source_span(unit->loc);
    if (parent_id == NULL || span == NULL) {
        Py_XDECREF(parent_id);
        Py_XDECREF(span);
        goto done;
    }
    PyObject *row = Py_BuildValue("(nOOiiO)", unit->final_id, parent_id, code,
                                  unit->scope_kind, unit->symtable_kind, span);
    Py_DECREF(parent_id);
    Py_DECREF(span);
    if (soac_append_owned(nodes, row) < 0) {
        goto done;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(code->co_consts); i++) {
        PyObject *constant = PyTuple_GET_ITEM(code->co_consts, i);
        if (PyCode_Check(constant) && soac_collect_final_tree(
                collector, (PyCodeObject *)constant, unit, nodes) < 0) {
            goto done;
        }
    }
    result = SUCCESS;
done:
    Py_LeaveRecursiveCall();
    return result;
}

static PyObject *
soac_owner_rows(soac_code_bindings *unit);

static int
soac_same_location(location left, location right)
{
    return left.lineno == right.lineno && left.end_lineno == right.end_lineno &&
        left.col_offset == right.col_offset && left.end_col_offset == right.end_col_offset;
}

static int
soac_same_slot(soac_code_bindings *unit, Py_ssize_t left, Py_ssize_t right)
{
    return unit->slots.items[left].final_index == unit->slots.items[right].final_index;
}

static Py_ssize_t
soac_region_representative(soac_code_bindings *unit, Py_ssize_t region)
{
    return region < 0 ? -1 : unit->regions.items[region].representative;
}

static int
soac_same_region_shape(soac_code_bindings *unit,
                        soac_binding_region *left, soac_binding_region *right)
{
    if (!soac_same_location(left->loc, right->loc) ||
        !soac_same_location(left->outer_loc, right->outer_loc) ||
        left->expression != right->expression || left->kind != right->kind ||
        left->is_async != right->is_async ||
        left->entry_ops.count != right->entry_ops.count ||
        soac_region_representative(unit, left->parent) !=
            soac_region_representative(unit, right->parent)) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < left->entry_ops.count; i++) {
        soac_binding_entry_op *a = &left->entry_ops.items[i];
        soac_binding_entry_op *b = &right->entry_ops.items[i];
        if (a->role != b->role || !soac_same_slot(unit, a->slot, b->slot) ||
            unit->owners.items[a->owner].kind != unit->owners.items[b->owner].kind) {
            return 0;
        }
    }
    return 1;
}

static int
soac_same_access(soac_code_bindings *unit,
                 soac_binding_access *left, soac_binding_access *right)
{
    return left->origin == right->origin &&
        soac_same_location(left->loc, right->loc) &&
        left->context == right->context && left->mode == right->mode &&
        soac_same_slot(unit, left->slot, right->slot);
}

static int
soac_same_capture(soac_code_bindings *unit,
                  soac_binding_capture *left, soac_binding_capture *right)
{
    return left->child == right->child && left->free_ordinal == right->free_ordinal &&
        soac_same_location(left->loc, right->loc) &&
        soac_same_slot(unit, left->slot, right->slot);
}

static int
soac_same_region_body(soac_code_bindings *unit, Py_ssize_t left, Py_ssize_t right)
{
    Py_ssize_t i = 0, j = 0;
    for (;;) {
        while (i < unit->accesses.count && unit->accesses.items[i].region != left) i++;
        while (j < unit->accesses.count && unit->accesses.items[j].region != right) j++;
        if (i == unit->accesses.count || j == unit->accesses.count) {
            if (i != unit->accesses.count || j != unit->accesses.count) return 0;
            break;
        }
        if (!soac_same_access(unit, &unit->accesses.items[i++], &unit->accesses.items[j++])) {
            return 0;
        }
    }
    i = j = 0;
    for (;;) {
        while (i < unit->captures.count && unit->captures.items[i].region != left) i++;
        while (j < unit->captures.count && unit->captures.items[j].region != right) j++;
        if (i == unit->captures.count || j == unit->captures.count) {
            return i == unit->captures.count && j == unit->captures.count;
        }
        if (!soac_same_capture(unit, &unit->captures.items[i++], &unit->captures.items[j++])) {
            return 0;
        }
    }
}

static int
soac_same_region_scope_data(soac_code_bindings *unit, Py_ssize_t left, Py_ssize_t right)
{
    Py_ssize_t i = 0, j = 0;
    for (;;) {
        while (i < unit->scope_source_bindings.count && unit->scope_source_bindings.items[i].region != left) i++;
        while (j < unit->scope_source_bindings.count && unit->scope_source_bindings.items[j].region != right) j++;
        if (i == unit->scope_source_bindings.count || j == unit->scope_source_bindings.count) {
            return i == unit->scope_source_bindings.count && j == unit->scope_source_bindings.count;
        }
        soac_scope_source_binding *a = &unit->scope_source_bindings.items[i++];
        soac_scope_source_binding *b = &unit->scope_source_bindings.items[j++];
        soac_reference_origin *x = soac_operation_at(unit, a->origin);
        soac_reference_origin *y = soac_operation_at(unit, b->origin);
        if (a->role != b->role || a->generator != b->generator ||
            x->origin != y->origin || x->kind != y->kind || x->phase != y->phase ||
            x->initial_opcode != y->initial_opcode || x->initial_slot != y->initial_slot ||
            !soac_same_location(x->loc, y->loc)) {
            return 0;
        }
    }
}

static int
soac_normalize_class_bindings(soac_code_bindings *unit)
{
    /* Native finally codegen can visit one original AST twice. Normalize only
     * after final slot fixup and actual AddConst canonicalization, preserving
     * every scoped operation and rejecting different choices for one origin. */
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        soac_binding_region *region = &unit->regions.items[i];
        for (Py_ssize_t j = 0; j < i; j++) {
            soac_binding_region *previous = &unit->regions.items[j];
            if (previous->representative != j || previous->origin != region->origin) continue;
            if (!soac_same_region_shape(unit, region, previous)) {
                return soac_binding_error("conflicting repeated original comprehension");
            }
            region->representative = j;
            for (Py_ssize_t k = 0; k < region->entry_ops.count; k++) {
                Py_ssize_t owner = region->entry_ops.items[k].owner;
                unit->owners.items[owner].representative = previous->entry_ops.items[k].owner;
            }
            break;
        }
        region->final_id = region->representative == i ? unit->final_region_count++
            : unit->regions.items[region->representative].final_id;
    }
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        Py_ssize_t previous = unit->regions.items[i].representative;
        if (previous != i && (!soac_same_region_body(unit, i, previous) ||
                              !soac_same_region_scope_data(unit, i, previous))) {
            return soac_binding_error("conflicting repeated comprehension body bindings");
        }
    }
    for (Py_ssize_t i = 0; i < unit->owners.count; i++) {
        soac_binding_owner *owner = &unit->owners.items[i];
        if (owner->representative < 0 || owner->representative > i) {
            return soac_binding_error("invalid canonical owner ordering");
        }
        owner->final_id = owner->representative == i ? unit->final_owner_count++
            : unit->owners.items[owner->representative].final_id;
    }
    for (Py_ssize_t i = 0; i < unit->accesses.count; i++) {
        soac_binding_access *access = &unit->accesses.items[i];
        for (Py_ssize_t j = 0; j < i; j++) {
            soac_binding_access *previous = &unit->accesses.items[j];
            if (!previous->emit || previous->context != access->context ||
                !soac_same_location(previous->loc, access->loc)) continue;
            if (!soac_same_access(unit, access, previous) ||
                soac_region_representative(unit, access->region) !=
                    soac_region_representative(unit, previous->region)) {
                return soac_binding_error("conflicting repeated original Name access");
            }
            access->emit = 0;
            break;
        }
    }
    for (Py_ssize_t i = 0; i < unit->captures.count; i++) {
        soac_binding_capture *capture = &unit->captures.items[i];
        for (Py_ssize_t j = 0; j < i; j++) {
            soac_binding_capture *previous = &unit->captures.items[j];
            if (!previous->emit || previous->child != capture->child ||
                previous->free_ordinal != capture->free_ordinal ||
                !soac_same_location(previous->loc, capture->loc)) continue;
            if (!soac_same_capture(unit, capture, previous) ||
                soac_region_representative(unit, capture->region) !=
                    soac_region_representative(unit, previous->region)) {
                return soac_binding_error("conflicting repeated original closure capture");
            }
            capture->emit = 0;
            break;
        }
    }
    return SUCCESS;
}

static PyObject *
soac_owner_rows(soac_code_bindings *unit)
{
    PyObject *rows = PyTuple_New(unit->final_owner_count);
    if (rows == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->owners.count; i++) {
        soac_binding_owner *owner = &unit->owners.items[i];
        if (owner->representative != i) {
            continue;
        }
        soac_binding_slot *slot = &unit->slots.items[owner->slot];
        unsigned char kind = (unsigned char)PyBytes_AS_STRING(
            unit->code->co_localspluskinds)[slot->final_index];
        PyObject *region = soac_optional_id(owner->region < 0 ? -1 :
            unit->regions.items[owner->region].final_id);
        if (region == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyObject *row = Py_BuildValue("(niiiO)", owner->final_id, owner->kind,
            slot->final_index, (int)kind, region);
        Py_DECREF(region);
        if (row == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyTuple_SET_ITEM(rows, owner->final_id, row);
    }
    return rows;
}

static PyObject *
soac_current_slot(soac_code_bindings *unit, Py_ssize_t slot)
{
    return Py_BuildValue("(ii)", Py_SOAC_CLASS_CURRENT_SLOT,
                         unit->slots.items[slot].final_index);
}

static PyObject *
soac_capture_rows(soac_binding_collector *collector, soac_code_bindings *unit)
{
    PyObject *rows = PyList_New(0);
    if (rows == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->captures.count; i++) {
        soac_binding_capture *capture = &unit->captures.items[i];
        if (!capture->emit) {
            continue;
        }
        soac_code_bindings *child = soac_unit_for_code(collector, capture->child);
        if (child == NULL) {
            goto error;
        }
        if (child->final_id < 0) {
            /* Native CFG optimization removed this original child constant. */
            continue;
        }
        if (child->parent != unit || capture->free_ordinal < 0 ||
            capture->free_ordinal >= child->code->co_nfreevars) {
            soac_binding_error("closure edge disagrees with the actual native child");
            goto error;
        }
        PyObject *span = soac_source_span(capture->loc);
        PyObject *slot = soac_current_slot(unit, capture->slot);
        if (span == NULL || slot == NULL) {
            Py_XDECREF(span);
            Py_XDECREF(slot);
            goto error;
        }
        PyObject *region = soac_optional_id(capture->region < 0 ? -1 :
            unit->regions.items[capture->region].final_id);
        PyObject *row = region == NULL ? NULL : Py_BuildValue("(nOiOO)", child->final_id, span,
                                      capture->free_ordinal, slot, region);
        Py_XDECREF(region);
        Py_DECREF(span);
        Py_DECREF(slot);
        if (soac_append_owned(rows, row) < 0) {
            goto error;
        }
    }
    /* A removed/provisionally deduplicated constant cannot silently erase a
     * required edge on a different final child. Coverage is by exact identity. */
    for (Py_ssize_t i = 0; i < collector->units.count; i++) {
        soac_code_bindings *child = collector->units.items[i];
        if (child->final_id < 0 || child->parent != unit) {
            continue;
        }
        for (int ordinal = 0; ordinal < child->code->co_nfreevars; ordinal++) {
            int found = 0;
            for (Py_ssize_t j = 0; j < unit->captures.count; j++) {
                soac_binding_capture *capture = &unit->captures.items[j];
                if (capture->child == child->code && capture->free_ordinal == ordinal) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                soac_binding_error("final native child has no closure binding edge");
                goto error;
            }
        }
    }
    PyObject *result = PyList_AsTuple(rows);
    Py_DECREF(rows);
    return result;
error:
    Py_DECREF(rows);
    return NULL;
}

static PyObject *
soac_export_rows(soac_code_bindings *unit)
{
    PyObject *rows = PyTuple_New(unit->exports.count);
    if (rows == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->exports.count; i++) {
        soac_binding_export *entry = &unit->exports.items[i];
        PyObject *slot = soac_current_slot(unit, entry->slot);
        if (slot == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyObject *row = Py_BuildValue("(iO)", entry->role, slot);
        Py_DECREF(slot);
        if (row == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyTuple_SET_ITEM(rows, i, row);
    }
    return rows;
}

static PyObject *
soac_access_rows(soac_code_bindings *unit)
{
    Py_ssize_t count = 0;
    for (Py_ssize_t i = 0; i < unit->accesses.count; i++) count += unit->accesses.items[i].emit;
    PyObject *rows = PyTuple_New(count);
    if (rows == NULL) {
        return NULL;
    }
    Py_ssize_t output_index = 0;
    for (Py_ssize_t i = 0; i < unit->accesses.count; i++) {
        soac_binding_access *access = &unit->accesses.items[i];
        if (!access->emit) {
            continue;
        }
        PyObject *span = soac_source_span(access->loc);
        PyObject *slot = soac_current_slot(unit, access->slot);
        if (span == NULL || span == Py_None || slot == NULL) {
            if (!PyErr_Occurred()) {
                soac_binding_error("original Name access has no source span");
            }
            Py_XDECREF(span);
            Py_XDECREF(slot);
            Py_DECREF(rows);
            return NULL;
        }
        PyObject *region = soac_optional_id(access->region < 0 ? -1 :
            unit->regions.items[access->region].final_id);
        PyObject *row = region == NULL ? NULL : Py_BuildValue("(OiiOO)", span, access->context,
                                      access->mode, slot, region);
        Py_XDECREF(region);
        Py_DECREF(span);
        Py_DECREF(slot);
        if (row == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyTuple_SET_ITEM(rows, output_index++, row);
    }
    return rows;
}

/* Data-only source operation provenance. Original AST identities and native
 * instruction lanes are compile-local; only immutable value rows escape. */
static int
soac_operation_location(location loc)
{
    return loc.lineno >= 1 && loc.end_lineno >= loc.lineno &&
        loc.col_offset >= 0 && loc.end_col_offset >= 0 &&
        (loc.end_lineno != loc.lineno || loc.end_col_offset > loc.col_offset);
}

static int
soac_push_operation(soac_code_bindings *unit, soac_reference_origin origin,
                    uint32_t *out)
{
    if (unit->reference_origins.count >= UINT32_MAX) {
        return soac_binding_error("too many native source operation origins");
    }
    RETURN_IF_ERROR(SOAC_PUSH(unit->reference_origins, origin));
    *out = (uint32_t)unit->reference_origins.count;
    return SUCCESS;
}

static soac_reference_origin *
soac_operation_at(soac_code_bindings *unit, uint32_t id)
{
    assert(id <= unit->reference_origins.count);
    return id == 0 ? NULL : &unit->reference_origins.items[id - 1];
}

static int
soac_attach_operation(compiler *c, uint32_t id)
{
    if (id == 0) {
        return SUCCESS;
    }
    instr_sequence *seq = c->u->u_instr_sequence;
    if (seq->s_used == 0) {
        return soac_binding_error("source operation has no emitted instruction");
    }
    _PyInstruction *instruction = &seq->s_instrs[seq->s_used - 1];
    if (instruction->i_soac_origins.lane[0] != 0 ||
        instruction->i_soac_origins.lane[1] != 0) {
        return soac_binding_error("two source operations claim one native lane");
    }
    instruction->i_soac_origins.lane[0] = id;
    return SUCCESS;
}


static asdl_comprehension_seq *
soac_scope_generators(const soac_binding_region *region)
{
    switch (region->expression->kind) {
        case ListComp_kind: return region->expression->v.ListComp.generators;
        case SetComp_kind: return region->expression->v.SetComp.generators;
        case DictComp_kind: return region->expression->v.DictComp.generators;
        default: Py_UNREACHABLE();
    }
}


_PySoacScopeBindingContext
_PyCompile_SoacScopeBindingContext(compiler *c, int role, int generator)
{
    _PySoacScopeBindingContext previous = {-1, -1};
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit != NULL && unit->active_region >= 0) {
        previous = (_PySoacScopeBindingContext){unit->scope_binding_role,
                                                unit->scope_binding_generator};
        unit->scope_binding_role = role;
        unit->scope_binding_generator = generator;
    }
    return previous;
}

void
_PyCompile_SoacRestoreScopeBindingContext(compiler *c, _PySoacScopeBindingContext previous)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit != NULL && unit->active_region >= 0) {
        unit->scope_binding_role = previous.role;
        unit->scope_binding_generator = previous.generator;
    }
}


int
_PyCompile_SoacPushContext(compiler *c, int owner_kind,
                          location owner_loc, const void *owner,
                          int item, int entry, location transfer_loc,
                          const void *transfer, int payload,
                          Py_ssize_t *previous)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    *previous = -1;
    if (unit == NULL) {
        return SUCCESS;
    }
    *previous = unit->active_emission_context;
    if (unit->active_emission_context == -2 || owner == NULL) {
        unit->active_emission_context = -2;
        return SUCCESS;
    }
    if (!soac_operation_location(owner_loc) ||
        owner_kind < Py_SOAC_CONTEXT_TRY_FINALLY ||
        owner_kind > Py_SOAC_CONTEXT_ASYNC_WITH_EXIT ||
        entry < Py_SOAC_CONTEXT_FALLTHROUGH || entry > Py_SOAC_CONTEXT_CONTINUE ||
        payload < Py_SOAC_CONTEXT_NO_PAYLOAD || payload > Py_SOAC_CONTEXT_EXCEPTION_VALUE ||
        ((owner_kind >= Py_SOAC_CONTEXT_WITH_EXIT) != (item >= 0)) ||
        ((entry >= Py_SOAC_CONTEXT_RETURN) != (transfer != NULL)) ||
        (transfer != NULL && !soac_operation_location(transfer_loc))) {
        return soac_binding_error("invalid original cleanup continuation");
    }
    soac_emission_context context = {
        owner, owner_loc, owner_kind, item, entry, transfer, transfer_loc,
        payload, unit->active_emission_context,
    };
    RETURN_IF_ERROR(SOAC_PUSH(unit->emission_contexts, context));
    unit->active_emission_context = unit->emission_contexts.count - 1;
    return SUCCESS;
}

void
_PyCompile_SoacPopContext(compiler *c, Py_ssize_t previous)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit != NULL) {
        assert(previous >= -2 && previous < unit->emission_contexts.count);
        unit->active_emission_context = previous;
    }
}

Py_ssize_t
_PyCompile_SoacBeginUnwindContext(compiler *c)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return -1;
    }
    Py_ssize_t previous = unit->active_emission_context;
    /* A transfer from inside an already visited finally replaces pending
     * control. Until that replacement is proven against the native fblocks,
     * do not copy its old transfer into an outer cleanup's provenance. */
    if (previous != -1) {
        unit->active_emission_context = -2;
    }
    return previous;
}

static int
soac_store_form(int opcode)
{
    switch (opcode) {
        case STORE_FAST: return Py_SOAC_STORE_FAST;
        case STORE_FAST_STORE_FAST: return Py_SOAC_STORE_FAST_PAIR;
        case STORE_FAST_LOAD_FAST: return Py_SOAC_STORE_FAST_THEN_LOAD;
        case STORE_DEREF: return Py_SOAC_STORE_CELL;
        case STORE_NAME: return Py_SOAC_STORE_NAME;
        case STORE_GLOBAL: return Py_SOAC_STORE_GLOBAL;
        case STORE_ATTR: return Py_SOAC_STORE_ATTRIBUTE;
        case STORE_SUBSCR: return Py_SOAC_STORE_SUBSCRIPT;
        case STORE_SLICE: return Py_SOAC_STORE_SLICE;
        case DELETE_FAST: return Py_SOAC_DELETE_FAST;
        case DELETE_DEREF: return Py_SOAC_DELETE_CELL;
        case DELETE_NAME: return Py_SOAC_DELETE_NAME;
        case DELETE_GLOBAL: return Py_SOAC_DELETE_GLOBAL;
        case DELETE_ATTR: return Py_SOAC_DELETE_ATTRIBUTE;
        case DELETE_SUBSCR: return Py_SOAC_DELETE_SUBSCRIPT;
        default: return -1;
    }
}

static int
soac_call_form(int opcode)
{
    switch (opcode) {
        case CALL: return Py_SOAC_CALL_POSITIONAL;
        case CALL_KW: return Py_SOAC_CALL_KEYWORDS;
        case CALL_FUNCTION_EX: return Py_SOAC_CALL_EXPANDED;
        default: return -1;
    }
}


int
_PyCompile_SoacBindingOrigin(compiler *c, location source_loc,
                            const void *original, int kind, int phase,
                            expr_context_ty context)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    instr_sequence *seq = c->u->u_instr_sequence;
    if (original == NULL || seq->s_used == 0 || !soac_operation_location(source_loc) ||
        kind < Py_SOAC_BINDING_NAME || kind > Py_SOAC_BINDING_SUBSCRIPT ||
        phase < Py_SOAC_BINDING_PUBLISH || phase > Py_SOAC_BINDING_CLEANUP_DELETE) {
        return soac_binding_error("invalid original source binding");
    }
    _PyInstruction *instruction = &seq->s_instrs[seq->s_used - 1];
    int opcode = instruction->i_opcode;
    if ((context != Store && context != Del) || soac_store_form(opcode) < 0) {
        return soac_binding_error("source binding disagrees with native operation");
    }
    soac_reference_origin origin = {
        .origin = original, .loc = source_loc, .kind = kind,
        .context = context, .phase = phase,
        .family = SOAC_ORIGIN_STORE,
        .initial_opcode = opcode, .initial_slot = instruction->i_oparg,
        .emission_context = unit->active_emission_context,
        .keyword_constant = -1,
    };
    uint32_t id;
    RETURN_IF_ERROR(soac_push_operation(unit, origin, &id));
    if (context == Store && unit->active_region >= 0 && unit->scope_binding_role >= 0) {
        soac_scope_source_binding binding = {unit->active_region,
            unit->scope_binding_role, unit->scope_binding_generator, id};
        RETURN_IF_ERROR(SOAC_PUSH(unit->scope_source_bindings, binding));
    }
    return soac_attach_operation(c, id);
}

int
_PyCompile_SoacReferenceOrigin(compiler *c, location source_loc,
                              const void *original, int kind,
                              expr_context_ty context)
{
    int phase = context == Del ? Py_SOAC_BINDING_SOURCE_DELETE : Py_SOAC_BINDING_PUBLISH;
    return _PyCompile_SoacBindingOrigin(c, source_loc, original, kind, phase, context);
}

int
_PyCompile_SoacCallStart(compiler *c, location source_loc,
                        const void *original, int kind, int detail,
                        PyCodeObject *child, uint32_t *out)
{
    *out = 0;
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (original == NULL || !soac_operation_location(source_loc) ||
        kind < Py_SOAC_CALL_SOURCE || kind > Py_SOAC_CALL_ASSERT) {
        return soac_binding_error("invalid original Call producer");
    }
    PyObject *index = detail < 0 ? NULL : PyLong_FromLong(detail);
    if (detail >= 0 && index == NULL) {
        return ERROR;
    }
    soac_reference_origin origin = {
        .origin = original, .loc = source_loc, .kind = kind,
        .family = SOAC_ORIGIN_CALL, .initial_opcode = -1, .initial_slot = -1,
        .emission_context = unit->active_emission_context,
        .detail = index, .child = child, .positional_kind = -1,
        .keyword_kind = -1, .keyword_constant = -1,
    };
    if (soac_push_operation(unit, origin, out) < 0) {
        Py_XDECREF(index);
        return ERROR;
    }
    return SUCCESS;
}

int
_PyCompile_SoacCallInput(compiler *c, uint32_t id, int channel,
                        int preloaded, int positional_kind,
                        asdl_expr_seq *args, int injected_generic_base,
                        int keyword_kind, asdl_keyword_seq *keywords)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    soac_reference_origin *origin = soac_operation_at(unit, id);
    if (origin->family != SOAC_ORIGIN_CALL || origin->positional_entries != NULL ||
        channel < 0 || channel > 2 || preloaded < 0 ||
        positional_kind < 0 || positional_kind > 5 ||
        keyword_kind < 0 || keyword_kind > 2) {
        return soac_binding_error("invalid native Call input producer");
    }
    Py_ssize_t count = asdl_seq_LEN(args);
    PyObject *positional = PyTuple_New(count + (injected_generic_base != 0));
    PyObject *kw = PyTuple_New(asdl_seq_LEN(keywords));
    PyObject *groups = PyList_New(0);
    if (positional == NULL || kw == NULL || groups == NULL) {
        goto error;
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        expr_ty argument = asdl_seq_GET(args, i);
        PyObject *span = soac_source_span(SRC_LOCATION_FROM_AST(argument));
        PyObject *row = span == NULL ? NULL : Py_BuildValue("(iO)",
            argument->kind == Starred_kind ? Py_SOAC_POSITIONAL_STAR : Py_SOAC_POSITIONAL_SOURCE,
            span);
        Py_XDECREF(span);
        if (row == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(positional, i, row);
    }
    if (injected_generic_base) {
        PyObject *row = Py_BuildValue("(iO)", Py_SOAC_POSITIONAL_GENERIC_BASE, Py_None);
        if (row == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(positional, count, row);
    }
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(keywords); i++) {
        keyword_ty keyword = asdl_seq_GET(keywords, i);
        PyObject *span = soac_source_span(SRC_LOCATION_FROM_AST(keyword));
        PyObject *row = span == NULL ? NULL : Py_BuildValue("(iOO)",
            keyword->arg == NULL ? Py_SOAC_KEYWORD_MAPPING : Py_SOAC_KEYWORD_NAMED,
            span, keyword->arg == NULL ? Py_None : keyword->arg);
        Py_XDECREF(span);
        if (row == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(kw, i, row);
    }
    origin->channel = channel;
    origin->preloaded = preloaded;
    origin->positional_kind = positional_kind;
    origin->keyword_kind = keyword_kind;
    origin->positional_entries = positional;
    origin->keyword_entries = kw;
    origin->keyword_groups = groups;
    return SUCCESS;
error:
    Py_XDECREF(positional);
    Py_XDECREF(kw);
    Py_XDECREF(groups);
    return ERROR;
}

int
_PyCompile_SoacCallGroup(compiler *c, uint32_t id, int kind,
                        Py_ssize_t first, Py_ssize_t count, int map_style)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_reference_origin *origin = soac_operation_at(c->u->u_metadata.u_soac_bindings, id);
    if (origin->keyword_kind != Py_SOAC_KEYWORDS_EXPANDED_GROUPS ||
        origin->keyword_groups == NULL || first < 0 || count < 1) {
        return soac_binding_error("keyword group has no original Call inputs");
    }
    PyObject *style = soac_optional_id(map_style);
    PyObject *row = style == NULL ? NULL : Py_BuildValue("(innO)", kind, first, count, style);
    Py_XDECREF(style);
    return soac_append_owned(origin->keyword_groups, row);
}

int
_PyCompile_SoacCallPreparation(compiler *c, uint32_t id)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    soac_reference_origin *call = soac_operation_at(unit, id);
    instr_sequence *seq = c->u->u_instr_sequence;
    if (call->family != SOAC_ORIGIN_CALL || seq->s_used == 0) {
        return soac_binding_error("preparation has no original Call");
    }
    _PyInstruction *instruction = &seq->s_instrs[seq->s_used - 1];
    soac_reference_origin origin = {
        .origin = call->origin, .loc = call->loc, .kind = call->kind,
        .family = SOAC_ORIGIN_CALL_PREPARATION,
        .initial_opcode = instruction->i_opcode,
        .initial_slot = instruction->i_oparg,
        .emission_context = call->emission_context, .call_owner = id,
        .keyword_constant = -1,
    };
    uint32_t preparation;
    RETURN_IF_ERROR(soac_push_operation(unit, origin, &preparation));
    return soac_attach_operation(c, preparation);
}

int
_PyCompile_SoacCallKeywordNames(compiler *c, uint32_t id, PyObject *names)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_reference_origin *origin = soac_operation_at(c->u->u_metadata.u_soac_bindings, id);
    if (origin->family != SOAC_ORIGIN_CALL ||
        origin->keyword_kind != Py_SOAC_KEYWORDS_NAMES_TUPLE ||
        origin->keyword_names != NULL || !PyTuple_CheckExact(names)) {
        return soac_binding_error("keyword names are not the original native tuple producer");
    }
    origin->keyword_names = Py_NewRef(names);
    return SUCCESS;
}

int
_PyCompile_SoacCallKeywordConstant(compiler *c, uint32_t id)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_reference_origin *origin = soac_operation_at(c->u->u_metadata.u_soac_bindings, id);
    instr_sequence *seq = c->u->u_instr_sequence;
    if (origin->keyword_kind != Py_SOAC_KEYWORDS_NAMES_TUPLE ||
        origin->keyword_names == NULL || seq->s_used == 0 ||
        seq->s_instrs[seq->s_used - 1].i_opcode != LOAD_CONST) {
        return soac_binding_error("keyword names lack their actual constant producer");
    }
    origin->keyword_constant = seq->s_instrs[seq->s_used - 1].i_oparg;
    return _PyCompile_SoacCallPreparation(c, id);
}

int
_PyCompile_SoacCallEmit(compiler *c, uint32_t id)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_reference_origin *origin = soac_operation_at(c->u->u_metadata.u_soac_bindings, id);
    instr_sequence *seq = c->u->u_instr_sequence;
    if (origin->family != SOAC_ORIGIN_CALL || seq->s_used == 0 ||
        soac_call_form(seq->s_instrs[seq->s_used - 1].i_opcode) < 0 ||
        origin->initial_opcode >= 0) {
        return soac_binding_error("Call origin disagrees with native CALL emission");
    }
    origin->initial_opcode = seq->s_instrs[seq->s_used - 1].i_opcode;
    origin->initial_slot = seq->s_instrs[seq->s_used - 1].i_oparg;
    return soac_attach_operation(c, id);
}

int
_PyCompile_SoacCallAlternative(compiler *c, uint32_t id, int reason)
{
    if (id == 0) {
        return SUCCESS;
    }
    soac_reference_origin *origin = soac_operation_at(c->u->u_metadata.u_soac_bindings, id);
    if (origin->family != SOAC_ORIGIN_CALL ||
        (reason != Py_SOAC_OPERATION_GAP_GUARDED_NONCALL &&
         reason != Py_SOAC_OPERATION_GAP_LOWERED_NONCALL)) {
        return soac_binding_error("invalid native non-CALL alternative");
    }
    origin->alternative = reason;
    return SUCCESS;
}

PyObject *
_PyCompile_SoacPatternLeaf(compiler *c, pattern_ty pattern, int kind)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    assert(unit != NULL);
    if (pattern == NULL || kind < 0 || kind > 2 || !soac_operation_location(SRC_LOCATION_FROM_AST(pattern))) {
        soac_binding_error("invalid original pattern capture leaf");
        return NULL;
    }
    soac_reference_origin origin = {
        .origin = pattern, .loc = SRC_LOCATION_FROM_AST(pattern), .kind = kind,
        .family = SOAC_ORIGIN_PATTERN_LEAF, .context = Store,
        .initial_opcode = -1, .initial_slot = -1,
        .emission_context = unit->active_emission_context, .keyword_constant = -1,
    };
    uint32_t id;
    if (soac_push_operation(unit, origin, &id) < 0) {
        return NULL;
    }
    PyObject *number = PyLong_FromUnsignedLong(id);
    PyObject *tuple = number == NULL ? NULL : PyTuple_Pack(1, number);
    Py_XDECREF(number);
    return tuple;
}

int
_PyCompile_SoacPatternStore(compiler *c, pattern_ty owner, PyObject *leaf_origins)
{
    soac_code_bindings *unit = c->u->u_metadata.u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (!PyTuple_CheckExact(leaf_origins) || PyTuple_GET_SIZE(leaf_origins) == 0) {
        return soac_binding_error("pattern Store has no actual capture leaves");
    }
    PyObject *ordered = PyList_New(0), *detail = NULL, *identities = NULL;
    if (ordered == NULL) {
        return ERROR;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(leaf_origins); i++) {
        unsigned long id = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(leaf_origins, i));
        if (PyErr_Occurred()) {
            goto error;
        }
        if (id == 0 || id > (unsigned long)unit->reference_origins.count) {
            soac_binding_error("pattern leaf origin is outside its native compile unit");
            goto error;
        }
        soac_reference_origin *leaf = soac_operation_at(unit, (uint32_t)id);
        if (leaf->family != SOAC_ORIGIN_PATTERN_LEAF) {
            soac_binding_error("pattern leaf refers to a different source operation");
            goto error;
        }
        PyObject *span = soac_source_span(leaf->loc);
        PyObject *row = span == NULL ? NULL : Py_BuildValue("(Oik)", span, leaf->kind, id);
        Py_XDECREF(span);
        if (soac_append_owned(ordered, row) < 0) {
            goto error;
        }
    }
    if (PyList_Sort(ordered) < 0) {
        goto error;
    }
    Py_ssize_t count = PyList_GET_SIZE(ordered);
    detail = PyTuple_New(count);
    identities = PyTuple_New(count);
    if (detail == NULL || identities == NULL) {
        goto error;
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *row = PyList_GET_ITEM(ordered, i);
        PyObject *span = PyTuple_GET_ITEM(row, 0), *kind = PyTuple_GET_ITEM(row, 1);
        if (i > 0) {
            PyObject *previous = PyList_GET_ITEM(ordered, i - 1);
            int equal = PyObject_RichCompareBool(span, PyTuple_GET_ITEM(previous, 0), Py_EQ);
            if (equal < 0) {
                goto error;
            }
            if (equal) {
                int same_kind = PyObject_RichCompareBool(kind, PyTuple_GET_ITEM(previous, 1), Py_EQ);
                if (same_kind < 0) {
                    goto error;
                }
                if (same_kind) {
                    soac_binding_error("ambiguous original pattern capture leaf");
                    goto error;
                }
            }
        }
        PyObject *leaf = PyTuple_Pack(2, kind, span);
        if (leaf == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(detail, i, leaf);
        PyTuple_SET_ITEM(identities, i, Py_NewRef(PyTuple_GET_ITEM(row, 2)));
    }
    if (_PyCompile_SoacBindingOrigin(c, SRC_LOCATION_FROM_AST(owner), owner,
            Py_SOAC_BINDING_PATTERN_CAPTURE, Py_SOAC_BINDING_PUBLISH, Store) < 0) {
        goto error;
    }
    soac_reference_origin *origin = &unit->reference_origins.items[unit->reference_origins.count - 1];
    origin->detail = detail;
    origin->pattern_leaves = identities;
    Py_DECREF(ordered);
    return SUCCESS;
error:
    Py_XDECREF(ordered);
    Py_XDECREF(detail);
    Py_XDECREF(identities);
    return ERROR;
}


int
_PyCompile_SoacFinalReferenceInstruction(_PyCompile_CodeUnitMetadata *umd,
                                         int ordinal, int opcode, int oparg,
                                         _PySoacReadOrigins origins)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) return SUCCESS;
    if (unit->reference_instruction_count >= 0 || ordinal != unit->reference_instructions.count ||
        origins.lane[0] > unit->reference_origins.count || origins.lane[1] > unit->reference_origins.count) {
        return soac_binding_error("invalid final source operation provenance");
    }
    /* Native ordinals identify interpreter publication/CALL sites; this is
     * neither a SOAC execution schedule nor a lifetime/handler recipe. */
    soac_reference_instruction instruction = {
        .ordinal = ordinal, .opcode = opcode, .oparg = oparg, .origins = origins,
        .opcode_offset = -1, .first_byte = -1, .end_byte = -1,
    };
    return SOAC_PUSH(unit->reference_instructions, instruction);
}

int
_PyCompile_SoacFinishReferences(_PyCompile_CodeUnitMetadata *umd, int count)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (count < 0 || unit->reference_instruction_count >= 0 ||
        unit->active_emission_context != -1) {
        return soac_binding_error("native source operation finalization is incomplete");
    }
    if (count != unit->reference_instructions.count) {
        return soac_binding_error("native instruction observations are not dense");
    }
    unit->reference_instruction_count = count;
    return SUCCESS;
}


int
_PyCompile_SoacAssembledInstruction(_PyCompile_CodeUnitMetadata *umd,
                                    int ordinal, const _PyInstruction *instruction,
                                    Py_ssize_t opcode_offset,
                                    Py_ssize_t first_byte, Py_ssize_t end_byte)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (ordinal != unit->assembled_instruction_count || ordinal < 0 ||
        ordinal >= unit->reference_instruction_count || unit->assembled_code_size >= 0 ||
        first_byte < 0 || first_byte > opcode_offset || opcode_offset >= end_byte ||
        first_byte % sizeof(_Py_CODEUNIT) || opcode_offset % sizeof(_Py_CODEUNIT) ||
        end_byte % sizeof(_Py_CODEUNIT) ||
        first_byte != (ordinal == 0 ? 0 : unit->reference_instructions.items[ordinal - 1].end_byte)) {
        return soac_binding_error("native assembler lost final instruction correspondence");
    }
    soac_reference_instruction *expected = &unit->reference_instructions.items[ordinal];
    if (expected->ordinal != ordinal ||
        expected->origins.lane[0] != instruction->i_soac_origins.lane[0] ||
        expected->origins.lane[1] != instruction->i_soac_origins.lane[1]) {
        return soac_binding_error("assembled source operation differs from final native scan");
    }
    if (expected->origins.lane[0] || expected->origins.lane[1]) {
        if (expected->opcode != instruction->i_opcode || expected->oparg != instruction->i_oparg) {
            return soac_binding_error("assembled tagged operation changed its native form or operand");
        }
    }
    /* Ordinary jump resolution may change untagged opcodes/operands. The
     * actual assembler supplies their final identity, not a reconstructed CFG. */
    expected->opcode = instruction->i_opcode;
    expected->oparg = instruction->i_oparg;
    expected->opcode_offset = opcode_offset;
    expected->first_byte = first_byte;
    expected->end_byte = end_byte;
    unit->assembled_reference_cursor++;
    unit->assembled_instruction_count++;
    return SUCCESS;
}

int
_PyCompile_SoacFinishAssembly(_PyCompile_CodeUnitMetadata *umd, int count,
                             Py_ssize_t code_size)
{
    soac_code_bindings *unit = umd->u_soac_bindings;
    if (unit == NULL) {
        return SUCCESS;
    }
    if (unit->assembled_code_size >= 0 || count != unit->reference_instruction_count ||
        unit->assembled_instruction_count != count ||
        unit->assembled_reference_cursor != unit->reference_instructions.count ||
        code_size <= 0 || code_size % sizeof(_Py_CODEUNIT) != 0 ||
        code_size / sizeof(_Py_CODEUNIT) < (size_t)count) {
        return soac_binding_error("native assembled source operation inventory is incomplete");
    }
    if (count == 0 || unit->reference_instructions.items[count - 1].end_byte != code_size) {
        return soac_binding_error("native instruction extents do not cover assembled code");
    }
    unit->assembled_code_size = code_size;
    return SUCCESS;
}

/* Equality is over original compiler identities and semantic values, never
 * display names, bytecode text or the order SOAC happens to clone blocks. */
static int
soac_same_optional_value(PyObject *left, PyObject *right)
{
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return PyObject_RichCompareBool(left, right, Py_EQ);
}

static int
soac_same_emission_context(soac_code_bindings *left, Py_ssize_t x,
                           soac_code_bindings *right, Py_ssize_t y)
{
    while (x >= 0 && y >= 0) {
        assert(x < left->emission_contexts.count && y < right->emission_contexts.count);
        soac_emission_context *a = &left->emission_contexts.items[x];
        soac_emission_context *b = &right->emission_contexts.items[y];
        if (a->owner != b->owner || a->owner_kind != b->owner_kind ||
            !soac_same_location(a->owner_loc, b->owner_loc) || a->item != b->item ||
            a->entry != b->entry || a->transfer != b->transfer ||
            !soac_same_location(a->transfer_loc, b->transfer_loc) || a->payload != b->payload) {
            return 0;
        }
        x = a->parent;
        y = b->parent;
    }
    return x == y;
}

static int
soac_same_operation_identity(soac_code_bindings *left, soac_reference_origin *a,
                             soac_code_bindings *right, soac_reference_origin *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->origin != b->origin || a->family != b->family || a->kind != b->kind ||
        a->context != b->context || a->phase != b->phase || a->child != b->child ||
        !soac_same_location(a->loc, b->loc)) {
        return 0;
    }
    int same = soac_same_optional_value(a->detail, b->detail);
    if (same <= 0) {
        return same;
    }
    if (a->pattern_leaves == NULL || b->pattern_leaves == NULL) {
        return a->pattern_leaves == b->pattern_leaves;
    }
    if (PyTuple_GET_SIZE(a->pattern_leaves) != PyTuple_GET_SIZE(b->pattern_leaves)) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(a->pattern_leaves); i++) {
        unsigned long x = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(a->pattern_leaves, i));
        unsigned long y = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(b->pattern_leaves, i));
        if (PyErr_Occurred()) {
            return ERROR;
        }
        soac_reference_origin *first = soac_operation_at(left, (uint32_t)x);
        soac_reference_origin *second = soac_operation_at(right, (uint32_t)y);
        if (first->origin != second->origin || first->kind != second->kind ||
            !soac_same_location(first->loc, second->loc)) {
            return 0;
        }
    }
    return 1;
}

static int
soac_same_final_origin(soac_code_bindings *left, soac_reference_origin *a,
                       soac_code_bindings *right, soac_reference_origin *b)
{
    int same = soac_same_operation_identity(left, a, right, b);
    if (same <= 0 || a == NULL) {
        return same;
    }
    if (a->initial_opcode != b->initial_opcode || a->initial_slot != b->initial_slot ||
        a->channel != b->channel || a->preloaded != b->preloaded ||
        a->positional_kind != b->positional_kind || a->keyword_kind != b->keyword_kind ||
        a->keyword_constant != b->keyword_constant || a->alternative != b->alternative ||
        !soac_same_emission_context(left, a->emission_context, right, b->emission_context)) {
        return 0;
    }
    PyObject *a_values[] = {a->positional_entries, a->keyword_entries,
                            a->keyword_groups, a->keyword_names};
    PyObject *b_values[] = {b->positional_entries, b->keyword_entries,
                            b->keyword_groups, b->keyword_names};
    for (size_t i = 0; i < Py_ARRAY_LENGTH(a_values); i++) {
        same = soac_same_optional_value(a_values[i], b_values[i]);
        if (same <= 0) {
            return same;
        }
    }
    if (a->family == SOAC_ORIGIN_CALL_PREPARATION) {
        return soac_same_operation_identity(left, soac_operation_at(left, a->call_owner),
                                            right, soac_operation_at(right, b->call_owner));
    }
    return 1;
}

static int
soac_same_final_references(soac_code_bindings *left, soac_code_bindings *right)
{
    if (left->reference_instruction_count < 0 ||
        left->reference_instruction_count != right->reference_instruction_count ||
        left->assembled_code_size != right->assembled_code_size ||
        left->reference_origins.count != right->reference_origins.count ||
        left->reference_instructions.count != right->reference_instructions.count) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < left->reference_origins.count; i++) {
        int same = soac_same_final_origin(left, &left->reference_origins.items[i],
                                         right, &right->reference_origins.items[i]);
        if (same <= 0) {
            return same;
        }
    }
    for (Py_ssize_t i = 0; i < left->reference_instructions.count; i++) {
        soac_reference_instruction *a = &left->reference_instructions.items[i];
        soac_reference_instruction *b = &right->reference_instructions.items[i];
        if (a->ordinal != b->ordinal || a->opcode != b->opcode ||
            a->oparg != b->oparg || a->opcode_offset != b->opcode_offset ||
            a->first_byte != b->first_byte ||
            a->end_byte != b->end_byte) {
            return 0;
        }
        for (int lane = 0; lane < 2; lane++) {
            int same = soac_same_final_origin(left, soac_operation_at(left, a->origins.lane[lane]),
                                              right, soac_operation_at(right, b->origins.lane[lane]));
            if (same <= 0) {
                return same;
            }
        }
    }
    return 1;
}

static PyObject *
soac_context_row(soac_code_bindings *unit, Py_ssize_t id)
{
    if (id == -2) {
        return Py_NewRef(Py_None);
    }
    Py_ssize_t count = 0;
    for (Py_ssize_t parent = id; parent >= 0; parent = unit->emission_contexts.items[parent].parent) {
        assert(parent < unit->emission_contexts.count);
        count++;
    }
    PyObject *rows = PyTuple_New(count);
    if (rows == NULL) {
        return NULL;
    }
    while (id >= 0) {
        soac_emission_context *context = &unit->emission_contexts.items[id];
        PyObject *owner = soac_source_span(context->owner_loc);
        PyObject *item = soac_optional_id(context->item);
        PyObject *transfer = context->transfer == NULL
            ? Py_NewRef(Py_None) : soac_source_span(context->transfer_loc);
        PyObject *row = owner == NULL || item == NULL || transfer == NULL ? NULL :
            Py_BuildValue("(iOOiOi)", context->owner_kind, owner, item,
                          context->entry, transfer, context->payload);
        Py_XDECREF(owner);
        Py_XDECREF(item);
        Py_XDECREF(transfer);
        if (row == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyTuple_SET_ITEM(rows, --count, row);
        id = context->parent;
    }
    assert(id == -1 && count == 0);
    return rows;
}

static PyObject *
soac_binding_origin_row(soac_reference_origin *origin)
{
    if (origin == NULL || origin->family != SOAC_ORIGIN_STORE) {
        return Py_NewRef(Py_None);
    }
    PyObject *span = soac_source_span(origin->loc);
    PyObject *row = span == NULL ? NULL : Py_BuildValue("(iOiO)", origin->kind,
        span, origin->phase, origin->detail == NULL ? Py_None : origin->detail);
    Py_XDECREF(span);
    return row;
}

static PyObject *
soac_call_origin_row(soac_binding_collector *collector, soac_code_bindings *unit,
                     soac_reference_origin *origin)
{
    PyObject *span = soac_source_span(origin->loc);
    PyObject *detail = Py_XNewRef(origin->detail);
    if (origin->child != NULL) {
        soac_code_bindings *child = soac_unit_for_code(collector, origin->child);
        if (child == NULL || child->parent != unit || child->final_id < 0 || detail != NULL) {
            Py_XDECREF(span);
            Py_XDECREF(detail);
            if (!PyErr_Occurred()) {
                soac_binding_error("Call child is not its retained direct native code unit");
            }
            return NULL;
        }
        detail = PyLong_FromSsize_t(child->final_id);
    }
    else if (detail == NULL) {
        detail = Py_NewRef(Py_None);
    }
    PyObject *row = span == NULL || detail == NULL ? NULL :
        Py_BuildValue("(iOO)", origin->kind, span, detail);
    Py_XDECREF(span);
    Py_XDECREF(detail);
    return row;
}

/* A compiler-role child can disappear with unreachable native code. Wire3
 * contains only retained code nodes, so there is no ID to serialize for it.
 * Omit that role ONLY after checking every final lane: this is absence of a
 * receipt, not permission to select an executable source operation. */
static int
soac_operation_has_retained_child(soac_binding_collector *collector,
                                   soac_code_bindings *unit, uint32_t id)
{
    soac_reference_origin *origin = soac_operation_at(unit, id);
    if (origin->family != SOAC_ORIGIN_CALL ||
        (origin->kind != Py_SOAC_CALL_CLASS &&
         origin->kind != Py_SOAC_CALL_GENERIC_SCOPE &&
         origin->kind != Py_SOAC_CALL_GENERATOR)) {
        return 1;
    }
    if (origin->child == NULL) {
        return soac_binding_error("compiler Call has no actual child code unit");
    }
    soac_code_bindings *child = soac_unit_for_code(collector, origin->child);
    if (child == NULL) {
        return ERROR;
    }
    if (child->parent != unit) {
        return soac_binding_error("compiler Call child has a different original parent");
    }
    if (child->final_id >= 0) {
        return 1;
    }
    if (origin->alternative != 0) {
        return soac_binding_error("unretained compiler Call has an ambiguous native alternative");
    }
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        for (int lane = 0; lane < 2; lane++) {
            if (instruction->origins.lane[lane] == id && instruction->opcode != NOP) {
                return soac_binding_error("surviving compiler Call lost its retained native child");
            }
        }
    }
    return 0;
}

static PyObject *
soac_operation_origin_row(soac_binding_collector *collector, soac_code_bindings *unit,
                          soac_reference_origin *origin)
{
    if (origin == NULL) {
        return Py_NewRef(Py_None);
    }
    PyObject *payload;
    switch (origin->family) {
        case SOAC_ORIGIN_STORE: payload = soac_binding_origin_row(origin); break;
        case SOAC_ORIGIN_CALL: payload = soac_call_origin_row(collector, unit, origin); break;
        default:
            soac_binding_error("transient preparation cannot become a source operation");
            return NULL;
    }
    PyObject *row = payload == NULL ? NULL : Py_BuildValue("(iO)", origin->family, payload);
    Py_XDECREF(payload);
    return row;
}

static int
soac_operation_gap(soac_binding_collector *collector, soac_code_bindings *unit,
                    PyObject *gaps, int reason, soac_reference_origin *origin,
                    int ordinal, int lane, int opcode, Py_ssize_t context)
{
    PyObject *source = soac_operation_origin_row(collector, unit, origin);
    PyObject *index = soac_optional_id(ordinal);
    PyObject *which = soac_optional_id(lane);
    PyObject *operation = soac_optional_id(opcode);
    PyObject *continuation = soac_context_row(unit, context);
    PyObject *row = source == NULL || index == NULL || which == NULL ||
        operation == NULL || continuation == NULL ? NULL :
        Py_BuildValue("(iOOOOO)", reason, source, index, which, operation, continuation);
    Py_XDECREF(source);
    Py_XDECREF(index);
    Py_XDECREF(which);
    Py_XDECREF(operation);
    Py_XDECREF(continuation);
    if (row == NULL) {
        return ERROR;
    }
    int contains = PySequence_Contains(gaps, row);
    if (contains < 0) {
        Py_DECREF(row);
        return ERROR;
    }
    if (contains) {
        Py_DECREF(row);
        return SUCCESS;
    }
    return soac_append_owned(gaps, row);
}

static int
soac_reference_slots(soac_code_bindings *unit, soac_reference_instruction *instruction,
                      int paired, int *first, int *second)
{
    *first = instruction->oparg;
    *second = -1;
    if (paired) {
        *first = instruction->oparg >> 4;
        *second = instruction->oparg & 15;
        if (instruction->oparg < 0 || *first >= 16) {
            return soac_binding_error("invalid native fused FAST operands");
        }
    }
    if (*first < 0 || *first >= unit->code->co_nlocalsplus ||
        *second >= unit->code->co_nlocalsplus) {
        return soac_binding_error("native operand is outside final localsplus");
    }
    return SUCCESS;
}

static int
soac_store_operands(soac_code_bindings *unit, soac_reference_instruction *instruction,
                     int form, int *domain, int *first, int *second)
{
    *second = -1;
    if (form <= Py_SOAC_STORE_CELL || form == Py_SOAC_DELETE_FAST || form == Py_SOAC_DELETE_CELL) {
        *domain = Py_SOAC_OPERAND_LOCALSPLUS;
        return soac_reference_slots(unit, instruction,
            form == Py_SOAC_STORE_FAST_PAIR || form == Py_SOAC_STORE_FAST_THEN_LOAD,
            first, second);
    }
    if (form == Py_SOAC_STORE_SUBSCRIPT || form == Py_SOAC_STORE_SLICE ||
        form == Py_SOAC_DELETE_SUBSCRIPT) {
        *domain = Py_SOAC_OPERAND_NO_INDEX;
        *first = -1;
        return SUCCESS;
    }
    *domain = Py_SOAC_OPERAND_NAMES;
    *first = instruction->oparg;
    if (*first < 0 || *first >= PyTuple_GET_SIZE(unit->code->co_names)) {
        return soac_binding_error("native Store operand is outside final names");
    }
    return SUCCESS;
}

static PyObject *
soac_operand_row(int domain, int index)
{
    PyObject *number = soac_optional_id(index);
    PyObject *row = number == NULL ? NULL : Py_BuildValue("(iO)", domain, number);
    Py_XDECREF(number);
    return row;
}


static int
soac_store_lane(int form, int lane)
{
    return form >= 0 && (lane == 0 || form == Py_SOAC_STORE_FAST_PAIR);
}

static int
soac_original_store_matches(soac_reference_origin *origin, int form, int lane)
{
    if (origin == NULL || origin->family != SOAC_ORIGIN_STORE || !soac_store_lane(form, lane)) {
        return 0;
    }
    if (form == Py_SOAC_STORE_FAST_PAIR || form == Py_SOAC_STORE_FAST_THEN_LOAD) {
        return origin->initial_opcode == STORE_FAST && origin->context == Store;
    }
    return soac_store_form(origin->initial_opcode) == form;
}


static int
soac_check_original_store_slot(soac_code_bindings *unit,
                               soac_reference_origin *origin, int form, int domain, int slot)
{
    if (domain == Py_SOAC_OPERAND_NO_INDEX) {
        return SUCCESS;
    }
    if (origin->initial_slot != slot) {
        return soac_binding_error("source Store receipt disagrees with native operand");
    }
    if (domain == Py_SOAC_OPERAND_LOCALSPLUS) {
        int expected = form == Py_SOAC_STORE_CELL || form == Py_SOAC_DELETE_CELL
            ? CO_FAST_CELL | CO_FAST_FREE : CO_FAST_LOCAL;
        if (!(_PyLocals_GetKind(unit->code->co_localspluskinds, slot) & expected)) {
            return soac_binding_error("source Store disagrees with native localsplus kind");
        }
    }
    return SUCCESS;
}

static PyObject *
soac_call_input_row(soac_reference_origin *origin)
{
    if (origin->positional_entries == NULL || origin->keyword_entries == NULL ||
        origin->keyword_groups == NULL || origin->positional_kind < 0 || origin->keyword_kind < 0) {
        soac_binding_error("native Call has no explicit producer input layout");
        return NULL;
    }
    if ((origin->keyword_kind == Py_SOAC_KEYWORDS_NAMES_TUPLE) != (origin->keyword_names != NULL)) {
        soac_binding_error("native Call keyword plan lost its actual names tuple");
        return NULL;
    }
    PyObject *positional = Py_BuildValue("(iO)", origin->positional_kind, origin->positional_entries);
    PyObject *groups = PyList_AsTuple(origin->keyword_groups);
    PyObject *keywords = groups == NULL ? NULL : Py_BuildValue("(iOOO)", origin->keyword_kind,
        origin->keyword_names == NULL ? Py_None : origin->keyword_names, origin->keyword_entries, groups);
    PyObject *row = positional == NULL || keywords == NULL ? NULL :
        Py_BuildValue("(iiOO)", origin->channel, origin->preloaded, positional, keywords);
    Py_XDECREF(positional);
    Py_XDECREF(groups);
    Py_XDECREF(keywords);
    return row;
}

/* Preparation tags are proof observations, not an opcode program to execute.
 * A rewritten/eliminated observation invalidates the plan. A changed constant
 * index is allowed only when the actual final constant is still the emitter's
 * exact tuple. Multiple physical copies need equal complete observations; this
 * does not associate any of them with a later SOAC clone. */
static int
soac_call_input_survived(soac_code_bindings *unit, uint32_t call_id)
{
    soac_reference_origin *call = soac_operation_at(unit, call_id);
    Py_ssize_t calls = 0;
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        if (instruction->origins.lane[0] == call_id && soac_call_form(instruction->opcode) >= 0) {
            calls++;
        }
    }
    int observed = 0;
    for (Py_ssize_t id = 0; id < unit->reference_origins.count; id++) {
        soac_reference_origin *origin = &unit->reference_origins.items[id];
        if (origin->family != SOAC_ORIGIN_CALL_PREPARATION || origin->call_owner != call_id) {
            continue;
        }
        observed = 1;
        Py_ssize_t copies = 0;
        for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
            soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
            for (int lane = 0; lane < 2; lane++) {
                if (instruction->origins.lane[lane] != (uint32_t)id + 1 || instruction->opcode == NOP) {
                    continue;
                }
                int folded_null = origin->initial_opcode == PUSH_NULL &&
                    instruction->opcode == LOAD_GLOBAL && (instruction->oparg & 1);
                /* This tag is transferred only at the actual native
                 * LOAD_GLOBAL/PUSH_NULL fold, never by opcode search. */
                if (lane != 0 || (!folded_null && instruction->opcode != origin->initial_opcode)) {
                    return 0;
                }
                if (origin->initial_opcode == LOAD_CONST && call->keyword_names != NULL &&
                    origin->initial_slot == call->keyword_constant) {
                    if (instruction->oparg < 0 || instruction->oparg >= PyTuple_GET_SIZE(unit->code->co_consts)) {
                        return soac_binding_error("native keyword constant exceeds final constants");
                    }
                    PyObject *constant = PyTuple_GET_ITEM(unit->code->co_consts, instruction->oparg);
                    int same = PyTuple_CheckExact(constant)
                        ? PyObject_RichCompareBool(constant, call->keyword_names, Py_EQ) : 0;
                    if (same <= 0) {
                        return same;
                    }
                }
                else if (!folded_null && instruction->oparg != origin->initial_slot) {
                    return 0;
                }
                copies++;
            }
        }
        if (copies != calls || copies == 0) {
            return 0;
        }
    }
    /* Decorator application has no argument-preparation operation: its only
     * value is the already produced function in the leading channel. The
     * retained CALL0 is the whole preparation shape, not a missing builder. */
    return observed || (call->kind == Py_SOAC_CALL_DECORATOR &&
        call->initial_opcode == CALL && call->initial_slot == 0 &&
        call->channel == Py_SOAC_CALL_LEADING_CHANNEL && call->preloaded == 0 &&
        call->positional_kind == Py_SOAC_POSITIONAL_VECTOR &&
        PyTuple_GET_SIZE(call->positional_entries) == 0 &&
        call->keyword_kind == Py_SOAC_KEYWORDS_NONE);
}

static int
soac_check_assembled_operations(soac_code_bindings *unit)
{
    if (unit->code == NULL || unit->final_id < 0 || unit->final_id > UINT32_MAX ||
        unit->reference_instruction_count < 0 ||
        unit->assembled_code_size <= 0 || unit->assembled_code_size > UINT32_MAX ||
        unit->assembled_code_size != (Py_ssize_t)_PyCode_NBYTES(unit->code) ||
        PyTuple_GET_SIZE(unit->code->co_names) > UINT32_MAX) {
        return soac_binding_error("operation table has no exact assembled native code unit");
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(unit->code->co_names); i++) {
        if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(unit->code->co_names, i))) {
            return soac_binding_error("final native names contain a non-exact string");
        }
    }
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        if (instruction->ordinal >= unit->reference_instruction_count ||
            instruction->opcode_offset < 0 || instruction->opcode_offset >= unit->assembled_code_size ||
            instruction->opcode_offset % sizeof(_Py_CODEUNIT) != 0 ||
            _PyCode_CODE(unit->code)[instruction->opcode_offset / sizeof(_Py_CODEUNIT)].op.code !=
                instruction->opcode) {
            return soac_binding_error("assembled source operation is not its exact native opcode");
        }
    }
    return SUCCESS;
}

static int
soac_physical_operation_gaps(soac_binding_collector *collector, soac_code_bindings *unit,
                             PyObject *gaps)
{
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        int store = soac_store_form(instruction->opcode);
        int call = soac_call_form(instruction->opcode);
        for (int lane = 0; lane < 2; lane++) {
            soac_reference_origin *origin = soac_operation_at(unit, instruction->origins.lane[lane]);
            if (soac_store_lane(store, lane) && !soac_original_store_matches(origin, store, lane)) {
                RETURN_IF_ERROR(soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_MISSING_STORE,
                    NULL, instruction->ordinal, lane, instruction->opcode, -2));
            }
        }
        soac_reference_origin *origin = soac_operation_at(unit, instruction->origins.lane[0]);
        if (call >= 0 && (origin == NULL || origin->family != SOAC_ORIGIN_CALL)) {
            RETURN_IF_ERROR(soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_MISSING_CALL,
                NULL, instruction->ordinal, 0, instruction->opcode, -2));
        }
        if (instruction->opcode == MAKE_CELL || instruction->opcode == COPY_FREE_VARS ||
            instruction->opcode == LOAD_FAST_AND_CLEAR) {
            RETURN_IF_ERROR(soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_COMPILER_ORIGIN,
                NULL, instruction->ordinal, 0, instruction->opcode, -2));
        }
    }
    return SUCCESS;
}

/* One source row retains ALL final physical emissions. No dedup by byte offset
 * or continuation, and no "first row wins" policy selection. */
static PyObject *
soac_operation_emissions(soac_binding_collector *collector, soac_code_bindings *unit,
                         Py_ssize_t representative, Py_ssize_t *representatives,
                         PyObject *gaps)
{
    soac_reference_origin *original = &unit->reference_origins.items[representative];
    PyObject *emissions = PyList_New(0), *first_key = NULL;
    int unsupported = 0, divergent = 0, alternative = 0;
    if (emissions == NULL) {
        return NULL;
    }
    for (Py_ssize_t id = 0; id < unit->reference_origins.count; id++) {
        soac_reference_origin *origin = &unit->reference_origins.items[id];
        if (representatives[id] != representative || origin->family != SOAC_ORIGIN_CALL ||
            origin->alternative == 0) {
            continue;
        }
        alternative = 1;
        if (soac_operation_gap(collector, unit, gaps, origin->alternative, origin,
                -1, -1, -1, origin->emission_context) < 0) {
            goto error;
        }
    }
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        if (instruction->opcode == NOP) {
            continue;
        }
        for (int lane = 0; lane < 2; lane++) {
            uint32_t id = instruction->origins.lane[lane];
            if (id == 0 || representatives[id - 1] != representative) {
                continue;
            }
            soac_reference_origin *origin = soac_operation_at(unit, id);
            PyObject *context = soac_context_row(unit, origin->emission_context);
            PyObject *emission = NULL, *key = NULL;
            if (context == NULL) {
                goto error;
            }
            int form = -1;
            if (origin->family == SOAC_ORIGIN_STORE) {
                form = soac_store_form(instruction->opcode);
                if (!soac_original_store_matches(origin, form, lane)) {
                    form = -1;
                }
                if (form >= 0) {
                    int domain, first, second;
                    if (soac_store_operands(unit, instruction, form, &domain, &first, &second) < 0 ||
                        soac_check_original_store_slot(unit, origin, form, domain,
                            lane == 0 ? first : second) < 0) {
                        Py_DECREF(context);
                        goto error;
                    }
                    PyObject *one = soac_operand_row(domain, first);
                    PyObject *two = second < 0 ? Py_NewRef(Py_None) :
                        soac_operand_row(Py_SOAC_OPERAND_LOCALSPLUS, second);
                    if (one != NULL && two != NULL) {
                        emission = Py_BuildValue("(iiOOiO)", instruction->ordinal, form, one, two, lane, context);
                        key = Py_BuildValue("(iOOi)", form, one, two, lane);
                    }
                    Py_XDECREF(one);
                    Py_XDECREF(two);
                }
            }
            else if (origin->family == SOAC_ORIGIN_CALL) {
                form = lane == 0 ? soac_call_form(instruction->opcode) : -1;
                if (form >= 0) {
                    if (form == Py_SOAC_CALL_EXPANDED && instruction->oparg != 0) {
                        Py_DECREF(context);
                        soac_binding_error("native CALL_FUNCTION_EX has a nonzero argument");
                        goto error;
                    }
                    PyObject *offset = soac_optional_id(instruction->opcode_offset);
                    PyObject *count = form == Py_SOAC_CALL_EXPANDED
                        ? Py_NewRef(Py_None) : PyLong_FromLong(instruction->oparg);
                    PyObject *input = soac_call_input_row(origin);
                    if (offset != NULL && count != NULL && input != NULL) {
                        emission = Py_BuildValue("(iOiOOO)", instruction->ordinal, offset, form, count, input, context);
                        key = Py_BuildValue("(iOO)", form, count, input);
                    }
                    Py_XDECREF(offset);
                    Py_XDECREF(count);
                    Py_XDECREF(input);
                    int survived = soac_call_input_survived(unit, id);
                    if (survived < 0 || (!survived && soac_operation_gap(collector, unit, gaps,
                            Py_SOAC_OPERATION_GAP_CALL_INPUT, origin, instruction->ordinal,
                            lane, instruction->opcode, origin->emission_context) < 0)) {
                        Py_DECREF(context);
                        Py_XDECREF(emission);
                        Py_XDECREF(key);
                        goto error;
                    }
                }
            }
            Py_DECREF(context);
            if (form < 0) {
                unsupported = 1;
                if (soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_UNSUPPORTED,
                        origin, instruction->ordinal, lane, instruction->opcode, origin->emission_context) < 0) {
                    goto error;
                }
                continue;
            }
            if (emission == NULL || key == NULL) {
                Py_XDECREF(emission);
                Py_XDECREF(key);
                goto error;
            }
            if (first_key == NULL) {
                first_key = Py_NewRef(key);
            }
            else {
                int same = PyObject_RichCompareBool(first_key, key, Py_EQ);
                if (same < 0) {
                    Py_DECREF(emission);
                    Py_DECREF(key);
                    goto error;
                }
                divergent |= !same;
            }
            Py_DECREF(key);
            if (soac_append_owned(emissions, emission) < 0 ||
                (origin->emission_context == -2 && soac_operation_gap(collector, unit, gaps,
                    Py_SOAC_OPERATION_GAP_MISSING_CONTEXT, origin, instruction->ordinal,
                    lane, instruction->opcode, -2) < 0)) {
                goto error;
            }
        }
    }
    if (PyList_GET_SIZE(emissions) == 0 && !unsupported && !alternative &&
        soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_ELIMINATED, original,
                           -1, -1, -1, -2) < 0) {
        goto error;
    }
    if (divergent && soac_operation_gap(collector, unit, gaps, Py_SOAC_OPERATION_GAP_DIVERGENT,
                                      original, -1, -1, -1, -2) < 0) {
        goto error;
    }
    PyObject *tuple = PyList_AsTuple(emissions);
    Py_DECREF(emissions);
    Py_XDECREF(first_key);
    return tuple;
error:
    Py_DECREF(emissions);
    Py_XDECREF(first_key);
    return NULL;
}

static PyObject *
soac_reference_table(soac_binding_collector *collector, soac_code_bindings *unit)
{
    PyObject *rows[2] = {PyList_New(0), PyList_New(0)};
    PyObject *gaps = PyList_New(0), *result = NULL;
    Py_ssize_t *representatives = NULL;
    if (rows[0] == NULL || rows[1] == NULL || gaps == NULL ||
        soac_check_assembled_operations(unit) < 0 ||
        soac_physical_operation_gaps(collector, unit, gaps) < 0) {
        goto done;
    }
    Py_ssize_t count = unit->reference_origins.count;
    if ((size_t)count > SIZE_MAX / sizeof(*representatives)) {
        PyErr_NoMemory();
        goto done;
    }
    if (count > 0) {
        representatives = PyMem_Malloc((size_t)count * sizeof(*representatives));
        if (representatives == NULL) {
            PyErr_NoMemory();
            goto done;
        }
    }
    for (Py_ssize_t id = 0; id < count; id++) {
        representatives[id] = id;
        soac_reference_origin *origin = &unit->reference_origins.items[id];
        if (origin->family > SOAC_ORIGIN_CALL) {
            continue;
        }
        int retained = soac_operation_has_retained_child(collector, unit, (uint32_t)id + 1);
        if (retained < 0) {
            goto done;
        }
        if (!retained) {
            representatives[id] = -1;
            continue;
        }
        for (Py_ssize_t previous = 0; previous < id; previous++) {
            if (representatives[previous] < 0) {
                continue;
            }
            soac_reference_origin *other = &unit->reference_origins.items[previous];
            int same = soac_same_operation_identity(unit, origin, unit, other);
            if (same < 0) {
                goto done;
            }
            if (same) {
                representatives[id] = representatives[previous];
                break;
            }
        }
    }
    for (Py_ssize_t id = 0; id < count; id++) {
        soac_reference_origin *origin = &unit->reference_origins.items[id];
        if (representatives[id] != id || origin->family > SOAC_ORIGIN_CALL) {
            continue;
        }
        PyObject *source = soac_operation_origin_row(collector, unit, origin);
        PyObject *emissions = source == NULL ? NULL :
            soac_operation_emissions(collector, unit, id, representatives, gaps);
        PyObject *row = emissions == NULL ? NULL :
            PyTuple_Pack(2, PyTuple_GET_ITEM(source, 1), emissions);
        Py_XDECREF(source);
        Py_XDECREF(emissions);
        if (soac_append_owned(rows[origin->family - SOAC_ORIGIN_STORE], row) < 0) {
            goto done;
        }
    }
    PyObject *stores = PyList_AsTuple(rows[0]);
    PyObject *calls = stores == NULL ? NULL : PyList_AsTuple(rows[1]);
    PyObject *missing = calls == NULL ? NULL : PyList_AsTuple(gaps);
    if (missing != NULL) {
        result = Py_BuildValue("(ninOOOO)", unit->final_id, unit->reference_instruction_count,
            unit->assembled_code_size, unit->code->co_names, stores, calls, missing);
    }
    Py_XDECREF(stores);
    Py_XDECREF(calls);
    Py_XDECREF(missing);
done:
    PyMem_Free(representatives);
    Py_XDECREF(rows[0]);
    Py_XDECREF(rows[1]);
    Py_XDECREF(gaps);
    return result;
}

static int
soac_validate_scope_slots(soac_code_bindings *unit)
{
    PyCodeObject *code = unit->code;
    if (code == NULL || unit->final_id < 0 || unit->active_region >= 0 ||
        unit->slots.count != code->co_nlocalsplus ||
        unit->cell_count != code->co_ncellvars || unit->free_count != code->co_nfreevars) {
        return soac_binding_error("incomplete final native scope layout");
    }
    Py_ssize_t parameters = _PyCode_NumFrameParameters(code);
    if (parameters < 0 || parameters > code->co_nlocalsplus ||
        (unit->scope_kind == Py_SOAC_SCOPE_CLASS && parameters != 0)) {
        return soac_binding_error("native binding parameter layout is inconsistent");
    }
    for (int index = 0; index < code->co_nlocalsplus; index++) {
        unsigned char kind = _PyLocals_GetKind(code->co_localspluskinds, index);
        if (!!(kind & CO_FAST_ARG) != (index < parameters) ||
            ((kind & CO_FAST_ARG) && !(kind & CO_FAST_LOCAL))) {
            return soac_binding_error("native successful-bind seed disagrees with ARG slots");
        }
    }
    for (Py_ssize_t i = 0; i < unit->slots.count; i++) {
        soac_binding_slot *slot = &unit->slots.items[i];
        int index = slot->final_index;
        if (index < 0 || index >= code->co_nlocalsplus) {
            return soac_binding_error("unresolved final native scope slot");
        }
        unsigned char kind = _PyLocals_GetKind(code->co_localspluskinds, index);
        int expected = !slot->is_deref ? CO_FAST_LOCAL :
            slot->raw_index < unit->cell_count ? CO_FAST_CELL : CO_FAST_FREE;
        if (!(kind & expected)) {
            return soac_binding_error("native scope slot kind disagrees with resolved operand");
        }
        for (Py_ssize_t j = 0; j < i; j++) {
            if (unit->slots.items[j].final_index == index) {
                return soac_binding_error("two symbolic scope slots alias after native fixup");
            }
        }
    }
    return SUCCESS;
}

static PyObject *
soac_scope_seed_rows(soac_code_bindings *unit)
{
    PyCodeObject *code = unit->code;
    Py_ssize_t parameters = _PyCode_NumFrameParameters(code);
    PyObject *rows = PyTuple_New(code->co_nlocalsplus);
    if (rows == NULL) {
        return NULL;
    }
    for (int i = 0; i < code->co_nlocalsplus; i++) {
        int is_parameter = i < parameters;
        PyObject *ordinal = soac_optional_id(is_parameter ? i : -1);
        PyObject *row = ordinal == NULL ? NULL : Py_BuildValue("(iiiO)", i,
            (int)_PyLocals_GetKind(code->co_localspluskinds, i),
            is_parameter ? Py_SOAC_SCOPE_SEED_PARAMETER : Py_SOAC_SCOPE_SEED_CLEARED, ordinal);
        Py_XDECREF(ordinal);
        if (row == NULL) {
            Py_DECREF(rows);
            return NULL;
        }
        PyTuple_SET_ITEM(rows, i, row);
    }
    return rows;
}


static PyObject *
soac_scope_source_binding_rows(soac_code_bindings *unit, Py_ssize_t region)
{
    PyObject *rows = PyList_New(0);
    if (rows == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->scope_source_bindings.count; i++) {
        soac_scope_source_binding *binding = &unit->scope_source_bindings.items[i];
        if (binding->region != region) {
            continue;
        }
        soac_reference_origin *origin = soac_operation_at(unit, binding->origin);
        int form = soac_store_form(origin->initial_opcode);
        int domain, index = origin->initial_slot;
        Py_ssize_t generators = asdl_seq_LEN(soac_scope_generators(&unit->regions.items[region]));
        if ((binding->role == Py_SOAC_SCOPE_BINDING_TARGET
                ? binding->generator < 0 || binding->generator >= generators
                : binding->role != Py_SOAC_SCOPE_BINDING_WALRUS || binding->generator != -1)) {
            soac_binding_error("comprehension source binding has a different original role");
            goto error;
        }
        if (form < 0 || form == Py_SOAC_STORE_FAST_PAIR || form == Py_SOAC_STORE_FAST_THEN_LOAD ||
            origin->family != SOAC_ORIGIN_STORE || origin->context != Store) {
            soac_binding_error("comprehension binding lost its original native Store");
            goto error;
        }
        if (form == Py_SOAC_STORE_FAST || form == Py_SOAC_STORE_CELL) {
            domain = Py_SOAC_OPERAND_LOCALSPLUS;
        }
        else if (form == Py_SOAC_STORE_NAME || form == Py_SOAC_STORE_GLOBAL || form == Py_SOAC_STORE_ATTRIBUTE) {
            domain = Py_SOAC_OPERAND_NAMES;
        }
        else {
            domain = Py_SOAC_OPERAND_NO_INDEX;
            index = -1;
        }
        if ((domain == Py_SOAC_OPERAND_LOCALSPLUS && (index < 0 || index >= unit->code->co_nlocalsplus)) ||
            (domain == Py_SOAC_OPERAND_NAMES && (index < 0 || index >= PyTuple_GET_SIZE(unit->code->co_names)))) {
            soac_binding_error("comprehension source binding has an invalid final native operand");
            goto error;
        }
        if (soac_check_original_store_slot(unit, origin, form, domain, index) < 0) {
            goto error;
        }
        PyObject *generator = soac_optional_id(binding->generator);
        PyObject *source = soac_binding_origin_row(origin);
        PyObject *operand = soac_operand_row(domain, index);
        PyObject *row = generator == NULL || source == NULL || operand == NULL ? NULL :
            Py_BuildValue("(iOOiO)", binding->role, generator, source, form, operand);
        Py_XDECREF(generator);
        Py_XDECREF(source);
        Py_XDECREF(operand);
        if (row == NULL) {
            goto error;
        }
        int found = PySequence_Contains(rows, row);
        if (found < 0) {
            Py_DECREF(row);
            goto error;
        }
        if (found) {
            Py_DECREF(row);
        }
        else if (soac_append_owned(rows, row) < 0) {
            goto error;
        }
    }
    PyObject *result = PyList_AsTuple(rows);
    Py_DECREF(rows);
    return result;
error:
    Py_DECREF(rows);
    return NULL;
}

static PyObject *
soac_scope_region_rows(soac_code_bindings *unit)
{
    PyObject *rows = PyTuple_New(unit->final_region_count);
    if (rows == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        soac_binding_region *region = &unit->regions.items[i];
        if (region->representative != i) {
            continue;
        }
        PyObject *parent = soac_optional_id(region->parent < 0 ? -1 :
            unit->regions.items[region->parent].final_id);
        PyObject *span = soac_source_span(region->loc);
        PyObject *outer = soac_source_span(region->outer_loc);
        PyObject *ops = PyTuple_New(region->entry_ops.count);
        PyObject *bindings = soac_scope_source_binding_rows(unit, i);
        if (parent == NULL || span == NULL || span == Py_None || outer == NULL || outer == Py_None ||
            ops == NULL || bindings == NULL) {
            if (!PyErr_Occurred()) {
                soac_binding_error("incomplete native eager-comprehension region");
            }
            goto region_error;
        }
        for (Py_ssize_t j = 0; j < region->entry_ops.count; j++) {
            soac_binding_entry_op *op = &region->entry_ops.items[j];
            PyObject *row = Py_BuildValue("(iin)", op->role, unit->slots.items[op->slot].final_index,
                                          unit->owners.items[op->owner].final_id);
            if (row == NULL) {
                goto region_error;
            }
            PyTuple_SET_ITEM(ops, j, row);
        }
        PyObject *row = Py_BuildValue("(nOiOOiOO)",
            region->final_id, parent, region->kind, span, outer, region->is_async, ops, bindings);
        if (row == NULL) {
            goto region_error;
        }
        Py_DECREF(parent);
        Py_DECREF(span);
        Py_DECREF(outer);
        Py_DECREF(ops);
        Py_DECREF(bindings);
        PyTuple_SET_ITEM(rows, region->final_id, row);
        continue;
region_error:
        Py_XDECREF(parent);
        Py_XDECREF(span);
        Py_XDECREF(outer);
        Py_XDECREF(ops);
        Py_XDECREF(bindings);
        Py_DECREF(rows);
        return NULL;
    }
    return rows;
}

static PyObject *
soac_scope_class_actions(soac_code_bindings *unit)
{
    if (unit->scope_kind != Py_SOAC_SCOPE_CLASS) {
        if (unit->initializers.count || unit->exports.count) {
            soac_binding_error("non-class scope has class header/export actions");
            return NULL;
        }
        return Py_NewRef(Py_None);
    }
    PyObject *header = PyTuple_New(unit->initializers.count);
    PyObject *exports = NULL, *result = NULL;
    if (header == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < unit->initializers.count; i++) {
        soac_binding_init *init = &unit->initializers.items[i];
        if (init->phase != Py_SOAC_CLASS_PHASE_HEADER ||
            (init->role != Py_SOAC_CLASS_INIT_NAMESPACE && init->role != Py_SOAC_CLASS_INIT_CONDITIONAL_SET) ||
            init->operand >= 0) {
            soac_binding_error("class action is not an actual native header initialization");
            goto done;
        }
        PyObject *row = Py_BuildValue("(niO)", unit->owners.items[init->owner].final_id,
                                      init->role, Py_None);
        if (row == NULL) {
            goto done;
        }
        PyTuple_SET_ITEM(header, i, row);
    }
    exports = soac_export_rows(unit);
    result = exports == NULL ? NULL : PyTuple_Pack(2, header, exports);
done:
    Py_XDECREF(exports);
    Py_DECREF(header);
    return result;
}

static int
soac_validate_scope_regions(soac_code_bindings *unit)
{
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        soac_binding_region *region = &unit->regions.items[i];
        if (region->parent >= i) {
            return soac_binding_error("native comprehension has an inconsistent lexical boundary");
        }
        for (Py_ssize_t j = 0; j < region->entry_ops.count; j++) {
            soac_binding_entry_op *entry = &region->entry_ops.items[j];
            if (entry->slot < 0 || entry->slot >= unit->slots.count ||
                entry->owner < 0 || entry->owner >= unit->owners.count) {
                return soac_binding_error("native comprehension has an invalid slot or owner");
            }
            soac_binding_owner *owner = &unit->owners.items[entry->owner];
            int expected = entry->role == Py_SOAC_CLASS_OP_SAVE_CLEAR
                ? Py_SOAC_CLASS_OWNER_SAVED_SLOT : Py_SOAC_CLASS_OWNER_FRESH_CELL;
            if ((entry->role != Py_SOAC_CLASS_OP_SAVE_CLEAR && entry->role != Py_SOAC_CLASS_OP_MAKE_CELL) ||
                owner->slot != entry->slot || owner->region != i || owner->kind != expected) {
                return soac_binding_error("native comprehension binding differs from its lexical owner");
            }
        }
    }
    return SUCCESS;
}

static PyObject *
soac_scope_recipe(soac_binding_collector *collector, soac_code_bindings *unit)
{
    if (soac_validate_scope_slots(unit) < 0 || soac_validate_scope_regions(unit) < 0 ||
        soac_normalize_class_bindings(unit) < 0) return NULL;
    PyObject *seeds = soac_scope_seed_rows(unit);
    PyObject *owners = seeds == NULL ? NULL : soac_owner_rows(unit);
    PyObject *regions = owners == NULL ? NULL : soac_scope_region_rows(unit);
    PyObject *captures = regions == NULL ? NULL : soac_capture_rows(collector, unit);
    PyObject *accesses = captures == NULL ? NULL : soac_access_rows(unit);
    PyObject *actions = accesses == NULL ? NULL : soac_scope_class_actions(unit);
    PyObject *result = actions == NULL ? NULL : Py_BuildValue("(nOOOOOO)",
        unit->final_id, seeds, owners, regions, captures, accesses, actions);
    Py_XDECREF(seeds);
    Py_XDECREF(owners);
    Py_XDECREF(regions);
    Py_XDECREF(captures);
    Py_XDECREF(accesses);
    Py_XDECREF(actions);
    return result;
}

static PyObject *
soac_binding_details(soac_binding_collector *collector, PyCodeObject *root)
{
    PyObject *nodes = PyList_New(0), *recipes = PyList_New(0), *operations = PyList_New(0);
    PyObject *result = NULL;
    if (nodes == NULL || recipes == NULL || operations == NULL ||
        soac_collect_final_tree(collector, root, NULL, nodes) < 0) {
        goto done;
    }
    for (Py_ssize_t id = 0; id < PyList_GET_SIZE(nodes); id++) {
        PyCodeObject *code = (PyCodeObject *)PyTuple_GET_ITEM(PyList_GET_ITEM(nodes, id), 2);
        soac_code_bindings *unit = soac_unit_for_code(collector, code);
        if (unit == NULL || soac_append_owned(recipes, soac_scope_recipe(collector, unit)) < 0 ||
            soac_append_owned(operations, soac_reference_table(collector, unit)) < 0) {
            goto done;
        }
    }
    PyObject *node_tuple = PyList_AsTuple(nodes);
    PyObject *recipe_tuple = node_tuple == NULL ? NULL : PyList_AsTuple(recipes);
    PyObject *operation_tuple = recipe_tuple == NULL ? NULL : PyList_AsTuple(operations);
    if (operation_tuple != NULL) {
        result = Py_BuildValue("(iOOO)", Py_SOAC_CLASS_BINDINGS_SCHEMA,
                               node_tuple, recipe_tuple, operation_tuple);
    }
    Py_XDECREF(node_tuple);
    Py_XDECREF(recipe_tuple);
    Py_XDECREF(operation_tuple);
done:
    Py_XDECREF(nodes);
    Py_XDECREF(recipes);
    Py_XDECREF(operations);
    return result;
}

static int
compiler_setup(compiler *c, mod_ty mod, PyObject *filename,
               PyCompilerFlags *flags, int optimize, PyArena *arena,
               PyObject *module)
{
    PyCompilerFlags local_flags = _PyCompilerFlags_INIT;

    c->c_const_cache = PyDict_New();
    if (!c->c_const_cache) {
        return ERROR;
    }

    c->c_stack = PyList_New(0);
    if (!c->c_stack) {
        return ERROR;
    }

    c->c_filename = Py_NewRef(filename);
    if (!_PyFuture_FromAST(mod, filename, &c->c_future)) {
        return ERROR;
    }
    c->c_module = Py_XNewRef(module);
    if (!flags) {
        flags = &local_flags;
    }
    int merged = c->c_future.ff_features | flags->cf_flags;
    c->c_future.ff_features = merged;
    flags->cf_flags = merged;
    c->c_flags = *flags;
    c->c_optimize = (optimize == -1) ? _Py_GetConfig()->optimization_level : optimize;
    c->c_save_nested_seqs = false;

    if (!_PyAST_Preprocess(mod, arena, filename, c->c_optimize, merged,
                           0, 1, module))
    {
        return ERROR;
    }
    c->c_st = _PySymtable_Build(mod, filename, &c->c_future);
    if (c->c_st == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "no symtable");
        }
        return ERROR;
    }
    return SUCCESS;
}

static void
compiler_free(compiler *c)
{
    soac_binding_collector_free(c->c_soac_bindings);
    if (c->c_st) {
        _PySymtable_Free(c->c_st);
    }
    Py_XDECREF(c->c_filename);
    Py_XDECREF(c->c_module);
    Py_XDECREF(c->c_const_cache);
    Py_XDECREF(c->c_stack);
    PyMem_Free(c);
}

static compiler*
new_compiler(mod_ty mod, PyObject *filename, PyCompilerFlags *pflags,
             int optimize, PyArena *arena, PyObject *module)
{
    compiler *c = PyMem_Calloc(1, sizeof(compiler));
    if (c == NULL) {
        return NULL;
    }
    if (compiler_setup(c, mod, filename, pflags, optimize, arena, module) < 0) {
        compiler_free(c);
        return NULL;
    }
    return c;
}

static void
compiler_unit_free(struct compiler_unit *u)
{
    Py_CLEAR(u->u_instr_sequence);
    Py_CLEAR(u->u_stashed_instr_sequence);
    Py_CLEAR(u->u_ste);
    Py_CLEAR(u->u_metadata.u_name);
    Py_CLEAR(u->u_metadata.u_qualname);
    Py_CLEAR(u->u_metadata.u_consts);
    Py_CLEAR(u->u_metadata.u_names);
    Py_CLEAR(u->u_metadata.u_varnames);
    Py_CLEAR(u->u_metadata.u_freevars);
    Py_CLEAR(u->u_metadata.u_cellvars);
    Py_CLEAR(u->u_metadata.u_fasthidden);
    Py_CLEAR(u->u_private);
    Py_CLEAR(u->u_static_attributes);
    Py_CLEAR(u->u_deferred_annotations);
    Py_CLEAR(u->u_conditional_annotation_indices);
    PyMem_Free(u);
}

#define CAPSULE_NAME "compile.c compiler unit"

int
_PyCompile_MaybeAddStaticAttributeToClass(compiler *c, expr_ty e)
{
    assert(e->kind == Attribute_kind);
    expr_ty attr_value = e->v.Attribute.value;
    if (attr_value->kind != Name_kind ||
        e->v.Attribute.ctx != Store ||
        !_PyUnicode_EqualToASCIIString(attr_value->v.Name.id, "self"))
    {
        return SUCCESS;
    }
    Py_ssize_t stack_size = PyList_GET_SIZE(c->c_stack);
    for (Py_ssize_t i = stack_size - 1; i >= 0; i--) {
        PyObject *capsule = PyList_GET_ITEM(c->c_stack, i);
        struct compiler_unit *u = (struct compiler_unit *)PyCapsule_GetPointer(
                                                              capsule, CAPSULE_NAME);
        assert(u);
        if (u->u_scope_type == COMPILE_SCOPE_CLASS) {
            assert(u->u_static_attributes);
            RETURN_IF_ERROR(PySet_Add(u->u_static_attributes, e->v.Attribute.attr));
            break;
        }
    }
    return SUCCESS;
}

static int
compiler_set_qualname(compiler *c)
{
    Py_ssize_t stack_size;
    struct compiler_unit *u = c->u;
    PyObject *name, *base;

    base = NULL;
    stack_size = PyList_GET_SIZE(c->c_stack);
    assert(stack_size >= 1);
    if (stack_size > 1) {
        int scope, force_global = 0;
        struct compiler_unit *parent;
        PyObject *mangled, *capsule;

        capsule = PyList_GET_ITEM(c->c_stack, stack_size - 1);
        parent = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
        assert(parent);
        if (parent->u_scope_type == COMPILE_SCOPE_ANNOTATIONS) {
            /* The parent is an annotation scope, so we need to
               look at the grandparent. */
            if (stack_size == 2) {
                // If we're immediately within the module, we can skip
                // the rest and just set the qualname to be the same as name.
                u->u_metadata.u_qualname = Py_NewRef(u->u_metadata.u_name);
                return SUCCESS;
            }
            capsule = PyList_GET_ITEM(c->c_stack, stack_size - 2);
            parent = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
            assert(parent);
        }

        if (u->u_scope_type == COMPILE_SCOPE_FUNCTION
            || u->u_scope_type == COMPILE_SCOPE_ASYNC_FUNCTION
            || u->u_scope_type == COMPILE_SCOPE_CLASS) {
            assert(u->u_metadata.u_name);
            mangled = _Py_Mangle(parent->u_private, u->u_metadata.u_name);
            if (!mangled) {
                return ERROR;
            }

            scope = _PyST_GetScope(parent->u_ste, mangled);
            Py_DECREF(mangled);
            RETURN_IF_ERROR(scope);
            assert(scope != GLOBAL_IMPLICIT);
            if (scope == GLOBAL_EXPLICIT)
                force_global = 1;
        }

        if (!force_global) {
            if (parent->u_scope_type == COMPILE_SCOPE_FUNCTION
                || parent->u_scope_type == COMPILE_SCOPE_ASYNC_FUNCTION
                || parent->u_scope_type == COMPILE_SCOPE_LAMBDA)
            {
                _Py_DECLARE_STR(dot_locals, ".<locals>");
                base = PyUnicode_Concat(parent->u_metadata.u_qualname,
                                        &_Py_STR(dot_locals));
                if (base == NULL) {
                    return ERROR;
                }
            }
            else {
                base = Py_NewRef(parent->u_metadata.u_qualname);
            }
        }
    }

    if (base != NULL) {
        name = PyUnicode_Concat(base, _Py_LATIN1_CHR('.'));
        Py_DECREF(base);
        if (name == NULL) {
            return ERROR;
        }
        PyUnicode_Append(&name, u->u_metadata.u_name);
        if (name == NULL) {
            return ERROR;
        }
    }
    else {
        name = Py_NewRef(u->u_metadata.u_name);
    }
    u->u_metadata.u_qualname = name;

    return SUCCESS;
}

/* Merge const *o* and return constant key object.
 * If recursive, insert all elements if o is a tuple or frozen set.
 */
static PyObject*
const_cache_insert(PyObject *const_cache, PyObject *o, bool recursive)
{
    assert(PyDict_CheckExact(const_cache));
    // None and Ellipsis are immortal objects, and key is the singleton.
    // No need to merge object and key.
    if (o == Py_None || o == Py_Ellipsis) {
        return o;
    }

    PyObject *key = _PyCode_ConstantKey(o);
    if (key == NULL) {
        return NULL;
    }

    PyObject *t;
    int res = PyDict_SetDefaultRef(const_cache, key, key, &t);
    if (res != 0) {
        // o was not inserted into const_cache. t is either the existing value
        // or NULL (on error).
        Py_DECREF(key);
        return t;
    }
    Py_DECREF(t);

    if (!recursive) {
        return key;
    }

    // We registered o in const_cache.
    // When o is a tuple or frozenset, we want to merge its
    // items too.
    if (PyTuple_CheckExact(o)) {
        Py_ssize_t len = PyTuple_GET_SIZE(o);
        for (Py_ssize_t i = 0; i < len; i++) {
            PyObject *item = PyTuple_GET_ITEM(o, i);
            PyObject *u = const_cache_insert(const_cache, item, recursive);
            if (u == NULL) {
                Py_DECREF(key);
                return NULL;
            }

            // See _PyCode_ConstantKey()
            PyObject *v;  // borrowed
            if (PyTuple_CheckExact(u)) {
                v = PyTuple_GET_ITEM(u, 1);
            }
            else {
                v = u;
            }
            if (v != item) {
                PyTuple_SET_ITEM(o, i, Py_NewRef(v));
                Py_DECREF(item);
            }

            Py_DECREF(u);
        }
    }
    else if (PyFrozenSet_CheckExact(o)) {
        // *key* is tuple. And its first item is frozenset of
        // constant keys.
        // See _PyCode_ConstantKey() for detail.
        assert(PyTuple_CheckExact(key));
        assert(PyTuple_GET_SIZE(key) == 2);

        Py_ssize_t len = PySet_GET_SIZE(o);
        if (len == 0) {  // empty frozenset should not be re-created.
            return key;
        }
        PyObject *tuple = PyTuple_New(len);
        if (tuple == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        Py_ssize_t i = 0, pos = 0;
        PyObject *item;
        Py_hash_t hash;
        while (_PySet_NextEntry(o, &pos, &item, &hash)) {
            PyObject *k = const_cache_insert(const_cache, item, recursive);
            if (k == NULL) {
                Py_DECREF(tuple);
                Py_DECREF(key);
                return NULL;
            }
            PyObject *u;
            if (PyTuple_CheckExact(k)) {
                u = Py_NewRef(PyTuple_GET_ITEM(k, 1));
                Py_DECREF(k);
            }
            else {
                u = k;
            }
            PyTuple_SET_ITEM(tuple, i, u);  // Steals reference of u.
            i++;
        }

        // Instead of rewriting o, we create new frozenset and embed in the
        // key tuple.  Caller should get merged frozenset from the key tuple.
        PyObject *new = PyFrozenSet_New(tuple);
        Py_DECREF(tuple);
        if (new == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        assert(PyTuple_GET_ITEM(key, 1) == o);
        Py_DECREF(o);
        PyTuple_SET_ITEM(key, 1, new);
    }

    return key;
}

static PyObject*
merge_consts_recursive(PyObject *const_cache, PyObject *o)
{
    return const_cache_insert(const_cache, o, true);
}

Py_ssize_t
_PyCompile_DictAddObj(PyObject *dict, PyObject *o)
{
    PyObject *v;
    Py_ssize_t arg;

    if (PyDict_GetItemRef(dict, o, &v) < 0) {
        return ERROR;
    }
    if (!v) {
        arg = PyDict_GET_SIZE(dict);
        v = PyLong_FromSsize_t(arg);
        if (!v) {
            return ERROR;
        }
        if (PyDict_SetItem(dict, o, v) < 0) {
            Py_DECREF(v);
            return ERROR;
        }
    }
    else
        arg = PyLong_AsLong(v);
    Py_DECREF(v);
    return arg;
}

Py_ssize_t
_PyCompile_AddConst(compiler *c, PyObject *o)
{
    PyObject *key = merge_consts_recursive(c->c_const_cache, o);
    if (key == NULL) {
        return ERROR;
    }

    Py_ssize_t arg = _PyCompile_DictAddObj(c->u->u_metadata.u_consts, key);
    if (arg >= 0 && c->c_soac_bindings != NULL && PyCode_Check(o) &&
        soac_rebind_code_constant(c, (PyCodeObject *)o, key) < 0) {
        arg = ERROR;
    }
    Py_DECREF(key);
    return arg;
}

static PyObject *
list2dict(PyObject *list)
{
    Py_ssize_t i, n;
    PyObject *v, *k;
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    n = PyList_Size(list);
    for (i = 0; i < n; i++) {
        v = PyLong_FromSsize_t(i);
        if (!v) {
            Py_DECREF(dict);
            return NULL;
        }
        k = PyList_GET_ITEM(list, i);
        if (PyDict_SetItem(dict, k, v) < 0) {
            Py_DECREF(v);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(v);
    }
    return dict;
}

/* Return new dict containing names from src that match scope(s).

src is a symbol table dictionary.  If the scope of a name matches
either scope_type or flag is set, insert it into the new dict.  The
values are integers, starting at offset and increasing by one for
each key.
*/

static PyObject *
dictbytype(PyObject *src, int scope_type, int flag, Py_ssize_t offset)
{
    Py_ssize_t i = offset, num_keys, key_i;
    PyObject *k, *v, *dest = PyDict_New();
    PyObject *sorted_keys;

    assert(offset >= 0);
    if (dest == NULL)
        return NULL;

    /* Sort the keys so that we have a deterministic order on the indexes
       saved in the returned dictionary.  These indexes are used as indexes
       into the free and cell var storage.  Therefore if they aren't
       deterministic, then the generated bytecode is not deterministic.
    */
    sorted_keys = PyDict_Keys(src);
    if (sorted_keys == NULL) {
        Py_DECREF(dest);
        return NULL;
    }
    if (PyList_Sort(sorted_keys) != 0) {
        Py_DECREF(sorted_keys);
        Py_DECREF(dest);
        return NULL;
    }
    num_keys = PyList_GET_SIZE(sorted_keys);

    for (key_i = 0; key_i < num_keys; key_i++) {
        k = PyList_GET_ITEM(sorted_keys, key_i);
        v = PyDict_GetItemWithError(src, k);
        if (!v) {
            if (!PyErr_Occurred()) {
                PyErr_SetObject(PyExc_KeyError, k);
            }
            Py_DECREF(sorted_keys);
            Py_DECREF(dest);
            return NULL;
        }
        long vi = PyLong_AsLong(v);
        if (vi == -1 && PyErr_Occurred()) {
            Py_DECREF(sorted_keys);
            Py_DECREF(dest);
            return NULL;
        }
        if (SYMBOL_TO_SCOPE(vi) == scope_type || vi & flag) {
            PyObject *item = PyLong_FromSsize_t(i);
            if (item == NULL) {
                Py_DECREF(sorted_keys);
                Py_DECREF(dest);
                return NULL;
            }
            i++;
            if (PyDict_SetItem(dest, k, item) < 0) {
                Py_DECREF(sorted_keys);
                Py_DECREF(item);
                Py_DECREF(dest);
                return NULL;
            }
            Py_DECREF(item);
        }
    }
    Py_DECREF(sorted_keys);
    return dest;
}

int
_PyCompile_EnterScope(compiler *c, identifier name, int scope_type,
                       void *key, int lineno, PyObject *private,
                      _PyCompile_CodeUnitMetadata *umd)
{
    struct compiler_unit *u;
    u = (struct compiler_unit *)PyMem_Calloc(1, sizeof(struct compiler_unit));
    if (!u) {
        PyErr_NoMemory();
        return ERROR;
    }
    u->u_scope_type = scope_type;
    if (umd != NULL) {
        u->u_metadata = *umd;
    }
    else {
        u->u_metadata.u_argcount = 0;
        u->u_metadata.u_posonlyargcount = 0;
        u->u_metadata.u_kwonlyargcount = 0;
    }
    u->u_ste = _PySymtable_Lookup(c->c_st, key);
    if (!u->u_ste) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_metadata.u_name = Py_NewRef(name);
    u->u_metadata.u_varnames = list2dict(u->u_ste->ste_varnames);
    if (!u->u_metadata.u_varnames) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_metadata.u_cellvars = dictbytype(u->u_ste->ste_symbols, CELL, DEF_COMP_CELL, 0);
    if (!u->u_metadata.u_cellvars) {
        compiler_unit_free(u);
        return ERROR;
    }
    if (u->u_ste->ste_needs_class_closure) {
        /* Cook up an implicit __class__ cell. */
        Py_ssize_t res;
        assert(u->u_scope_type == COMPILE_SCOPE_CLASS);
        res = _PyCompile_DictAddObj(u->u_metadata.u_cellvars, &_Py_ID(__class__));
        if (res < 0) {
            compiler_unit_free(u);
            return ERROR;
        }
    }
    if (u->u_ste->ste_needs_classdict) {
        /* Cook up an implicit __classdict__ cell. */
        Py_ssize_t res;
        assert(u->u_scope_type == COMPILE_SCOPE_CLASS);
        res = _PyCompile_DictAddObj(u->u_metadata.u_cellvars, &_Py_ID(__classdict__));
        if (res < 0) {
            compiler_unit_free(u);
            return ERROR;
        }
    }
    if (u->u_ste->ste_has_conditional_annotations) {
        /* Cook up an implicit __conditional_annotations__ cell */
        Py_ssize_t res;
        assert(u->u_scope_type == COMPILE_SCOPE_CLASS || u->u_scope_type == COMPILE_SCOPE_MODULE);
        res = _PyCompile_DictAddObj(u->u_metadata.u_cellvars, &_Py_ID(__conditional_annotations__));
        if (res < 0) {
            compiler_unit_free(u);
            return ERROR;
        }
    }

    u->u_metadata.u_freevars = dictbytype(u->u_ste->ste_symbols, FREE, DEF_FREE_CLASS,
                               PyDict_GET_SIZE(u->u_metadata.u_cellvars));
    if (!u->u_metadata.u_freevars) {
        compiler_unit_free(u);
        return ERROR;
    }

    u->u_metadata.u_fasthidden = PyDict_New();
    if (!u->u_metadata.u_fasthidden) {
        compiler_unit_free(u);
        return ERROR;
    }

    u->u_nfblocks = 0;
    u->u_in_inlined_comp = 0;
    u->u_metadata.u_firstlineno = lineno;
    u->u_metadata.u_consts = PyDict_New();
    if (!u->u_metadata.u_consts) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_metadata.u_names = PyDict_New();
    if (!u->u_metadata.u_names) {
        compiler_unit_free(u);
        return ERROR;
    }
    if (soac_register_code_unit(c, u) < 0) {
        compiler_unit_free(u);
        return ERROR;
    }

    u->u_deferred_annotations = NULL;
    u->u_conditional_annotation_indices = NULL;
    u->u_next_conditional_annotation_index = 0;
    if (scope_type == COMPILE_SCOPE_CLASS) {
        u->u_static_attributes = PySet_New(0);
        if (!u->u_static_attributes) {
            compiler_unit_free(u);
            return ERROR;
        }
    }
    else {
        u->u_static_attributes = NULL;
    }

    u->u_instr_sequence = (instr_sequence*)_PyInstructionSequence_New();
    if (!u->u_instr_sequence) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_stashed_instr_sequence = NULL;

    /* Push the old compiler_unit on the stack. */
    if (c->u) {
        PyObject *capsule = PyCapsule_New(c->u, CAPSULE_NAME, NULL);
        if (!capsule || PyList_Append(c->c_stack, capsule) < 0) {
            Py_XDECREF(capsule);
            compiler_unit_free(u);
            return ERROR;
        }
        Py_DECREF(capsule);
        if (private == NULL) {
            private = c->u->u_private;
        }
    }

    u->u_private = Py_XNewRef(private);

    c->u = u;
    if (scope_type != COMPILE_SCOPE_MODULE) {
        RETURN_IF_ERROR(compiler_set_qualname(c));
    }
    return SUCCESS;
}

void
_PyCompile_ExitScope(compiler *c)
{
    // Don't call PySequence_DelItem() with an exception raised
    PyObject *exc = PyErr_GetRaisedException();

    instr_sequence *nested_seq = NULL;
    if (c->c_save_nested_seqs) {
        nested_seq = c->u->u_instr_sequence;
        Py_INCREF(nested_seq);
    }
    compiler_unit_free(c->u);
    /* Restore c->u to the parent unit. */
    Py_ssize_t n = PyList_GET_SIZE(c->c_stack) - 1;
    if (n >= 0) {
        PyObject *capsule = PyList_GET_ITEM(c->c_stack, n);
        c->u = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
        assert(c->u);
        /* we are deleting from a list so this really shouldn't fail */
        if (PySequence_DelItem(c->c_stack, n) < 0) {
            PyErr_FormatUnraisable("Exception ignored while removing "
                                   "the last compiler stack item");
        }
        if (nested_seq != NULL) {
            if (_PyInstructionSequence_AddNested(c->u->u_instr_sequence, nested_seq) < 0) {
                PyErr_FormatUnraisable("Exception ignored while appending "
                                       "nested instruction sequence");
            }
        }
    }
    else {
        c->u = NULL;
    }
    Py_XDECREF(nested_seq);

    PyErr_SetRaisedException(exc);
}

/*
 * Frame block handling functions
 */

int
_PyCompile_PushFBlock(compiler *c, location loc,
                     fblocktype t, jump_target_label block_label,
                     jump_target_label exit, void *datum)
{
    fblockinfo *f;
    if (c->u->u_nfblocks >= CO_MAXBLOCKS) {
        return _PyCompile_Error(c, loc, "too many statically nested blocks");
    }
    f = &c->u->u_fblock[c->u->u_nfblocks++];
    f->fb_type = t;
    f->fb_block = block_label;
    f->fb_loc = loc;
    f->fb_exit = exit;
    f->fb_datum = datum;
    f->fb_soac_owner = NULL;
    f->fb_soac_owner_kind = -1;
    f->fb_soac_item = -1;
    if (t == COMPILE_FBLOCK_FINALLY_END) {
        c->c_disable_warning++;
    }
    return SUCCESS;
}

void
_PyCompile_PopFBlock(compiler *c, fblocktype t, jump_target_label block_label)
{
    struct compiler_unit *u = c->u;
    assert(u->u_nfblocks > 0);
    u->u_nfblocks--;
    assert(u->u_fblock[u->u_nfblocks].fb_type == t);
    assert(SAME_JUMP_TARGET_LABEL(u->u_fblock[u->u_nfblocks].fb_block, block_label));
    if (t == COMPILE_FBLOCK_FINALLY_END) {
        c->c_disable_warning--;
    }
}

fblockinfo *
_PyCompile_TopFBlock(compiler *c)
{
    if (c->u->u_nfblocks == 0) {
        return NULL;
    }
    return &c->u->u_fblock[c->u->u_nfblocks - 1];
}

void
_PyCompile_DeferredAnnotations(compiler *c,
                               PyObject **deferred_annotations,
                               PyObject **conditional_annotation_indices)
{
    *deferred_annotations = Py_XNewRef(c->u->u_deferred_annotations);
    *conditional_annotation_indices = Py_XNewRef(c->u->u_conditional_annotation_indices);
}

static location
start_location(asdl_stmt_seq *stmts)
{
    if (asdl_seq_LEN(stmts) > 0) {
        /* Set current line number to the line number of first statement.
         * This way line number for SETUP_ANNOTATIONS will always
         * coincide with the line number of first "real" statement in module.
         * If body is empty, then lineno will be set later in the assembly stage.
         */
        stmt_ty st = (stmt_ty)asdl_seq_GET(stmts, 0);
        return SRC_LOCATION_FROM_AST(st);
    }
    return (const _Py_SourceLocation){1, 1, 0, 0};
}

static int
compiler_codegen(compiler *c, mod_ty mod)
{
    RETURN_IF_ERROR(_PyCodegen_EnterAnonymousScope(c, mod));
    assert(c->u->u_scope_type == COMPILE_SCOPE_MODULE);
    switch (mod->kind) {
    case Module_kind: {
        asdl_stmt_seq *stmts = mod->v.Module.body;
        RETURN_IF_ERROR(_PyCodegen_Module(c, start_location(stmts), stmts, false));
        break;
    }
    case Interactive_kind: {
        c->c_interactive = 1;
        asdl_stmt_seq *stmts = mod->v.Interactive.body;
        RETURN_IF_ERROR(_PyCodegen_Module(c, start_location(stmts), stmts, true));
        break;
    }
    case Expression_kind: {
        RETURN_IF_ERROR(_PyCodegen_Expression(c, mod->v.Expression.body));
        break;
    }
    default: {
        PyErr_Format(PyExc_SystemError,
                     "module kind %d should not be possible",
                     mod->kind);
        return ERROR;
    }}
    return SUCCESS;
}

static PyCodeObject *
compiler_mod(compiler *c, mod_ty mod)
{
    PyCodeObject *co = NULL;
    int addNone = mod->kind != Expression_kind;
    if (compiler_codegen(c, mod) < 0) {
        goto finally;
    }
    co = _PyCompile_OptimizeAndAssemble(c, addNone);
finally:
    _PyCompile_ExitScope(c);
    return co;
}

int
_PyCompile_GetRefType(compiler *c, PyObject *name)
{
    if (c->u->u_scope_type == COMPILE_SCOPE_CLASS &&
        (_PyUnicode_EqualToASCIIString(name, "__class__") ||
         _PyUnicode_EqualToASCIIString(name, "__classdict__") ||
         _PyUnicode_EqualToASCIIString(name, "__conditional_annotations__"))) {
        return CELL;
    }
    PySTEntryObject *ste = c->u->u_ste;
    int scope = _PyST_GetScope(ste, name);
    if (scope == 0) {
        PyErr_Format(PyExc_SystemError,
                     "_PyST_GetScope(name=%R) failed: "
                     "unknown scope in unit %S (%R); "
                     "symbols: %R; locals: %R; "
                     "globals: %R",
                     name,
                     c->u->u_metadata.u_name, ste->ste_id,
                     ste->ste_symbols, c->u->u_metadata.u_varnames,
                     c->u->u_metadata.u_names);
        return ERROR;
    }
    return scope;
}

static int
dict_lookup_arg(PyObject *dict, PyObject *name)
{
    PyObject *v = PyDict_GetItemWithError(dict, name);
    if (v == NULL) {
        return ERROR;
    }
    return PyLong_AsLong(v);
}

int
_PyCompile_LookupCellvar(compiler *c, PyObject *name)
{
    assert(c->u->u_metadata.u_cellvars);
    return dict_lookup_arg(c->u->u_metadata.u_cellvars, name);
}

int
_PyCompile_LookupArg(compiler *c, PyCodeObject *co, PyObject *name)
{
    /* Special case: If a class contains a method with a
     * free variable that has the same name as a method,
     * the name will be considered free *and* local in the
     * class.  It should be handled by the closure, as
     * well as by the normal name lookup logic.
     */
    int reftype = _PyCompile_GetRefType(c, name);
    if (reftype == -1) {
        return ERROR;
    }
    int arg;
    if (reftype == CELL) {
        arg = dict_lookup_arg(c->u->u_metadata.u_cellvars, name);
    }
    else {
        arg = dict_lookup_arg(c->u->u_metadata.u_freevars, name);
    }
    if (arg == -1 && !PyErr_Occurred()) {
        PyObject *freevars = _PyCode_GetFreevars(co);
        if (freevars == NULL) {
            PyErr_Clear();
        }
        PyErr_Format(PyExc_SystemError,
            "compiler_lookup_arg(name=%R) with reftype=%d failed in %S; "
            "freevars of code %S: %R",
            name,
            reftype,
            c->u->u_metadata.u_name,
            co->co_name,
            freevars);
        Py_XDECREF(freevars);
        return ERROR;
    }
    return arg;
}

PyObject *
_PyCompile_StaticAttributesAsTuple(compiler *c)
{
    assert(c->u->u_static_attributes);
    PyObject *static_attributes_unsorted = PySequence_List(c->u->u_static_attributes);
    if (static_attributes_unsorted == NULL) {
        return NULL;
    }
    if (PyList_Sort(static_attributes_unsorted) != 0) {
        Py_DECREF(static_attributes_unsorted);
        return NULL;
    }
    PyObject *static_attributes = PySequence_Tuple(static_attributes_unsorted);
    Py_DECREF(static_attributes_unsorted);
    return static_attributes;
}

int
_PyCompile_ResolveNameop(compiler *c, PyObject *mangled, int scope,
                          _PyCompile_optype *optype, Py_ssize_t *arg)
{
    PyObject *dict = c->u->u_metadata.u_names;
    *optype = COMPILE_OP_NAME;

    assert(scope >= 0);
    switch (scope) {
    case FREE:
        dict = c->u->u_metadata.u_freevars;
        *optype = COMPILE_OP_DEREF;
        break;
    case CELL:
        dict = c->u->u_metadata.u_cellvars;
        *optype = COMPILE_OP_DEREF;
        break;
    case LOCAL:
        if (_PyST_IsFunctionLike(c->u->u_ste)) {
            *optype = COMPILE_OP_FAST;
        }
        else {
            PyObject *item;
            RETURN_IF_ERROR(PyDict_GetItemRef(c->u->u_metadata.u_fasthidden, mangled,
                                              &item));
            if (item == Py_True) {
                *optype = COMPILE_OP_FAST;
            }
            Py_XDECREF(item);
        }
        break;
    case GLOBAL_IMPLICIT:
        if (_PyST_IsFunctionLike(c->u->u_ste)) {
            *optype = COMPILE_OP_GLOBAL;
        }
        break;
    case GLOBAL_EXPLICIT:
        *optype = COMPILE_OP_GLOBAL;
        break;
    default:
        /* scope can be 0 */
        break;
    }
    if (*optype != COMPILE_OP_FAST) {
        *arg = _PyCompile_DictAddObj(dict, mangled);
        RETURN_IF_ERROR(*arg);
    }
    return SUCCESS;
}

int
_PyCompile_TweakInlinedComprehensionScopes(compiler *c, location loc,
                                            PySTEntryObject *entry,
                                            _PyCompile_InlinedComprehensionState *state)
{
    int in_class_block = (c->u->u_ste->ste_type == ClassBlock) && !c->u->u_in_inlined_comp;
    c->u->u_in_inlined_comp++;

    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(entry->ste_symbols, &pos, &k, &v)) {
        long symbol = PyLong_AsLong(v);
        assert(symbol >= 0 || PyErr_Occurred());
        RETURN_IF_ERROR(symbol);
        long scope = SYMBOL_TO_SCOPE(symbol);

        long outsymbol = _PyST_GetSymbol(c->u->u_ste, k);
        RETURN_IF_ERROR(outsymbol);
        long outsc = SYMBOL_TO_SCOPE(outsymbol);

        // If a name has different scope inside than outside the comprehension,
        // we need to temporarily handle it with the right scope while
        // compiling the comprehension. If it's free in the comprehension
        // scope, no special handling; it should be handled the same as the
        // enclosing scope. (If it's free in outer scope and cell in inner
        // scope, we can't treat it as both cell and free in the same function,
        // but treating it as free throughout is fine; it's *_DEREF
        // either way.)
        if ((scope != outsc && scope != FREE && !(scope == CELL && outsc == FREE))
                || in_class_block) {
            if (state->temp_symbols == NULL) {
                state->temp_symbols = PyDict_New();
                if (state->temp_symbols == NULL) {
                    return ERROR;
                }
            }
            // update the symbol to the in-comprehension version and save
            // the outer version; we'll restore it after running the
            // comprehension
            if (PyDict_SetItem(c->u->u_ste->ste_symbols, k, v) < 0) {
                return ERROR;
            }
            PyObject *outv = PyLong_FromLong(outsymbol);
            if (outv == NULL) {
                return ERROR;
            }
            int res = PyDict_SetItem(state->temp_symbols, k, outv);
            Py_DECREF(outv);
            RETURN_IF_ERROR(res);
        }
        // locals handling for names bound in comprehension (DEF_LOCAL |
        // DEF_NONLOCAL occurs in assignment expression to nonlocal)
        if ((symbol & DEF_LOCAL && !(symbol & DEF_NONLOCAL)) || in_class_block) {
            if (!_PyST_IsFunctionLike(c->u->u_ste)) {
                // non-function scope: override this name to use fast locals
                PyObject *orig;
                if (PyDict_GetItemRef(c->u->u_metadata.u_fasthidden, k, &orig) < 0) {
                    return ERROR;
                }
                assert(orig == NULL || orig == Py_True || orig == Py_False);
                if (orig != Py_True) {
                    if (PyDict_SetItem(c->u->u_metadata.u_fasthidden, k, Py_True) < 0) {
                        return ERROR;
                    }
                    if (state->fast_hidden == NULL) {
                        state->fast_hidden = PySet_New(NULL);
                        if (state->fast_hidden == NULL) {
                            return ERROR;
                        }
                    }
                    if (PySet_Add(state->fast_hidden, k) < 0) {
                        return ERROR;
                    }
                }
            }
        }
    }
    return SUCCESS;
}

int
_PyCompile_RevertInlinedComprehensionScopes(compiler *c, location loc,
                                             _PyCompile_InlinedComprehensionState *state)
{
    c->u->u_in_inlined_comp--;
    if (state->temp_symbols) {
        PyObject *k, *v;
        Py_ssize_t pos = 0;
        while (PyDict_Next(state->temp_symbols, &pos, &k, &v)) {
            if (PyDict_SetItem(c->u->u_ste->ste_symbols, k, v)) {
                return ERROR;
            }
        }
        Py_CLEAR(state->temp_symbols);
    }
    if (state->fast_hidden) {
        while (PySet_Size(state->fast_hidden) > 0) {
            PyObject *k = PySet_Pop(state->fast_hidden);
            if (k == NULL) {
                return ERROR;
            }
            // we set to False instead of clearing, so we can track which names
            // were temporarily fast-locals and should use CO_FAST_HIDDEN
            if (PyDict_SetItem(c->u->u_metadata.u_fasthidden, k, Py_False)) {
                Py_DECREF(k);
                return ERROR;
            }
            Py_DECREF(k);
        }
        Py_CLEAR(state->fast_hidden);
    }
    return SUCCESS;
}

void
_PyCompile_EnterConditionalBlock(struct _PyCompiler *c)
{
    c->u->u_in_conditional_block++;
}

void
_PyCompile_LeaveConditionalBlock(struct _PyCompiler *c)
{
    assert(c->u->u_in_conditional_block > 0);
    c->u->u_in_conditional_block--;
}

int
_PyCompile_AddDeferredAnnotation(compiler *c, stmt_ty s,
                                 PyObject **conditional_annotation_index)
{
    if (c->u->u_deferred_annotations == NULL) {
        c->u->u_deferred_annotations = PyList_New(0);
        if (c->u->u_deferred_annotations == NULL) {
            return ERROR;
        }
    }
    if (c->u->u_conditional_annotation_indices == NULL) {
        c->u->u_conditional_annotation_indices = PyList_New(0);
        if (c->u->u_conditional_annotation_indices == NULL) {
            return ERROR;
        }
    }
    PyObject *ptr = PyLong_FromVoidPtr((void *)s);
    if (ptr == NULL) {
        return ERROR;
    }
    if (PyList_Append(c->u->u_deferred_annotations, ptr) < 0) {
        Py_DECREF(ptr);
        return ERROR;
    }
    Py_DECREF(ptr);
    PyObject *index;
    if (c->u->u_scope_type == COMPILE_SCOPE_MODULE || c->u->u_in_conditional_block) {
        index = PyLong_FromLong(c->u->u_next_conditional_annotation_index);
        if (index == NULL) {
            return ERROR;
        }
        *conditional_annotation_index = Py_NewRef(index);
        c->u->u_next_conditional_annotation_index++;
    }
    else {
        index = PyLong_FromLong(-1);
        if (index == NULL) {
            return ERROR;
        }
    }
    int rc = PyList_Append(c->u->u_conditional_annotation_indices, index);
    Py_DECREF(index);
    RETURN_IF_ERROR(rc);
    return SUCCESS;
}

/* Raises a SyntaxError and returns ERROR.
 * If something goes wrong, a different exception may be raised.
 */
int
_PyCompile_Error(compiler *c, location loc, const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    PyObject *msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (msg == NULL) {
        return ERROR;
    }
    _PyErr_RaiseSyntaxError(msg, c->c_filename, loc.lineno, loc.col_offset + 1,
                            loc.end_lineno, loc.end_col_offset + 1);
    Py_DECREF(msg);
    return ERROR;
}

/* Emits a SyntaxWarning and returns 0 on success.
   If a SyntaxWarning raised as error, replaces it with a SyntaxError
   and returns -1.
*/
int
_PyCompile_Warn(compiler *c, location loc, const char *format, ...)
{
    if (c->c_disable_warning) {
        return 0;
    }
    va_list vargs;
    va_start(vargs, format);
    PyObject *msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (msg == NULL) {
        return ERROR;
    }
    int ret = _PyErr_EmitSyntaxWarning(msg, c->c_filename, loc.lineno, loc.col_offset + 1,
                                       loc.end_lineno, loc.end_col_offset + 1,
                                       c->c_module);
    Py_DECREF(msg);
    return ret;
}

PyObject *
_PyCompile_Mangle(compiler *c, PyObject *name)
{
    return _Py_Mangle(c->u->u_private, name);
}

PyObject *
_PyCompile_MaybeMangle(compiler *c, PyObject *name)
{
    return _Py_MaybeMangle(c->u->u_private, c->u->u_ste, name);
}

instr_sequence *
_PyCompile_InstrSequence(compiler *c)
{
    return c->u->u_instr_sequence;
}

int
_PyCompile_StartAnnotationSetup(struct _PyCompiler *c)
{
    instr_sequence *new_seq = (instr_sequence *)_PyInstructionSequence_New();
    if (new_seq == NULL) {
        return ERROR;
    }
    assert(c->u->u_stashed_instr_sequence == NULL);
    c->u->u_stashed_instr_sequence = c->u->u_instr_sequence;
    c->u->u_instr_sequence = new_seq;
    return SUCCESS;
}

int
_PyCompile_EndAnnotationSetup(struct _PyCompiler *c)
{
    assert(c->u->u_stashed_instr_sequence != NULL);
    instr_sequence *parent_seq = c->u->u_stashed_instr_sequence;
    instr_sequence *anno_seq = c->u->u_instr_sequence;
    c->u->u_stashed_instr_sequence = NULL;
    c->u->u_instr_sequence = parent_seq;
    if (_PyInstructionSequence_SetAnnotationsCode(parent_seq, anno_seq) == ERROR) {
        Py_DECREF(anno_seq);
        return ERROR;
    }
    return SUCCESS;
}


int
_PyCompile_FutureFeatures(compiler *c)
{
    return c->c_future.ff_features;
}

struct symtable *
_PyCompile_Symtable(compiler *c)
{
    return c->c_st;
}

PySTEntryObject *
_PyCompile_SymtableEntry(compiler *c)
{
    return c->u->u_ste;
}

int
_PyCompile_OptimizationLevel(compiler *c)
{
    return c->c_optimize;
}

int
_PyCompile_IsInteractiveTopLevel(compiler *c)
{
    assert(c->c_stack != NULL);
    assert(PyList_CheckExact(c->c_stack));
    bool is_nested_scope = PyList_GET_SIZE(c->c_stack) > 0;
    return c->c_interactive && !is_nested_scope;
}

int
_PyCompile_ScopeType(compiler *c)
{
    return c->u->u_scope_type;
}

int
_PyCompile_IsInInlinedComp(compiler *c)
{
    return c->u->u_in_inlined_comp;
}

PyObject *
_PyCompile_Qualname(compiler *c)
{
    assert(c->u->u_metadata.u_qualname);
    return c->u->u_metadata.u_qualname;
}

_PyCompile_CodeUnitMetadata *
_PyCompile_Metadata(compiler *c)
{
    return &c->u->u_metadata;
}

// Merge *obj* with constant cache, without recursion.
int
_PyCompile_ConstCacheMergeOne(PyObject *const_cache, PyObject **obj)
{
    PyObject *key = const_cache_insert(const_cache, *obj, false);
    if (key == NULL) {
        return ERROR;
    }
    if (PyTuple_CheckExact(key)) {
        PyObject *item = PyTuple_GET_ITEM(key, 1);
        Py_SETREF(*obj, Py_NewRef(item));
        Py_DECREF(key);
    }
    else {
        Py_SETREF(*obj, key);
    }
    return SUCCESS;
}

static PyObject *
consts_dict_keys_inorder(PyObject *dict)
{
    PyObject *consts, *k, *v;
    Py_ssize_t i, pos = 0, size = PyDict_GET_SIZE(dict);

    consts = PyList_New(size);   /* PyCode_Optimize() requires a list */
    if (consts == NULL)
        return NULL;
    while (PyDict_Next(dict, &pos, &k, &v)) {
        assert(PyLong_CheckExact(v));
        i = PyLong_AsLong(v);
        /* The keys of the dictionary can be tuples wrapping a constant.
         * (see _PyCompile_DictAddObj and _PyCode_ConstantKey). In that case
         * the object we want is always second. */
        if (PyTuple_CheckExact(k)) {
            k = PyTuple_GET_ITEM(k, 1);
        }
        assert(i < size);
        assert(i >= 0);
        PyList_SET_ITEM(consts, i, Py_NewRef(k));
    }
    return consts;
}

static int
compute_code_flags(compiler *c)
{
    PySTEntryObject *ste = c->u->u_ste;
    int flags = 0;
    if (_PyST_IsFunctionLike(ste)) {
        flags |= CO_NEWLOCALS | CO_OPTIMIZED;
        if (ste->ste_nested)
            flags |= CO_NESTED;
        if (ste->ste_generator && !ste->ste_coroutine)
            flags |= CO_GENERATOR;
        if (ste->ste_generator && ste->ste_coroutine)
            flags |= CO_ASYNC_GENERATOR;
        if (ste->ste_varargs)
            flags |= CO_VARARGS;
        if (ste->ste_varkeywords)
            flags |= CO_VARKEYWORDS;
        if (ste->ste_has_docstring)
            flags |= CO_HAS_DOCSTRING;
        if (ste->ste_method)
            flags |= CO_METHOD;
    }

    if (ste->ste_coroutine && !ste->ste_generator) {
        flags |= CO_COROUTINE;
    }

    /* (Only) inherit compilerflags in PyCF_MASK */
    flags |= (c->c_flags.cf_flags & PyCF_MASK);

    return flags;
}

static PyCodeObject *
optimize_and_assemble_code_unit(struct compiler_unit *u, PyObject *const_cache,
                                int code_flags, PyObject *filename)
{
    if (soac_complete_binding_layout(&u->u_metadata) < 0) {
        return NULL;
    }
    cfg_builder *g = NULL;
    instr_sequence optimized_instrs;
    memset(&optimized_instrs, 0, sizeof(instr_sequence));

    PyCodeObject *co = NULL;
    PyObject *consts = consts_dict_keys_inorder(u->u_metadata.u_consts);
    if (consts == NULL) {
        goto error;
    }
    g = _PyCfg_FromInstructionSequence(u->u_instr_sequence);
    if (g == NULL) {
        goto error;
    }
    int nlocals = (int)PyDict_GET_SIZE(u->u_metadata.u_varnames);
    int nparams = (int)PyList_GET_SIZE(u->u_ste->ste_varnames);
    assert(u->u_metadata.u_firstlineno);

    if (_PyCfg_OptimizeCodeUnit(g, consts, const_cache, nlocals,
                                nparams, u->u_metadata.u_firstlineno) < 0) {
        goto error;
    }

    int stackdepth;
    int nlocalsplus;
    if (_PyCfg_OptimizedCfgToInstructionSequence(g, &u->u_metadata,
                                                 &stackdepth, &nlocalsplus,
                                                 &optimized_instrs) < 0) {
        goto error;
    }

    /** Assembly **/
    co = _PyAssemble_MakeCodeObject(&u->u_metadata, const_cache, consts,
                                    stackdepth, &optimized_instrs, nlocalsplus,
                                    code_flags, filename);
    if (co != NULL && u->u_metadata.u_soac_bindings != NULL) {
        soac_code_bindings *bindings = u->u_metadata.u_soac_bindings;
        if (bindings->code != NULL) {
            soac_binding_error("one compiler unit assembled more than once");
            Py_CLEAR(co);
        }
        else {
            bindings->code = (PyCodeObject *)Py_NewRef(co);
        }
    }

error:
    Py_XDECREF(consts);
    PyInstructionSequence_Fini(&optimized_instrs);
    _PyCfgBuilder_Free(g);
    return co;
}


PyCodeObject *
_PyCompile_OptimizeAndAssemble(compiler *c, int addNone)
{
    struct compiler_unit *u = c->u;
    PyObject *const_cache = c->c_const_cache;
    PyObject *filename = c->c_filename;

    int code_flags = compute_code_flags(c);
    if (code_flags < 0) {
        return NULL;
    }

    if (_PyCodegen_AddReturnAtEnd(c, addNone) < 0) {
        return NULL;
    }

    return optimize_and_assemble_code_unit(u, const_cache, code_flags, filename);
}

static PyCodeObject *
ast_compile_with_binding_details(mod_ty mod, PyObject *filename, PyCompilerFlags *pflags,
                                  int optimize, PyArena *arena, PyObject *module,
                                  PyObject **bindings)
{
    assert(!PyErr_Occurred());
    compiler *c = new_compiler(mod, filename, pflags, optimize, arena, module);
    if (c == NULL) {
        return NULL;
    }
    if (bindings != NULL) {
        *bindings = NULL;
        c->c_soac_bindings = PyMem_Calloc(1, sizeof(*c->c_soac_bindings));
        if (c->c_soac_bindings == NULL) {
            PyErr_NoMemory();
            compiler_free(c);
            return NULL;
        }
    }

    PyCodeObject *co = compiler_mod(c, mod);
    if (co != NULL && bindings != NULL) {
        *bindings = soac_binding_details(c->c_soac_bindings, co);
        if (*bindings == NULL) {
            Py_CLEAR(co);
        }
    }
    compiler_free(c);
    assert(co || PyErr_Occurred());
    return co;
}

PyCodeObject *
_PyAST_Compile(mod_ty mod, PyObject *filename, PyCompilerFlags *pflags,
               int optimize, PyArena *arena, PyObject *module)
{
    return ast_compile_with_binding_details(mod, filename, pflags,
                                            optimize, arena, module, NULL);
}

PyCodeObject *
_PyAST_CompileWithSoacClassBindings(mod_ty mod, PyObject *filename,
                                   PyCompilerFlags *pflags, int optimize,
                                   PyArena *arena, PyObject *module,
                                   PyObject **bindings)
{
    if (bindings == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    *bindings = NULL;
    return ast_compile_with_binding_details(mod, filename, pflags,
                                            optimize, arena, module, bindings);
}

int
_PyCompile_AstPreprocess(mod_ty mod, PyObject *filename, PyCompilerFlags *cf,
                         int optimize, PyArena *arena, int no_const_folding,
                         PyObject *module)
{
    _PyFutureFeatures future;
    if (!_PyFuture_FromAST(mod, filename, &future)) {
        return -1;
    }
    int flags = future.ff_features | cf->cf_flags;
    if (optimize == -1) {
        optimize = _Py_GetConfig()->optimization_level;
    }
    if (!_PyAST_Preprocess(mod, arena, filename, optimize, flags,
                           no_const_folding, 0, module))
    {
        return -1;
    }
    return 0;
}

// C implementation of inspect.cleandoc()
//
// Difference from inspect.cleandoc():
// - Do not remove leading and trailing blank lines to keep lineno.
PyObject *
_PyCompile_CleanDoc(PyObject *doc)
{
    doc = PyObject_CallMethod(doc, "expandtabs", NULL);
    if (doc == NULL) {
        return NULL;
    }

    Py_ssize_t doc_size;
    const char *doc_utf8 = PyUnicode_AsUTF8AndSize(doc, &doc_size);
    if (doc_utf8 == NULL) {
        Py_DECREF(doc);
        return NULL;
    }
    const char *p = doc_utf8;
    const char *pend = p + doc_size;

    // First pass: find minimum indentation of any non-blank lines
    // after first line.
    while (p < pend && *p++ != '\n') {
    }

    Py_ssize_t margin = PY_SSIZE_T_MAX;
    while (p < pend) {
        const char *s = p;
        while (*p == ' ') p++;
        if (p < pend && *p != '\n') {
            margin = Py_MIN(margin, p - s);
        }
        while (p < pend && *p++ != '\n') {
        }
    }
    if (margin == PY_SSIZE_T_MAX) {
        margin = 0;
    }

    // Second pass: write cleandoc into buff.

    // copy first line without leading spaces.
    p = doc_utf8;
    while (*p == ' ') {
        p++;
    }
    if (p == doc_utf8 && margin == 0 ) {
        // doc is already clean.
        return doc;
    }

    char *buff = PyMem_Malloc(doc_size);
    if (buff == NULL){
        Py_DECREF(doc);
        PyErr_NoMemory();
        return NULL;
    }

    char *w = buff;

    while (p < pend) {
        int ch = *w++ = *p++;
        if (ch == '\n') {
            break;
        }
    }

    // copy subsequent lines without margin.
    while (p < pend) {
        for (Py_ssize_t i = 0; i < margin; i++, p++) {
            if (*p != ' ') {
                assert(*p == '\n' || *p == '\0');
                break;
            }
        }
        while (p < pend) {
            int ch = *w++ = *p++;
            if (ch == '\n') {
                break;
            }
        }
    }

    Py_DECREF(doc);
    PyObject *res = PyUnicode_FromStringAndSize(buff, w - buff);
    PyMem_Free(buff);
    return res;
}

/* Access to compiler optimizations for unit tests.
 *
 * _PyCompile_CodeGen takes an AST, applies code-gen and
 * returns the unoptimized CFG as an instruction list.
 *
 */
PyObject *
_PyCompile_CodeGen(PyObject *ast, PyObject *filename, PyCompilerFlags *pflags,
                   int optimize, int compile_mode)
{
    PyObject *res = NULL;
    PyObject *metadata = NULL;

    if (!PyAST_Check(ast)) {
        PyErr_SetString(PyExc_TypeError, "expected an AST");
        return NULL;
    }

    PyArena *arena = _PyArena_New();
    if (arena == NULL) {
        return NULL;
    }

    mod_ty mod = PyAST_obj2mod(ast, arena, compile_mode);
    if (mod == NULL || !_PyAST_Validate(mod)) {
        _PyArena_Free(arena);
        return NULL;
    }

    compiler *c = new_compiler(mod, filename, pflags, optimize, arena, NULL);
    if (c == NULL) {
        _PyArena_Free(arena);
        return NULL;
    }
    c->c_save_nested_seqs = true;

    metadata = PyDict_New();
    if (metadata == NULL) {
        return NULL;
    }

    if (compiler_codegen(c, mod) < 0) {
        goto finally;
    }

    _PyCompile_CodeUnitMetadata *umd = &c->u->u_metadata;

#define SET_METADATA_INT(key, value) do { \
        PyObject *v = PyLong_FromLong((long)value); \
        if (v == NULL) goto finally; \
        int res = PyDict_SetItemString(metadata, key, v); \
        Py_XDECREF(v); \
        if (res < 0) goto finally; \
    } while (0);

    SET_METADATA_INT("argcount", umd->u_argcount);
    SET_METADATA_INT("posonlyargcount", umd->u_posonlyargcount);
    SET_METADATA_INT("kwonlyargcount", umd->u_kwonlyargcount);
#undef SET_METADATA_INT

    int addNone = mod->kind != Expression_kind;
    if (_PyCodegen_AddReturnAtEnd(c, addNone) < 0) {
        goto finally;
    }

    if (_PyInstructionSequence_ApplyLabelMap(_PyCompile_InstrSequence(c)) < 0) {
        return NULL;
    }
    /* Allocate a copy of the instruction sequence on the heap */
    res = PyTuple_Pack(2, _PyCompile_InstrSequence(c), metadata);

finally:
    Py_XDECREF(metadata);
    _PyCompile_ExitScope(c);
    compiler_free(c);
    _PyArena_Free(arena);
    return res;
}

int _PyCfg_JumpLabelsToTargets(cfg_builder *g);

PyCodeObject *
_PyCompile_Assemble(_PyCompile_CodeUnitMetadata *umd, PyObject *filename,
                    PyObject *seq)
{
    if (!_PyInstructionSequence_Check(seq)) {
        PyErr_SetString(PyExc_TypeError, "expected an instruction sequence");
        return NULL;
    }
    /* This low-level assembly API has no owned-source compilation. Its callers
     * may initialize only the public metadata fields; never read a supplied or
     * uninitialized collector pointer as source provenance. */
    umd->u_soac_bindings = NULL;
    cfg_builder *g = NULL;
    PyCodeObject *co = NULL;
    instr_sequence optimized_instrs;
    memset(&optimized_instrs, 0, sizeof(instr_sequence));

    PyObject *const_cache = PyDict_New();
    if (const_cache == NULL) {
        return NULL;
    }

    g = _PyCfg_FromInstructionSequence((instr_sequence*)seq);
    if (g == NULL) {
        goto error;
    }

    if (_PyCfg_JumpLabelsToTargets(g) < 0) {
        goto error;
    }

    int code_flags = 0;
    int stackdepth, nlocalsplus;
    if (_PyCfg_OptimizedCfgToInstructionSequence(g, umd,
                                                 &stackdepth, &nlocalsplus,
                                                 &optimized_instrs) < 0) {
        goto error;
    }

    PyObject *consts = consts_dict_keys_inorder(umd->u_consts);
    if (consts == NULL) {
        goto error;
    }
    co = _PyAssemble_MakeCodeObject(umd, const_cache,
                                    consts, stackdepth, &optimized_instrs,
                                    nlocalsplus, code_flags, filename);
    Py_DECREF(consts);

error:
    Py_DECREF(const_cache);
    _PyCfgBuilder_Free(g);
    PyInstructionSequence_Fini(&optimized_instrs);
    return co;
}

/* Retained for API compatibility.
 * Optimization is now done in _PyCfg_OptimizeCodeUnit */

PyObject *
PyCode_Optimize(PyObject *code, PyObject* Py_UNUSED(consts),
                PyObject *Py_UNUSED(names), PyObject *Py_UNUSED(lnotab_obj))
{
    return Py_NewRef(code);
}
