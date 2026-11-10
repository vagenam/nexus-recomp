/* ps3_setjmp.h -- setjmp/longjmp that never unwind.
 *
 * The runtime leaves guest code with longjmp (a guest thread's
 * sys_ppu_thread_exit, an SPU halt, an SPU interrupt return). Those jumps
 * cross lifted code, trampolines and C++ frames such as ps3_hle_call's TOC
 * guard, and expect a plain register restore -- which is what glibc/bionic
 * longjmp is.
 *
 * mingw/UCRT longjmp instead UNWINDS (RtlUnwindEx). On real Windows that died
 * with STATUS_BAD_FUNCTION_TABLE the first time a guest thread exited, and
 * elsewhere ran a skipped frame's destructor on a dead context; Wine's
 * lenient unwinder hid both. On mingw x64 these map to ps3_setjmp_win64.c:
 * save/restore the Win64 callee-saved state, nothing else. MSVC keeps its own
 * setjmp; POSIX hosts are untouched. */
#ifndef PS3_SETJMP_H
#define PS3_SETJMP_H
#include <setjmp.h>
#if defined(_WIN32) && !defined(_MSC_VER) && defined(__x86_64__)
#ifdef __cplusplus
extern "C" {
#endif
int  ps3_setjmp(void* buf) __attribute__((returns_twice));
void ps3_longjmp(void* buf, int val) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#  undef setjmp
#  undef longjmp
#  define setjmp(b)     ps3_setjmp((void*)(b))
#  define longjmp(b, v) ps3_longjmp((void*)(b), (v))
#endif
#endif
