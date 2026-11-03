/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * ARM specific definitions for NOLIBC
 * Copyright (C) 2017-2022 Willy Tarreau <w@1wt.eu>
 */

#ifndef _NOLIBC_ARCH_ARM_H
#define _NOLIBC_ARCH_ARM_H

#include "compiler.h"
#include "crt.h"

/* Syscalls for ARM in ARM or Thumb modes :
 * - registers are 32-bit
 * - stack is 8-byte aligned
 * - syscall number is passed in r7
 * - arguments are in r0, r1, r2, r3, r4, r5
 * - the system call is performed by calling svc #0
 * - syscall return comes in r0.
 * - only lr is clobbered.
 * - in thumb mode without -fomit-frame-pointer, r7 is also used to store the
 *   frame pointer, so we swap with r6.
 */

#if (defined(__THUMBEB__) || defined(__THUMBEL__)) && \
    !defined(NOLIBC_OMIT_FRAME_POINTER)
#define _NOLIBC_SYSCALL_REG "r6"
#define _NOLIBC_THUMB_SET_R7 "eor r7, r6\neor r6, r7\neor r7, r6\n"
#define _NOLIBC_THUMB_RESTORE_R7 "mov r7, r6\n"
#else
#define _NOLIBC_SYSCALL_REG "r7"
#define _NOLIBC_THUMB_SET_R7 ""
#define _NOLIBC_THUMB_RESTORE_R7 ""
#endif

#define __nolibc_syscall0(num)                                                \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0");                                   \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r"(_num)                                     \
		: "r"(_arg1),                                                 \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall1(num, arg1)                                          \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1),                                                 \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall2(num, arg1, arg2)                                    \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1), "r"(_arg2),                                     \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall3(num, arg1, arg2, arg3)                              \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3),                         \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall4(num, arg1, arg2, arg3, arg4)                        \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),             \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall5(num, arg1, arg2, arg3, arg4, arg5)                  \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("r4") = (long)(arg5);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_num)                                                   \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)            \
({                                                                            \
	register long _num __asm__(_NOLIBC_SYSCALL_REG) = (num);              \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("r4") = (long)(arg5);                    \
	register long _arg6 __asm__ ("r5") = (long)(arg6);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_THUMB_SET_R7                                          \
		"svc #0\n"                                                    \
		_NOLIBC_THUMB_RESTORE_R7                                      \
		: "=r"(_arg1), "=r" (_num)                                    \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_arg6), "r"(_num)                                       \
		: "memory", "cc", "lr"                                        \
	);                                                                    \
	_arg1;                                                                \
})

#ifndef NOLIBC_NO_RUNTIME
/* startup code */
void __attribute__((weak, noreturn)) __nolibc_entrypoint __nolibc_no_stack_protector _start(void)
{
	__asm__ volatile (
		"mov r0, sp\n" /* save stack pointer to %r0, as arg1 of _start_c */
		"bl _start_c\n" /* transfer to c runtime */
	);
	__nolibc_entrypoint_epilogue();
}
#endif /* NOLIBC_NO_RUNTIME */

#endif /* _NOLIBC_ARCH_ARM_H */
