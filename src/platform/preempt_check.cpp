// The slow path of rt64_3ds_preempt.h's preemption check: called from the
// middle of a recompiled function (rdram in r0, the caller's r0 and lr
// already saved), so it keeps everything the AAPCS lets ultramodern_preempt
// change - r1-r3, ip, d0-d7 and FPSCR - and realigns the stack, which a leaf
// function only keeps to 4 bytes.
#include <cstdint>

extern "C" void ultramodern_preempt(uint8_t* rdram);

extern "C" __attribute__((naked, used)) void rt64_3ds_preempt_stub() {
    __asm__ volatile(
        "push {r1, r2, r3, r4, ip, lr}\n\t"
        "mov r4, sp\n\t"
        "bic sp, sp, #7\n\t"
        "vpush {d0-d7}\n\t"
        "vmrs r1, fpscr\n\t"
        "push {r1, r2}\n\t"
        "bl ultramodern_preempt\n\t"
        "pop {r1, r2}\n\t"
        "vmsr fpscr, r1\n\t"
        "vpop {d0-d7}\n\t"
        "mov sp, r4\n\t"
        "pop {r1, r2, r3, r4, ip, pc}\n\t");
}
