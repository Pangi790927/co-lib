### 0. create_killer comment is something else, it looks like a wall of text poured by the bucket

### 1.

if (curr == root && next)
    return;
curr->self.destroy();
if (curr == root)
    return;

A better form:

if (curr == root) {
    if (!next)
        curr->self.destroy();
    return ;
}

### 2. close_wait(w.state); -- those are in the windows side, I hope those are also in the linux side,
right? meaning in the linux side the ordering is checked, I think I have wsl here, can you use that
to check? what would you need?

### 3.
#if COLIB_ENABLE_MULTITHREAD_SCHED
        /* This is either way lazy execution, but we need this execution to make a better informed
        response to the question: are tasks ready? So we do the same as in the next_task_state */
        if (thread_pushed_new_tasks) {
            std::lock_guard guard(lock);
            for (auto &s : ready_thread_tasks) {
                /* scheduled like pool_t::sched() does, on the pool's thread */
                if (do_sched_modifs(external_init_task(s, pool)) == ERROR_OK)
                    ready_tasks.push_back(s);
            }
            ready_thread_tasks.clear();
            thread_pushed_new_tasks = false;
        }
#endif /* COLIB_ENABLE_MULTITHREAD_SCHED */

this thig becomes larger and larger

### 4.

inline bool wait_had_effect(state_t *s) {
    if (state_access_t::wait_sem(s))
        return true;
    io_desc_t *io = state_access_t::wait_io(s);
    return io && s->pool->get_internal()->io_completed(*io);
}

This sounds very strange, at least how it looks, first on the if:

inline bool wait_had_effect(state_t *s) {
    if (state_access_t::wait_sem(s) != nullptr)
        return true;
    io_desc_t *io = state_access_t::wait_io(s);
    return io && s->pool->get_internal()->io_completed(*io);
}

Second, it is unclear to me why it's named wait HAD effect? Isn't it a more wait is in effect there?
Because the semaphore as I understand it is in wait at that point, or am I missunderstanding things?

### 5.

This is something else:

void queue_caller(state_t *root_state) {
    pool_internal_t *pool = root_state->pool->get_internal();
    state_t *caller = root_state->caller_state;
    if (caller && !(turn && pool->replace_ready(turn, caller)))
        pool->push_ready(caller);
    if (turn)
        pool->remove_ready(turn);
    turn = nullptr;
}

let's simplify for exposition (slot == turn, I think it's a better name)

void queue_caller(state_t *s) {
    pool = s->pool();
    caller = s->caller_state;

    if (caller && !(slot && pool->replace_ready(slot, caller)))
        pool->push_ready(caller);
    if (slot)
        pool->remove_ready(slot);
    slot = nullptr;
}

Those ifs inside, can't they maybe be like:

if (caller && slot && pool->replace_ready(slot, caller))
    pool->remove_ready(slot)
    slot = nullptr
    return
if (caller && !slot)
    pool->push_ready(caller)
    return
if (!caller && slot)
    // the function is called queue_caller, guess who shouldn't be null
    slot = nullptr // just so I make my point clear
    return
if (!caller && !slot)
    return

So with the final cleanup:

if (caller && slot && pool->replace_ready(slot, caller))
    pool->remove_ready(slot)
    slot = nullptr
    return
if (caller && !slot)
    pool->push_ready(caller)
    return
slot = nullptr

With you now on task to tell me if this is not enough (I would guess not, but should double check):

if (slot && pool->replace_ready(slot, caller))
    pool->remove_ready(slot)
    slot = nullptr
    return
if (!slot)
    pool->push_ready(caller)
    return
slot = nullptr

