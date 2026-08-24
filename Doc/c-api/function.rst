.. highlight:: c

.. _function-objects:

Function Objects
----------------

.. index:: pair: object; function

There are a few functions specific to Python functions.


.. c:type:: PyFunctionObject

   The C structure used for functions.


.. c:var:: PyTypeObject PyFunction_Type

   .. index:: single: MethodType (in module types)

   This is an instance of :c:type:`PyTypeObject` and represents the Python function
   type.  It is exposed to Python programmers as ``types.FunctionType``.


.. c:function:: int PyFunction_Check(PyObject *o)

   Return true if *o* is a function object (has type :c:data:`PyFunction_Type`).
   The parameter must not be ``NULL``.  This function always succeeds.


.. c:function:: PyObject* PyFunction_New(PyObject *code, PyObject *globals)

   Return a new function object associated with the code object *code*. *globals*
   must be a dictionary with the global variables accessible to the function.

   The function's docstring and name are retrieved from the code object.
   :attr:`~function.__module__`
   is retrieved from *globals*. The argument defaults, annotations and closure are
   set to ``NULL``. :attr:`~function.__qualname__` is set to the same value as
   the code object's :attr:`~codeobject.co_qualname` field.


.. c:function:: PyObject* PyFunction_NewWithQualName(PyObject *code, PyObject *globals, PyObject *qualname)

   As :c:func:`PyFunction_New`, but also allows setting the function object's
   :attr:`~function.__qualname__` attribute.
   *qualname* should be a unicode object or ``NULL``;
   if ``NULL``, the :attr:`!__qualname__` attribute is set to the same value as
   the code object's :attr:`~codeobject.co_qualname` field.

   .. versionadded:: 3.3


.. c:function:: PyObject* PyFunction_GetCode(PyObject *op)

   Return the code object associated with the function object *op*.


.. c:function:: PyObject* PyFunction_GetGlobals(PyObject *op)

   Return the globals dictionary associated with the function object *op*.


.. c:function:: PyObject* PyFunction_GetModule(PyObject *op)

   Return a :term:`borrowed reference` to the :attr:`~function.__module__`
   attribute of the :ref:`function object <user-defined-funcs>` *op*.
   It can be *NULL*.

   This is normally a :class:`string <str>` containing the module name,
   but can be set to any other object by Python code.


.. c:function:: PyObject* PyFunction_GetDefaults(PyObject *op)

   Return the argument default values of the function object *op*. This can be a
   tuple of arguments or ``NULL``.


.. c:function:: int PyFunction_SetDefaults(PyObject *op, PyObject *defaults)

   Set the argument default values for the function object *op*. *defaults* must be
   ``Py_None`` or a tuple.

   Raises :exc:`SystemError` and returns ``-1`` on failure.


.. c:function:: void PyFunction_SetVectorcall(PyFunctionObject *func, vectorcallfunc vectorcall)

   Set the vectorcall field of a given function object *func*.

   Warning: extensions using this API must preserve the behavior
   of the unaltered (default) vectorcall function!

   .. versionadded:: 3.12


.. c:function:: PyObject* PyFunction_GetKwDefaults(PyObject *op)

   Return the keyword-only argument default values of the function object *op*. This can be a
   dictionary of arguments or ``NULL``.


.. c:function:: int PyFunction_SetKwDefaults(PyObject *op, PyObject *defaults)

   Set the keyword-only argument default values of the function object *op*.
   *defaults* must be a dictionary of keyword-only arguments or ``Py_None``.

   This function returns ``0`` on success, and returns ``-1`` with an exception
   set on failure.


.. c:function:: PyObject* PyFunction_GetClosure(PyObject *op)

   Return the closure associated with the function object *op*. This can be ``NULL``
   or a tuple of cell objects.


.. c:function:: int PyFunction_SetClosure(PyObject *op, PyObject *closure)

   Set the closure associated with the function object *op*. *closure* must be
   ``Py_None`` or a tuple of cell objects.

   Raises :exc:`SystemError` and returns ``-1`` on failure.


.. c:function:: PyObject *PyFunction_GetAnnotations(PyObject *op)

   Return the annotations of the function object *op*. This can be a
   mutable dictionary or ``NULL``.


.. c:function:: int PyFunction_SetAnnotations(PyObject *op, PyObject *annotations)

   Set the annotations for the function object *op*. *annotations*
   must be a dictionary or ``Py_None``.

   Raises :exc:`SystemError` and returns ``-1`` on failure.


.. c:function:: PyObject *PyFunction_GET_CODE(PyObject *op)
                PyObject *PyFunction_GET_GLOBALS(PyObject *op)
                PyObject *PyFunction_GET_MODULE(PyObject *op)
                PyObject *PyFunction_GET_DEFAULTS(PyObject *op)
                PyObject *PyFunction_GET_KW_DEFAULTS(PyObject *op)
                PyObject *PyFunction_GET_CLOSURE(PyObject *op)
                PyObject *PyFunction_GET_ANNOTATIONS(PyObject *op)

   These functions are similar to their ``PyFunction_Get*`` counterparts, but
   do not do type checking. Passing anything other than an instance of
   :c:data:`PyFunction_Type` is undefined behavior.


.. c:function:: int PyFunction_AddWatcher(PyFunction_WatchCallback callback)

   Register *callback* as a function watcher for the current interpreter.
   Return an ID which may be passed to :c:func:`PyFunction_ClearWatcher`.
   In case of error (e.g. no more watcher IDs available),
   return ``-1`` and set an exception.

   .. versionadded:: 3.12


.. c:function:: int PyFunction_ClearWatcher(int watcher_id)

   Clear watcher identified by *watcher_id* previously returned from
   :c:func:`PyFunction_AddWatcher` for the current interpreter.
   Return ``0`` on success, or ``-1`` and set an exception on error
   (e.g.  if the given *watcher_id* was never registered.)

   .. versionadded:: 3.12


.. c:type:: PyFunction_WatchEvent

    Enumeration of possible function watcher events:

    - ``PyFunction_EVENT_CREATE``
    - ``PyFunction_EVENT_DESTROY``
    - ``PyFunction_EVENT_MODIFY_CODE``
    - ``PyFunction_EVENT_MODIFY_DEFAULTS``
    - ``PyFunction_EVENT_MODIFY_KWDEFAULTS``

   .. versionadded:: 3.12

    - ``PyFunction_PYFUNC_EVENT_MODIFY_QUALNAME``

   .. versionadded:: 3.15

.. c:type:: int (*PyFunction_WatchCallback)(PyFunction_WatchEvent event, PyFunctionObject *func, PyObject *new_value)

   Type of a function watcher callback function.

   If *event* is ``PyFunction_EVENT_CREATE`` or ``PyFunction_EVENT_DESTROY``
   then *new_value* will be ``NULL``. Otherwise, *new_value* will hold a
   :term:`borrowed reference` to the new value that is about to be stored in
   *func* for the attribute that is being modified.

   The callback may inspect but must not modify *func*; doing so could have
   unpredictable effects, including infinite recursion.

   If *event* is ``PyFunction_EVENT_CREATE``, then the callback is invoked
   after *func* has been fully initialized. Otherwise, the callback is invoked
   before the modification to *func* takes place, so the prior state of *func*
   can be inspected. The runtime is permitted to optimize away the creation of
   function objects when possible. In such cases no event will be emitted.
   Although this creates the possibility of an observable difference of
   runtime behavior depending on optimization decisions, it does not change
   the semantics of the Python code being executed.

   If *event* is ``PyFunction_EVENT_DESTROY``, taking a reference in the
   callback to the about-to-be-destroyed function will resurrect it, preventing
   it from being freed at this time. When the resurrected object is destroyed
   later, any watcher callbacks active at that time will be called again.

   If the callback sets an exception, it must return ``-1``; this exception will
   be printed as an unraisable exception using :c:func:`PyErr_WriteUnraisable`.
   Otherwise it should return ``0``.

   There may already be a pending exception set on entry to the callback. In
   this case, the callback should return ``0`` with the same exception still
   set. This means the callback may not call any other API that can set an
   exception unless it saves and clears the exception state first, and restores
   it before returning.

   .. versionadded:: 3.12


SOAC Metadata Sealing
=====================

The private SOAC function seal is permanent. Semantic setters check it both
before invoking user callbacks and again before each remaining write. In
particular, an audit hook, warning handler, or function watcher may install a
seal or required-code boundary during a previously valid setter call. The
outer setter then raises the native strict mutation error without performing
its pending write. The public function constructor also checks before
installing defaults or closure cells after a CREATE watcher; a constructor
with no remaining protected write can still return the sealed function.
This does not authorize original strict bytecode to execute;
ordinary generated bytecode remains ordinary unless a separate entry contract
is installed.

Annotation setters preserve CPython's sequential writes and reference-release
order. Replacing the primary annotation/provider reference can run a finalizer
before the companion reference is cleared. If that finalizer seals the
function, the first, pre-seal write remains visible and the setter raises
before the companion clear. It does not roll back the first write, delay the
finalizer, or mutate metadata after sealing. A missing companion requires no
write and does not cause this partial-progress error.

The internal builtin-descriptor seals compare the actual static/class method
callable or every property accessor before freezing those component bindings.
They are mechanical seals, not proofs of source ownership. Reinitializing a
sealed descriptor is rejected even with identical components. If releasing
an earlier property component runs a finalizer that seals it, already visible
writes remain visible and the initializer rejects each later component write.
Ordinary property name/doc metadata and newly returned accessor copies do not
acquire a component seal from the original descriptor.


SOAC Dataclass Invocations
-------------------------

The private dataclass protocol authorizes one explicit adapter invocation,
not arbitrary execution of strict code. The trusted runtime installs one
``PySoacDataclassCallbacks`` ABI-2 table by value in the current interpreter.
Registration is single assignment; only the identical callback values can
be registered again. Interpreter teardown closes the table before clearing
references. Free-threaded builds do not support this protocol.

``PySoac_NewDataclassInvocation`` owns the active, GC-visible catalog owner.
``PySoac_DataclassVectorcall`` invokes an individually authenticated ordinary
Python root through normal argument binding. The factory stage precedes class
construction; the application stage requires ``PySoac_DataclassBindClass``
with the actual native class and contract owner. Before binding, explicit
decline releases active catalog edges and preserves ordinary decorator
behavior. After binding, an incompatible transition fails permanently; it
does not downgrade or revoke installed restrictions. Successful completion
also releases active catalog edges. An inert invocation retained by a
generated function does not retain the class, builder, or helper graph.

The runtime must independently attest the actual ordinary stdlib helper
graph against verified source, including nested code, call-site offsets,
flags/layout, globals, builtins, defaults, closure values, and native entry.
An equivalent pre-catalog function copy can be an attested helper for this
one invocation; this does not give it strict-source, JIT, or immutability
authority. Dynamically created decorators, factories, and members instead
require their exact native creation records. Names or code similarity alone
cannot authenticate those objects.

``PySoac_MatchesBuiltinFunction(actual, name, name_length)`` matches a trusted
counted native name against the compiled builtin method table. It requires
the exact builtin-function type, the private method-table entry and its
default native calling-convention entry, and the current builtin-module self
captured through the original native ``exec`` witness. It does not read
Python attributes or consult a mutable module binding. An equivalent copy
using the same native method entry, self, and default vectorcall may match;
this identifies a calling implementation, not a source or optimization
capability. Privileged exec/member bridges still require their exact
canonical builtin objects. Missing/dead witnesses return zero; invalid
operands or a closed interpreter protocol return minus one with an error.
Success performs no allocation or Python callback.

``PySoac_GetDataclassRecipe`` supplies a fresh ordinary root code object for
``Py_SOAC_DATACLASS_RECIPE_DATACLASSES`` or
``Py_SOAC_DATACLASS_RECIPE_REPRLIB``. The selected native library embeds
build-generated marshal bytes; retrieval never consults ``__file__``, reads a
runtime source path, executes a module, or stores a persistent Python root.
The recipes use optimization level zero and ``<frozen NAME>`` filenames.
Consumers must handle that filename projection explicitly; differently
optimized or otherwise unmatched actual graphs must decline before binding.
``make regen-soac-dataclass-recipes`` regenerates these build-directory headers
with the same bootstrap compiler as normal frozen modules. The recipe names
are not registered as frozen imports.

``PySoac_GetCodeView(code, out, out_size)`` exposes the exact code's borrowed
immutable fields and scalar layout without allocation or Python calls.
``out_size`` must be ``sizeof(PySoacCodeView)``; the returned view has ABI
version ``Py_SOAC_CODE_VIEW_ABI``. The caller keeps the code alive. The view
includes names/constants, locals-plus names and kinds, line/exception tables,
flags and argument/frame counts, and the native strict source ID. Mutable
quickening, monitoring, version, and executor state is not structural source
authority. ``PyCode_GetCode`` may materialize bytecode during cold preparation;
it must not run in effect-free role callbacks. Neither API authenticates an
arbitrary actual function, globals, closure, or helper class by itself.

Callbacks receive borrowed, callback-duration frame views. Instruction
offsets count code units including inline caches, not byte offsets or
instruction ordinals. Local and cell accessors do not materialize locals or
invoke Python. Callbacks must not call Python; successful validation must not
allocate, except for the explicitly allocating ``created`` callback below.
Entry runs after binding, with exact executed code and actual
callee/environment operands. Entry and generated-boundary views precede
``MAKE_CELL``/``COPY_FREE_VARS``: parameters are raw bound values and captured
free variables still reside in the authenticated function's actual closure.
Later views use the executed code's explicit local/cell projection, never a
slot kind guessed from the value's type. ``PySoac_DataclassFrameInvocation``
returns the exact invocation carried by that view, including an Enter view
whose frame is not yet attached. It does not consult ambient interpreter
frames. Generated checked-call views return NULL: they carry no construction
authority. Only a direct opcode-dispatched Python edge
can carry an immediate parent's context. Public C call APIs, C proxies,
ordinary callbacks, and type-call trampolines do not inherit that context.
Monitoring and specialized call paths preserve this boundary explicitly.

Native ``MAKE_FUNCTION`` attaches a creation record before GC tracking and
CREATE watchers. The ``create`` role validator is pure/idempotent and can run
more than once around native allocations. The subsequent ``created`` callback
receives the actual unpublished function and explicit invocation. It may
configure a prebuilt generated-boundary delegate, but must not execute Python
or deliberately publish a strong function reference. Configuration must
precede allocating any GC-visible weak function witness, because observers
can acquire that weak referent before CREATE. Any one-way per-function
birth-slot consumption belongs here, not in ``create``. Native revalidation of the producer, code,
role, invocation, and installed boundary precedes publication. Failure marks
the native owner terminal before releasing the captured callback owner or
constructor reference. Normal function destruction clears weakrefs and its
actual current fields; an escaped reference remains a valid terminal object,
never a freed allocation or an unchecked required body.
The record uses the existing single-assignment function owner slot.
The record has a callback-free weak code witness and no strong edge back to
its function. Deallocation tombstones the nonowning function identity before
weak-reference callbacks and reference releases. ``PyFunction_HasSoacDataclassCreation``
identifies that exact attached role, even after ordinary decline;
``PyFunction_MatchesSoacDataclassCreation`` additionally requires the live
invocation, unchanged original code, and expected creation role. Public
constructors and function/code copies do not copy these records.

Three ``_types`` construction bridges record reached source fragments, execute
the exact verified generated text, and install one generated member. Their
ordinary C entries delegate normally and grant no context. Opcode dispatch
requires the exact canonical helper object and its implementation. Canonical
helpers and the original ``exec``/``setattr`` objects have weak witnesses
captured at native creation; late Python bindings cannot register replacements,
and dead witnesses are never regranted. The compiled callback receives a
C-allocated recursive weak-code tree rather than retaining the compiled root
or its ``co_consts`` graph. Before this callback, native code materializes
each code object's ordinary bytecode cache and revalidates the compile edge
after those allocations. The callback can resolve code-unit call sites
without allocating or performing a second Python compilation. The generated
factory keeps its ordinary direct
``CALL_FUNCTION_EX``; no extra C factory wrapper changes argument lifetimes.

Generated-member installation consumes one fresh record, freezes function
metadata before commit watchers, and passes an explicit operation through the
normal type/dictionary transaction. Only an exact frozen-setter/deleter role
can install its corresponding protected hook. Final members, field
descriptors, sealed classes, and terminal owners remain protected. Operation
completion precedes displaced-reference finalizers. A generated method still
has ordinary bytecode and no source/JIT capability. A required signature
policy uses the distinct checked-entry protocol below; a metadata seal alone
does not install checks. Admission must decline if it needs a generated
policy that the runtime has not implemented. Original ``CO_FUTURE_STRICT``
code remains denied at every frame entry; no dataclass context relaxes that
guard.

Required generated calls and components
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``PyFunction_ConfigureSoacDataclassBoundary`` accepts only the exact
invocation-owned, unpublished member during ``created``. It copies an ABI-1
``PySoacDataclassBoundarySpec`` and captures the GC-visible check owner.
The code must be an ordinary synchronous optimized function with a fixed
positional/keyword-only signature; variadic arguments, generators, coroutines,
and strict-source code are unsupported. The parameter count includes self.
The native specification has one byte per parameter, with a zero/one factory
mask, and sorted unique CALL code-unit offsets paired with distinct factory
parameter indices. Every selected factory parameter has one required value
site. This immutable specification is not recovered from mutable Python
attributes, defaults, names, or a code object's shared specialization state.

Configuration installs a dedicated native checked vectorcall and permanent
required-entry marker before CREATE watchers. Constructors still supply
defaults and closure through normal ``SET_FUNCTION_ATTRIBUTE`` operations;
an early watcher call with an incomplete closure fails explicitly. Each call
captures its immutable delegate before callback-capable binding, runs the
ordinary CPython binder, and records which parameters the caller supplied
before inserting defaults. The ``bound`` callback sees those supplied bits
and actual bound values. It can defer only an omitted selected factory slot
whose bound value is the attested placeholder. Passing that placeholder
explicitly still undergoes the normal entry predicate.

The additional ``_types._dataclass_init_value`` bridge selects only an
individually authenticated factory-expression edge. The ``init_value``
callback validates the actual Field, complete expression, locals dictionary,
and immutable generation plan and returns a prebuilt collision-free helper
name. Native code wraps the entire conditional initialization value with the
canonical ``_types._dataclass_check_value`` helper and captures that object in
the actual builder locals. It revalidates after allocation, lookups, and
dictionary watchers. Completed dictionary writes are not rolled back on a
later failure, but no invalid generated text is returned. Unselected or
ordinary indirect calls return the original expression unchanged.

At the recorded CALL site, the actual frame's checked activation requires
the exact canonical value-helper object, even if another callable or C proxy
has been substituted. Warmed and instrumented calls preserve this check.
The ``value`` callback runs only for deferred parameters, once per dynamic
evaluation of their whole conditional, before the generated assignment. A
trace jump that repeats the expression checks the new result again; the
deferred classification remains immutable for the activation. Supplied values
already checked at entry are not rechecked after ordinary body/tracing
changes. This bounded cohort has no required return predicate.

``PyFunction_MatchesSoacDataclassBoundary`` proves the exact installed check
owner, not merely a required bit. Forwarding C vectorcalls to the captured
checked entry remain valid. Restoring ``_PyFunction_Vectorcall`` through the
supported setter cannot bypass checks: a required generated body's actual
frame must own the matching completed activation. No code-wide flag or
thread-local permit is involved. Function copies stay ordinary, record-free,
and incapable of acquiring source/JIT or check authority from shared code.
Failed or completed construction invocations do not revoke already installed
required boundaries; their active catalogs and class edges are released.

``PyFunction_AdoptSoacDataclassComponent`` separately seals only the exact
fresh annotation provider or repr implementation of a fresh member. The
native creation roles, original code, active invocation, actual
``func_annotate`` or explicit closure-cell relationship, and fixed
``validate_component`` policy callback must all agree. Sealing is one-way and
grants only metadata protection, not a source/JIT or required-call capability.
Shared helpers, user factories, and other closure values are not adopted.
The component's record has no strong method/class backedge; retaining a
component does not extend the parent method or class lifetime.


SOAC Annotation Replay
----------------------

These interpreter-specific APIs derive ordinary code for ``annotationlib``'s
``FORWARDREF`` and ``STRING`` evaluation. They never authorize execution of
original strict bytecode or grant optimizer or mutation authority.

.. c:function:: PyObject *PySoac_CloneAnnotationReplayCode(PyObject *provider, PyObject *expected_owner, PyObject *verified_code)

   The trusted native caller must first authenticate the exact provider's
   annotation role, source, logical owner, and closure layout. The function
   checks that *provider* is an exact function with the same attached live
   native *expected_owner*, that its actual code is *verified_code*, and that
   the code came from the authenticated native compiler.

   Return a new ordinary code tree with all ``CO_FUTURE_STRICT`` bits and
   private SOAC source IDs removed recursively. Code extras, executors, and
   monitoring state are not copied. Nested lambdas, comprehensions, and
   generators therefore remain ordinary if they escape an annotation replay.
   The original tree is unchanged and remains subject to every native strict
   frame-entry guard. Code copying through ``code.replace`` or ``marshal``
   does not invoke this operation or receive its source authentication.

.. c:type:: PyObject *(*PySoacAnnotationReplayResolver)(PyObject *provider, PyObject *logical_owner, int format)

   Resolve one authenticated annotation-provider replay for public format 3
   (``FORWARDREF``) or 4 (``STRING``). Return a new reference to ordinary code,
   or ``NULL`` with an exception. The returned code must also accept the
   internal ``VALUE_WITH_FAKE_GLOBALS`` argument used by ``annotationlib``.
   This callback owns no interpreter references through its function pointer;
   all source and Python state must have explicit, GC-visible owners.

.. c:function:: int PySoac_SetAnnotationReplayResolver(PySoacAnnotationReplayResolver resolver)

   Install the trusted resolver once in the current interpreter. Registering
   the identical callback again succeeds. Replacing or removing it fails.
   Subinterpreters do not inherit the registration. Interpreter teardown
   clears and permanently closes it before object callbacks can run.

The private ``_typing._soac_annotation_replay_code`` stdlib bridge dispatches
owned strict providers through that native registration and verifies that the
whole returned code tree is ordinary. Python attributes, mutable helper names,
and code flags never install a resolver or authenticate a provider. Ordinary
callbacks retain the existing ``__code__`` lookup and ``annotationlib`` replay
behavior. Type-alias and type-parameter evaluate callbacks require their own
authenticated source/capture integration; this API alone does not supply it.
