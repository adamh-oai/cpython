/* Value-only lifecycle proofs for the existing original native scope product.
 * Included by compile.c after its scope/operation helpers. No body, reference
 * representation or execution eligibility follows from these rows.
 *
 * A proof is positive evidence from actual emitter origins, CFG handler and
 * successor relations, observed native rewrites and final assembler extents.
 * Missing/ambiguous evidence returns None and retains the explicit scope gap.
 */

static PyObject *
soac_scope_event_key(const soac_scope_event *event)
{
    return Py_BuildValue("(ii)", event->phase, event->step);
}

static PyObject *
soac_scope_handler_row(soac_code_bindings *unit, const soac_reference_instruction *instruction)
{
    if (instruction->handler_ordinal < 0) {
        return Py_NewRef(Py_None);
    }
    if (instruction->handler_ordinal >= unit->reference_instruction_count ||
        instruction->handler_depth < 0 ||
        (instruction->handler_lasti != 0 && instruction->handler_lasti != 1)) {
        soac_binding_error("lifecycle instruction has invalid native handler metadata");
        return NULL;
    }
    return Py_BuildValue("(iii)", instruction->handler_ordinal,
        instruction->handler_depth, instruction->handler_lasti);
}

static PyObject *
soac_scope_instruction_row(soac_code_bindings *unit,
                            const soac_reference_instruction *instruction,
                            Py_ssize_t context)
{
    if (instruction->opcode_offset < instruction->first_byte ||
        instruction->opcode_offset >= instruction->end_byte ||
        instruction->end_byte > unit->assembled_code_size) {
        soac_binding_error("lifecycle instruction has no complete assembler extent");
        return NULL;
    }
    PyObject *handler = soac_scope_handler_row(unit, instruction);
    PyObject *continuation = soac_context_row(unit, context);
    PyObject *row = handler == NULL || continuation == NULL ? NULL :
        Py_BuildValue("(iiiiOinOO)", instruction->ordinal, 0,
            Py_SOAC_SCOPE_FORM_DIRECT, instruction->oparg, Py_None,
            instruction->opcode, instruction->opcode_offset, handler, continuation);
    Py_XDECREF(handler);
    Py_XDECREF(continuation);
    return row;
}

static soac_scope_event *
soac_scope_original_event(soac_code_bindings *unit, Py_ssize_t region,
                           int phase, int kind, int auxiliary)
{
    soac_scope_event *found = NULL;
    for (Py_ssize_t i = 0; i < unit->scope_events.count; i++) {
        soac_scope_event *event = &unit->scope_events.items[i];
        if (event->region == region && event->kind == kind &&
            (phase < 0 || event->phase == phase) &&
            (auxiliary < -1 || event->auxiliary == auxiliary)) {
            if (found != NULL) {
                return NULL;
            }
            found = event;
        }
    }
    return found;
}

/* -1 absent, -2 multiple physical copies. Never select a clone by visit order. */
static int
soac_scope_origin_instruction(soac_code_bindings *unit, uint32_t origin)
{
    int found = -1;
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        if (instruction->opcode == NOP) {
            continue;
        }
        for (int lane = 0; lane < 2; lane++) {
            if (instruction->origins.lane[lane] == origin) {
                if (found >= 0) {
                    return -2;
                }
                found = instruction->ordinal;
            }
        }
    }
    return found;
}

static soac_reference_instruction *
soac_scope_direct_event_instruction(soac_code_bindings *unit,
                                     soac_scope_event *event, Py_ssize_t context)
{
    if (event == NULL || event->alternative) {
        return NULL;
    }
    soac_reference_origin *origin = soac_operation_at(unit, event->origin);
    if (context == -2 || origin->emission_context == -2 ||
        !soac_same_emission_context(unit, context, unit, origin->emission_context)) {
        return NULL;
    }
    int ordinal = soac_scope_origin_instruction(unit, event->origin);
    if (ordinal < 0) {
        return NULL;
    }
    soac_reference_instruction *instruction = &unit->reference_instructions.items[ordinal];
    if (instruction->origins.lane[0] != event->origin || instruction->origins.lane[1] != 0) {
        return NULL;
    }
    int first, second;
    int form = soac_scope_emission_form(unit, event, instruction, 0, &first, &second);
    return form == Py_SOAC_SCOPE_FORM_DIRECT ? instruction : NULL;
}

static int
soac_scope_same_handler(const soac_reference_instruction *a,
                          const soac_reference_instruction *b)
{
    return a->handler_ordinal == b->handler_ordinal &&
        a->handler_depth == b->handler_depth && a->handler_lasti == b->handler_lasti;
}

static PyObject *
soac_scope_protection_span(soac_code_bindings *unit, int first, int end)
{
    soac_reference_instruction *begin = &unit->reference_instructions.items[first];
    soac_reference_instruction *last = &unit->reference_instructions.items[end - 1];
    PyObject *handler = soac_scope_handler_row(unit, begin);
    PyObject *row = handler == NULL ? NULL :
        Py_BuildValue("(iinnO)", first, end, begin->first_byte, last->end_byte, handler);
    Py_XDECREF(handler);
    return row;
}

static PyObject *
soac_scope_protection_row(soac_code_bindings *unit, Py_ssize_t region_id,
                           const soac_reference_instruction *allocation,
                           Py_ssize_t context)
{
    if (unit->scope_protection_uncertain || context == -2) {
        return Py_NewRef(Py_None);
    }
    soac_scope_protection_member *selected = NULL;
    for (Py_ssize_t i = 0; i < unit->scope_protection_members.count; i++) {
        soac_scope_protection_member *member = &unit->scope_protection_members.items[i];
        soac_scope_event *enter = soac_scope_event_at(unit, soac_operation_at(unit, member->enter));
        if (member->ordinal == allocation->ordinal && enter->region == region_id) {
            if (selected != NULL) {
                return Py_NewRef(Py_None);
            }
            selected = member;
        }
    }
    if (selected == NULL || selected->handler < 0 ||
        selected->handler >= unit->reference_instruction_count) {
        return Py_NewRef(Py_None);
    }
    soac_reference_origin *begin_origin = soac_operation_at(unit, selected->enter);
    soac_reference_origin *end_origin = soac_operation_at(unit, selected->leave);
    if (!soac_same_emission_context(unit, context, unit, begin_origin->emission_context) ||
        !soac_same_emission_context(unit, context, unit, end_origin->emission_context)) {
        return Py_NewRef(Py_None);
    }
    soac_scope_event *begin = soac_scope_event_at(unit, begin_origin);
    soac_scope_event *end = soac_scope_event_at(unit, end_origin);
    PyObject *spans = PyList_New(0);
    unsigned char *covered = PyMem_Calloc((size_t)unit->reference_instruction_count, 1);
    if (spans == NULL || covered == NULL) {
        Py_XDECREF(spans);
        PyMem_Free(covered);
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        return NULL;
    }
    int span_first = -1, span_end = -1;
    for (Py_ssize_t i = 0; i < unit->scope_protection_members.count; i++) {
        soac_scope_protection_member *member = &unit->scope_protection_members.items[i];
        if (member->enter != selected->enter || member->handler != selected->handler) {
            continue;
        }
        if (member->leave != selected->leave || member->ordinal == selected->handler) {
            goto unsupported;
        }
        soac_reference_instruction *instruction = &unit->reference_instructions.items[member->ordinal];
        if (instruction->handler_ordinal < 0 || member->ordinal < span_end || covered[member->ordinal]) {
            goto unsupported;
        }
        covered[member->ordinal] = 1;
        soac_scope_event *event = soac_scope_event_at(unit,
            soac_operation_at(unit, instruction->origins.lane[0]));
        if (event != NULL && event->region == region_id &&
            (event->kind == Py_SOAC_SCOPE_SAVE_CLEAR || event->kind == Py_SOAC_SCOPE_REGION_CELL)) {
            goto unsupported;  /* The private handler cannot cover partial prefix acquisition. */
        }
        if (span_first >= 0 &&
            (member->ordinal != span_end ||
             !soac_scope_same_handler(&unit->reference_instructions.items[span_first], instruction))) {
            if (soac_append_owned(spans, soac_scope_protection_span(unit, span_first, span_end)) < 0) {
                goto error;
            }
            span_first = -1;
        }
        if (span_first < 0) {
            span_first = member->ordinal;
        }
        span_end = member->ordinal + 1;
    }
    if (span_first < 0) {
        goto unsupported;
    }
    /* Distinct CFG handler labels can collapse to the same final ordinal.
     * A row may not claim only part of that physical direct-handler coverage. */
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        if (unit->reference_instructions.items[i].handler_ordinal == selected->handler &&
            !covered[i]) {
            goto unsupported;
        }
    }
    if (soac_append_owned(spans, soac_scope_protection_span(unit, span_first, span_end)) < 0) {
        goto error;
    }
    PyObject *span_tuple = PyList_AsTuple(spans);
    PyObject *enter_key = soac_scope_event_key(begin);
    PyObject *exit_key = soac_scope_event_key(end);
    PyObject *continuation = soac_context_row(unit, context);
    PyObject *row = span_tuple == NULL || enter_key == NULL || exit_key == NULL || continuation == NULL
        ? NULL : Py_BuildValue("(iOOOiO)", allocation->ordinal, continuation,
            enter_key, exit_key, selected->handler, span_tuple);
    Py_XDECREF(span_tuple);
    Py_XDECREF(enter_key);
    Py_XDECREF(exit_key);
    Py_XDECREF(continuation);
    Py_DECREF(spans);
    PyMem_Free(covered);
    return row;
unsupported:
    Py_DECREF(spans);
    PyMem_Free(covered);
    return Py_NewRef(Py_None);
error:
    Py_DECREF(spans);
    PyMem_Free(covered);
    return NULL;
}

enum soac_scope_stack_symbol {
    SOAC_STACK_ITERABLE = -1,
    SOAC_STACK_RESULT = -2,
    SOAC_STACK_ITERATOR = -3,
    SOAC_STACK_INDEX = -4,
    SOAC_STACK_COMPLETION_VALUE = -5,
};

/* This is only the relative owner suffix emitted by the original inlined
 * comprehension helper. Unknown caller operands below it are never touched.
 * Nonnegative entries are the existing exact saved-owner IDs. */
typedef struct {
    Py_ssize_t *items;
    Py_ssize_t size;
    Py_ssize_t capacity;
} soac_scope_owner_stack;

static int
soac_scope_stack_push(soac_scope_owner_stack *stack, Py_ssize_t value)
{
    if (stack->size >= stack->capacity) {
        return 0;
    }
    stack->items[stack->size++] = value;
    return 1;
}

static int
soac_scope_stack_pop(soac_scope_owner_stack *stack, Py_ssize_t expected)
{
    if (stack->size <= 0 || stack->items[stack->size - 1] != expected) {
        return 0;
    }
    stack->size--;
    return 1;
}

static int
soac_scope_stack_swap(soac_scope_owner_stack *stack, int depth)
{
    if (depth < 1 || depth > stack->size) {
        return 0;
    }
    Py_ssize_t top = stack->items[stack->size - 1];
    stack->items[stack->size - 1] = stack->items[stack->size - depth];
    stack->items[stack->size - depth] = top;
    return 1;
}

static int
soac_scope_prefix_stack(soac_code_bindings *unit, Py_ssize_t region_id,
                         soac_reference_instruction *allocation,
                         soac_reference_instruction *advance,
                         Py_ssize_t context, soac_scope_owner_stack *stack)
{
    soac_binding_region *region = &unit->regions.items[region_id];
    int start = allocation->ordinal;
    if (region->entry_ops.count != 0) {
        soac_scope_event *first = NULL;
        for (Py_ssize_t i = 0; i < unit->scope_events.count; i++) {
            soac_scope_event *event = &unit->scope_events.items[i];
            if (event->region == region_id && event->phase == Py_SOAC_SCOPE_PHASE_ISOLATION &&
                event->step == 0) {
                first = event;
            }
        }
        soac_reference_instruction *instruction = soac_scope_direct_event_instruction(unit, first, context);
        if (instruction == NULL) {
            return 0;
        }
        start = instruction->ordinal;
    }
    if (!soac_scope_stack_push(stack, SOAC_STACK_ITERABLE)) {
        return 0;
    }
    Py_ssize_t entry_index = 0, iterations = 0;
    int built = 0, acquired = 0, entry_rotated = 0, collection_rotated = 0;
    for (int ordinal = start; ordinal >= 0 && iterations++ < unit->reference_instruction_count;) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[ordinal];
        if (ordinal == advance->ordinal) {
            if (!built || !acquired || !collection_rotated ||
                entry_index != region->entry_ops.count ||
                entry_rotated != (region->restores.count != 0) ||
                stack->size != region->restores.count + 3 ||
                stack->items[stack->size - 3] != SOAC_STACK_RESULT ||
                stack->items[stack->size - 2] != SOAC_STACK_ITERATOR ||
                stack->items[stack->size - 1] != SOAC_STACK_INDEX) {
                return 0;
            }
            return 1;
        }
        if (instruction->opcode == NOP) {
            ordinal = instruction->fallthrough;
            continue;
        }
        soac_scope_event *event = soac_scope_event_at(unit,
            soac_operation_at(unit, instruction->origins.lane[0]));
        if (event == NULL || event->region != region_id ||
            soac_scope_direct_event_instruction(unit, event, context) != instruction) {
            return 0;
        }
        switch (event->kind) {
            case Py_SOAC_SCOPE_SAVE_CLEAR:
            case Py_SOAC_SCOPE_REGION_CELL: {
                if (built || entry_index >= region->entry_ops.count) {
                    return 0;
                }
                soac_binding_entry_op *op = &region->entry_ops.items[entry_index++];
                int role = event->kind == Py_SOAC_SCOPE_SAVE_CLEAR
                    ? Py_SOAC_CLASS_OP_SAVE_CLEAR : Py_SOAC_CLASS_OP_MAKE_CELL;
                if (op->role != role || op->slot != event->slot || op->owner != event->owner ||
                    (event->kind == Py_SOAC_SCOPE_SAVE_CLEAR &&
                     !soac_scope_stack_push(stack, event->owner))) {
                    return 0;
                }
                break;
            }
            case Py_SOAC_SCOPE_ENTRY_ROTATION:
                if (built || entry_rotated++ ||
                    !soac_scope_stack_swap(stack, instruction->oparg)) {
                    return 0;
                }
                break;
            case Py_SOAC_SCOPE_BUILD_COLLECTION:
                if (instruction != allocation || built++ ||
                    !soac_scope_stack_push(stack, SOAC_STACK_RESULT)) {
                    return 0;
                }
                break;
            case Py_SOAC_SCOPE_COLLECTION_ROTATION:
                if (!built || collection_rotated++ ||
                    !soac_scope_stack_swap(stack, instruction->oparg)) {
                    return 0;
                }
                break;
            case Py_SOAC_SCOPE_GET_ITER:
                if (!built || event->auxiliary != 0 || acquired++ ||
                    !soac_scope_stack_pop(stack, SOAC_STACK_ITERABLE) ||
                    !soac_scope_stack_push(stack, SOAC_STACK_ITERATOR) ||
                    !soac_scope_stack_push(stack, SOAC_STACK_INDEX)) {
                    return 0;
                }
                break;
            default:
                return 0;
        }
        ordinal = instruction->fallthrough;
    }
    return 0;
}

static int
soac_scope_append_trace(soac_code_bindings *unit, PyObject *trace, int kind,
                         const soac_scope_event *event,
                         const soac_reference_instruction *instruction,
                         Py_ssize_t context)
{
    PyObject *key = event == NULL ? Py_NewRef(Py_None) : soac_scope_event_key(event);
    PyObject *emission = soac_scope_instruction_row(unit, instruction, context);
    PyObject *row = key == NULL || emission == NULL ? NULL : Py_BuildValue("(iOO)", kind, key, emission);
    Py_XDECREF(key);
    Py_XDECREF(emission);
    return soac_append_owned(trace, row);
}

static int
soac_scope_trace_has_origin(soac_code_bindings *unit, PyObject *trace,
                             _PySoacReadOrigins origins)
{
    if (origins.lane[0] == 0 || origins.lane[1] != 0) {
        return 0;
    }
    int count = 0;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(trace); i++) {
        PyObject *row = PyList_GET_ITEM(trace, i);
        PyObject *emission = PyTuple_GET_ITEM(row, 2);
        int ordinal = PyLong_AsInt(PyTuple_GET_ITEM(emission, 0));
        if (ordinal < 0 || PyErr_Occurred()) {
            return 0;
        }
        count += unit->reference_instructions.items[ordinal].origins.lane[0] == origins.lane[0];
    }
    return count == 1;
}

static PyObject *
soac_scope_semantic_schedule(PyObject *consumer, PyObject *trace)
{
    PyObject *actions = PyTuple_New(PyList_GET_SIZE(trace));
    if (actions == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(trace); i++) {
        PyObject *step = PyList_GET_ITEM(trace, i);
        PyObject *emission = PyTuple_GET_ITEM(step, 2);
        /* All accepted trace rows are lane 0, Direct, with no second operand.
         * Keep operation, exact original event and every owner-bearing operand.
         * Only instruction/byte locations and source continuation differ by copy. */
        PyObject *action = PyTuple_Pack(4, PyTuple_GET_ITEM(step, 0),
            PyTuple_GET_ITEM(step, 1), PyTuple_GET_ITEM(emission, 5),
            PyTuple_GET_ITEM(emission, 3));
        if (action == NULL) {
            Py_DECREF(actions);
            return NULL;
        }
        PyTuple_SET_ITEM(actions, i, action);
    }
    PyObject *schedule = PyTuple_Pack(2, consumer, actions);
    Py_DECREF(actions);
    return schedule;
}

static int
soac_scope_enqueue_height(int *heights, int *queue, int *used, int count,
                           int ordinal, int height)
{
    if (ordinal < 0 || ordinal >= count || height < 0) {
        return 0;
    }
    if (heights[ordinal] != INT_MIN) {
        return heights[ordinal] == height;
    }
    assert(*used < count);
    heights[ordinal] = height;
    queue[(*used)++] = ordinal;
    return 1;
}

/* Native collection updates preserve their collection and intervening stack
 * operands. A deep access below the temporary suffix is allowed only when it
 * targets this exact Result position, never a saved owner or iterator token. */
static int
soac_scope_collection_update_values(int collection_kind, int opcode)
{
    switch (collection_kind) {
        case Py_SOAC_EAGER_LIST:
            return opcode == LIST_APPEND || opcode == LIST_EXTEND ? 1 : -1;
        case Py_SOAC_EAGER_SET:
            return opcode == SET_ADD || opcode == SET_UPDATE ? 1 : -1;
        case Py_SOAC_EAGER_DICT:
            return opcode == MAP_ADD ? 2 : opcode == DICT_UPDATE ? 1 : -1;
        default:
            return -1;
    }
}

static int
soac_scope_body_preserves_suffix(soac_code_bindings *unit,
                                  const soac_binding_region *region,
                                  const soac_reference_instruction *advance,
                                  const soac_reference_instruction *completion)
{
    int count = unit->reference_instruction_count;
    int *heights = PyMem_New(int, count);
    int *queue = PyMem_New(int, count);
    if (heights == NULL || queue == NULL) {
        PyMem_Free(heights);
        PyMem_Free(queue);
        PyErr_NoMemory();
        return 0;
    }
    for (int i = 0; i < count; i++) {
        heights[i] = INT_MIN;
    }
    int used = 0, next = 0, backedges = 0, valid = 0;
    /* Actual FOR_ITER fallthrough adds one next-item above the unchanged
     * [saved owners..., Result, iterator, index] suffix. */
    if (!soac_scope_enqueue_height(heights, queue, &used, count, advance->fallthrough, 1)) {
        goto done;
    }
    while (next < used) {
        int ordinal = queue[next++];
        int height = heights[ordinal];
        if (ordinal == advance->ordinal) {
            if (height != 0) {
                goto done;
            }
            backedges++;
            continue;
        }
        if (ordinal == completion->ordinal) {
            goto done;  /* No unrepresented exit bypassing the real advance. */
        }
        soac_reference_instruction *instruction = &unit->reference_instructions.items[ordinal];
        int popped = _PyOpcode_num_popped(instruction->opcode, instruction->oparg);
        int pushed = _PyOpcode_num_pushed(instruction->opcode, instruction->oparg);
        if (popped < 0 || pushed < 0) {
            goto done;
        }
        if (popped > height) {
            int values = soac_scope_collection_update_values(region->kind, instruction->opcode);
            if (values < 0 || height < values ||
                (Py_ssize_t)popped != (Py_ssize_t)height + 3 ||
                pushed != popped - values) {
                goto done;
            }
        }
        Py_ssize_t after = (Py_ssize_t)height - popped + pushed;
        if (after < 0 || after > INT_MAX) {
            goto done;
        }
        /* Native generated stack metadata is the same source used by
         * calculate_stackdepth. It models FOR_ITER's two CFG successors with
         * one next/completion value; the generic runtime skip closes no value
         * at END_FOR and is represented separately by the completion entries. */
        if (instruction->target >= 0 &&
            !soac_scope_enqueue_height(heights, queue, &used, count,
                instruction->target, (int)after)) {
            goto done;
        }
        if (instruction->fallthrough >= 0 &&
            !soac_scope_enqueue_height(heights, queue, &used, count,
                instruction->fallthrough, (int)after)) {
            goto done;
        }
        if (instruction->target < 0 && instruction->fallthrough < 0) {
            goto done;
        }
    }
    valid = backedges != 0;
done:
    PyMem_Free(heights);
    PyMem_Free(queue);
    return valid;
}

static PyObject *
soac_scope_normal_row(soac_code_bindings *unit, Py_ssize_t region_id,
                       soac_reference_instruction *allocation, Py_ssize_t context)
{
    soac_binding_region *region = &unit->regions.items[region_id];
    asdl_comprehension_seq *generators = soac_scope_generators(region);
    if (region->is_async || asdl_seq_LEN(generators) != 1 || context == -2 || region->result_kind < 0) {
        return Py_NewRef(Py_None);
    }
    soac_scope_event *advance_event = soac_scope_original_event(unit, region_id,
        Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_ITERATOR_ADVANCE, 0);
    soac_scope_event *completion_event = soac_scope_original_event(unit, region_id,
        Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_COMPLETION_VALUE_RETIRE, 0);
    soac_scope_event *iterator_event = soac_scope_original_event(unit, region_id,
        Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_ITERATOR_RETIRE, 0);
    soac_reference_instruction *advance = soac_scope_direct_event_instruction(unit, advance_event, context);
    soac_reference_instruction *completion = soac_scope_direct_event_instruction(unit, completion_event, context);
    soac_reference_instruction *iterator = soac_scope_direct_event_instruction(unit, iterator_event, context);
    if (advance == NULL || completion == NULL || iterator == NULL) {
        return PyErr_Occurred() ? NULL : Py_NewRef(Py_None);
    }
    /* Generic FOR_ITER skips the one actual END_FOR code unit. A native
     * FOR_ITER_GEN return enters that END_FOR with its completion value.
     * The target/operand came from the real CFG and resolve_jump_offsets. */
    if (advance->target != completion->ordinal || advance->oparg < 0 ||
        completion->first_byte != advance->end_byte + (Py_ssize_t)advance->oparg * sizeof(_Py_CODEUNIT) ||
        completion->opcode_offset != completion->first_byte ||
        completion->end_byte != completion->first_byte + sizeof(_Py_CODEUNIT) ||
        completion->fallthrough != iterator->ordinal ||
        iterator->first_byte != completion->end_byte) {
        return Py_NewRef(Py_None);
    }
    if (region->restores.count > PY_SSIZE_T_MAX - 5) {
        PyErr_NoMemory();
        return NULL;
    }
    soac_scope_owner_stack stack = {NULL, 0, region->restores.count + 5};
    stack.items = PyMem_Calloc((size_t)stack.capacity, sizeof(*stack.items));
    unsigned char *restored = PyMem_Calloc((size_t)Py_MAX(region->restores.count, 1), 1);
    PyObject *trace = PyList_New(0), *rewritten = PyList_New(0);
    PyObject *consumer = NULL, *entries = NULL, *row = NULL, *schedule = NULL;
    if (stack.items == NULL || restored == NULL || trace == NULL || rewritten == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        goto error;
    }
    if (!soac_scope_prefix_stack(unit, region_id, allocation, advance, context, &stack) ||
        !soac_scope_body_preserves_suffix(unit, region, advance, completion) ||
        !soac_scope_stack_push(&stack, SOAC_STACK_COMPLETION_VALUE)) {
        goto unsupported;
    }
    /* The exact final CFG has now proved that every normal body edge preserves
     * the relative owner suffix and every backedge restores its original height.
     * The following walk proves all post-loop ownership actions mechanically. */
    Py_ssize_t restore_count = 0, iterations = 0;
    int completion_closed = 0, iterator_closed = 0, consumed = 0, transported = 0;
    soac_scope_event *rotation = region->restores.count == 0 ? NULL :
        soac_scope_original_event(unit, region_id, Py_SOAC_SCOPE_PHASE_NORMAL_RESTORE,
            Py_SOAC_SCOPE_RESTORE_ROTATION, -2);
    if (region->restores.count != 0 && rotation == NULL) {
        goto unsupported;
    }
    for (int ordinal = completion->ordinal; ordinal >= 0 &&
         iterations++ < unit->reference_instruction_count;) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[ordinal];
        if (instruction->opcode == NOP) {
            ordinal = instruction->fallthrough;
            continue;
        }
        soac_reference_origin *origin = soac_operation_at(unit, instruction->origins.lane[0]);
        soac_scope_event *event = soac_scope_event_at(unit, origin);
        int kind = -1;
        if (event != NULL && event->region == region_id &&
            soac_scope_direct_event_instruction(unit, event, context) == instruction) {
            if (event == completion_event) {
                if (completion_closed++ || iterator_closed || restore_count ||
                    !soac_scope_stack_pop(&stack, SOAC_STACK_COMPLETION_VALUE)) {
                    goto unsupported;
                }
                kind = Py_SOAC_SCOPE_TRACE_COMPLETION_VALUE;
            }
            else if (event == iterator_event) {
                if (completion_closed != 1 || iterator_closed++ || restore_count ||
                    !soac_scope_stack_pop(&stack, SOAC_STACK_INDEX) ||
                    !soac_scope_stack_pop(&stack, SOAC_STACK_ITERATOR)) {
                    goto unsupported;
                }
                kind = Py_SOAC_SCOPE_TRACE_ITERATOR;
            }
            else if (event == rotation) {
                if (iterator_closed != 1 || transported++ ||
                    !soac_scope_stack_swap(&stack, instruction->oparg)) {
                    goto unsupported;
                }
                kind = Py_SOAC_SCOPE_TRACE_TRANSPORT;
            }
            else if (event->phase == Py_SOAC_SCOPE_PHASE_NORMAL_RESTORE &&
                     event->kind == Py_SOAC_SCOPE_RESTORE_SLOT) {
                if (iterator_closed != 1 || !soac_scope_stack_pop(&stack, event->owner)) {
                    goto unsupported;
                }
                Py_ssize_t saved = -1;
                for (Py_ssize_t i = 0; i < region->restores.count; i++) {
                    if (region->restores.items[i].owner == event->owner &&
                        region->restores.items[i].slot == event->slot) {
                        saved = i;
                    }
                }
                if (saved < 0 || restored[saved]) {
                    goto unsupported;
                }
                restored[saved] = 1;
                restore_count++;
                kind = Py_SOAC_SCOPE_TRACE_RESTORE;
            }
        }
        else if (origin != NULL && region->result_origin != 0 &&
                 instruction->origins.lane[0] == region->result_origin &&
                 instruction->origins.lane[1] == 0 &&
                 soac_same_emission_context(unit, context, unit, origin->emission_context)) {
            if (iterator_closed != 1 || consumed++ ||
                !soac_scope_stack_pop(&stack, SOAC_STACK_RESULT)) {
                goto unsupported;
            }
            if (region->result_kind == Py_SOAC_SCOPE_RESULT_DISCARD) {
                if (origin->family != SOAC_ORIGIN_SCOPE_RESULT ||
                    instruction->opcode != POP_TOP || instruction->oparg != 0) {
                    goto unsupported;
                }
            }
            else if (region->result_kind == Py_SOAC_SCOPE_RESULT_PUBLISH) {
                int form = soac_store_form(instruction->opcode);
                if (origin->family != SOAC_ORIGIN_STORE || origin->kind != Py_SOAC_BINDING_NAME ||
                    origin->phase != Py_SOAC_BINDING_PUBLISH ||
                    (form != Py_SOAC_STORE_FAST && form != Py_SOAC_STORE_CELL &&
                     form != Py_SOAC_STORE_NAME && form != Py_SOAC_STORE_GLOBAL) ||
                    !soac_original_store_matches(origin, form, 0)) {
                    goto unsupported;
                }
            }
            else {
                goto unsupported;
            }
            if (soac_scope_origin_instruction(unit, region->result_origin) != ordinal) {
                goto unsupported;
            }
            kind = Py_SOAC_SCOPE_TRACE_RESULT;
            event = NULL;  /* The original source Store keeps its own operation row. */
        }
        if (kind < 0) {
            goto unsupported;  /* No unrecorded callback, branch or stack action. */
        }
        if (soac_scope_append_trace(unit, trace, kind, event, instruction, context) < 0) {
            goto error;
        }
        if (iterator_closed == 1 && restore_count == region->restores.count) {
            if (consumed ? stack.size != 0 :
                (stack.size != 1 || stack.items[0] != SOAC_STACK_RESULT)) {
                goto unsupported;
            }
            break;
        }
        ordinal = instruction->fallthrough;
    }
    if (completion_closed != 1 || iterator_closed != 1 ||
        restore_count != region->restores.count || PyList_GET_SIZE(trace) < 2) {
        goto unsupported;
    }
    if (rotation != NULL && !transported) {
        if (soac_scope_origin_instruction(unit, rotation->origin) != -1 || !consumed) {
            goto unsupported;
        }
        soac_scope_static_swap *selected = NULL;
        for (Py_ssize_t i = 0; i < unit->scope_static_swaps.count; i++) {
            soac_scope_static_swap *candidate = &unit->scope_static_swaps.items[i];
            if (candidate->rotation == rotation->origin) {
                if (selected != NULL) {
                    goto unsupported;
                }
                selected = candidate;
            }
        }
        if (selected == NULL || selected->depth != region->restores.count + 1 ||
            !soac_scope_trace_has_origin(unit, trace, selected->first) ||
            !soac_scope_trace_has_origin(unit, trace, selected->last) ||
            (selected->first.lane[0] != region->result_origin &&
             selected->last.lane[0] != region->result_origin)) {
            goto unsupported;
        }
        if (soac_append_owned(rewritten, soac_scope_event_key(rotation)) < 0) {
            goto error;
        }
    }
    if (!consumed) {
        /* An original namespace/other source sink strictly AFTER restoration
         * stays outside this lifecycle. Positive Result survival is required. */
        consumer = Py_BuildValue("(iO)", Py_SOAC_SCOPE_RESULT_KEEP, Py_None);
    }
    else {
        PyObject *payload = region->result_kind == Py_SOAC_SCOPE_RESULT_DISCARD
            ? soac_source_span(region->result_statement)
            : soac_binding_origin_row(soac_operation_at(unit, region->result_origin));
        consumer = payload == NULL ? NULL : Py_BuildValue("(iO)", region->result_kind, payload);
        Py_XDECREF(payload);
    }
    entries = Py_BuildValue("((iii)(iii))",
        Py_SOAC_SCOPE_ENTRY_GENERIC_EXHAUSTION, advance->ordinal, iterator->ordinal,
        Py_SOAC_SCOPE_ENTRY_GENERATOR_COMPLETION, advance->ordinal, completion->ordinal);
    PyObject *trace_tuple = PyList_AsTuple(trace);
    PyObject *rewritten_tuple = PyList_AsTuple(rewritten);
    PyObject *continuation = soac_context_row(unit, context);
    schedule = consumer == NULL ? NULL : soac_scope_semantic_schedule(consumer, trace);
    if (consumer != NULL && entries != NULL && trace_tuple != NULL &&
        rewritten_tuple != NULL && continuation != NULL && schedule != NULL) {
        row = Py_BuildValue("(iOOOOO)", allocation->ordinal, continuation, consumer,
            entries, trace_tuple, rewritten_tuple);
    }
    Py_XDECREF(trace_tuple);
    Py_XDECREF(rewritten_tuple);
    Py_XDECREF(continuation);
    if (row == NULL) {
        goto error;
    }
    Py_XSETREF(region->normal_schedule, schedule);
    schedule = NULL;
    region->rewritten_rotation = rotation != NULL && !transported ? rotation->origin : 0;
    goto done;
unsupported:
    if (!PyErr_Occurred()) {
        row = Py_NewRef(Py_None);
    }
error:
done:
    PyMem_Free(stack.items);
    PyMem_Free(restored);
    Py_XDECREF(trace);
    Py_XDECREF(rewritten);
    Py_XDECREF(consumer);
    Py_XDECREF(entries);
    Py_XDECREF(schedule);
    return row;
}

/* This predicate starts with a positive remove_unreachable observation of
 * the exact native BuildCollection instruction. Absence is only a further
 * consistency check: a surviving prefix, consumer, handler member or cloned
 * lane keeps the copy unsupported, even if the allocation itself vanished. */
static int
soac_scope_copy_observed_unreachable(soac_code_bindings *unit, Py_ssize_t region_id)
{
    soac_binding_region *region = &unit->regions.items[region_id];
    if (region->unreachable_allocation == 0 || unit->scope_protection_uncertain ||
        region->protection != Py_SOAC_SCOPE_FINALLY_AFTER_PREFIX) {
        return 0;
    }
    soac_scope_event *build = soac_scope_original_event(unit, region_id,
        Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_BUILD_COLLECTION, region->kind);
    if (build == NULL || build->origin != region->unreachable_allocation ||
        soac_operation_at(unit, build->origin)->emission_context == -2) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < unit->scope_events.count; i++) {
        soac_scope_event *event = &unit->scope_events.items[i];
        if (event->region == region_id && event->alternative) {
            return 0;
        }
    }
    for (Py_ssize_t i = 0; i < unit->reference_instructions.count; i++) {
        soac_reference_instruction *instruction = &unit->reference_instructions.items[i];
        if (instruction->opcode == NOP) {
            continue;
        }
        for (int lane = 0; lane < 2; lane++) {
            uint32_t id = instruction->origins.lane[lane];
            soac_scope_event *event = soac_scope_event_at(unit, soac_operation_at(unit, id));
            if ((event != NULL && event->region == region_id) ||
                (id != 0 && id == region->result_origin)) {
                return 0;
            }
        }
    }
    for (Py_ssize_t i = 0; i < unit->scope_protection_members.count; i++) {
        soac_scope_protection_member *member = &unit->scope_protection_members.items[i];
        soac_scope_event *enter = soac_scope_event_at(unit, soac_operation_at(unit, member->enter));
        if (enter == NULL || enter->region == region_id) {
            return 0;
        }
    }
    return 1;
}

static PyObject *
soac_scope_lifecycle_rows(soac_code_bindings *unit, Py_ssize_t representative, PyObject *gaps)
{
    PyObject *protections = PyList_New(0), *normal = PyList_New(0);
    if (protections == NULL || normal == NULL) {
        Py_XDECREF(protections);
        Py_XDECREF(normal);
        return NULL;
    }
    int retained_allocation = 0;
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        if (soac_region_representative(unit, i) != representative) {
            continue;
        }
        soac_binding_region *region = &unit->regions.items[i];
        soac_scope_event *build = soac_scope_original_event(unit, i,
            Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_BUILD_COLLECTION, region->kind);
        soac_reference_origin *origin = build == NULL ? NULL : soac_operation_at(unit, build->origin);
        if (origin != NULL && soac_scope_direct_event_instruction(unit, build,
                origin->emission_context) != NULL) {
            retained_allocation = 1;
        }
        else if (PyErr_Occurred()) {
            goto error;
        }
    }
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        if (soac_region_representative(unit, i) != representative) {
            continue;
        }
        soac_binding_region *region = &unit->regions.items[i];
        region->lifecycle_copy_omitted = 0;
        region->protection_complete = region->normal_complete = region->normal_group_coherent = 0;
        region->rewritten_rotation = 0;
        Py_CLEAR(region->normal_schedule);
        /* A wholly eliminated original still publishes its old refusal gaps.
         * Only a dead physical copy of a surviving original can be omitted. */
        if (retained_allocation && soac_scope_copy_observed_unreachable(unit, i)) {
            region->lifecycle_copy_omitted = 1;
            continue;
        }
        soac_scope_event *build = soac_scope_original_event(unit, i,
            Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_BUILD_COLLECTION, region->kind);
        soac_reference_origin *origin = build == NULL ? NULL : soac_operation_at(unit, build->origin);
        Py_ssize_t context = origin == NULL ? -2 : origin->emission_context;
        soac_reference_instruction *allocation = soac_scope_direct_event_instruction(unit, build, context);
        if (allocation != NULL) {
            if (region->protection == Py_SOAC_SCOPE_FINALLY_AFTER_PREFIX) {
                PyObject *proof = soac_scope_protection_row(unit, i, allocation, context);
                if (proof == NULL) {
                    goto error;
                }
                if (proof == Py_None) {
                    Py_DECREF(proof);
                }
                else {
                    if (soac_append_owned(protections, proof) < 0) {
                        goto error;
                    }
                    region->protection_complete = 1;
                }
            }
            PyObject *proof = soac_scope_normal_row(unit, i, allocation, context);
            if (proof == NULL) {
                goto error;
            }
            if (proof == Py_None) {
                Py_DECREF(proof);
            }
            else {
                if (soac_append_owned(normal, proof) < 0) {
                    goto error;
                }
                region->normal_complete = 1;
            }
        }
        else if (PyErr_Occurred()) {
            goto error;
        }
        if (!region->normal_complete &&
            soac_scope_gap(unit, gaps, Py_SOAC_SCOPE_GAP_NORMAL_COMPLETION,
                i, NULL, -1, -1, -1, context) < 0) {
            goto error;
        }
    }
    /* A shared consumer cannot pick whichever physical finally copy happens to
     * authorize its desired owner order. Retain every row and refuse disagreement. */
    PyObject *first_schedule = NULL;
    int complete = 1, divergent = 0;
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        if (soac_region_representative(unit, i) != representative) {
            continue;
        }
        soac_binding_region *region = &unit->regions.items[i];
        if (region->lifecycle_copy_omitted) {
            continue;
        }
        complete &= region->normal_complete;
        if (!region->normal_complete) {
            continue;
        }
        assert(region->normal_schedule != NULL);
        if (first_schedule == NULL) {
            first_schedule = region->normal_schedule;
        }
        else {
            int same = PyObject_RichCompareBool(first_schedule, region->normal_schedule, Py_EQ);
            if (same < 0) {
                goto error;
            }
            divergent |= !same;
        }
    }
    for (Py_ssize_t i = 0; i < unit->regions.count; i++) {
        if (soac_region_representative(unit, i) != representative) {
            continue;
        }
        soac_binding_region *region = &unit->regions.items[i];
        if (region->lifecycle_copy_omitted) {
            continue;
        }
        region->normal_group_coherent = complete && !divergent;
        if (divergent) {
            soac_scope_event *build = soac_scope_original_event(unit, i,
                Py_SOAC_SCOPE_PHASE_BODY, Py_SOAC_SCOPE_BUILD_COLLECTION, region->kind);
            soac_reference_origin *origin = build == NULL ? NULL : soac_operation_at(unit, build->origin);
            if (soac_scope_gap(unit, gaps, Py_SOAC_SCOPE_GAP_NORMAL_COMPLETION,
                    i, NULL, -1, -1, -1, origin == NULL ? -2 : origin->emission_context) < 0) {
                goto error;
            }
        }
    }
    PyObject *protected_tuple = PyList_AsTuple(protections);
    PyObject *normal_tuple = PyList_AsTuple(normal);
    PyObject *result = protected_tuple == NULL || normal_tuple == NULL ? NULL :
        PyTuple_Pack(2, protected_tuple, normal_tuple);
    Py_XDECREF(protected_tuple);
    Py_XDECREF(normal_tuple);
    Py_DECREF(protections);
    Py_DECREF(normal);
    return result;
error:
    Py_DECREF(protections);
    Py_DECREF(normal);
    return NULL;
}

static int
soac_scope_event_has_lifecycle_proof(soac_code_bindings *unit, soac_scope_event *event)
{
    int found = 0;
    for (Py_ssize_t i = 0; i < unit->scope_events.count; i++) {
        soac_scope_event *candidate = &unit->scope_events.items[i];
        if (!soac_scope_event_in_group(unit, event, candidate)) {
            continue;
        }
        if (candidate->region < 0) {
            return 0;
        }
        soac_binding_region *region = &unit->regions.items[candidate->region];
        if (region->lifecycle_copy_omitted) {
            continue;
        }
        found = 1;
        if (event->kind == Py_SOAC_SCOPE_PROTECT_ENTER || event->kind == Py_SOAC_SCOPE_PROTECT_EXIT) {
            if (!region->protection_complete) {
                return 0;
            }
        }
        else if (event->kind == Py_SOAC_SCOPE_RESTORE_ROTATION &&
                 event->phase == Py_SOAC_SCOPE_PHASE_NORMAL_RESTORE) {
            if (!region->normal_complete || region->rewritten_rotation != candidate->origin) {
                return 0;
            }
        }
        else if (event->kind == Py_SOAC_SCOPE_ITERATOR_ADVANCE && event->auxiliary == 0) {
            if (!region->normal_complete || !region->normal_group_coherent) {
                return 0;
            }
        }
        else {
            return 0;
        }
    }
    return found;
}
