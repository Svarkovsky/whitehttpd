/*
 * =============================================================================
 * @file      whitehttpd.s
 * @brief     whitehttpd - static file server in one 4096-byte memory page.
 * @version   1.0.0
 *
 * @details
 *   Handwritten GNU Assembler for MIPS32r2 Big-Endian, Linux O32 ABI.
 *   No libc and no libgcc: every service comes from a raw syscall.
 *
 *   This is the size reference of the project. It answers one question only:
 *   can a real event-driven HTTP server fit inside a single page of virtual
 *   memory. It can, at 4092 bytes, so the binary occupies exactly one page and
 *   one TLB entry, and the whole text segment stays resident in L1i.
 *
 *   It is not the product. The C version in whitehttpd_c/ is the product:
 *   portable across architectures, with directory listing, cache validators,
 *   logs and a wider contract. Comparing the two by size or by feature set is
 *   incorrect, because they are built for different jobs.
 *
 * @par Build
 *   mipsel-linux-gnu-as  -EB -mips32r2 -O2 -32 whitehttpd.s -o whitehttpd.o
 *   mipsel-linux-gnu-ld  -EB -s -N --no-warn-rwx-segments whitehttpd.o -o whitehttpd
 *   then tools/sstrip.py to drop the section table.
 *
 * @par Layout
 *   .sdata   strings, MIME table, constants. No runtime writes.
 *   .sbss    working buffers under 16 KB, reached with a single $gp instruction.
 *   .bss     the two large buffers, reached with la.
 *   Buffers come last so that p_filesz stays small: uninitialised space costs
 *   nothing on disk.
 *
 * @par Contract
 *   GET and HEAD. HTTP/1.0 and HTTP/1.1, keep-alive and pipelining. Single
 *   byte range with 206 and 416. Cache-Control. Directory access returns 403
 *   rather than a listing. Paths are walked one component at a time with
 *   O_NOFOLLOW at every step.
 *
 * @par Not implemented
 *   TLS, CGI, WebSocket, HTTP/2, compression, authentication, listing, logs.
 *
 * @par Verified on
 *   ASUS RT-AC57U V3, Qualcomm Atheros QCA5502, Linux 3.3.8, 128 MB RAM.
 *   12 to 16 KB resident, 4.4 KB file served at 1.29 GB/s, 60 parallel
 *   connections without loss.
 *
 * @author    Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * @copyright Copyright (C) 2026 Ivan Svarkovsky
 * @par License
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * @par Dual-Licensed Software
 *   1. Open source: GNU Affero General Public License v3.0 or later
 *      (AGPL-3.0-or-later). Strong copyleft: a modified version offered over a
 *      network must publish its source.
 *   2. Commercial: available for proprietary and closed-source distributions,
 *      IoT products and commercial firmware integration without AGPLv3
 *      copyleft. Contact ivansvarkovsky@gmail.com.
 *
 *   .text + .sdata <= 3960  ->  sstrip'ed ELF fits into one 4096-byte page.
 * =============================================================================
 */
.set noreorder
.set mips32r2
.global __start

SYS_exit          = 4001
SYS_fork          = 4002
SYS_read          = 4003
SYS_write         = 4004
SYS_open          = 4005
SYS_close         = 4006
SYS_chdir         = 4012
SYS_accept        = 4168
SYS_bind          = 4169
SYS_listen        = 4174
SYS_setsockopt    = 4181
SYS_socket        = 4183
SYS_rt_sigaction  = 4194
SYS_sendfile      = 4207
SYS_fstat64       = 4215
SYS_epoll_ctl     = 4249
SYS_epoll_wait    = 4250
SYS_openat        = 4288
SYS_epoll_create1 = 4326

AF_INET6          = 10
SOCK_STREAM       = 2
SOL_SOCKET        = 0xFFFF
SO_REUSEADDR      = 0x0004
SO_SNDTIMEO       = 0x1005
SO_RCVTIMEO       = 0x1006
IPPROTO_TCP       = 6
TCP_NODELAY       = 1
IPPROTO_IPV6      = 41
IPV6_V6ONLY       = 26

.macro g_la reg, sym
    addiu \reg, $gp, %gp_rel(\sym)
.endm

.text

/*
 * -----------------------------------------------------------------------------
 * @brief   Copy a NUL-terminated string.
 * @param   a0 = destination
 * @param   a1 = source
 * @return  a0 = pointer to the terminating NUL, v0 = same
 * -----------------------------------------------------------------------------
 */
strcpy:
.L_sc:
    lbu   $t0, 0($a1)
    addiu $a1, $a1, 1
    sb    $t0, 0($a0)
    bnez  $t0, .L_sc
    addiu $a0, $a0, 1           # [delay slot]
    addiu $a0, $a0, -1
    jr    $ra
    move  $v0, $a0              # [delay slot]

/*
 * -----------------------------------------------------------------------------
 * @brief   Compare two NUL-terminated strings.
 * @param   a0 = first string
 * @param   a1 = second string
 * @return  v0 = 0 when equal, non-zero otherwise
 * -----------------------------------------------------------------------------
 */
strcmp:
.L_cmp:
    lbu   $t0, 0($a0)
    lbu   $t1, 0($a1)
    subu  $v0, $t0, $t1
    bnez  $v0, .L_cmp_d
    addiu $a0, $a0, 1           # [delay slot]
    bnez  $t0, .L_cmp
    addiu $a1, $a1, 1           # [delay slot]
.L_cmp_d:
    jr    $ra
    nop

/*
 * -----------------------------------------------------------------------------
 * @brief   Find a substring inside the request buffer.
 * @param   a1 = needle, already lowercased by the caller
 * @return  v0 = pointer to the match, or 0
 * @note    Searches req_buf only, up to the buffered end. The haystack is
 *          folded to lower case in place while scanning.
 * -----------------------------------------------------------------------------
 */
strstr:
    g_la  $a0, req_buf
.L_ss_o:
    lbu   $t1, 0($a0)
    beqz  $t1, .L_ss_nf
    move  $t2, $a0              # [delay slot]
    move  $t3, $a1
.L_ss_i:
    lbu   $t5, 0($t3)
    beqz  $t5, .L_ss_f
    lbu   $t4, 0($t2)           # [delay slot]
    addiu $t6, $t4, -65
    sltiu $t6, $t6, 26
    sll   $t6, $t6, 5
    or    $t4, $t4, $t6         # lowercase ASCII letter
    addiu $t2, $t2, 1
    beq   $t4, $t5, .L_ss_i
    addiu $t3, $t3, 1           # [delay slot]
    b     .L_ss_o
    addiu $a0, $a0, 1           # [delay slot]
.L_ss_f:
    jr    $ra
    move  $v0, $t2              # [delay slot] pointer right after match!
.L_ss_nf:
    jr    $ra
    move  $v0, $zero            # [delay slot]

/*
 * -----------------------------------------------------------------------------
 * @brief   Parse an unsigned decimal number.
 * @param   a0 = string
 * @return  v0 = value, v1 = number of digits consumed, a0 advanced past them
 * -----------------------------------------------------------------------------
 *            (at most 9 digits are consumed -> no 32-bit overflow possible)
 */
parse_dec:
    move  $v0, $zero
    move  $v1, $zero
    li    $t0, 10
.L_pd:
    lbu   $t1, 0($a0)
    addiu $t2, $t1, -48
    sltiu $t3, $t2, 10
    beqz  $t3, .L_pd_d
    slti  $t3, $v1, 9           # [delay slot]
    beqz  $t3, .L_pd_d
    addiu $v1, $v1, 1           # [delay slot]
    mul   $v0, $v0, $t0
    addu  $v0, $v0, $t2
    b     .L_pd
    addiu $a0, $a0, 1           # [delay slot]
.L_pd_d:
    jr    $ra
    nop

/*
 * -----------------------------------------------------------------------------
 * @brief   Parse a port or count for the command line.
 * @param   a0 = string
 * @return  v0 = value
 * @note    Exits the process when the input is empty, zero, non-numeric or
 *          above 65535.
 * -----------------------------------------------------------------------------
 */
parse_uint:
    move  $t9, $ra
    jal   parse_dec
    nop
    beqz  $v0, .L_die           # empty or zero -> invalid
    lbu   $t0, 0($a0)           # [delay slot] string must end here ("8080junk" -> die)
    bnez  $t0, .L_die
    srl   $t0, $v0, 16          # [delay slot]
    bnez  $t0, .L_die
    nop
    jr    $t9
    nop

/*
 * -----------------------------------------------------------------------------
 * @brief   Format an unsigned value as decimal text.
 * @param   a0 = destination
 * @param   a1 = value
 * @return  a0 = pointer past the last digit, v0 = same
 * -----------------------------------------------------------------------------
 */
uint_to_str:
    g_la  $t0, tmp_num + 31
    li    $t1, 10
.L_u2s:
    divu  $zero, $a1, $t1
    mfhi  $t2
    mflo  $a1
    addiu $t0, $t0, -1
    addiu $t2, $t2, 48
    bnez  $a1, .L_u2s
    sb    $t2, 0($t0)           # [delay slot]
    b     strcpy
    move  $a1, $t0              # [delay slot]

/*
 * -----------------------------------------------------------------------------
 * @brief   Percent-decode the request target.
 * @param   a0 = source (raw request target)
 * @param   a1 = destination
 * @return  v0 = end of the decoded path, v1 = 1 on success / 0 on bad input,
 *          $fp = end of the raw target, used for logging and redirects
 * @note    The query string stops decoding. Rejects control bytes, which is
 *          what keeps a CRLF payload out of a response header.
 * -----------------------------------------------------------------------------
 */
urldecode:
    move  $t8, $a0
    move  $t9, $a1
    li    $t7, 63               # '?'
.L_ud:
    subu  $t1, $t8, $a0
    sltiu $t1, $t1, 1024
    beqz  $t1, .L_ud_bad
    lbu   $t0, 0($t8)           # [delay slot]
    addiu $t1, $t0, -33
    sltiu $t1, $t1, 94          # printable 33..126 ?
    beqz  $t1, .L_ud_sp         # space / CTL / DEL / NUL -> end of URL
    move  $fp, $t8              # [delay slot] $fp = end of raw URL (callee-saved!)
    bne   $t0, $t7, 1f          # '?' -> drop the query string
    nop
.L_ud_skip_q:
    addiu $t8, $t8, 1
    lbu   $t0, 0($t8)
    addiu $t1, $t0, -33
    sltiu $t1, $t1, 94
    bnez  $t1, .L_ud_skip_q
    nop
    move  $fp, $t8
    addiu $t1, $t0, -32
    bnez  $t1, .L_ud_bad        # not ' ' -> CTL/NUL/DEL, malformed
    sb    $zero, 0($t9)         # [delay slot]
    b     .L_ud_ok
    move  $v0, $t9              # [delay slot]
1:  li    $t1, 37               # '%'
    bne   $t0, $t1, .L_ud_p
    move  $t5, $zero            # [delay slot]

    li    $t6, 2
.L_hex2:
    lbu   $t2, 1($t8)
    addiu $t3, $t2, -48
    sltiu $t4, $t3, 10
    bnez  $t4, 2f
    andi  $t2, $t2, 0xDF        # [delay slot] to upper case
    addiu $t3, $t2, -65
    sltiu $t4, $t3, 6           # 'A'..'F' ?
    beqz  $t4, .L_ud_bad
    addiu $t3, $t3, 10          # [delay slot]
2:  sll   $t5, $t5, 4
    or    $t5, $t5, $t3
    addiu $t6, $t6, -1
    bnez  $t6, .L_hex2
    addiu $t8, $t8, 1           # [delay slot]
    sltiu $t1, $t5, 32
    bnez  $t1, .L_ud_bad
    li    $t1, 47               # [delay slot] '%2F'
    beq   $t5, $t1, .L_ud_bad
    li    $t2, 127              # [delay slot] '%7F'
    beq   $t5, $t2, .L_ud_bad
    move  $t0, $t5              # [delay slot]

.L_ud_p:
    sb    $t0, 0($t9)
    addiu $t8, $t8, 1
    b     .L_ud
    addiu $t9, $t9, 1           # [delay slot]

.L_ud_sp:
    li    $t1, 32
    bne   $t0, $t1, .L_ud_bad
    sb    $zero, 0($t9)         # [delay slot]
.L_ud_ok:
    move  $v0, $t9
    jr    $ra
    li    $v1, 1                # [delay slot]
.L_ud_bad:
    jr    $ra
    move  $v1, $zero            # [delay slot]

/*
 * =============================================================================
 * @brief   Serve one request on an accepted connection.
 * @param   a0 = connection descriptor
 * @return  v0 = 1 to keep the connection open, 0 to close it
 * @details
 *   The heart of the server. Reads until the header terminator, parses the
 *   request line with a single 32-bit load and compare of "GET " or "HEAD",
 *   walks the header list, resolves the path one component at a time with
 *   O_NOFOLLOW, then builds the response. The body goes out through sendfile
 *   without a copy. Methods other than GET and HEAD answer 501.
 * =============================================================================
 */
handle_client_epoll:
    addiu $sp, $sp, -72
    sw    $ra, 68($sp)
    sw    $s0, 64($sp)
    sw    $s1, 60($sp)
    sw    $s2, 56($sp)
    sw    $s3, 52($sp)
    sw    $s4, 48($sp)
    sw    $s5, 44($sp)
    sw    $s6, 40($sp)
    sw    $s7, 36($sp)
    move  $s0, $a0
    move  $s2, $zero            # total bytes in req_buf = 0
    li    $t0, 49               # default HTTP/1.1 ('1') for early 400 errors
    sb    $t0, 28($sp)

.L_hce_read_more:
    g_la  $t0, req_buf
    addu  $a1, $t0, $s2
    li    $a2, 8191
    subu  $a2, $a2, $s2
    blez  $a2, .L_hce_400       # header > 8KB -> 400 Bad Request
    move  $a0, $s0              # [delay slot]
    li    $v0, SYS_read
    syscall
    bnez  $a3, .L_hce_close
    addu  $s2, $s2, $v0         # [delay slot]
    blez  $v0, .L_hce_close
    g_la  $t0, req_buf          # [delay slot]
    addu  $t1, $t0, $s2
    sb    $zero, 0($t1)

.L_chk_eoh:
    move  $s1, $zero            # is_head = 0
    g_la  $t0, req_buf
.L_eoh_lp:
    lbu   $t1, 0($t0)
    beqz  $t1, .L_hce_read_more
    li    $t2, 10               # [delay slot] '\n'
    bne   $t1, $t2, .L_eoh_lp
    addiu $t0, $t0, 1           # [delay slot]
    lbu   $t1, 0($t0)
    li    $t3, 13               # '\r'
    bne   $t1, $t3, 1f
    nop
    addiu $t0, $t0, 1
1:  lbu   $t1, 0($t0)
    bne   $t1, $t2, .L_eoh_lp
    addiu $t4, $t0, 1           # [delay slot]
.L_eoh_ok:
    sw    $t4, 16($sp)          # save pipelined next_req pointer!
    sb    $zero, -1($t4)        # null-terminate current request

    g_la  $t0, req_buf
    lw    $t1, 0($t0)
    li    $t2, 0x47455420       # "GET "
    beq   $t1, $t2, .L_hce_url
    addiu $s5, $t0, 4           # [delay slot]
    li    $t2, 0x48454144       # "HEAD"
    bne   $t1, $t2, .L_hce_501
    lbu   $t3, 4($t0)           # [delay slot]
    li    $t4, 32
    bne   $t3, $t4, .L_hce_501
    li    $s1, 1                # [delay slot] is_head = 1
    addiu $s5, $t0, 5

.L_hce_url:
    lbu   $t0, 0($s5)
    li    $t1, 47               # '/'
    bne   $t0, $t1, .L_hce_400
    g_la  $s2, path_buf         # [delay slot]

    move  $a0, $s5
    jal   urldecode
    move  $a1, $s2              # [delay slot]
    beqz  $v1, .L_hce_400
    move  $s3, $v0              # [delay slot]

    # Verify " HTTP/1." at $t8
    lwl   $t0, 0($t8)
    lwr   $t0, 3($t8)
    lui   $t1, 0x2048           # " HTT"
    ori   $t1, $t1, 0x5454
    bne   $t0, $t1, .L_hce_400
    lwl   $t0, 4($t8)           # [delay slot]
    lwr   $t0, 7($t8)
    lui   $t1, 0x502f           # "P/1."
    ori   $t1, $t1, 0x312e
    bne   $t0, $t1, .L_hce_400
    lbu   $s6, 8($t8)           # [delay slot] '0' or '1'
    addiu $t0, $s6, -48
    sltiu $t0, $t0, 2
    beqz  $t0, .L_hce_400
    sb    $s6, 28($sp)          # [delay slot] version char on stack (0 writes to .sdata!)
    addiu $t9, $t8, 9           # first byte after the version digit
    lbu   $t0, 0($t9)
    addiu $t9, $t9, 1           # skip LF (or CR)
    addiu $t1, $t0, -13
    sltiu $t1, $t1, 1           # CR -> also skip the following LF
    addu  $t9, $t9, $t1

.L_hdr_init:
    move  $s7, $zero            # $s7 = Host count
.L_hdr_line:
    lbu   $t0, 0($t9)
    beqz  $t0, .L_hdr_done
    li    $t1, 13               # [delay slot] '\r'
    beq   $t0, $t1, .L_hdr_done
    li    $t1, 10               # [delay slot] '\n'
    beq   $t0, $t1, .L_hdr_done
    move  $t4, $t9              # [delay slot]

.L_hdr_name:
    lbu   $t0, 0($t9)
    addiu $t1, $t0, -33
    sltiu $t2, $t1, 94          # 33..126 (VCHAR, no space) -> ok
    beqz  $t2, .L_hce_400
    addiu $t9, $t9, 1           # [delay slot]
    li    $t2, 25               # ':' - 33
    bne   $t1, $t2, .L_hdr_name
    lbu   $t0, 0($t9)           # [delay slot]

.L_hdr_colon:
    subu  $t5, $t9, $t4         # $t5 = name length + 1
    sltiu $t0, $t5, 2
    bnez  $t0, .L_hce_400       # empty header name -> 400
    li    $t1, 5                # [delay slot] "host:" = 4 chars + 1

.L_hdr_ows:
    lbu   $t0, 0($t9)
    addiu $t2, $t0, -32         # SP ?
    sltiu $t3, $t2, 1
    addiu $t2, $t0, -9          # HTAB ?
    sltiu $t2, $t2, 1
    or    $t2, $t2, $t3
    addu  $t9, $t9, $t2         # advance over OWS only
    bnez  $t2, .L_hdr_ows
    nop

    bne   $t5, $t1, .L_hdr_val
    lwl   $t0, 0($t4)           # [delay slot]
    lwr   $t0, 3($t4)
    lui   $t1, 0x2020
    ori   $t1, $t1, 0x2020
    or    $t0, $t0, $t1
    lui   $t1, 0x686f           # "ho"
    ori   $t1, $t1, 0x7374      # "st"
    bne   $t0, $t1, .L_hdr_val
    lbu   $t0, 0($t9)           # [delay slot]
    sltiu $t1, $t0, 33
    bnez  $t1, .L_hce_400       # empty Host value
    addiu $s7, $s7, 1           # [delay slot]

.L_hdr_val:
    lbu   $t0, 0($t9)
    beqz  $t0, .L_hdr_done
    addiu $t1, $t0, -10
    beqz  $t1, .L_hdr_nxt       # '\n'
    addiu $t1, $t0, -13
    beqz  $t1, .L_hdr_cr        # '\r'
    addiu $t1, $t0, -32
    sltiu $t1, $t1, 95          # 32..126 -> ok
    bnez  $t1, .L_hdr_val_adv
    addiu $t2, $t0, -9          # [delay slot]
    bnez  $t2, .L_hce_400       # not HTAB -> CTL or DEL -> reject
    nop
.L_hdr_val_adv:
    b     .L_hdr_val
    addiu $t9, $t9, 1           # [delay slot]

.L_hdr_cr:
    lbu   $t0, 1($t9)
    beqz  $t0, .L_hdr_done
    li    $t1, 10               # [delay slot]
    bne   $t0, $t1, .L_hce_400
    addiu $t9, $t9, 1           # [delay slot]
.L_hdr_nxt:
    b     .L_hdr_line
    addiu $t9, $t9, 1           # [delay slot]

.L_hdr_done:
    li    $t0, 1
    slt   $t1, $t0, $s7         # more than one Host -> 400
    bnez  $t1, .L_hce_400
    li    $t2, 49               # [delay slot] '1'
    bne   $s6, $t2, .L_chk_framing
    move  $s4, $zero            # [delay slot] HTTP/1.0 -> keepalive off
    beqz  $s7, .L_hce_400       # HTTP/1.1 demands exactly one Host
    li    $s4, 1                # [delay slot]

.L_chk_framing:
    jal   strstr
    g_la  $a1, str_te           # [delay slot]
    bnez  $v0, .L_hce_400
    g_la  $a1, str_cl           # [delay slot]
    jal   strstr
    nop
    bnez  $v0, .L_hce_400
    g_la  $a1, str_close        # [delay slot]
    jal   strstr
    nop
    beqz  $v0, .L_open_target
    g_la  $a1, str_rng          # [delay slot]
    move  $s4, $zero

.L_open_target:
    # Check for Range header BEFORE terminating the raw URL at $fp!
    jal   strstr
    g_la  $a1, str_rng          # [delay slot] "\nrange: bytes="
    sw    $v0, 24($sp)          # save Range value pointer (or 0) on stack!

    li    $t1, 47               # '/'
    sb    $t1, 0($fp)           # raw URL gets its trailing slash ...
    sb    $zero, 1($fp)         # ... and is NUL terminated (used by the 301 redirect)

    lbu   $t0, 1($s2)
    beq   $t0, $t1, .L_hce_400  # reject "//"
    li    $a0, -100             # [delay slot] AT_FDCWD
    g_la  $a1, dot_str
    lui   $a2, 3                # 0x30000
    move  $a3, $zero
    li    $v0, SYS_openat
    syscall
    bnez  $a3, .L_hce_404
    move  $s6, $v0              # [delay slot] $s6 = dir_fd (callee-saved!)

.L_op_skip_sl:
    lbu   $t0, 0($s2)
    li    $t1, 47               # '/'
    beq   $t0, $t1, .L_op_skip_sl
    addiu $s2, $s2, 1           # [delay slot]
    addiu $s2, $s2, -1

    beqz  $t0, .L_hce_fstat
    li    $t1, 46               # [delay slot] '.'
    beq   $t0, $t1, .L_op_dot
    move  $s7, $s2              # [delay slot]

.L_op_scan:
    lbu   $t0, 0($s7)
    beqz  $t0, .L_op_seg
    li    $t1, 47               # [delay slot] '/'
    bne   $t0, $t1, .L_op_scan
    addiu $s7, $s7, 1           # [delay slot]
    addiu $s7, $s7, -1

.L_op_seg:
    move  $fp, $t0              # callee-saved!
    sb    $zero, 0($s7)
    lui   $a2, 2                # 0x20080
    beqz  $fp, 1f
    ori   $a2, $a2, 0x0080      # [delay slot]
    lui   $a2, 3                # 0x30080
    ori   $a2, $a2, 0x0080
1:  move  $a0, $s6
    move  $a1, $s2
    move  $a3, $zero
    li    $v0, SYS_openat
    syscall
    sb    $fp, 0($s7)
    move  $a0, $s6
    move  $s6, $v0
    move  $fp, $a3

    li    $v0, SYS_close
    syscall

    beqz  $fp, .L_op_skip_sl
    move  $s2, $s7              # [delay slot]

    li    $t0, 13
    beq   $s6, $t0, .L_hce_403
    nop
    b     .L_hce_404
    nop

.L_op_dot:
    move  $a0, $s6
    li    $v0, SYS_close
    syscall
    lbu   $t0, 1($s2)
    li    $t1, 46               # '.'
    beq   $t0, $t1, .L_hce_400
    nop
    b     .L_hce_403
    nop

.L_hce_fstat:
    move  $a0, $s6
    g_la  $a1, stat_buf
    li    $v0, SYS_fstat64
    syscall
    bnez  $a3, .L_sf_close
    g_la  $t0, stat_buf         # [delay slot]

    lw    $t1, 24($t0)
    andi  $t2, $t1, 0xF000
    li    $t3, 0x4000           # S_IFDIR
    beq   $t2, $t3, .L_hce_is_dir
    li    $t3, 0x8000           # [delay slot] S_IFREG
    beq   $t2, $t3, .L_hce_is_reg
    move  $a0, $s6              # [delay slot]

    li    $v0, SYS_close
    syscall
.L_hce_403:
    li    $s7, 403
    b     .L_hce_err
    g_la  $s2, msg_403          # [delay slot]

.L_hce_is_dir:
    lbu   $t1, -1($s3)
    li    $t2, 47               # '/'
    beq   $t1, $t2, .L_hce_dir_slash
    move  $a0, $s6              # [delay slot]

    li    $v0, SYS_close
    syscall

    # 301 redirect - reuse the generic header builder (Location is appended there)
    sw    $s5, 4($sp)           # raw (still percent-encoded) URL, already has '/'
    li    $s7, 301
    g_la  $s2, msg_301
    g_la  $s5, m_text
    g_la  $fp, s_html
    move  $s3, $zero            # Content-Length: 0
    li    $s6, -1
    b     .L_send_resp
    move  $s4, $zero            # [delay slot] always close

.L_hce_dir_slash:
    g_la  $a1, idx_name
    lui   $a2, 2
    ori   $a2, $a2, 0x0080
    move  $a3, $zero
    li    $v0, SYS_openat
    syscall
    move  $s7, $v0
    move  $s5, $a3

    move  $a0, $s6
    li    $v0, SYS_close
    syscall

    bnez  $s5, .L_hce_403
    move  $s6, $s7              # [delay slot]

    move  $a0, $s3
    jal   strcpy
    g_la  $a1, idx_name         # [delay slot]
    b     .L_hce_fstat
    nop

.L_hce_is_reg:
    lw    $s3, 60($t0)          # $s3 = total file_size
    sw    $s3, 12($sp)          # save total file_size at 12($sp)
    sw    $zero, 20($sp)        # default sendfile offset = 0
    li    $s7, 200              # default status = 200
    g_la  $s2, msg_200          # " OK"

    # ---------------- RFC 7233 Range parsing ----------------
    #  $s3 = file size, 24($sp) = ptr behind "bytes=" (0 = no Range header)
    #  "bytes=-N"          -> last N bytes (206); N >= size -> whole file
    #  "bytes=A-"          -> A .. size-1
    #  "bytes=A-B"         -> A .. B
    #  anything else       -> ignore Range, 200 OK (RFC 7233 3.1)
    #  from >= size        -> 416 with "Content-Range: bytes */size"
    lw    $a0, 24($sp)
    beqz  $a0, .L_do_mime       # No Range header -> normal 200 OK
    li    $t7, 45               # [delay slot] '-'
    blez  $s3, .L_hce_416       # Range on empty file -> 416
    lbu   $t0, 0($a0)           # [delay slot]
    bne   $t0, $t7, .L_rg_f1
    nop
    jal   parse_dec             # suffix-length N
    addiu $a0, $a0, 1           # [delay slot] skip '-'
    beqz  $v1, .L_do_mime       # no digits -> invalid syntax -> ignore (200)
    sltu  $t0, $v0, $s3         # [delay slot] N < size ?
    movz  $v0, $s3, $t0         # N >= size -> N = size (whole file, 206)
    subu  $t4, $s3, $v0         # from = size - N  ("-0" -> from = size -> 416)
    b     .L_rg_end
    addiu $t5, $s3, -1          # [delay slot] to = size - 1

.L_rg_f1:
    jal   parse_dec             # first-byte-pos
    nop
    beqz  $v1, .L_do_mime       # no digits -> ignore
    move  $t4, $v0              # [delay slot] from
    lbu   $t0, 0($a0)
    addiu $t1, $t0, -48
    sltiu $t1, $t1, 10
    bnez  $t1, .L_hce_416       # 10th digit -> >= 1G -> beyond any file -> 416
    nop                         # [delay slot] never branch in a delay slot
    bne   $t0, $t7, .L_do_mime  # must be '-'
    addiu $a0, $a0, 1           # [delay slot]
    lbu   $t0, 0($a0)
    addiu $t1, $t0, -48
    sltiu $t2, $t1, 10
    beqz  $t2, .L_rg_end        # "bytes=A-" -> open ended
    addiu $t5, $s3, -1          # [delay slot]
    jal   parse_dec             # last-byte-pos
    nop
    move  $t5, $v0

.L_rg_end:
    lbu   $t0, 0($a0)
    addiu $t1, $t0, -48         # '0'..'9' -> 0..9
    sltiu $t1, $t1, 10          # 1 if digit (parse_dec stopped at 9 digits)
    sltiu $t0, $t0, 32          # 1 if NUL/CR/LF
    or    $t0, $t0, $t1
    beqz  $t0, .L_do_mime       # junk / multi-range / space -> ignore Range
    sltu  $t0, $t4, $s3         # [delay slot]
    beqz  $t0, .L_hce_416       # from >= size -> 416
    sltu  $t0, $t5, $s3         # [delay slot] to < size ?
    addiu $t1, $s3, -1
    movz  $t5, $t1, $t0         # to >= size -> clamp to size-1
    sltu  $t0, $t5, $t4
    bnez  $t0, .L_hce_416
    sw    $t4, 20($sp)          # [delay slot] offset = range_from
    sw    $t5, 24($sp)          # save range_to
    subu  $s3, $t5, $t4
    addiu $s3, $s3, 1           # content_len = to - from + 1
    li    $s7, 206
    b     .L_do_mime
    g_la  $s2, msg_206          # [delay slot] " Partial Content"

.L_do_mime:
    # Inlined get_mime_type(path_buf): last '.' after the last '/' -> $t8 (0 = none)
    g_la  $a0, path_buf
    move  $t8, $zero
.L_gfe:
    lbu   $t1, 0($a0)
    beqz  $t1, .L_gmt_go
    addiu $a0, $a0, 1           # [delay slot]
    addiu $t2, $t1, -46         # '.' -> 0, '/' -> 1
    sltiu $t3, $t2, 2
    beqz  $t3, .L_gfe           # ordinary char
    sltiu $t3, $t2, 1           # [delay slot] 1 if '.'
    b     .L_gfe
    mul   $t8, $a0, $t3         # [delay slot] '.' -> ptr behind it, '/' -> 0

.L_gmt_go:
    g_la  $s5, m_appl           # default "application/" + "octet-stream"
    beqz  $t8, 1f
    g_la  $fp, mime_idx         # [delay slot] scan cursor
.L_gmt_scan:
    lbu   $t0, 0($fp)
    beqz  $t0, 1f               # end of table -> default
    move  $a0, $t8              # [delay slot]
    jal   strcmp
    move  $a1, $fp              # [delay slot]
.L_gmt_skip:
    lbu   $t2, 0($fp)
    bnez  $t2, .L_gmt_skip
    addiu $fp, $fp, 1           # [delay slot] -> past the NUL terminator
    lbu   $t1, 0($fp)           # pfx index (1..3)
    lbu   $t3, 1($fp)           # suffix offset
    addiu $fp, $fp, 2           # next entry
    bnez  $v0, .L_gmt_scan
    addiu $t1, $t1, -1          # [delay slot]
    sll   $t1, $t1, 2
    g_la  $t2, pfx_tab
    addu  $t2, $t2, $t1
    lw    $s5, 0($t2)           # $s5 = prefix string
    g_la  $fp, mime_sfx
    b     .L_send_resp
    addu  $fp, $fp, $t3         # [delay slot] $fp = suffix string
1:  b     .L_send_resp
    g_la  $fp, s_oct            # [delay slot] no match -> octet-stream

.L_hce_416:
    move  $a0, $s6
    li    $v0, SYS_close
    syscall
    li    $s7, 416
    b     .L_hce_err
    g_la  $s2, msg_416          # [delay slot] " Range Not Satisfiable"
.L_hce_400:
    li    $s7, 400
    b     .L_hce_err
    g_la  $s2, msg_400          # [delay slot]
.L_hce_404:
    li    $s7, 404
    b     .L_hce_err
    g_la  $s2, msg_404          # [delay slot]
.L_hce_501:
    li    $s7, 501
    g_la  $s2, msg_501
.L_hce_err:
    li    $s6, -1               # $s6 = -1 signals error response
    move  $s4, $zero            # keepalive = 0 on errors

    # Build error body in resp_hdr + 512 ("CODE Status")
    g_la  $a0, resp_hdr + 512
    jal   uint_to_str
    move  $a1, $s7              # [delay slot]
    jal   strcpy
    move  $a1, $s2              # [delay slot]
    g_la  $t0, resp_hdr + 512
    subu  $s3, $a0, $t0         # $s3 = content_len
    g_la  $s5, m_text           # $s5 = "text/",  $fp = "html"
    g_la  $fp, s_html

    # Unified inlined send_header ($s7=code, $s2=status, $s5/$fp=mime, $s3=clen, $s4=ka)
.L_send_resp:
    g_la  $a0, resp_hdr
    jal   strcpy
    g_la  $a1, http_proto       # [delay slot]
    jal   uint_to_str
    move  $a1, $s7              # [delay slot]
    jal   strcpy
    move  $a1, $s2              # [delay slot]
    jal   strcpy
    g_la  $a1, hdr_srv          # [delay slot]

    g_la  $a1, str_close        # "close\0keep-alive\0" -> keep-alive is at +6
    li    $t0, 6
    mul   $t0, $t0, $s4
    addu  $a1, $a1, $t0
    jal   strcpy
    li    $t9, 301              # [delay slot] $t9 survives strcpy, $t0 does not
    sltiu $t0, $s7, 400         # cacheable status (200/206/301), not 4xx/5xx
    beqz  $t0, 5f
    g_la  $a1, str_cc           # [delay slot]
    jal   strcpy
    nop
5:  bne   $s7, $t9, 2f          # 301 -> append Location
    g_la  $a1, hdr_loc          # [delay slot]
    jal   strcpy
    nop
    jal   strcpy
    lw    $a1, 4($sp)           # [delay slot] raw URL (already ends with '/')
2:  jal   strcpy
    g_la  $a1, hdr_ctype        # [delay slot]
    jal   strcpy
    move  $a1, $s5              # [delay slot]
    jal   strcpy
    move  $a1, $fp              # [delay slot]

    g_la  $t0, mime_sfx + 31    # charset for javascript/json/html/css/plain
    sltu  $t0, $fp, $t0
    beqz  $t0, 3f
    g_la  $a1, str_utf8         # [delay slot]
    jal   strcpy
    nop
3:  li    $t0, 206
    beq   $s7, $t0, 1f          # 206 -> "bytes from-to/total"
    li    $t0, 416              # [delay slot]
    bne   $s7, $t0, 4f
    g_la  $a1, hdr_crng         # [delay slot]
    jal   strcpy                # 416 -> "bytes */total"
    li    $t1, 42               # [delay slot] '*'
    sb    $t1, 0($a0)
    b     2f
    addiu $a0, $a0, 1           # [delay slot]
1:  jal   strcpy
    g_la  $a1, hdr_crng         # [delay slot]
    jal   uint_to_str
    lw    $a1, 20($sp)          # [delay slot] range_from
    li    $t1, 45               # '-'
    sb    $t1, 0($a0)
    addiu $a0, $a0, 1
    jal   uint_to_str
    lw    $a1, 24($sp)          # [delay slot] range_to
2:  li    $t1, 47               # '/'
    sb    $t1, 0($a0)
    addiu $a0, $a0, 1
    jal   uint_to_str
    lw    $a1, 12($sp)          # [delay slot] total file_size
4:  jal   strcpy
    g_la  $a1, hdr_clen         # [delay slot]
    jal   uint_to_str
    move  $a1, $s3              # [delay slot]
    jal   strcpy
    g_la  $a1, crlf_crlf        # [delay slot]

    g_la  $a1, resp_hdr
    lbu   $t0, 28($sp)          # HTTP version char ('0' or '1')
    sb    $t0, 7($a1)           # write into resp_hdr + 7 (in .sbss, 0 writes to .sdata!)
    subu  $a2, $a0, $a1
    move  $a0, $s0
    li    $v0, SYS_write
    syscall

    bltz  $s6, .L_send_err_body
    move  $a0, $s0              # [delay slot]
    bnez  $s1, .L_sf_close      # HEAD on file -> skip sendfile!
    move  $a1, $s6              # [delay slot]
.L_sf_loop:
    addiu $a2, $sp, 20          # &offset at 20($sp)!
    move  $a3, $s3
    li    $v0, SYS_sendfile
    syscall
    bnez  $a3, .L_sf_close
    subu  $s3, $s3, $v0         # [delay slot]
    bgtz  $v0, .L_sf_loop
    move  $a1, $s6              # [delay slot]

.L_sf_close:
    move  $a0, $s6
    li    $v0, SYS_close
    syscall

.L_hce_finish:
    beqz  $s4, .L_hce_close
    lw    $a1, 16($sp)          # [delay slot] check for pipelined next request!
    lbu   $t0, 0($a1)
    beqz  $t0, .L_hce_ret
    li    $v0, 1                # [delay slot] keep open
    g_la  $t0, req_buf
    jal   strcpy
    move  $a0, $t0              # [delay slot]
    b     .L_chk_eoh
    subu  $s2, $a0, $t0         # [delay slot] new $s2 = remaining length!

.L_send_err_body:
    bnez  $s1, .L_hce_close     # HEAD on error -> do NOT send body!
    move  $a2, $s3              # [delay slot]
    g_la  $a1, resp_hdr + 512
    li    $v0, SYS_write
    syscall

.L_hce_close:
    move  $v0, $zero
.L_hce_ret:
    lw    $s7, 36($sp)
    lw    $s6, 40($sp)
    lw    $s5, 44($sp)
    lw    $s4, 48($sp)
    lw    $s3, 52($sp)
    lw    $s2, 56($sp)
    lw    $s1, 60($sp)
    lw    $s0, 64($sp)
    lw    $ra, 68($sp)
    jr    $ra
    addiu $sp, $sp, 72          # [delay slot]

/*
 * -----------------------------------------------------------------------------
 * @brief   setsockopt on the shared option buffer.
 * @param   a1 = level
 * @param   a2 = option name
 * @param   a3 = option value pointer
 * @note    The file descriptor is taken from the shared state, so callers
 *          only pass the option triple.
 * -----------------------------------------------------------------------------
 */
do_sso_sol:
    li    $a1, SOL_SOCKET
do_sso:
    move  $a0, $s0
    li    $v0, SYS_setsockopt
    syscall
    jr    $ra
    nop

/*
 * =============================================================================
 * @brief   Parse the command line.
 * @param   a0 = argc
 * @param   a1 = argv
 * @return  $s5 = return address, $s6 = daemon flag, $s7 = port
 * @note    Unknown flags exit with status 2. The document root is chdir'd.
 * =============================================================================
 */
parse_args:
    move  $s5, $ra
    move  $s0, $a0
    move  $s1, $a1
    li    $s2, 1
.L_pa_loop:
    slt   $t0, $s2, $s0
    beqz  $t0, .L_pa_done
    sll   $t0, $s2, 2           # [delay slot]
    addu  $t0, $s1, $t0
    lw    $s3, 0($t0)

.L_pa_port:
    move  $a0, $s3
    jal   strcmp
    g_la  $a1, opt_port         # [delay slot]
    bnez  $v0, .L_pa_daemon
    g_la  $a1, opt_daemon       # [delay slot]
    addiu $s2, $s2, 1
    slt   $t0, $s2, $s0
    beqz  $t0, .L_die
    sll   $t0, $s2, 2           # [delay slot]
    addu  $t0, $s1, $t0
    jal   parse_uint
    lw    $a0, 0($t0)           # [delay slot]
    b     .L_pa_nxt
    move  $s7, $v0              # [delay slot]

.L_pa_daemon:
    jal   strcmp
    move  $a0, $s3              # [delay slot]
    bnez  $v0, .L_pa_chk
    li    $t2, 45               # [delay slot] '-'
    b     .L_pa_nxt
    li    $s6, 1                # [delay slot]

.L_pa_chk:
    lbu   $t1, 0($s3)
    beq   $t1, $t2, .L_die      # Reject unknown flags (including --addr)!
    move  $s4, $s3              # [delay slot] $s4 = wwwroot pointer!

.L_pa_nxt:
    b     .L_pa_loop
    addiu $s2, $s2, 1           # [delay slot]

.L_pa_done:
    jr    $s5
    nop

# ==============================================================================
# Process Entry Point (__start)
# ==============================================================================
__start:
    .set reorder
    la    $gp, _gp
    .set noreorder

    g_la  $s4, dot_str          # default wwwroot = "."
    li    $s7, 8080             # default port = 8080
    move  $s6, $zero            # default is_daemon = 0

    li    $a0, 13               # SIGPIPE -> SIG_IGN (sigpipe_act+8 is the zero optval)
    g_la  $a1, sigpipe_act
    move  $a2, $zero
    li    $a3, 16
    li    $v0, SYS_rt_sigaction
    syscall

    lw    $a0, 0($sp)
    jal   parse_args
    addiu $a1, $sp, 4           # [delay slot]

    move  $a0, $s4
    li    $v0, SYS_chdir
    syscall
    bnez  $a3, .L_die
    nop

    beqz  $s6, .L_not_daemon
    li    $v0, SYS_fork         # [delay slot]
    syscall
    beqz  $v0, .L_not_daemon
    move  $a0, $zero            # [delay slot]
    li    $v0, SYS_exit
    syscall
.L_not_daemon:

    li    $a0, AF_INET6
    li    $a1, SOCK_STREAM
    move  $a2, $zero
    li    $v0, SYS_socket
    syscall
    bnez  $a3, .L_die
    move  $s0, $v0              # [delay slot] $s0 = listen_fd

    li    $t0, 4
    sw    $t0, 16($sp)
    g_la  $a3, sigpipe_act + 4  # opt_val = 1
    bal   do_sso_sol            # SOL_SOCKET
    li    $a2, SO_REUSEADDR     # [delay slot]

    li    $a1, IPPROTO_IPV6
    g_la  $a3, sigpipe_act + 8  # opt_zero = 0
    bal   do_sso
    li    $a2, IPV6_V6ONLY      # [delay slot]

    li    $a1, IPPROTO_TCP
    g_la  $a3, sigpipe_act + 4  # opt_val = 1
    bal   do_sso
    li    $a2, TCP_NODELAY      # [delay slot]

    li    $t0, 8
    sw    $t0, 16($sp)
    g_la  $a3, sigpipe_act + 16 # struct timeval {5, 0}
    bal   do_sso_sol            # SOL_SOCKET
    li    $a2, SO_RCVTIMEO      # [delay slot]

    bal   do_sso_sol
    li    $a2, SO_SNDTIMEO      # [delay slot]

    g_la  $a1, serv_addr6
    li    $t1, AF_INET6
    sh    $t1, 0($a1)
    sh    $s7, 2($a1)

    move  $a0, $s0
    li    $a2, 28
    li    $v0, SYS_bind
    syscall
    bnez  $a3, .L_die
    move  $a0, $s0              # [delay slot]

    # Reuse serv_addr6 in writable .sbss as ev_tmp (events = EPOLLIN = 1)!
    li    $t0, 1
    sw    $t0, %gp_rel(serv_addr6)($gp)

    li    $a1, 128
    li    $v0, SYS_listen
    syscall
    bnez  $a3, .L_die
    move  $a0, $zero            # [delay slot]

    li    $v0, SYS_epoll_create1
    syscall
    bnez  $a3, .L_die
    move  $s1, $v0              # [delay slot] $s1 = epoll_fd

    move  $s7, $zero
    move  $s6, $zero
    b     .L_ev_add_epoll
    move  $a2, $s0              # [delay slot]

.L_epoll_loop:
    move  $a0, $s1
    g_la  $a1, ep_events
    li    $a2, 64
    li    $a3, 3000             # 3-second idle sweep timeout!
    li    $v0, SYS_epoll_wait
    syscall
    bnez  $a3, .L_epoll_loop
    move  $s7, $v0              # [delay slot] num_events
    bgtz  $v0, .L_ev_item
    move  $s6, $zero            # [delay slot] event_idx = 0

    addiu $s4, $s1, 1
.L_sweep:
    move  $a0, $s4
    li    $v0, SYS_close
    syscall
    sltiu $t0, $s4, 256
    bnez  $t0, .L_sweep
    addiu $s4, $s4, 1           # [delay slot]
    b     .L_epoll_loop
    nop

.L_ev_item:
    slt   $t0, $s6, $s7
    beqz  $t0, .L_epoll_loop
    sll   $t2, $s6, 4           # [delay slot]
    g_la  $t0, ep_events
    addu  $t0, $t0, $t2

    lw    $s5, 0($t0)
    lw    $s4, 8($t0)

    beq   $s4, $s0, .L_ev_accept
    andi  $t4, $s5, 0x0018      # [delay slot] EPOLLERR | EPOLLHUP
    bnez  $t4, .L_ev_close_fd
    move  $a0, $s4              # [delay slot]

    jal   handle_client_epoll
    nop
    bnez  $v0, .L_ev_next
    move  $a0, $s4              # [delay slot]

.L_ev_close_fd:
    li    $v0, SYS_close
    syscall
    b     .L_ev_next
    nop

.L_ev_accept:
    move  $a0, $s0
    move  $a1, $zero
    move  $a2, $zero
    li    $v0, SYS_accept
    syscall
    bnez  $a3, .L_ev_next
    move  $a2, $v0              # [delay slot]

.L_ev_add_epoll:
    g_la  $a3, serv_addr6       # ev_tmp in .sbss (0 writes to .sdata!)
    sw    $a2, 8($a3)
    move  $a0, $s1
    li    $a1, 1                # EPOLL_CTL_ADD
    li    $v0, SYS_epoll_ctl
    syscall

.L_ev_next:
    b     .L_ev_item
    addiu $s6, $s6, 1           # [delay slot]

.L_die:
    li    $a0, 1
    li    $v0, SYS_exit
    syscall

# ==============================================================================
# Read-Only Small Data Section (.sdata - 0 runtime writes!)
# ==============================================================================
.section .sdata
.align 2
# sa_handler = SIG_IGN(1), sa_flags = 1, sa_restorer = 0 (+8 = 4-byte zero optval),
# +16 = struct timeval {5, 0} used by SO_RCVTIMEO / SO_SNDTIMEO
sigpipe_act:    .long 1, 1, 0, 0, 5, 0

dot_str:        .asciiz "."
idx_name:       .asciiz "index.html"
crlf_crlf:      .byte 13, 10, 13, 10, 0
str_close:      .asciiz "close"         # +6:
                .asciiz "keep-alive"    #     Connection: close / keep-alive
str_te:         .asciiz "\ntransfer-encoding:"
str_cl:         .asciiz "\ncontent-length:"
str_rng:        .asciiz "\nrange: bytes="
str_utf8:       .asciiz "; charset=utf-8"

opt_port:       .asciiz "--port"
opt_daemon:     .asciiz "--daemon"

http_proto:     .asciiz "HTTP/1.1 "
hdr_srv:        .asciiz "\r\nServer: whitehttpd\r\nConnection: "
hdr_loc:        .asciiz "\r\nLocation: "
hdr_ctype:      .asciiz "\r\nContent-Type: "
hdr_crng:       .asciiz "\r\nContent-Range: bytes "
hdr_clen:       .asciiz "\r\nContent-Length: "
str_cc:         .asciiz "\r\nCache-Control: max-age=60"

msg_200:        .asciiz " OK"
msg_206:        .asciiz " Part"
msg_301:        .asciiz " Mv"
msg_400:        .asciiz " Bad Req"
msg_403:        .asciiz " Denied"
msg_404:        .asciiz " No"
msg_416:        .asciiz " Range"
msg_501:        .asciiz " Not Impl"

/*
 * @cond DOXYGEN_SKIP_DATA
 * Data tables below. Hidden from the generated documentation: they are data,
 * not code, and the C parser used for .s files trips over the ';' separators.
 */
# --- MIME types: 3 shared prefixes + 14 unique suffixes (prefix dedup) ---
#     pfx index: 1 = text/  2 = application/  3 = image/
.align 2
pfx_tab:        .long m_text, m_appl, m_img
m_text:         .asciiz "text/"
m_appl:         .asciiz "application/"
m_img:          .asciiz "image/"

#     suffixes 0..30 are the "text-ish" ones -> "; charset=utf-8" is appended
mime_sfx:
s_js:           .asciiz "javascript"      #  0
s_json:         .asciiz "json"            # 11
s_html:         .asciiz "html"            # 16
s_css:          .asciiz "css"             # 21
s_txt:          .asciiz "plain"           # 25
s_png:          .asciiz "png"             # 31
s_jpeg:         .asciiz "jpeg"            # 35
s_gif:          .asciiz "gif"             # 40
s_svg:          .asciiz "svg+xml"         # 44
s_ico:          .asciiz "x-icon"          # 52
s_pdf:          .asciiz "pdf"             # 59
s_wasm:         .asciiz "wasm"            # 63
s_zip:          .asciiz "zip"             # 68
s_oct:          .asciiz "octet-stream"    # 72

# --- extension table: "ext\0", pfx index (1..3), suffix offset ---
mime_idx:
    .asciiz "html"; .byte 1, 16
    .asciiz "htm";  .byte 1, 16
    .asciiz "css";  .byte 1, 21
    .asciiz "js";   .byte 2, 0
    .asciiz "mjs";  .byte 2, 0
    .asciiz "json"; .byte 2, 11
    .asciiz "txt";  .byte 1, 25
    .asciiz "md";   .byte 1, 25
    .asciiz "png";  .byte 3, 31
    .asciiz "jpg";  .byte 3, 35
    .asciiz "jpeg"; .byte 3, 35
    .asciiz "gif";  .byte 3, 40
    .asciiz "svg";  .byte 3, 44
    .asciiz "ico";  .byte 3, 52
    .asciiz "pdf";  .byte 2, 59
    .asciiz "wasm"; .byte 2, 63
    .asciiz "zip";  .byte 2, 68
    .byte 0

# ==============================================================================
# Small BSS Section (< 16 KB, addressed via 1-insn $gp)
# ==============================================================================
/*
 * @endcond
 */
.section .sbss
.align 2
serv_addr6:     .skip 32
tmp_num:        .skip 32
stat_buf:       .skip 160
ep_events:      .skip 1024
resp_hdr:       .skip 2048
path_buf:       .skip 4096
req_buf:        .skip 8192

.section .note.GNU-stack,"",@progbits
