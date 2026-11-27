/*
 * Support symbols that the MSVC toolchain normally provides and that
 * WdfDriverEntry.lib references when linked with LLVM lld-link.
 *
 * _guard_check_icall: Control Flow Guard check called before indirect calls.
 * These drivers are not built with /guard:cf, so it must do nothing. It is
 * naked so that it preserves every register (the ARM64 CFG thunk contract:
 * target in x15, argument registers untouched).
 */
__attribute__((naked)) void _guard_check_icall(void)
{
    __asm__ volatile("ret");
}
