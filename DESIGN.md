In this document there will be diverse design decisions that only I, the human, will modify. Those
explain diverse things that must hold in colib. This list keeps track of diverse decisions I've made
allong the way and wights them (most important first). Expect things like 1.12 to appear, those are
me reordering the list, the numbers are practically their floating point counterparts and do not
show inclusion so 1.1 is not part of 1. necesarily.

1. The killer must either kill it's target or throw, as such, it is guaranteed that the objects held
by the killed coroutine are gone.
2. The killer must be constructed from the public colib.h functions, it is an example at the same
time as it is a feature the user shouldn't want to re-implement, but it shouldn't use private
functions for the most part, only knowledge of behaviours. Of course the killer can't be isolated
by the intricacies of colib.h, but it must try to keep as clean of an interface as possible (killer
can depend on the library but the library can't depend on the killer)
3. A kill stops its target only at a wait. When the target's wait already did its work (a semaphore
just gave it its token, an io just completed), the kill doesn't end it there: it runs it on until its
next real wait (an io or a semaphore), where it ends it, or until it finishes by itself, and then
leaves it alone. Calls, returns and co_yield are control flow, not stops: the killer follows them to
know its chain, but runs through them. To a killer, computation takes no time. co::yield() is
treated like a wait.
