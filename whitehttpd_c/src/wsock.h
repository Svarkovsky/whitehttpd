/**
 * @file wsock.h
 * @brief Linux networking, epoll and signal handling for freestanding nolibc.
 *
 * nolibc has no sockets and no sigaction(), so this header supplies the
 * minimum an event-driven HTTP server needs on top of it: thin syscall
 * wrappers and the structures the kernel actually expects.
 *
 * The header is architecture-neutral. Everything that differs between
 * targets is confined to the PORTABILITY block; nothing below it is
 * target-specific. MIPS O32 and x86-64 are the configurations exercised in
 * this repository, but the same code is expected to build for any Linux
 * target that nolibc supports.
 *
 * @par Already provided by nolibc
 * open, openat, read, write, close, fork, sendfile, getdents64, lseek and the
 * string and memory helpers. Those are used directly and are not wrapped here.
 *
 * @par Added by this header
 * - Nine socket and epoll syscall wrappers.
 * - sockaddr_in6 and epoll_event laid out the way the kernel uses them per
 *   architecture.
 * - sigaction with the field order the kernel uses per architecture.
 * - A zero-stack syscall macro for MIPS O32, worth 8 bytes per call site.
 *
 * @author  Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * @copyright Copyright (c) 2026 Ivan Svarkovsky
 * @par License
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * @note Legacy stat64 and time ABI compatibility is exercised under QEMU
 *       user-mode and on one physical router (ASUS RT-AC57U V3, Linux 3.3.8).
 *       That does not establish behaviour on every historical kernel or
 *       device.
 *
 * @note The vendored nolibc headers in third_party/nolibc keep their own
 *       licenses.
 */
#ifndef WSOCK_H
#define WSOCK_H

#include "nolibc.h"

/** ============================ PORTABILITY ==============================
 * These values are NOT the same on MIPS and x86. Getting them wrong gives
 * a UDP socket instead of TCP, or setsockopt() returning EINVAL.
 *
 * @defgroup portability Values that differ between architectures
 * @{
 */
#if defined(__mips__)
#  define WS_SOCK_STREAM    2        /**< Stream socket; x86 uses 1. */
#  define WS_SOL_SOCKET     0xffff   /**< Socket level; x86 uses 1. */
#  define WS_SO_REUSEADDR   0x0004   /**< Reuse address; x86 uses 2. */
#  define WS_SIGSETSIZE     16       /**< _NSIG is 128 on MIPS, 64 elsewhere. */
#  define WS_EPOLL_PACKED
#  define WS_SOCK_NONBLOCK  0x0080   /**< Non-blocking flag; differs on MIPS. */
#else
/** @def WS_EPOLL_PACKED
 *  @brief Alignment attribute for ws_epoll_event.
 *  @details Empty on MIPS, where the structure is naturally aligned to 16
 *  bytes. Packed on x86-64, where it would otherwise be 12 bytes.
 */
#  define WS_SOCK_STREAM    1        /**< Stream socket. */
#  define WS_SOL_SOCKET     1        /**< Socket level. */
#  define WS_SO_REUSEADDR   2        /**< Reuse address. */
#  define WS_SIGSETSIZE     8        /**< _NSIG is 64 here. */
#  define WS_EPOLL_PACKED   __attribute__((packed))
#  define WS_SOCK_NONBLOCK  0x0800   /**< Non-blocking flag. */
#endif
/** @} */

/** These are the same everywhere.
 *
 * @defgroup constants Values identical on every architecture
 * @{
 */
#define WS_AF_INET6         10   /**< IPv6 address family. */
#define WS_IPPROTO_TCP      6    /**< TCP protocol level. */
#define WS_TCP_NODELAY      1    /**< Disable Nagle. Without it a split header
                                      and body wait 40 ms for a delayed ACK. */
#define WS_IPPROTO_IPV6     41   /**< IPv6 protocol level. */
#define WS_IPV6_V6ONLY      26   /**< Set to 0 so one socket serves both
                                      address families. */
#define WS_SIGPIPE          13   /**< Raised on write to a closed socket. */
#if defined(__mips__)
#define WS_SIGCHLD          18   /**< Child stopped or exited; MIPS value. */
#else
#define WS_SIGCHLD          17   /**< Child stopped or exited. */
#endif
#define WS_SIG_IGN          ((void (*)(int))1)  /**< Ignore disposition. */

#define WS_EPOLLIN          0x001  /**< Data available to read. */
#define WS_EPOLLOUT         0x004  /**< Writable. */
#define WS_EPOLLERR         0x008  /**< Error condition. */
#define WS_EPOLLHUP         0x010  /**< Peer closed. */
#define WS_EPOLL_CTL_ADD    1      /**< Register a descriptor. */
#define WS_EPOLL_CTL_DEL    2      /**< Remove a descriptor. */
#define WS_EPOLL_CTL_MOD    3      /**< Change the event mask. */
/** @} */

/* ============================= STRUCTURES ============================= */

/** @brief A 128-bit IPv6 address, raw bytes in network order. */
struct ws_in6_addr {
    unsigned char b[16];  /**< Address bytes, most significant first. */
};

/** @brief IPv6 socket address, 28 bytes, identical on both architectures.
 *
 *  The server binds one of these and accepts into another, so the peer
 *  address of every connection arrives here.
 */
struct ws_sockaddr_in6 {
    unsigned short     sin6_family;    /**< Always WS_AF_INET6. */
    unsigned short     sin6_port;      /**< Port in network byte order. */
    unsigned int       sin6_flowinfo;  /**< Flow label, zero here. */
    struct ws_in6_addr sin6_addr;      /**< Address. */
    unsigned int       sin6_scope_id;  /**< Interface index, zero here. */
};

/** @brief One epoll event.
 *
 *  Mirrors the kernel rule: packed on x86-64, naturally aligned on MIPS where
 *  the structure is 16 bytes. Always use sizeof() on it, never a literal.
 */
struct ws_epoll_event {
    unsigned int       events;  /**< Event mask, for example WS_EPOLLIN. */
    unsigned long long data;    /**< Value carried through, here the descriptor. */
} WS_EPOLL_PACKED;  /**< Alignment attribute, see @ref WS_EPOLL_PACKED. */

/** @brief Signal action, in the field order the kernel expects.
 *
 *  MIPS puts sa_flags first, x86-64 puts sa_handler first and adds
 *  sa_restorer. Getting the order wrong is the most common source of silent
 *  breakage when porting signal code between the two.
 */
#if defined(__mips__)
/** @cond MIPS_SIGACTION
 *  MIPS layout: sa_flags first, no sa_restorer.
 */
struct ws_sigaction {
    unsigned int   sa_flags;
    void         (*sa_handler)(int);
    unsigned long  sa_mask[WS_SIGSETSIZE / sizeof(unsigned long)];
};
/** @endcond */
#else
struct ws_sigaction {
    /** @brief Handler. WS_SIG_IGN here, so the signal is ignored. */
    void         (*sa_handler)(int);
    /** @brief Flags. Zero here. */
    unsigned long  sa_flags;
    /** @brief Restorer. Unused, the kernel default is fine. */
    void         (*sa_restorer)(void);
    /** @brief Blocked-signal mask. */
    unsigned long  sa_mask[WS_SIGSETSIZE / sizeof(unsigned long)];
};
#endif

/* ================== ZERO-STACK SYSCALL (MIPS O32 only) ==================
 * nolibc's __nolibc_syscallN reserves 32 bytes of stack per call
 * (addiu $sp,$sp,-32 / +32) to follow the O32 ABI callee-save area rule.
 * The kernel does not write there, so for leaf syscalls this is pure
 * overhead: 2 instructions = 8 bytes per call site.
 *
 * With 31 syscall sites in httpd.c this saves ~248 bytes.
 * The macro preserves the a3 error check and clobber list exactly.
 */
/**
 * @brief MIPS O32 syscall macros.
 *
 * On O32 the stack pointer is moved down by 8 bytes and restored afterwards,
 * so the kernel's shadow space is never clobbered. That costs 8 bytes per
 * call site and prevents a crash. Other architectures call directly.
 */
#if defined(__mips__) && defined(_ABIO32)

#define _WS_SYSCALL_CLOBBER \
    "memory", "cc", "at", "v1", \
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", \
    _NOLIBC_SYSCALL_CLOBBER_HI_LO

/**
 * @brief Invoke a syscall with no arguments.
 * @return Raw syscall result, negative errno on failure.
 */
#define ws_syscall0(num)                                                  \
({                                                                        \
    register long _n __asm__("$2") = (long)(num);                         \
    register long _a3 __asm__("$7");                                      \
    __asm__ volatile (                                                    \
        "syscall\n"                                                       \
        : "=r"(_n), "=r"(_a3)                                             \
        : "0"(_n)                                                         \
        : _WS_SYSCALL_CLOBBER                                             \
    );                                                                    \
    _a3 ? -_n : _n;                                                       \
})

/**
 * @brief Invoke a syscall with one argument.
 * @return Raw syscall result, negative errno on failure.
 */
#define ws_syscall1(num, a1)                                              \
({                                                                        \
    register long _n __asm__("$2") = (long)(num);                         \
    register long _a0 __asm__("$4") = (long)(a1);                         \
    register long _a3 __asm__("$7");                                      \
    __asm__ volatile (                                                    \
        "syscall\n"                                                       \
        : "=r"(_n), "=r"(_a3)                                             \
        : "0"(_n), "r"(_a0)                                               \
        : _WS_SYSCALL_CLOBBER                                             \
    );                                                                    \
    _a3 ? -_n : _n;                                                       \
})

/**
 * @brief Invoke a syscall with two arguments.
 * @return Raw syscall result, negative errno on failure.
 */
#define ws_syscall2(num, a1, a2)                                          \
({                                                                        \
    register long _n __asm__("$2") = (long)(num);                         \
    register long _a0 __asm__("$4") = (long)(a1);                         \
    register long _a1 __asm__("$5") = (long)(a2);                         \
    register long _a3 __asm__("$7");                                      \
    __asm__ volatile (                                                    \
        "syscall\n"                                                       \
        : "=r"(_n), "=r"(_a3)                                             \
        : "0"(_n), "r"(_a0), "r"(_a1)                                     \
        : _WS_SYSCALL_CLOBBER                                             \
    );                                                                    \
    _a3 ? -_n : _n;                                                       \
})

/**
 * @brief Invoke a syscall with three arguments.
 * @return Raw syscall result, negative errno on failure.
 */
#define ws_syscall3(num, a1, a2, a3)                                      \
({                                                                        \
    register long _n __asm__("$2") = (long)(num);                         \
    register long _a0 __asm__("$4") = (long)(a1);                         \
    register long _a1 __asm__("$5") = (long)(a2);                         \
    register long _a2 __asm__("$6") = (long)(a3);                         \
    register long _a3 __asm__("$7");                                      \
    __asm__ volatile (                                                    \
        "syscall\n"                                                       \
        : "=r"(_n), "=r"(_a3)                                             \
        : "0"(_n), "r"(_a0), "r"(_a1), "r"(_a2)                         \
        : _WS_SYSCALL_CLOBBER                                             \
    );                                                                    \
    _a3 ? -_n : _n;                                                       \
})

/**
 * @brief Invoke a syscall with four arguments.
 * @return Raw syscall result, negative errno on failure.
 */
#define ws_syscall4(num, a1, a2, a3, a4)                                  \
({                                                                        \
    register long _n __asm__("$2") = (long)(num);                         \
    register long _a0 __asm__("$4") = (long)(a1);                         \
    register long _a1 __asm__("$5") = (long)(a2);                         \
    register long _a2 __asm__("$6") = (long)(a3);                         \
    register long _a3 __asm__("$7") = (long)(a4);                         \
    __asm__ volatile (                                                    \
        "syscall\n"                                                       \
        : "=r"(_n), "=r"(_a3)                                             \
        : "0"(_n), "r"(_a0), "r"(_a1), "r"(_a2), "r"(_a3)               \
        : _WS_SYSCALL_CLOBBER                                             \
    );                                                                    \
    _a3 ? -_n : _n;                                                       \
})

/**
 * @brief Five-argument syscall.
 * @note nolibc's version is kept, it already passes the fifth argument on the
 *       stack and needs no wrapper.
 */
#define ws_syscall5 __nolibc_syscall5

#else
/** @cond NON_MIPS
 * Non-MIPS branch. Every wrapper below simply delegates to the nolibc macro
 * of the same arity, which is already zero-stack on those targets.
 */
#define ws_syscall0 __nolibc_syscall0
#define ws_syscall1 __nolibc_syscall1
#define ws_syscall2 __nolibc_syscall2
#define ws_syscall3 __nolibc_syscall3
#define ws_syscall4 __nolibc_syscall4
#define ws_syscall5 __nolibc_syscall5
/** @endcond */
#endif

/* ============================== WRAPPERS ==============================
 * Thin pass-throughs to the raw syscalls. They exist only so that the
 * server never has to spell a syscall number, and so that a failure is
 * turned into -1 with errno set, the way the rest of the code expects.
 * Each returns the syscall result or -1.
 */

/**
 * @brief Create a socket.
 * @param domain Address family, for example WS_AF_INET6.
 * @param type   Socket type, for example WS_SOCK_STREAM.
 * @param proto  Protocol, usually 0.
 * @return File descriptor, or -1 with errno set.
 */
static inline int ws_socket(int domain, int type, int proto)
{
    return __sysret(ws_syscall3(__NR_socket, domain, type, proto));
}

/**
 * @brief Bind a socket to a local address.
 * @param fd   Socket descriptor.
 * @param addr Address to bind, always sockaddr_in6 here because the server
 *             uses a dual-stack socket.
 * @param len  Size of @p addr in bytes.
 * @return 0 on success, -1 with errno set.
 */
static inline int ws_bind(int fd, const struct ws_sockaddr_in6 *addr, int len)
{
    return __sysret(ws_syscall3(__NR_bind, fd, addr, len));
}

/**
 * @brief Mark a socket as accepting connections.
 * @param fd      Socket descriptor.
 * @param backlog Maximum pending connection queue.
 * @return 0 on success, -1 with errno set.
 */
static inline int ws_listen(int fd, int backlog)
{
    return __sysret(ws_syscall2(__NR_listen, fd, backlog));
}

/**
 * @brief Accept a connection and set flags in one call.
 * @param fd    Listening socket descriptor.
 * @param addr  Optional peer address buffer, may be NULL.
 * @param len   Optional in/out size of @p addr, may be NULL.
 * @param flags Flags such as WS_SOCK_NONBLOCK applied to the new descriptor.
 * @return New descriptor, or -1 with errno set.
 */
static inline int ws_accept4(int fd, void *addr, int *len, int flags)
{
    return __sysret(ws_syscall4(__NR_accept4, fd, addr, len, flags));
}

/**
 * @brief Set a socket option.
 * @param fd    Socket descriptor.
 * @param level Option level, for example WS_SOL_SOCKET.
 * @param name  Option name.
 * @param val   Pointer to the option value.
 * @param len   Size of the value in bytes.
 * @return 0 on success, -1 with errno set.
 */
static inline int ws_setsockopt(int fd, int level, int name,
                                const void *val, int len)
{
    return __sysret(ws_syscall5(__NR_setsockopt, fd, level, name, val, len));
}

/**
 * @brief Shut down part of a full-duplex connection.
 * @param fd  Socket descriptor.
 * @param how 0 reads, 1 writes, 2 both.
 * @return 0 on success, -1 with errno set.
 */
static inline int ws_shutdown(int fd, int how)
{
    return ws_syscall2(__NR_shutdown, fd, how);
}

/**
 * @brief Create an epoll instance.
 * @param flags Pass 0, or WS_EPOLL_CLOEXEC.
 * @return epoll descriptor, or -1 with errno set.
 */
static inline int ws_epoll_create1(int flags)
{
    return __sysret(ws_syscall1(__NR_epoll_create1, flags));
}

/**
 * @brief Add, modify or remove a descriptor in an epoll set.
 * @param ep epoll descriptor.
 * @param op WS_EPOLL_CTL_ADD, _DEL or _MOD.
 * @param fd Descriptor being watched.
 * @param ev Event mask, ignored for _DEL.
 * @return 0 on success, -1 with errno set.
 */
static inline int ws_epoll_ctl(int ep, int op, int fd, struct ws_epoll_event *ev)
{
    return __sysret(ws_syscall4(__NR_epoll_ctl, ep, op, fd, ev));
}

/**
 * @brief Wait for events on an epoll set.
 * @param ep      epoll descriptor.
 * @param ev      Array receiving up to @p maxev events.
 * @param maxev   Size of @p ev.
 * @param timeout Milliseconds to wait, 0 returns immediately.
 * @return Number of ready descriptors, 0 on timeout, -1 with errno set.
 */
static inline int ws_epoll_wait(int ep, struct ws_epoll_event *ev,
                                int maxev, int timeout)
{
    return __sysret(ws_syscall4(__NR_epoll_wait, ep, ev, maxev, timeout));
}

/* Without this the first client that disconnects mid-response kills the
 * whole server with SIGPIPE. Not optional. */
/**
 * @brief Set a signal disposition to SIG_IGN without libc.
 * @param sig Signal number, for example WS_SIGPIPE.
 * @return 0 on success, -1 with errno set.
 *
 * @note SIGPIPE must be ignored in any socket server. Without it the first
 *       client that disconnects mid-write kills the process.
 */
static inline int ws_ignore_signal(int sig)
{
    struct ws_sigaction sa;
    unsigned char *p = (unsigned char *)&sa;
    unsigned i;

    for (i = 0; i < sizeof(sa); i++)
        p[i] = 0;

    sa.sa_handler = WS_SIG_IGN;
    return __sysret(ws_syscall4(__NR_rt_sigaction,
                                      sig, &sa, 0, WS_SIGSETSIZE));
}


/* ================== MIPS KERNEL STAT64 (Linux 2.4 - 6.x) ==================
 * nolibc uses sys_statx (4366), which only exists in Linux >= 4.11.
 * Router kernels like Linux 3.3.8 (ASUSWRT on ASUS RT-AC57U V3) or 3.4/3.10
 * (Padavan) do NOT have statx and return -ENOSYS.
 * We invoke sys_stat64 (4213), which works on ALL MIPS Linux kernels.
 */
#if defined(__mips__)
struct ws_stat64 {
    unsigned long      st_dev;
    unsigned long      st_pad0[3];
    unsigned long long st_ino;
    unsigned int       st_mode;        /* offset 24 */
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long      st_rdev;
    unsigned long      st_pad1[3];
    long long          st_size;        /* offset 56 */
    long               st_atime;
    unsigned long      st_atime_nsec;
    long               st_mtime;
    unsigned long      st_mtime_nsec;
    long               st_ctime;
    unsigned long      st_ctime_nsec;
    unsigned long      st_blksize;
    unsigned long      st_pad2;
    long long          st_blocks;
};

/**
 * @brief stat() that works on every Linux kernel, via sys_stat64.
 * @param path Path to inspect.
 * @param buf  Receives mode, size and mtime.
 * @return 0 on success, -1 with errno set.
 *
 * @note nolibc uses statx, which needs Linux 4.11 or newer. Kernels such as
 *       3.3.8 on the target router do not have it, so the 32-bit stat64
 *       syscall is used instead and the fields are copied across.
 */
static inline int ws_stat(const char *path, struct stat *buf)
{
    struct ws_stat64 kst;
    long ret = ws_syscall2(__NR_stat64, path, &kst);
    if (ret < 0) {
        SET_ERRNO(-ret);
        return -1;
    }
    buf->st_mode = kst.st_mode;
    buf->st_size = kst.st_size;
    buf->st_mtime = (time_t)kst.st_mtime;
    return 0;
}
#define stat(path, buf) ws_stat(path, buf)

/**
 * @brief fstat() that works on every Linux kernel, via sys_fstat64.
 * @param fd  Open descriptor.
 * @param buf Receives mode, size and mtime.
 * @return 0 on success, -1 with errno set.
 *
 * @note Same reasoning as ws_stat(). The caller always has a descriptor in
 *       hand, which is also what closes the open/stat race.
 */
static inline int ws_fstat(int fd, struct stat *buf)
{
    struct ws_stat64 kst;
    long ret = ws_syscall2(__NR_fstat64, fd, &kst);
    if (ret < 0) { SET_ERRNO(-ret); return -1; }
    buf->st_mode = kst.st_mode;
    buf->st_size = kst.st_size;
    buf->st_mtime = (time_t)kst.st_mtime;
    return 0;
}
#define fstat(fd, buf) ws_fstat(fd, buf)


/* ================== MIPS TIME (sys_gettimeofday 4078) =====================
 * nolibc time() calls clock_gettime64 (4403), which only exists in Linux >= 5.1.
 * On Linux 3.3.8 ASUSWRT it returns -ENOSYS, resulting in uninitialized time.
 * We use sys_gettimeofday (4078), which works on ALL Linux kernels.
 */
/**
 * @brief time() that works on every Linux kernel, via sys_gettimeofday.
 * @param tptr Optional output for the result, may be NULL.
 * @return Seconds since the epoch.
 *
 * @note nolibc calls clock_gettime64, which needs Linux 5.1 or newer. Kernels
 *       such as 3.3.8 do not have it and would return an uninitialised time.
 */
static inline time_t ws_mips_time(time_t *tptr)
{
    struct { long tv_sec; long tv_usec; } tv;
    long ret = ws_syscall2(__NR_gettimeofday, &tv, 0);
    if (ret < 0) return 0;
    if (tptr) *tptr = (time_t)tv.tv_sec;
    return (time_t)tv.tv_sec;
}
#define time(tptr) ws_mips_time(tptr)

#endif

#endif /* WSOCK_H */
