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

/* Compile the same authenticated source once, returning (code, strings).
 * strings is an exact tuple of (line, column, end_line, end_column, text)
 * entries for future-stringized annotations in that native AST. The returned
 * ordinary data is not an execution or optimizer capability. */
PyAPI_FUNC(PyObject *) PySoac_CompileVerifiedSourceDetails(
    const char *source, Py_ssize_t length, PyObject *filename, int optimize);

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
