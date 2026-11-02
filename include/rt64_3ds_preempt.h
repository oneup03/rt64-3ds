/* rt64-3ds's preemption check for a game's recompiled code (its trace.h's
 * TRACE_ENTRY, with N64Recomp's trace_mode), 3DS builds only.
 *
 * RT64_3DS_PREEMPT_CHECK(rdram) tests the runtime's preemption flag
 * (ultramodern.hpp's ultramodern_preempt_flag: a cache line of its own,
 * RT64_3DS_PREEMPT_FLAG_OFFSET bytes below RDRAM) from the rdram pointer
 * every recompiled function holds, and while it is raised calls
 * ultramodern_preempt through rt64_3ds_preempt_stub (preempt_check.cpp),
 * which keeps every register that call could change. With the flag down it
 * is a load, a compare and a branch.
 *
 * Testing ultramodern_preempt_pending in C instead loads the global's
 * address from the function's literal pool first, and the call the
 * compiler sees makes every leaf function save lr and set up a frame. This
 * form, with N64Recomp's trace_skip_simple_leaves (no check in a function
 * without calls or loops), takes 187 KB of Banjo's code. Neither changed
 * the frame rate measurably on an Old 3DS: what the checks cost there was
 * the preemptions themselves, ~0.4 ms each (2026-11-02), which the Audio
 * row's priorities now limit (recomp3ds_run.cpp's preempt_gate).
 */
#ifndef RT64_3DS_PREEMPT_H
#define RT64_3DS_PREEMPT_H

/* ultramodern.hpp's ULTRAMODERN_PREEMPT_FLAG_OFFSET. */
#define RT64_3DS_PREEMPT_FLAG_OFFSET 32

/* A statement of its own (its semicolon included): trace_mode's TRACE_ENTRY()
 * has none after it. */
#define RT64_3DS_PREEMPT_CHECK(rdram_ptr) \
    __asm__ volatile( \
        "ldr ip, [%0, #-%c1]\n\t" \
        "cmp ip, #0\n\t" \
        "beq 1f\n\t" \
        "push {r0, lr}\n\t" \
        "mov r0, %0\n\t" \
        "bl rt64_3ds_preempt_stub\n\t" \
        "pop {r0, lr}\n" \
        "1:" \
        : : "r"(rdram_ptr), "i"(RT64_3DS_PREEMPT_FLAG_OFFSET) : "ip", "cc", "memory");

#endif
