#ifndef Py_INTERNAL_COMPILE_H
#define Py_INTERNAL_COMPILE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdbool.h>

#include "pycore_ast.h"       // mod_ty
#include "pycore_symtable.h"  // _Py_SourceLocation
#include "pycore_instruction_sequence.h"

/* A soft limit for stack use, to avoid excessive
 * memory use for large constants, etc.
 *
 * The value 30 is plucked out of thin air.
 * Code that could use more stack than this is
 * rare, so the exact value is unimportant.
 */
#define _PY_STACK_USE_GUIDELINE 30

struct _arena;   // Type defined in pycore_pyarena.h
struct _mod;     // Type defined in pycore_ast.h

// Export for 'test_peg_generator' shared extension
PyAPI_FUNC(PyCodeObject*) _PyAST_Compile(
    struct _mod *mod,
    PyObject *filename,
    PyCompilerFlags *flags,
    int optimize,
    struct _arena *arena,
    PyObject *module);

/* AST preprocessing */
extern int _PyCompile_AstPreprocess(
    struct _mod *mod,
    PyObject *filename,
    PyCompilerFlags *flags,
    int optimize,
    struct _arena *arena,
    int syntax_check_only,
    PyObject *module);

extern int _PyAST_Preprocess(
    struct _mod *,
    struct _arena *arena,
    PyObject *filename,
    int optimize,
    int ff_features,
    int syntax_check_only,
    int enable_warnings,
    PyObject *module);


struct _PySoacCodeUnitBindings;

typedef struct {
    PyObject *u_name;
    PyObject *u_qualname;  /* dot-separated qualified name (lazy) */

    /* The following fields are dicts that map objects to
       the index of them in co_XXX.      The index is used as
       the argument for opcodes that refer to those collections.
    */
    PyObject *u_consts;    /* all constants */
    PyObject *u_names;     /* all names */
    PyObject *u_varnames;  /* local variables */
    PyObject *u_cellvars;  /* cell variables */
    PyObject *u_freevars;  /* free variables */
    PyObject *u_fasthidden; /* dict; keys are names that are fast-locals only
                               temporarily within an inlined comprehension. When
                               value is True, treat as fast-local. */

    Py_ssize_t u_argcount;        /* number of arguments for block */
    Py_ssize_t u_posonlyargcount;        /* number of positional only arguments for block */
    Py_ssize_t u_kwonlyargcount; /* number of keyword only arguments for block */

    int u_firstlineno; /* the first lineno of the block */
    /* Non-NULL only for the explicit verified-source details compilation. */
    struct _PySoacCodeUnitBindings *u_soac_bindings;
} _PyCompile_CodeUnitMetadata;

struct _PyCompiler;

/* Data-only native class-binding collection. Ordinary compiler entrypoints do
 * not construct a collector, and these hooks never authorize code execution. */
PyCodeObject *_PyAST_CompileWithSoacClassBindings(
    struct _mod *mod, PyObject *filename, PyCompilerFlags *flags,
    int optimize, struct _arena *arena, PyObject *module, PyObject **bindings);
int _PyCompile_SoacEnterComprehension(struct _PyCompiler *, expr_ty, PySTEntryObject *);
int _PyCompile_SoacSaveLocal(struct _PyCompiler *, int local_index);
int _PyCompile_SoacMakeCell(struct _PyCompiler *, int deref_index);
int _PyCompile_SoacLeaveComprehension(struct _PyCompiler *);
int _PyCompile_SoacCapture(struct _PyCompiler *, PyCodeObject *,
                          _Py_SourceLocation, int free_ordinal, int deref_index);
int _PyCompile_SoacClassInitializer(struct _PyCompiler *, PyObject *name, int role);
int _PyCompile_SoacClassExport(struct _PyCompiler *, int deref_index, int role);
int _PyCompile_SoacNameAccess(struct _PyCompiler *, _Py_SourceLocation,
                             expr_ty original, expr_context_ty, int mode, int native_index);
int _PyCompile_SoacFixSlots(_PyCompile_CodeUnitMetadata *,
                           const int *fixed, int noffsets);
int _PyCompile_SoacReferenceOrigin(struct _PyCompiler *, _Py_SourceLocation,
                                  const void *original, int kind, expr_context_ty);
int _PyCompile_SoacFinalReferenceInstruction(_PyCompile_CodeUnitMetadata *,
                                            int ordinal, int opcode, int oparg,
                                            _PySoacReadOrigins);
int _PyCompile_SoacFinishReferences(_PyCompile_CodeUnitMetadata *, int count);

/* Native ownership facts in the same compile-local source catalogue. */
typedef struct {
    int role;
    int generator;
} _PySoacScopeBindingContext;

_PySoacScopeBindingContext _PyCompile_SoacScopeBindingContext(
    struct _PyCompiler *, int role, int generator);
void _PyCompile_SoacRestoreScopeBindingContext(
    struct _PyCompiler *, _PySoacScopeBindingContext);

/* Original source operation provenance, only for the existing opt-in product.
 * None of these compiler hooks grants runtime or source-body authority. */
int _PyCompile_SoacBindingOrigin(struct _PyCompiler *, _Py_SourceLocation,
                                const void *, int kind, int phase,
                                expr_context_ty);
int _PyCompile_SoacCallStart(struct _PyCompiler *, _Py_SourceLocation,
                            const void *, int kind, int detail,
                            PyCodeObject *child, uint32_t *origin);
int _PyCompile_SoacCallInput(struct _PyCompiler *, uint32_t origin,
                            int channel, int preloaded, int positional_kind,
                            asdl_expr_seq *, int injected_generic_base,
                            int keyword_kind, asdl_keyword_seq *);
int _PyCompile_SoacCallGroup(struct _PyCompiler *, uint32_t origin,
                            int kind, Py_ssize_t first, Py_ssize_t count,
                            int map_style);
int _PyCompile_SoacCallKeywordConstant(struct _PyCompiler *, uint32_t origin);
int _PyCompile_SoacCallKeywordNames(struct _PyCompiler *, uint32_t origin,
                                   PyObject *names);
int _PyCompile_SoacCallEmit(struct _PyCompiler *, uint32_t origin);
int _PyCompile_SoacCallAlternative(struct _PyCompiler *, uint32_t origin,
                                  int reason);
int _PyCompile_SoacCallPreparation(struct _PyCompiler *, uint32_t origin);
PyObject *_PyCompile_SoacPatternLeaf(struct _PyCompiler *, pattern_ty, int kind);
int _PyCompile_SoacPatternStore(struct _PyCompiler *, pattern_ty,
                               PyObject *leaf_origins);
int _PyCompile_SoacPushContext(struct _PyCompiler *, int owner_kind,
                              _Py_SourceLocation owner_loc, const void *owner,
                              int item, int entry,
                              _Py_SourceLocation transfer_loc, const void *transfer,
                              int payload, Py_ssize_t *previous);
void _PyCompile_SoacPopContext(struct _PyCompiler *, Py_ssize_t previous);
Py_ssize_t _PyCompile_SoacBeginUnwindContext(struct _PyCompiler *);
int _PyCompile_SoacAssembledInstruction(_PyCompile_CodeUnitMetadata *,
                                      int ordinal, const _PyInstruction *,
                                      Py_ssize_t opcode_offset,
                                       Py_ssize_t first_byte, Py_ssize_t end_byte);
int _PyCompile_SoacFinishAssembly(_PyCompile_CodeUnitMetadata *, int count,
                                 Py_ssize_t code_size);

typedef enum {
    COMPILE_OP_FAST,
    COMPILE_OP_GLOBAL,
    COMPILE_OP_DEREF,
    COMPILE_OP_NAME,
} _PyCompile_optype;

/* _PyCompile_FBlockInfo tracks the current frame block.
 *
 * A frame block is used to handle loops, try/except, and try/finally.
 * It's called a frame block to distinguish it from a basic block in the
 * compiler IR.
 */

enum _PyCompile_FBlockType {
     COMPILE_FBLOCK_WHILE_LOOP,
     COMPILE_FBLOCK_FOR_LOOP,
     COMPILE_FBLOCK_ASYNC_FOR_LOOP,
     COMPILE_FBLOCK_TRY_EXCEPT,
     COMPILE_FBLOCK_FINALLY_TRY,
     COMPILE_FBLOCK_FINALLY_END,
     COMPILE_FBLOCK_WITH,
     COMPILE_FBLOCK_ASYNC_WITH,
     COMPILE_FBLOCK_HANDLER_CLEANUP,
     COMPILE_FBLOCK_POP_VALUE,
     COMPILE_FBLOCK_EXCEPTION_HANDLER,
     COMPILE_FBLOCK_EXCEPTION_GROUP_HANDLER,
     COMPILE_FBLOCK_ASYNC_COMPREHENSION_GENERATOR,
     COMPILE_FBLOCK_STOP_ITERATION,
};

typedef struct {
    enum _PyCompile_FBlockType fb_type;
    _PyJumpTargetLabel fb_block;
    _Py_SourceLocation fb_loc;
    /* (optional) type-specific exit or cleanup block */
    _PyJumpTargetLabel fb_exit;
    /* (optional) additional information required for unwinding */
    void *fb_datum;
    /* Original semantic cleanup owner; never inferred from fb_loc or names. */
    const void *fb_soac_owner;
    int fb_soac_owner_kind;
    int fb_soac_item;
} _PyCompile_FBlockInfo;


int _PyCompile_PushFBlock(struct _PyCompiler *c, _Py_SourceLocation loc,
                          enum _PyCompile_FBlockType t,
                          _PyJumpTargetLabel block_label,
                          _PyJumpTargetLabel exit, void *datum);
void _PyCompile_PopFBlock(struct _PyCompiler *c, enum _PyCompile_FBlockType t,
                          _PyJumpTargetLabel block_label);
_PyCompile_FBlockInfo *_PyCompile_TopFBlock(struct _PyCompiler *c);

int _PyCompile_EnterScope(struct _PyCompiler *c, identifier name, int scope_type,
                          void *key, int lineno, PyObject *private,
                          _PyCompile_CodeUnitMetadata *umd);
void _PyCompile_ExitScope(struct _PyCompiler *c);
Py_ssize_t _PyCompile_AddConst(struct _PyCompiler *c, PyObject *o);
_PyInstructionSequence *_PyCompile_InstrSequence(struct _PyCompiler *c);
int _PyCompile_StartAnnotationSetup(struct _PyCompiler *c);
int _PyCompile_EndAnnotationSetup(struct _PyCompiler *c);
int _PyCompile_FutureFeatures(struct _PyCompiler *c);
void _PyCompile_DeferredAnnotations(
    struct _PyCompiler *c, PyObject **deferred_annotations,
    PyObject **conditional_annotation_indices);
PyObject *_PyCompile_Mangle(struct _PyCompiler *c, PyObject *name);
PyObject *_PyCompile_MaybeMangle(struct _PyCompiler *c, PyObject *name);
int _PyCompile_MaybeAddStaticAttributeToClass(struct _PyCompiler *c, expr_ty e);
int _PyCompile_GetRefType(struct _PyCompiler *c, PyObject *name);
int _PyCompile_LookupCellvar(struct _PyCompiler *c, PyObject *name);
int _PyCompile_ResolveNameop(struct _PyCompiler *c, PyObject *mangled, int scope,
                             _PyCompile_optype *optype, Py_ssize_t *arg);

int _PyCompile_IsInteractiveTopLevel(struct _PyCompiler *c);
int _PyCompile_IsInInlinedComp(struct _PyCompiler *c);
int _PyCompile_ScopeType(struct _PyCompiler *c);
int _PyCompile_OptimizationLevel(struct _PyCompiler *c);
int _PyCompile_LookupArg(struct _PyCompiler *c, PyCodeObject *co, PyObject *name);
PyObject *_PyCompile_Qualname(struct _PyCompiler *c);
_PyCompile_CodeUnitMetadata *_PyCompile_Metadata(struct _PyCompiler *c);
PyObject *_PyCompile_StaticAttributesAsTuple(struct _PyCompiler *c);

struct symtable *_PyCompile_Symtable(struct _PyCompiler *c);
PySTEntryObject *_PyCompile_SymtableEntry(struct _PyCompiler *c);

enum {
    COMPILE_SCOPE_MODULE,
    COMPILE_SCOPE_CLASS,
    COMPILE_SCOPE_FUNCTION,
    COMPILE_SCOPE_ASYNC_FUNCTION,
    COMPILE_SCOPE_LAMBDA,
    COMPILE_SCOPE_COMPREHENSION,
    COMPILE_SCOPE_ANNOTATIONS,
};


typedef struct {
    PyObject *pushed_locals;
    PyObject *temp_symbols;
    PyObject *fast_hidden;
    _PyJumpTargetLabel cleanup;
} _PyCompile_InlinedComprehensionState;

int _PyCompile_TweakInlinedComprehensionScopes(struct _PyCompiler *c, _Py_SourceLocation loc,
                                               PySTEntryObject *entry,
                                               _PyCompile_InlinedComprehensionState *state);
int _PyCompile_RevertInlinedComprehensionScopes(struct _PyCompiler *c, _Py_SourceLocation loc,
                                                _PyCompile_InlinedComprehensionState *state);
int _PyCompile_AddDeferredAnnotation(struct _PyCompiler *c, stmt_ty s,
                                     PyObject **conditional_annotation_index);
void _PyCompile_EnterConditionalBlock(struct _PyCompiler *c);
void _PyCompile_LeaveConditionalBlock(struct _PyCompiler *c);

int _PyCodegen_AddReturnAtEnd(struct _PyCompiler *c, int addNone);
int _PyCodegen_EnterAnonymousScope(struct _PyCompiler* c, mod_ty mod);
int _PyCodegen_Expression(struct _PyCompiler *c, expr_ty e);
int _PyCodegen_Module(struct _PyCompiler *c, _Py_SourceLocation loc, asdl_stmt_seq *stmts,
                      bool is_interactive);

int _PyCompile_ConstCacheMergeOne(PyObject *const_cache, PyObject **obj);

PyCodeObject *_PyCompile_OptimizeAndAssemble(struct _PyCompiler *c, int addNone);

Py_ssize_t _PyCompile_DictAddObj(PyObject *dict, PyObject *o);
int _PyCompile_Error(struct _PyCompiler *c, _Py_SourceLocation loc, const char *format, ...);
int _PyCompile_Warn(struct _PyCompiler *c, _Py_SourceLocation loc, const char *format, ...);

// Export for '_opcode' extension module
PyAPI_FUNC(PyObject*) _PyCompile_GetUnaryIntrinsicName(int index);
PyAPI_FUNC(PyObject*) _PyCompile_GetBinaryIntrinsicName(int index);

/* Access compiler internals for unit testing */

// Export for '_testinternalcapi' shared extension
PyAPI_FUNC(PyObject*) _PyCompile_CleanDoc(PyObject *doc);

// Export for '_testinternalcapi' shared extension
PyAPI_FUNC(PyObject*) _PyCompile_CodeGen(
        PyObject *ast,
        PyObject *filename,
        PyCompilerFlags *flags,
        int optimize,
        int compile_mode);

// Export for '_testinternalcapi' shared extension
PyAPI_FUNC(PyCodeObject*)
_PyCompile_Assemble(_PyCompile_CodeUnitMetadata *umd, PyObject *filename,
                    PyObject *instructions);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COMPILE_H */
