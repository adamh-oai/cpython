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
 * (5, unchanged_CodeNodes, ScopeBindingRecipes, unchanged_OperationTables)).
 * One exact immutable scope recipe belongs to every retained native code node.
 * This is native compiler/binder data, not projection or execution authority.
 * All containers are exact immutable tuples. Unchanged nodes are
 * (id,parent_id,exact_code,scope_kind,symtable_kind,source_span).
 * Unchanged strings entries are (line,column,end_line,end_column,text) for
 * future-stringized annotations in the same native AST. Spans use native
 * UTF-8 byte columns; None means unavailable, never an invented location.
 *
 * scope=(code_id, slot_seeds, entry_prefix, owners, regions, captures,
 *        accesses, class_actions_or_None, gaps)
 * seed=(final_slot, native_kind, FrameCleared0/BoundParameter1, ordinal_or_None)
 * owner=(id, kind, final_slot, native_kind, region_or_None)
 * region=(id,parent,kind,source_span,outer_iterable_span,is_async,protection,
 *         entry_ops,restores,source_bindings,events,lifecycle)
 * event=(phase,step,kind,slot_or_None,owner_or_None,auxiliary_or_None,emissions)
 * emission=(ordinal,lane,form,first_operand,second_or_None,native_opcode,
 *           actual_opcode_byte_offset,handler_or_None,wire3_context_or_None)
 * handler=(target_ordinal,unwind_depth,preserve_lasti)
 * capture=(child_id,creation_span,free_ordinal,current_slot,region_or_None)
 * access=(original_Name_span,context,mode,current_slot,region_or_None)
 * class_actions=(header_initializers,exports), absent outside native classes
 * scope_gap=(reason,region_or_None,event_key_or_None,ordinal_or_None,
 *            lane_or_None,native_opcode_or_None,context_or_None)
 *
 * Seeds describe successful native binding; actual TakeBinding tokens still
 * have to move into the corresponding runtime primaries. MAKE_CELL uses the
 * actual current value. SaveClear/MakeCell precede the private restoration
 * handler, so partial prefix failure does not acquire an invented rollback.
 * OperationTable compiler-origin and fused-restore gaps remain unchanged.
 * Native source-operation tuple/tag definitions below retain wire3 meanings.
 * lifecycle=(protections,normal_completions)
 * protection=(allocation_ordinal,context,enter_key,exit_key,handler_ordinal,spans)
 * span=(first_ordinal,end_ordinal,first_byte,end_byte,effective_handler)
 * normal=(allocation_ordinal,context,consumer,entries,trace,rewritten_rotations)
 * entry=(generic0/generator1,advance_ordinal,first_ordinal)
 * trace_step=(kind,event_key_or_None,actual_scope_emission)
 * Span ends are exclusive; byte extents include EXTENDED_ARG and inline caches.
 * Normal generator completion closes its return value at END_FOR before
 * POP_ITER. Generic exhaustion skips END_FOR after consuming StopIteration.
 * Exact lifecycle tuple/tag constraints are specified by joint SCHEMA-5.md.
 */
PyAPI_FUNC(PyObject *) PySoac_CompileVerifiedSourceDetails(
    const char *source, Py_ssize_t length, PyObject *filename, int optimize);

/* Fixed wire values, independent of internal compiler enum numbering. */
#define Py_SOAC_CLASS_BINDINGS_SCHEMA 5
/* Source-operation receipts share the SAME final code-node tree.
 * table = (code_id, instruction_count, code_size_bytes, exact_native_names,
 *          reads, stores, calls, gaps)
 * read = (original_Name_span, emissions)
 * read_emission = (ordinal, form, first_slot, second_slot_or_None, lane,
 *                  preceding_binding_or_None, semantic_context_or_None)
 * store = (binding_origin, emissions)
 * binding_origin = (kind, original_AST_span, phase, detail_or_None)
 * store_emission = (ordinal, form, first_operand, second_operand_or_None,
 *                   lane, semantic_context_or_None)
 * operand = (domain, index_or_None)
 * call = (call_origin, emissions), call_origin = (kind, owner_span, detail)
 * call_emission = (ordinal, opcode_byte_offset_or_None, form,
 *                 value_arg_count_or_None, input_layout, context_or_None)
 * input_layout = (channel, preloaded_count, positional_plan, keyword_plan)
 * positional_plan = (kind, ordered (kind, original_span_or_None) entries)
 * keyword_plan = (kind, native_names_or_None, original_entries, emitted_groups)
 * context = tuple of (owner_kind, owner_span, item_or_None, entry_kind,
 *                    transfer_span_or_None, payload_kind), or None if unproven
 * gap = (reason, operation_origin_or_None, ordinal_or_None, lane_or_None,
 *        native_opcode_or_None, context_or_None)
 * operation_origin = (family, original_Name_span / binding_origin / call_origin)
 *
 * Final ordinals precede EXTENDED_ARG expansion. CALL opcode byte offsets come
 * from the native assembler, never from line tables or disassembly matching.
 * All source emissions survive as rows or explicit gaps. Missing/ambiguous
 * receipts grant neither execution authority nor a default reference policy.
 * The fixed tags below define wire3; no earlier-wire decoder fallback exists. */

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
/* Wire3: exact tuples and tags are specified by the joint SCHEMA-V3.md.
 * Operation tables extend the SAME final CodeNode catalogue; no execution
 * selector, instruction interpreter or second source namespace is exposed.
 * table=(code_id, instruction_count, code_size_bytes, native_names,
 *        reads, stores, calls, gaps)
 */
#define Py_SOAC_OPERATION_READ 0
#define Py_SOAC_OPERATION_STORE 1
#define Py_SOAC_OPERATION_CALL 2
#define Py_SOAC_BINDING_TYPEVAR 7
#define Py_SOAC_BINDING_TYPEVARTUPLE 8
#define Py_SOAC_BINDING_PARAMSPEC 9
#define Py_SOAC_BINDING_PATTERN_CAPTURE 10
#define Py_SOAC_BINDING_ATTRIBUTE 11
#define Py_SOAC_BINDING_SUBSCRIPT 12
#define Py_SOAC_BINDING_PUBLISH 0
#define Py_SOAC_BINDING_SOURCE_DELETE 1
#define Py_SOAC_BINDING_CLEANUP_STORE_NONE 2
#define Py_SOAC_BINDING_CLEANUP_DELETE 3
#define Py_SOAC_PATTERN_AS 0
#define Py_SOAC_PATTERN_STAR 1
#define Py_SOAC_PATTERN_MAPPING_REST 2

#define Py_SOAC_OPERAND_LOCALSPLUS 0
#define Py_SOAC_OPERAND_NAMES 1
#define Py_SOAC_OPERAND_NO_INDEX 2
#define Py_SOAC_STORE_FAST 0
#define Py_SOAC_STORE_FAST_PAIR 1
#define Py_SOAC_STORE_FAST_THEN_LOAD 2
#define Py_SOAC_STORE_CELL 3
#define Py_SOAC_STORE_NAME 4
#define Py_SOAC_STORE_GLOBAL 5
#define Py_SOAC_STORE_ATTRIBUTE 6
#define Py_SOAC_STORE_SUBSCRIPT 7
#define Py_SOAC_STORE_SLICE 8
#define Py_SOAC_DELETE_FAST 9
#define Py_SOAC_DELETE_CELL 10
#define Py_SOAC_DELETE_NAME 11
#define Py_SOAC_DELETE_GLOBAL 12
#define Py_SOAC_DELETE_ATTRIBUTE 13
#define Py_SOAC_DELETE_SUBSCRIPT 14

#define Py_SOAC_CALL_SOURCE 0
#define Py_SOAC_CALL_DECORATOR 1
#define Py_SOAC_CALL_CLASS 2
#define Py_SOAC_CALL_GENERIC_SCOPE 3
#define Py_SOAC_CALL_GENERATOR 4
#define Py_SOAC_CALL_WITH_ENTER 5
#define Py_SOAC_CALL_WITH_EXIT 6
#define Py_SOAC_CALL_ASYNC_WITH_ENTER 7
#define Py_SOAC_CALL_ASYNC_WITH_EXIT 8
#define Py_SOAC_CALL_ASSERT 9
#define Py_SOAC_CALL_POSITIONAL 0
#define Py_SOAC_CALL_KEYWORDS 1
#define Py_SOAC_CALL_EXPANDED 2
#define Py_SOAC_CALL_NULL_CHANNEL 0
#define Py_SOAC_CALL_METHOD_CHANNEL 1
#define Py_SOAC_CALL_LEADING_CHANNEL 2
#define Py_SOAC_POSITIONAL_VECTOR 0
#define Py_SOAC_POSITIONAL_EXPANDED_EMPTY 1
#define Py_SOAC_POSITIONAL_SOLE_STAR 2
#define Py_SOAC_POSITIONAL_DIRECT_TUPLE 3
#define Py_SOAC_POSITIONAL_LIST_AT_FIRST_STAR 4
#define Py_SOAC_POSITIONAL_LIST_BEFORE_ARGUMENTS 5
#define Py_SOAC_POSITIONAL_SOURCE 0
#define Py_SOAC_POSITIONAL_STAR 1
#define Py_SOAC_POSITIONAL_GENERIC_BASE 2
#define Py_SOAC_KEYWORDS_NONE 0
#define Py_SOAC_KEYWORDS_NAMES_TUPLE 1
#define Py_SOAC_KEYWORDS_EXPANDED_GROUPS 2
#define Py_SOAC_KEYWORD_NAMED 0
#define Py_SOAC_KEYWORD_MAPPING 1
#define Py_SOAC_KEYWORD_GROUP_NAMED 0
#define Py_SOAC_KEYWORD_GROUP_MAPPING 1
#define Py_SOAC_KEYWORD_BUILD_MAP 0
#define Py_SOAC_KEYWORD_MAP_ADD 1

#define Py_SOAC_CONTEXT_TRY_FINALLY 0
#define Py_SOAC_CONTEXT_TRY_STAR_FINALLY 1
#define Py_SOAC_CONTEXT_EXCEPT_CLEANUP 2
#define Py_SOAC_CONTEXT_EXCEPT_STAR_CLEANUP 3
#define Py_SOAC_CONTEXT_WITH_EXIT 4
#define Py_SOAC_CONTEXT_ASYNC_WITH_EXIT 5
#define Py_SOAC_CONTEXT_FALLTHROUGH 0
#define Py_SOAC_CONTEXT_EXCEPTION 1
#define Py_SOAC_CONTEXT_RETURN 2
#define Py_SOAC_CONTEXT_BREAK 3
#define Py_SOAC_CONTEXT_CONTINUE 4
#define Py_SOAC_CONTEXT_NO_PAYLOAD 0
#define Py_SOAC_CONTEXT_RETURN_VALUE 1
#define Py_SOAC_CONTEXT_EXCEPTION_VALUE 2

#define Py_SOAC_OPERATION_GAP_MISSING_CALL 5
#define Py_SOAC_OPERATION_GAP_MISSING_CALL_POSITION 6
#define Py_SOAC_OPERATION_GAP_GUARDED_NONCALL 7
#define Py_SOAC_OPERATION_GAP_LOWERED_NONCALL 8
#define Py_SOAC_OPERATION_GAP_MISSING_CONTEXT 9
#define Py_SOAC_OPERATION_GAP_CALL_INPUT 10
#define Py_SOAC_OPERATION_GAP_COMPILER_ORIGIN 11
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

/* Wire5: one shared ownership-data recipe per retained native CodeNode.
 * These tags describe native compiler/binder facts, never execution grants. */
#define Py_SOAC_SCOPE_SEED_CLEARED 0
#define Py_SOAC_SCOPE_SEED_PARAMETER 1
#define Py_SOAC_EAGER_LIST 0
#define Py_SOAC_EAGER_SET 1
#define Py_SOAC_EAGER_DICT 2
#define Py_SOAC_SCOPE_NO_PRIVATE_HANDLER 0
#define Py_SOAC_SCOPE_FINALLY_AFTER_PREFIX 1
#define Py_SOAC_SCOPE_BINDING_TARGET 0
#define Py_SOAC_SCOPE_BINDING_WALRUS 1
#define Py_SOAC_SCOPE_PHASE_ENTRY 0
#define Py_SOAC_SCOPE_PHASE_ISOLATION 1
#define Py_SOAC_SCOPE_PHASE_BODY 2
#define Py_SOAC_SCOPE_PHASE_ERROR_CLEANUP 3
#define Py_SOAC_SCOPE_PHASE_NORMAL_RESTORE 4
#define Py_SOAC_SCOPE_PHASE_ERROR_RESTORE 5
#define Py_SOAC_SCOPE_COPY_FREE 0
#define Py_SOAC_SCOPE_ENTRY_CELL 1
#define Py_SOAC_SCOPE_SAVE_CLEAR 2
#define Py_SOAC_SCOPE_REGION_CELL 3
#define Py_SOAC_SCOPE_PROTECT_ENTER 4
#define Py_SOAC_SCOPE_BUILD_COLLECTION 5
#define Py_SOAC_SCOPE_GET_ITER 6
#define Py_SOAC_SCOPE_GET_AITER 7
#define Py_SOAC_SCOPE_DISCARD_RESULT 8
#define Py_SOAC_SCOPE_RESTORE_SLOT 9
#define Py_SOAC_SCOPE_PROPAGATE 10
#define Py_SOAC_SCOPE_PROTECT_EXIT 11
#define Py_SOAC_SCOPE_ERROR_ENTRY 12
#define Py_SOAC_SCOPE_RESTORE_ROTATION 13
#define Py_SOAC_SCOPE_ENTRY_ROTATION 14
#define Py_SOAC_SCOPE_COLLECTION_ROTATION 15
#define Py_SOAC_SCOPE_COMPLETION_VALUE_RETIRE 16
#define Py_SOAC_SCOPE_ITERATOR_RETIRE 17
#define Py_SOAC_SCOPE_ITERATOR_ADVANCE 18
#define Py_SOAC_SCOPE_FORM_DIRECT 0
#define Py_SOAC_SCOPE_FORM_STORE_PAIR 1
#define Py_SOAC_SCOPE_FORM_STORE_LOAD 2
#define Py_SOAC_SCOPE_GAP_ELIMINATED 0
#define Py_SOAC_SCOPE_GAP_UNSUPPORTED 1
#define Py_SOAC_SCOPE_GAP_MISSING_ORIGIN 2
#define Py_SOAC_SCOPE_GAP_PROTECTION 3
#define Py_SOAC_SCOPE_GAP_DIVERGENT 4
#define Py_SOAC_SCOPE_GAP_CONTEXT 5
#define Py_SOAC_SCOPE_GAP_SUSPENSION 6
#define Py_SOAC_SCOPE_GAP_PAIRED_RESTORE 7
#define Py_SOAC_SCOPE_GAP_NONITERATOR 8
#define Py_SOAC_SCOPE_GAP_NORMAL_COMPLETION 9
#define Py_SOAC_SCOPE_RESULT_KEEP 0
#define Py_SOAC_SCOPE_RESULT_DISCARD 1
#define Py_SOAC_SCOPE_RESULT_PUBLISH 2
#define Py_SOAC_SCOPE_ENTRY_GENERIC_EXHAUSTION 0
#define Py_SOAC_SCOPE_ENTRY_GENERATOR_COMPLETION 1
#define Py_SOAC_SCOPE_TRACE_TRANSPORT 0
#define Py_SOAC_SCOPE_TRACE_RESTORE 1
#define Py_SOAC_SCOPE_TRACE_RESULT 2
#define Py_SOAC_SCOPE_TRACE_COMPLETION_VALUE 3
#define Py_SOAC_SCOPE_TRACE_ITERATOR 4

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
