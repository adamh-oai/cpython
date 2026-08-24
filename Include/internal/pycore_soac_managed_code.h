/* Private shared metadata, not an additional exported API. */
#ifndef Py_INTERNAL_SOAC_MANAGED_CODE_H
#define Py_INTERNAL_SOAC_MANAGED_CODE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "native_managed_code_v1.h"

enum soac_gen_phase {
    SOAC_GEN_BINDING,
    SOAC_GEN_LIVE,
    SOAC_GEN_COMPLETING,
    SOAC_GEN_TERMINAL
};

enum soac_gen_code_mode {
    SOAC_GEN_LEGACY_CODE,
    SOAC_GEN_TRANSFERRED_CODE
};

struct _PySoacManagedGenerator {
    PySoacGeneratorSpec spec;
    PyObject *owner;
    enum soac_gen_phase phase;
    enum soac_gen_code_mode code_mode;
    PySoacGeneratorCleanupV1 *cleanup;  /* borrowed, address-stable */
    PyObject *retiring_owner;           /* borrowed during fixed clear */
};
#endif
