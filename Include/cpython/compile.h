#ifndef Py_CPYTHON_COMPILE_H
#  error "this header file must not be included directly"
#endif

/* Public interface */
#define PyCF_MASK (CO_FUTURE_DIVISION | CO_FUTURE_ABSOLUTE_IMPORT | \
                   CO_FUTURE_WITH_STATEMENT | CO_FUTURE_PRINT_FUNCTION | \
                   CO_FUTURE_UNICODE_LITERALS | CO_FUTURE_BARRY_AS_BDFL | \
                   CO_FUTURE_GENERATOR_STOP | CO_FUTURE_ANNOTATIONS | \
                   CO_FUTURE_STRICT)
#define PyCF_MASK_OBSOLETE (CO_NESTED)

/* bpo-39562: CO_FUTURE_ and PyCF_ constants must be kept unique.
   PyCF_ constants can use bits from 0x0100 to 0x10000.
   CO_FUTURE_ constants use bits starting at 0x20000. */
#define PyCF_SOURCE_IS_UTF8  0x0100
#define PyCF_DONT_IMPLY_DEDENT 0x0200
#define PyCF_ONLY_AST 0x0400
#define PyCF_IGNORE_COOKIE 0x0800
#define PyCF_TYPE_COMMENTS 0x1000
#define PyCF_ALLOW_TOP_LEVEL_AWAIT 0x2000
#define PyCF_ALLOW_INCOMPLETE_INPUT 0x4000
#define PyCF_OPTIMIZED_AST (0x8000 | PyCF_ONLY_AST)
#define PyCF_COMPILE_MASK (PyCF_ONLY_AST | PyCF_ALLOW_TOP_LEVEL_AWAIT | \
                           PyCF_TYPE_COMMENTS | PyCF_DONT_IMPLY_DEDENT | \
                           PyCF_ALLOW_INCOMPLETE_INPUT | PyCF_OPTIMIZED_AST)

typedef struct {
    int cf_flags;  /* bitmask of CO_xxx flags relevant to future */
    int cf_feature_version;  /* minor Python version (PyCF_ONLY_AST) */
} PyCompilerFlags;

#define _PyCompilerFlags_INIT \
    (PyCompilerFlags){.cf_flags = 0, .cf_feature_version = PY_MINOR_VERSION}

/* Future feature support */

#define FUTURE_NESTED_SCOPES "nested_scopes"
#define FUTURE_GENERATORS "generators"
#define FUTURE_DIVISION "division"
#define FUTURE_ABSOLUTE_IMPORT "absolute_import"
#define FUTURE_WITH_STATEMENT "with_statement"
#define FUTURE_PRINT_FUNCTION "print_function"
#define FUTURE_UNICODE_LITERALS "unicode_literals"
#define FUTURE_BARRY_AS_BDFL "barry_as_FLUFL"
#define FUTURE_GENERATOR_STOP "generator_stop"
#define FUTURE_ANNOTATIONS "annotations"
#define FUTURE_STRICT "strict"

/* Trusted native loader boundary. The caller must first authenticate these
 * exact source bytes against the immutable startup deployment authority.
 * Returned original code is inspectable, never an ordinary native fallback. */
PyAPI_FUNC(PyObject *) PySoac_CompileVerifiedSource(
    const char *source, Py_ssize_t length, PyObject *filename, int optimize);

/* Compile the same authenticated source once, returning (code, strings,
 * (Py_SOAC_CLASS_BINDINGS_SCHEMA, nodes, class_recipes, read_tables)). All containers are
 * exact immutable tuples. Each node refers to an exact member of code's final
 * constant tree; metadata never authorizes execution of that member.
 * strings is an exact tuple of (line, column, end_line, end_column, text)
 * entries for future-stringized annotations in that native AST. The returned
 * ordinary data is not an execution or optimizer capability.
 *
 * Version 2 preserves these class rows (IDs nonnegative, optional values None):
 * node = (id, parent_id, exact_code, scope_kind, symtable_kind, source_span)
 * recipe = (class_id, owners, initializers, regions, captures, exports, accesses)
 * owner = (id, kind, final_localsplus_index, native_kind_byte, region_id)
 * initializer = (phase, entry_owner_id, role, incoming_free_ordinal_or_None)
 * region = (id, parent_region_id, source_span, ordered_entry_ops, restores)
 * entry_op = (kind, final_slot_index, allocation_or_saved_owner_id)
 * restore = (final_slot_index, saved_owner_id)
 * current_slot = (Py_SOAC_CLASS_CURRENT_SLOT, final_slot_index)
 * capture = (child_id, creation_span, child_freevar_ordinal, current_slot)
 * export = (role, current_slot)
 * access = (original_Name_span, context, mode, current_slot)
 * source_span = (line, UTF8_byte_column, end_line, end_UTF8_byte_column),
 * or None when unavailable. Zero-width spans are permitted, not invented.
 *
 * Current slots, not allocation IDs, select closure/frame values after joins.
 * SaveClear moves the actual current raw value; MakeCellFromCurrent wraps that
 * raw value and replaces its slot only on success. Entry ops precede native
 * SETUP_FINALLY: restores protect the entered body/result, not partial entry.
 * Failed entry drops completed saved tokens as ordinary operand temporaries. */
PyAPI_FUNC(PyObject *) PySoac_CompileVerifiedSourceDetails(
    const char *source, Py_ssize_t length, PyObject *filename, int optimize);

/* Fixed wire values, independent of internal compiler enum numbering. */
#define Py_SOAC_CLASS_BINDINGS_SCHEMA 2
/* Native reference receipts share the same final code-node tree.
 * table = (code_id, final_CFG_instruction_count, reads, gaps)
 * read = (original_Name_span, emissions), including zero-emission dead sites
 * emission = (ordinal, form, first_slot, second_slot_or_None, lane,
 *             preceding_binding_origin_or_None)
 * binding_origin = (kind, original_AST_span), never guessed from i_loc
 * gap = (reason, original_Name_span_or_None, ordinal_or_None,
 *        lane_or_None, native_opcode_or_None)
 * A missing, unsupported, divergent or paired-only receipt cannot select a
 * standalone reference operation. Gap opcodes are pinned diagnostic data.
 * All ordinals precede assembler expansion and are not runtime positions. */
#define Py_SOAC_READ_FAST_DUPLICATE 0
#define Py_SOAC_READ_FAST_CHECKED_DUPLICATE 1
#define Py_SOAC_READ_FAST_BORROW 2
#define Py_SOAC_READ_FAST_PAIR_DUPLICATE 3
#define Py_SOAC_READ_FAST_PAIR_BORROW 4
#define Py_SOAC_READ_STORE_FAST_LOAD_FAST 5
#define Py_SOAC_BINDING_NAME 0
#define Py_SOAC_BINDING_FUNCTION 1
#define Py_SOAC_BINDING_ASYNC_FUNCTION 2
#define Py_SOAC_BINDING_CLASS 3
#define Py_SOAC_BINDING_IMPORT_ALIAS 4
#define Py_SOAC_BINDING_IMPORT_FROM_ALIAS 5
#define Py_SOAC_BINDING_EXCEPT_ALIAS 6
#define Py_SOAC_READ_GAP_ELIMINATED 0
#define Py_SOAC_READ_GAP_UNSUPPORTED 1
#define Py_SOAC_READ_GAP_MISSING_ORIGIN 2
#define Py_SOAC_READ_GAP_MISSING_STORE 3
#define Py_SOAC_READ_GAP_DIVERGENT 4
#define Py_SOAC_SCOPE_MODULE 0
#define Py_SOAC_SCOPE_CLASS 1
#define Py_SOAC_SCOPE_FUNCTION 2
#define Py_SOAC_SCOPE_ASYNC_FUNCTION 3
#define Py_SOAC_SCOPE_LAMBDA 4
#define Py_SOAC_SCOPE_COMPREHENSION 5
#define Py_SOAC_SCOPE_ANNOTATIONS 6
#define Py_SOAC_SYMTABLE_FUNCTION 0
#define Py_SOAC_SYMTABLE_CLASS 1
#define Py_SOAC_SYMTABLE_MODULE 2
#define Py_SOAC_SYMTABLE_ANNOTATION 3
#define Py_SOAC_SYMTABLE_TYPE_ALIAS 4
#define Py_SOAC_SYMTABLE_TYPE_PARAMETERS 5
#define Py_SOAC_SYMTABLE_TYPE_VARIABLE 6
#define Py_SOAC_CLASS_PHASE_ENTRY 0
#define Py_SOAC_CLASS_PHASE_HEADER 1
#define Py_SOAC_CLASS_INIT_UNBOUND 0
#define Py_SOAC_CLASS_INIT_EMPTY_CELL 1
#define Py_SOAC_CLASS_INIT_INCOMING_FREE 2
#define Py_SOAC_CLASS_INIT_NAMESPACE 3
#define Py_SOAC_CLASS_INIT_CONDITIONAL_SET 4
#define Py_SOAC_CLASS_OWNER_ENTRY 0
#define Py_SOAC_CLASS_OWNER_FRESH_CELL 1
#define Py_SOAC_CLASS_OWNER_SAVED_SLOT 2
#define Py_SOAC_CLASS_OP_SAVE_CLEAR 0
#define Py_SOAC_CLASS_OP_MAKE_CELL 1
#define Py_SOAC_CLASS_CURRENT_SLOT 0
#define Py_SOAC_CLASS_EXPORT_CLASSCELL 0
#define Py_SOAC_CLASS_EXPORT_CLASSDICTCELL 1
#define Py_SOAC_CLASS_ACCESS_LOAD 0
#define Py_SOAC_CLASS_ACCESS_STORE 1
#define Py_SOAC_CLASS_ACCESS_DEL 2
#define Py_SOAC_CLASS_ACCESS_RAW_SLOT 0
#define Py_SOAC_CLASS_ACCESS_CELL_VALUE 1
#define Py_SOAC_CLASS_ACCESS_NAMESPACE_OR_CELL 2

/* Execute SETUP_ANNOTATIONS against the explicit actual local namespace. */
PyAPI_FUNC(int) PySoac_SetupAnnotations(PyObject *locals);

/* Explicit consumers of compiler-selected type-expression operations. These
 * constructors do not execute evaluators or grant source/optimizer authority.
 * The caller authenticates the exact owned function and result separately. */
#define Py_SOAC_TYPE_PARAM_TYPEVAR 0
#define Py_SOAC_TYPE_PARAM_BOUND 1
#define Py_SOAC_TYPE_PARAM_CONSTRAINTS 2
#define Py_SOAC_TYPE_PARAM_PARAMSPEC 3
#define Py_SOAC_TYPE_PARAM_TYPEVARTUPLE 4

#define Py_SOAC_TYPE_EXPRESSION_ALIAS 0
#define Py_SOAC_TYPE_EXPRESSION_BOUND 1
#define Py_SOAC_TYPE_EXPRESSION_CONSTRAINTS 2
#define Py_SOAC_TYPE_EXPRESSION_DEFAULT 3

PyAPI_FUNC(PyObject *) PySoac_NewTypeAlias(
    PyObject *name, PyObject *type_params, PyObject *evaluator);
/* evaluator is NULL except for BOUND and CONSTRAINTS. */
PyAPI_FUNC(PyObject *) PySoac_NewTypeParameter(
    int kind, PyObject *name, PyObject *evaluator);
/* Attach a default only once to a freshly constructed parameter, retaining
 * native evaluation/creation order. Returns an owned alias to parameter. */
PyAPI_FUNC(PyObject *) PySoac_SetTypeParameterDefault(
    PyObject *parameter, PyObject *evaluator);
/* Consume the compiler-created exact native parameter tuple using the stock
 * Generic intrinsic, including its ordinary typing callbacks. */
PyAPI_FUNC(PyObject *) PySoac_SubscriptGeneric(PyObject *type_params);
/* Attach the exact tuple before decorators/completion. The native function
 * mutation guard still applies; this returns an owned alias to function and
 * never grants code execution or optimizer authority. */
PyAPI_FUNC(PyObject *) PySoac_SetFunctionTypeParameters(
    PyObject *function, PyObject *type_params);
/* 1/0 for exact private slot identity, -1 for invalid C arguments. Success is
 * allocation/callback-free and does not make an unowned function executable. */
PyAPI_FUNC(int) PySoac_MatchesTypeExpression(
    PyObject *target, int kind, PyObject *evaluator);

#define PY_INVALID_STACK_EFFECT INT_MAX
PyAPI_FUNC(int) PyCompile_OpcodeStackEffect(int opcode, int oparg);
PyAPI_FUNC(int) PyCompile_OpcodeStackEffectWithJump(int opcode, int oparg, int jump);
