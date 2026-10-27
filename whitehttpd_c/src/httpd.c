/**
 * @file httpd.c
 * @brief whitehttpd - static file server without libc.
 *
 * A single-translation-unit HTTP/1.0 and HTTP/1.1 server built on
 * third_party/nolibc and on the wsock.h layer beside it. There is no libc, no
 * dynamic allocation and no configuration file: everything is fixed at build
 * time or given on the command line.
 *
 * @par Design
 * One epoll loop per worker, a bounded connection array instead of a pool,
 * and a response state machine that never blocks. A slow client is left in
 * EPOLLOUT while the loop keeps serving everyone else. Workers are created
 * with fork before the loop starts and share the listening descriptor.
 *
 * All state lives in .bss. The kernel faults pages in on demand, so an idle
 * process is small and grows only with the connections actually held.
 *
 * @par Contract
 * Methods GET, HEAD and OPTIONS. Single byte range with 206 and 416. ETag,
 * Last-Modified, 304, If-None-Match, If-Modified-Since, If-Range and
 * Cache-Control. Directory listing with escaping. Paths open one component at
 * a time with O_NOFOLLOW at every step, and hidden entries are refused except
 * /.well-known/ for ACME.
 *
 * Not implemented, by design: TLS, CGI, WebSocket, HTTP/2, compression,
 * authentication, chroot, and multiple ranges in one request.
 *
 * @par Sizes
 * x86-64 17 008 bytes, MIPS32r2 BE/LE 20 892 and 20 908 bytes, measured
 * 2026-10-27 with gcc 12.2.0 and -Oz. See README.md for the full flags.
 *
 * @par Verified on
 * x86-64 and MIPS32r2 under QEMU user-mode, plus a physical ASUS RT-AC57U V3
 * running Linux 3.3.8. That is not a security certification.
 *
 * @author  Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * @copyright Copyright (c) 2026 Ivan Svarkovsky
 * @par License
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * @par Dual-Licensed Software
 * 1. Open source: GNU Affero General Public License v3.0 or later
 *    (AGPL-3.0-or-later). Strong copyleft: a modified version offered over a
 *    network must publish its source.
 * 2. Commercial: available for proprietary and closed-source distributions,
 *    IoT products and commercial firmware integration without AGPLv3
 *    copyleft. Contact ivansvarkovsky@gmail.com.
 */
#include "wsock.h"

/** @brief Version string, also sent in the Server header. */
#define WHITEHTTPD_VERSION "1.0.0"
/** @brief Author, printed by --help and --version. */
#define WHITEHTTPD_AUTHOR  "Ivan Svarkovsky <ivansvarkovsky@gmail.com>"
/** @brief License identifier, printed by --help and --version. */
#define WHITEHTTPD_LICENSE "AGPL-3.0-or-later"
/** @brief Project URL, printed by --help and --version. */
#define WHITEHTTPD_URL     "https://github.com/Svarkovsky/whitehttpd"

/* Memory budget. Kernel 3.3.8 reserves .bss eagerly, so these numbers are
 * real RAM on the router, not just address space.
 *   per worker = MAXFD * (128 + REQ_MAX + HDR_MAX)
 * MAXFD 128 gives 512 connections across the default four workers.
 * HDR_MAX 2048: the worst response is a 301 with a maximal Location, 1787
 * bytes measured, so 261 bytes of headroom remain.
 * REQ_MAX stays at 2048: a maximal percent-encoded URI plus ordinary headers
 * already needs about 2150 bytes, and lowering it would reject valid requests. */
/** @brief Highest descriptor the server tracks, hence connections per worker. */
#define MAXFD 128
/** @brief Request buffer per connection, in bytes. */
#define REQ_MAX 2048
/** @brief Longest accepted request path, in bytes. */
#define URI_MAX 512
/** @brief Response header buffer per connection, in bytes. */
#define HDR_MAX 2048
/** @brief Size of one directory listing page slot, in bytes. */
#define PAGE_MAX 65536
/** @brief Number of page slots, so two listings can be in flight. */
#define PAGE_SLOTS 2
/** @brief Seconds allowed to deliver a complete request header. */
#define REQUEST_SEC 15
/** @brief Seconds a kept-alive connection may stay idle. */
#define IDLE_SEC 30
/** @brief Seconds a connection may live without progress. */
#define KEEPALIVE_SEC 5
/** @brief Upper bound for --workers. */
#define MAX_WORKERS 16

/** @brief Where a connection is in its request/response cycle. */
enum state {
    FREE,        /**< Slot unused. */
    READING,     /**< Collecting the request header. */
    HEADER,      /**< Sending the response header. */
    FILE_BODY,   /**< Sending a file body by sendfile. */
    MEMORY_BODY, /**< Sending a directory listing from a page slot. */
    KEEPALIVE    /**< Response done, waiting for the next request. */
};

/** @brief Everything known about one connection.
 *
 *  Allocated statically for MAXFD descriptors and reset at the start of every
 *  request. Nothing here is heap allocated.
 */
struct connection {
    enum state state;      /**< Current phase. */
    int file;              /**< Open body descriptor, or -1. */
    int page;              /**< Page slot in use, or -1. */
    int used;              /**< Bytes buffered in requests[]. */
    int consumed;          /**< Bytes of requests[] already parsed. */
    int hlen;              /**< Header length queued in headers[]. */
    int hoff;              /**< Header bytes already sent. */
    int keep;              /**< Non-zero to keep the connection alive. */
    int http11;            /**< Non-zero when the request was HTTP/1.1. */
    int head;              /**< Non-zero for a HEAD request. */
    int has_meta;          /**< Non-zero when ETag and Last-Modified apply. */
    int o_method;          /**< Method offset in requests[], -1 when unknown. */
    int o_target;          /**< Target offset in requests[], -1 when unknown. */
    int options;           /**< Non-zero for an OPTIONS request. */
    long position;         /**< Body bytes already sent. */
    long end;              /**< Last body byte to send. */
    long mlen;             /**< Memory body length. */
    long fsize;            /**< File size, for the ETag. */
    long mtime;            /**< File modification time, for the ETag. */
    time_t progress;       /**< Last time the connection made progress. */
    time_t started;        /**< Time the current request began. */
    unsigned char peer[16];/**< Peer address, for the access log. */
};
/** @brief All connections, indexed by descriptor. */
static struct connection con[MAXFD];
/** @brief Per-connection request buffers, indexed by descriptor. */
static char requests[MAXFD][REQ_MAX];
/** @brief Per-connection response header buffers, indexed by descriptor. */
static char headers[MAXFD][HDR_MAX];
/** @brief Page slots holding directory listings while they are sent. */
static char pages[PAGE_SLOTS][PAGE_MAX];
/** @brief Bytes used in each page slot. */
static unsigned char page_used[PAGE_SLOTS];
/** @brief Descriptor of the document root, opened once at startup. */
static int root_fd;
/** @brief Log file descriptor, -1 when --log was not given. */
static int log_fd=-1;
/** @brief Set before header() to emit an Allow header, NULL otherwise. */
static const char *allow_hdr=0;
/** @brief File served for a directory, set by --index. */
static const char *index_name="index.html";
/** @brief Non-zero when directory listing is disabled by --no-listing. */
static int no_listing=0;
/** @brief Value of the Allow header sent with 405 and OPTIONS. */
#define ALLOW_METHODS "GET, HEAD, OPTIONS"
/* Log is overwritten from the start once it grows past this size. */
/** @brief Size at which the log is truncated and restarted, in bytes. */
#define LOG_MAX 262144
struct buffer { char *base; int length, capacity, failed; };
/** @brief Append one byte to a bounded buffer.
 *  @param b Target buffer.
 *  @param c Byte to append.
 *  @note Sets @c b->failed once the capacity is reached. Every later call
 *        becomes a no-op, so a truncated buffer is detected, never overrun.
 */
static void putc_b(struct buffer *b, char c) {
    if (b->length < b->capacity) b->base[b->length++] = c;
    else b->failed = 1;
}
/** @brief Append a NUL-terminated string to a bounded buffer.
 *  @param b Target buffer.
 *  @param s Text to append.
 */
static void puts_b(struct buffer *b, const char *s) { while (*s) putc_b(b, *s++); }
/** @brief Append an unsigned number in decimal.
 *  @param b Target buffer.
 *  @param n Value to append.
 */
static void number_b(struct buffer *b, unsigned long n) {
    char t[32]; int i = 0;
    do { t[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) putc_b(b, t[--i]);
}
/** @brief Case-sensitive string equality.
 *  @return Non-zero when the strings are equal.
 */
static int equal(const char *a, const char *b) { return strcmp(a,b) == 0; }
/** @brief Fold one ASCII character to lower case.
 *  @param c Character.
 *  @return Lower-case form, or the input when not a letter.
 */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/** @brief Compare two strings ignoring ASCII case.
 *  @return Non-zero when equal.
 */
static int iequal(const char *a, const char *b) {
    while (*a && *b && lower(*a)==lower(*b)) { a++; b++; }
    return *a == *b;
}
/** @brief Look for @p wanted as a whole token in a comma-separated list.
 *  @param s      Header value such as @c "keep-alive, Upgrade".
 *  @param wanted Token to find, compared ignoring case.
 *  @return Non-zero when found.
 *  @note Whole-token matching matters: @c "keep-alive" must not be reported
 *        as containing @c "close".
 */
static int token(const char *s, const char *wanted) {
    while (*s) {
        const char *p; int n=0;
        while (*s==' ' || *s=='\t' || *s==',') s++;
        p=s; while (*s && *s!=',' && *s!=' ' && *s!='\t') { s++; n++; }
        if (n==(int)strlen(wanted)) {
            int i; for(i=0;i<n && lower(p[i])==wanted[i];i++);
            if(i==n) return 1;
        }
        while (*s && *s!=',') s++;
    }
    return 0;
}
/** @brief Decode one hexadecimal digit.
 *  @return Value 0..15, or -1 when the character is not a hex digit.
 */
static int hex(int c) {
    if(c>='0'&&c<='9')return c-'0';
    c=lower(c); return c>='a'&&c<='f'?c-'a'+10:-1;
}
/** @brief Percent-decode the request target into a filesystem path.
 *  @param out Output buffer of URI_MAX bytes.
 *  @param in  Request target, may carry a query string.
 *  @return Length on success, -1 on malformed input, -2 when too long.
 *  @note The query string is dropped at the first '?'. Control bytes are
 *        refused, which is what stops a CRLF payload from reaching a header.
 */
static int decode_path(char *out, const char *in) {
    int n=0;
    while(*in && *in!='?') {
        unsigned char c=(unsigned char)*in++;
        if(c=='%') {
            int a,b; if(!in[0] || !in[1])return -1;
            a=hex(in[0]);b=hex(in[1]);if(a<0||b<0)return -1;
            c=(unsigned char)(a*16+b);in+=2;
        }
        if(n>=URI_MAX-1)return -2;
        if(c<32 || c==127)return -1;
        out[n++]=(char)c;
    }
    out[n]=0; return n && out[0]=='/' ? n:-1;
}
/** @brief Append text with HTML metacharacters escaped.
 *  @param b Target buffer.
 *  @param s Text, typically a file name from the directory listing.
 */
static void html(struct buffer *b,const char *s) {
    for(;*s;s++) switch(*s) {
        case '&':puts_b(b,"&amp;");break;case '<':puts_b(b,"&lt;");break;
        case '>':puts_b(b,"&gt;");break;case '"':puts_b(b,"&quot;");break;
        case '\'':puts_b(b,"&#39;");break;default:putc_b(b,*s);
    }
}
/** @brief Append text percent-encoded for use in a URL.
 *  @param b     Target buffer.
 *  @param s     Text to encode.
 *  @param slash When non-zero, '/' is left as is instead of encoded.
 */
static void url(struct buffer *b,const char *s,int slash) {
    const char *h="0123456789ABCDEF";
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
           c=='-'||c=='_'||c=='.'||c=='~'||(slash&&c=='/'))putc_b(b,(char)c);
        else {putc_b(b,'%');putc_b(b,h[c>>4]);putc_b(b,h[c&15]);}
    }
}
/** @brief Guess the content type from the file extension.
 *  @param p Path or file name.
 *  @return MIME type, or @c "application/octet-stream" when unknown.
 *  @note The match is on the suffix, ignoring case. Text types carry
 *        @c charset=utf-8 so non-ASCII content renders correctly.
 */
static const char *mime(const char *p) {
    static const struct {const char *ext,*type;} map[]={
        {".html","text/html; charset=utf-8"},{".htm","text/html; charset=utf-8"},
        {".css","text/css; charset=utf-8"},{".js","application/javascript; charset=utf-8"},
        {".mjs","application/javascript; charset=utf-8"},{".json","application/json"},
        {".txt","text/plain; charset=utf-8"},{".md","text/plain; charset=utf-8"},
        {".svg","image/svg+xml"},{".png","image/png"},{".jpg","image/jpeg"},
        {".jpeg","image/jpeg"},{".gif","image/gif"},{".ico","image/x-icon"},
        {".wasm","application/wasm"},{".pdf","application/pdf"},{".zip","application/zip"}};
    unsigned int i;size_t n=strlen(p);
    for(i=0;i<sizeof(map)/sizeof(map[0]);i++) {
        size_t m=strlen(map[i].ext);if(n>=m&&iequal(p+n-m,map[i].ext))return map[i].type;
    }
    return "application/octet-stream";
}
/* Wall clock for log stamps; now_sec() stays monotonic for timeouts. */
/** @brief Current wall-clock time in seconds since the epoch.
 *  @note Separate from now_sec(): this one is for log stamps and the Date
 *        header, the other is monotonic and only for timeouts.
 */
static long wall_sec(void) {
    struct { long sec,nsec; } ts;
    if(ws_syscall2(__NR_clock_gettime,0,&ts)<0)return 0;
    return (long)ts.sec;
}
/** @brief Monotonic time in seconds, used only for timeouts.
 *  @return Seconds from an arbitrary fixed point.
 */
static time_t now_sec(void) {
    struct { long sec,nsec; } ts;
    if(ws_syscall2(__NR_clock_gettime,1,&ts)<0)return 0; /* legacy ABI on O32 */
    return (time_t)ts.sec;
}
/** @brief Drop whatever the connection holds: a file or a page slot.
 *  @param fd Connection descriptor.
 */
static void release_payload(int fd) {
    if(con[fd].file>=0)close(con[fd].file);
    if(con[fd].page>=0)page_used[con[fd].page]=0;
    con[fd].file=con[fd].page=-1;
}
/** @brief Remove a connection from epoll and close it.
 *  @param ep epoll descriptor.
 *  @param fd Connection descriptor.
 */
static void close_connection(int ep,int fd) {
    release_payload(fd);
    ws_epoll_ctl(ep,WS_EPOLL_CTL_DEL,fd,0);
    ws_shutdown(fd,1);
    /* Drain only bytes already queued; the socket is nonblocking. */
    {char sink[512];int i;for(i=0;i<8&&read(fd,sink,sizeof sink)>0;i++);}
    close(fd);con[fd].state=FREE;
}
/** @brief Append a number as exactly two digits, zero padded.
 *  @param b Target buffer.
 *  @param v Value, 0..99.
 */
static void num2(struct buffer *b,int v) {
    putc_b(b,(char)('0'+v/10%10));putc_b(b,(char)('0'+v%10));
}
/** @brief Append a number in lowercase hexadecimal.
 *  @param b Target buffer.
 *  @param v Value to append.
 */
static void hexnum(struct buffer *b,unsigned long v) {
    char t[16];int n=0;
    if(!v){putc_b(b,'0');return;}
    while(v){int d=(int)(v&15);t[n++]=(char)(d<10?'0'+d:'a'+d-10);v>>=4;}
    while(n)putc_b(b,t[--n]);
}
/* days since 1970-01-01 -> civil date (Hinnant) */
/** @brief Convert days since 1970-01-01 to a civil date.
 *  @param z  Days since the epoch.
 *  @param yy Receives the year.
 *  @param mm Receives the month, 1..12.
 *  @param dd Receives the day of month, 1..31.
 */
static void civil(long z,int *yy,int *mm,int *dd) {
    long era,y;unsigned doe,yoe,doy,mp;
    z+=719468;era=(z>=0?z:z-146096)/146097;doe=(unsigned)(z-era*146097);
    yoe=(doe-doe/1460+doe/36524-doe/146096)/365;y=(long)yoe+era*400;
    doy=doe-(365*yoe+yoe/4-yoe/100);mp=(5*doy+2)/153;
    *dd=(int)(doy-(153*mp+2)/5+1);*mm=(int)(mp<10?mp+3:mp-9);*yy=(int)(y+(*mm<=2));
}
/** @brief Convert a civil date to days since 1970-01-01.
 *  @param y Year.
 *  @param m Month, 1..12.
 *  @param d Day of month.
 *  @return Days since the epoch.
 */
static long from_civil(int y,int m,int d) {
    long era,yoe;unsigned mp,doy;
    y-=m<=2;era=(y>=0?y:y-399)/400;yoe=(long)(y-era*400);
    mp=(unsigned)(m+(m>2?-3:9));doy=(153*mp+2)/5+(unsigned)d-1;
    return era*146097+(long)(yoe*365+yoe/4-yoe/100+doy)-719468;
}
/** @brief Parse a three-letter month name.
 *  @param s Pointer to the first letter.
 *  @return Month 1..12, or -1 when not recognised.
 */
static int month3(const char *s) {
    static const char *mn="JanFebMarAprMayJunJulAugSepOctNovDec";int i;
    for(i=0;i<12;i++)if(s[0]==mn[i*3]&&s[1]==mn[i*3+1]&&s[2]==mn[i*3+2])return i+1;
    return -1;
}
/** @brief Parse exactly two ASCII digits.
 *  @return Value 0..99, or -1 when either character is not a digit.
 */
static int two_digits(const char *s) {
    if(s[0]<'0'||s[0]>'9'||s[1]<'0'||s[1]>'9')return -1;
    return (s[0]-'0')*10+(s[1]-'0');
}
/* Shared date parts: both the HTTP date and the log stamp need the same
 * civil conversion and the same hh:mm:ss tail. */
/** @brief Split an epoch time into a civil date and seconds of the day.
 *  @param t    Seconds since the epoch.
 *  @param y    Receives the year.
 *  @param m    Receives the month.
 *  @param d    Receives the day.
 *  @param secs Receives seconds since midnight.
 *  @note Shared by the HTTP date and the log timestamp.
 */
static void date_parts(long t,int *y,int *m,int *d,long *secs) {
    long days=t/86400;*secs=t%86400;
    if(*secs<0){*secs+=86400;days--;}
    civil(days,y,m,d);
}
/** @brief Append @c hh:mm:ss.
 *  @param b    Target buffer.
 *  @param secs Seconds since midnight.
 */
static void hms(struct buffer *b,long secs) {
    num2(b,(int)(secs/3600));putc_b(b,':');
    num2(b,(int)(secs/60%60));putc_b(b,':');
    num2(b,(int)(secs%60));
}
/** @brief Append the three-letter English month name.
 *  @param b Target buffer.
 *  @param m Month, 1..12.
 */
static void mon(struct buffer *b,int m) {
    static const char *mn="JanFebMarAprMayJunJulAugSepOctNovDec";int k;
    for(k=0;k<3;k++)putc_b(b,mn[(m-1)*3+k]);
}
/** @brief Append an IMF-fixdate timestamp.
 *  @param b Target buffer.
 *  @param t Seconds since the epoch.
 *  @note Format is @c "Sun, 06 Nov 1994 08:49:37 GMT", as RFC 7231 requires
 *        for the Date and Last-Modified headers.
 */
static void http_date(struct buffer *b,long t) {
    static const char *dn="SunMonTueWedThuFriSat";
    int y,m,d,i,k;long secs;
    date_parts(t,&y,&m,&d,&secs);
    i=(int)((t/86400+4)%7);if(i<0)i+=7;
    for(k=0;k<3;k++)putc_b(b,dn[i*3+k]);
    puts_b(b,", ");num2(b,d);putc_b(b,' ');
    mon(b,m);putc_b(b,' ');number_b(b,(unsigned long)y);putc_b(b,' ');
    hms(b,secs);puts_b(b," GMT");
}
/* IMF-fixdate only: "Sun, 06 Nov 1994 08:49:37 GMT" */
/** @brief Parse an IMF-fixdate timestamp.
 *  @param s Header value, for example from If-Modified-Since.
 *  @return Seconds since the epoch, or -1 when not parseable.
 *  @note Only the IMF-fixdate form is accepted, which is what modern clients
 *        send and what http_date() produces.
 */
static long parse_date(const char *s) {
    int d,mo,h,mi,se,k;long y=0;
    if(strlen(s)<29)return -1;
    d=two_digits(s+5);mo=month3(s+8);
    for(k=0;k<4;k++){if(s[12+k]<'0'||s[12+k]>'9')return -1;y=y*10+(s[12+k]-'0');}
    h=two_digits(s+17);mi=two_digits(s+20);se=two_digits(s+23);
    if(d<1||mo<0||h<0||mi<0||se<0)return -1;
    return from_civil((int)y,mo,d)*86400+h*3600+mi*60+se;
}
/* Weak comparison is enough here: the tag is derived from mtime and size. */
/** @brief Compare an If-None-Match value against the file's tag.
 *  @param v     Header value, possibly a list or @c "*".
 *  @param mtime File modification time.
 *  @param fsize File size.
 *  @return Non-zero on a match.
 *  @note The tag is derived from mtime and size, so a weak comparison by
 *        substring is sufficient here.
 */
static int etag_match(const char *v,long mtime,long fsize) {
    char tag[48];struct buffer b={tag,0,(int)sizeof tag-1,0};
    putc_b(&b,'"');hexnum(&b,(unsigned long)mtime);putc_b(&b,'-');
    hexnum(&b,(unsigned long)fsize);putc_b(&b,'"');tag[b.length]=0;
    if(*v=='*')return 1;
    return strstr(v,tag)!=0;
}
/* A validator is an ETag when quoted (or "*"), otherwise an IMF-fixdate.
 * Weak comparison is enough: the tag is derived from mtime and size. */
/** @brief Match a cache validator, whichever form it takes.
 *  @param v     Validator value.
 *  @param mtime File modification time.
 *  @param fsize File size.
 *  @return Non-zero on a match.
 *  @note A quoted value or @c "*" is treated as an ETag, anything else as an
 *        HTTP date. Used by If-None-Match, If-Modified-Since and If-Range.
 */
static int valid_match(const char *v,long mtime,long fsize) {
    if(*v=='"'||*v=='W'||*v=='*')return etag_match(v,mtime,fsize);
    {long when=parse_date(v);return when>=0&&mtime<=when;}
}
/** @brief Format a 16-byte address as text.
 *  @param out Output buffer, at least 46 bytes.
 *  @param a   Address in network order.
 *  @note IPv4-mapped addresses print as dotted quad, everything else as
 *        eight hexadecimal groups.
 */
static void ip_str(char *out,const unsigned char *a) {
    static const char hx[]="0123456789abcdef";int i,k=0;
    if(!memcmp(a,"\0\0\0\0\0\0\0\0\0\0\xff\xff",12)) {
        for(i=12;i<16;i++){unsigned v=a[i];
            if(v>=100)out[k++]=(char)('0'+v/100);
            if(v>=10)out[k++]=(char)('0'+v/10%10);
            out[k++]=(char)('0'+v%10);
            if(i<15)out[k++]='.';}
        out[k]=0;return;
    }
    for(i=0;i<8;i++) {
        unsigned v=(unsigned)((a[i*2]<<8)|a[i*2+1]);
        if(i)out[k++]=':';
        out[k++]=hx[(v>>12)&15];out[k++]=hx[(v>>8)&15];
        out[k++]=hx[(v>>4)&15];out[k++]=hx[v&15];
    }
    out[k]=0;
}
/** @brief Append a Common Log Format timestamp.
 *  @param b Target buffer.
 *  @param t Seconds since the epoch.
 *  @note Format is @c "06/Nov/1994:08:49:37 +0000".
 */
static void clf_date(struct buffer *b,long t) {
    int y,m,d;long secs;
    date_parts(t,&y,&m,&d,&secs);
    num2(b,d);putc_b(b,'/');mon(b,m);putc_b(b,'/');
    number_b(b,(unsigned long)y);putc_b(b,':');
    hms(b,secs);puts_b(b," +0000");
}
/* Appends one Common Log Format line. When the file exceeds LOG_MAX it is
 * truncated and writing restarts from the beginning: no old copy is kept. */
/** @brief Append one access log entry.
 *  @param fd     Connection descriptor, for the peer address.
 *  @param code   Response status.
 *  @param length Response body length in bytes.
 *  @note Does nothing when logging is disabled. The file is truncated and
 *        restarted once it exceeds LOG_MAX; no previous copy is kept.
 */
static void log_line(int fd,int code,unsigned long length) {
    char line[384],ip[64];struct buffer b={line,0,(int)sizeof line-1,0};
    struct connection *c=&con[fd];
    if(log_fd<0)return;
    ip_str(ip,c->peer);
    puts_b(&b,ip);puts_b(&b," - - [");
    clf_date(&b,wall_sec());
    puts_b(&b,"] \"");
    if(c->o_method>=0)puts_b(&b,requests[fd]+c->o_method);else putc_b(&b,'-');
    putc_b(&b,' ');
    if(c->o_target>=0)puts_b(&b,requests[fd]+c->o_target);else putc_b(&b,'-');
    putc_b(&b,' ');
    puts_b(&b,c->http11?"HTTP/1.1":"HTTP/1.0");
    puts_b(&b,"\" ");number_b(&b,(unsigned long)code);
    putc_b(&b,' ');number_b(&b,length);putc_b(&b,'\n');
    if(b.failed)return;
    if(lseek(log_fd,0,SEEK_END)>LOG_MAX)ftruncate(log_fd,0);
    write(log_fd,line,(unsigned)b.length);
}
/** @brief Build a response header and queue it for sending.
 *  @param fd          Connection descriptor.
 *  @param code        HTTP status code.
 *  @param reason      Reason phrase, for example @c "OK".
 *  @param type        Content type.
 *  @param length      Body length in bytes.
 *  @param location    Optional Location value, used by 301. May be NULL.
 *  @param range_start First byte of a 206 or 416 body.
 *  @param range_end   Last byte of a 206 body.
 *  @param total       Full size of the representation for Content-Range.
 *  @return 0 on success, -1 when the header did not fit.
 *
 *  @note Emits Date always, Cache-Control for statuses below 400, and ETag,
 *        Last-Modified and Accept-Ranges when the response describes a file.
 *        A 304 is sent without Content-Type and without Content-Length, as
 *        RFC 7232 requires. The buffer is HDR_MAX bytes.
 */
static int header(int fd,int code,const char *reason,const char *type,
                  unsigned long length,const char *location,long range_start,
                  long range_end,long total) {
    struct buffer b={headers[fd],0,HDR_MAX,0};
    puts_b(&b,con[fd].http11?"HTTP/1.1 ":"HTTP/1.0 ");number_b(&b,(unsigned long)code);
    putc_b(&b,' ');puts_b(&b,reason);
    puts_b(&b,"\r\nServer: whitehttpd/" WHITEHTTPD_VERSION "\r\nConnection: ");
    puts_b(&b,con[fd].keep?"keep-alive":"close");
    if(code<400)puts_b(&b,"\r\nCache-Control: max-age=60");
    puts_b(&b,"\r\nDate: ");http_date(&b,wall_sec());
    if(con[fd].has_meta) {
        puts_b(&b,"\r\nAccept-Ranges: bytes");
        puts_b(&b,"\r\nETag: \"");hexnum(&b,(unsigned long)con[fd].mtime);
        putc_b(&b,'-');hexnum(&b,(unsigned long)con[fd].fsize);putc_b(&b,'"');
        puts_b(&b,"\r\nLast-Modified: ");http_date(&b,con[fd].mtime);
    }
    if(code!=304) {
        puts_b(&b,"\r\nContent-Type: ");puts_b(&b,type);
        puts_b(&b,"\r\nContent-Length: ");number_b(&b,length);
    }
    puts_b(&b,"\r\nX-Content-Type-Options: nosniff\r\n");
    if(allow_hdr){puts_b(&b,"Allow: ");puts_b(&b,allow_hdr);puts_b(&b,"\r\n");}
    if(location){puts_b(&b,"Location: ");puts_b(&b,location);puts_b(&b,"\r\n");}
    if(code==206||code==416) {
        puts_b(&b,"Content-Range: bytes ");
        if(code==416)putc_b(&b,'*');
        else {number_b(&b,(unsigned long)range_start);putc_b(&b,'-');number_b(&b,(unsigned long)range_end);}
        putc_b(&b,'/');number_b(&b,(unsigned long)total);puts_b(&b,"\r\n");
    }
    puts_b(&b,"\r\n");
    if(b.failed)return -1;
    log_line(fd,code,length);
    con[fd].hlen=b.length;con[fd].hoff=0;con[fd].state=HEADER;return 0;
}
/** @brief Build a small HTML error response.
 *  @param fd     Connection descriptor.
 *  @param code   HTTP status code.
 *  @param reason Reason phrase.
 *  @param total  Full size for Content-Range, 0 when not applicable.
 *  @return 1 when a response was queued, -1 on internal failure.
 *  @note The connection is marked not keep-alive, and HEAD sends no body.
 */
static int error_response(int fd,int code,const char *reason,long total) {
    char text[160];struct buffer b={text,0,(int)sizeof text,0};
    release_payload(fd);con[fd].keep=0;
    puts_b(&b,"<!doctype html><h1>");number_b(&b,(unsigned long)code);
    putc_b(&b,' ');puts_b(&b,reason);puts_b(&b,"</h1>\n");
    if(header(fd,code,reason,"text/html; charset=utf-8",(unsigned long)b.length,0,0,0,total)<0)return -1;
    if(!con[fd].head) {
        memcpy(headers[fd]+con[fd].hlen,text,(size_t)b.length);con[fd].hlen+=b.length;
    }
    return 1;
}
/* Resolve every component relative to root_fd. No pathname stat/open race and
 * no symlinks at intermediate OR final components. O_NONBLOCK also avoids FIFO
 * open hangs before the descriptor has been checked with fstat(). */
/** @brief Open a document without ever following a symbolic link.
 *  @param path Absolute path below the document root.
 *  @return Open descriptor, or -1 with errno set.
 *
 *  @note Every component is opened relative to the previous one, with
 *        O_NOFOLLOW each time. This is what closes the escape that only
 *        guarding the final component leaves open: a symlink to an
 *        intermediate directory used to walk out of the root. Hidden
 *        entries are refused, except the first component /.well-known,
 *        which ACME HTTP-01 needs.
 */
static int open_path(const char *path) {
    int current=root_fd,owned=0,first=1;const char *s=path;
    /* Hidden entries are never served, except the ACME challenge directory,
     * which HTTP-01 validation requires to be reachable. */
    int acme=!strncmp(path,"/.well-known/",13);
    while(*s=='/')s++;
    if(!*s)return openat(root_fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0);
    while(*s) {
        char name[256];int n=0,more,next;
        while(*s&&*s!='/') {if(n==255){if(owned)close(current);SET_ERRNO(ENAMETOOLONG);return -1;}name[n++]=*s++;}
        name[n]=0;while(*s=='/')s++;more=*s!=0;
        if(equal(name,"..")){if(owned)close(current);SET_ERRNO(EACCES);return -1;}
        if(equal(name,".")) {if(!more){next=openat(current,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0);if(owned)close(current);return next;}continue;}
        if(name[0]=='.'&&!(acme&&first&&equal(name,".well-known"))){if(owned)close(current);SET_ERRNO(EACCES);return -1;}
        first=0;
        next=openat(current,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|(more?O_DIRECTORY:0),0);
        if(owned)close(current);
        if(next<0)return -1;
        current=next;owned=1;
    }
    return current;
}
/** @brief Build an HTML directory listing.
 *  @param fd  Connection descriptor.
 *  @param dfd Open directory descriptor.
 *  @param path Request path, for the page title.
 *  @return 1 when a response was queued, -1 on failure.
 *  @note Names are HTML-escaped, so a file named after a script tag is inert in the
 *        listing. Output is capped at PAGE_MAX.
 */
static int directory_response(int fd,int dfd,const char *path) {
    int i,slot=-1;struct buffer b;char dents[4096];long n=0;
    for(i=0;i<PAGE_SLOTS;i++)if(!page_used[i]){slot=i;page_used[i]=1;break;}
    if(slot<0){close(dfd);return error_response(fd,503,"Service Unavailable",0);}
    con[fd].page=slot;b=(struct buffer){pages[slot],0,PAGE_MAX,0};
    puts_b(&b,"<!doctype html><meta charset=\"utf-8\"><title>Directory listing for ");html(&b,path);
    puts_b(&b,"</title><h1>Directory listing for ");html(&b,path);puts_b(&b,"</h1><hr><pre>\n");
    if(!equal(path,"/"))puts_b(&b,"<a href=\"../\">../</a>\n");
    while(!b.failed&&(n=getdents64(dfd,(struct linux_dirent64 *)dents,sizeof dents))>0) {
        long pos=0;
        while(pos<n) {
            struct linux_dirent64 *d=(struct linux_dirent64 *)(dents+pos);
            if(d->d_reclen<20 || d->d_reclen>n-pos){b.failed=1;break;}
            pos+=d->d_reclen;
            if(equal(d->d_name,".")||equal(d->d_name,".."))continue;
            puts_b(&b,"<a href=\"");url(&b,d->d_name,0);if(d->d_type==DT_DIR)putc_b(&b,'/');
            puts_b(&b,"\">");html(&b,d->d_name);if(d->d_type==DT_DIR)putc_b(&b,'/');puts_b(&b,"</a>\n");
        }
    }
    close(dfd);
    if(n<0)return error_response(fd,500,"Internal Server Error",0);
    puts_b(&b,"</pre>\n");
    if(b.failed)return error_response(fd,507,"Insufficient Storage",0);
    con[fd].mlen=b.length;con[fd].position=0;
    if(con[fd].head){release_payload(fd);con[fd].mlen=0;}
    if(header(fd,200,"OK","text/html; charset=utf-8",(unsigned long)b.length,0,0,0,0)<0)return -1;
    return 1;
}
/** @brief Parse an unsigned decimal number, detecting overflow.
 *  @param p     In/out pointer to the first character; advanced past the digits.
 *  @param value Receives the parsed value.
 *  @return 0 on success, -1 when absent or overflowing.
 *  @note The overflow check is what turns a huge Range value into a clean
 *        416 instead of a wrong byte count.
 */
static int decimal(const char **p,long *value) {
    const char *s=*p;unsigned long n=0,limit=(~0UL)>>1;
    if(*s<'0'||*s>'9')return -1;
    while(*s>='0'&&*s<='9') {
        unsigned int d=(unsigned int)(*s++-'0');
        if(n>(limit-d)/10)return -1;
        n=n*10+d;
    }
    *value=(long)n;*p=s;return 0;
}
/* Returns 0 for a syntactically valid but deliberately ignored multi-range. */
/** @brief Parse a single-range Range header.
 *  @param s     Header value, for example @c "bytes=0-499".
 *  @param size  Size of the representation.
 *  @param first Receives the first byte.
 *  @param last  Receives the last byte.
 *  @return 1 for a usable range, 0 for a valid multi-range that is
 *          deliberately ignored, -1 when unsatisfiable.
 *  @note Suffix form @c bytes=-500 is supported. A multi-range is answered
 *        with a full 200, which RFC 7233 section 3.1 permits.
 */
static int range_value(const char *s,long size,long *first,long *last) {
    long a,b=-1;
    if(strncmp(s,"bytes=",6)!=0)return 0;
    s+=6;
    if(strchr(s,','))return 0;
    if(*s=='-') {
        s++;if(decimal(&s,&a)<0||*s||a==0||size==0)return -1;
        *first=a>=size?0:size-a;*last=size-1;return 1;
    }
    if(decimal(&s,&a)<0||*s++!='-')return -1;
    if(*s&&decimal(&s,&b)<0)return -1;
    if(*s||a>=size||(b>=0&&b<a))return -1;
    *first=a;*last=b<0||b>=size?size-1:b;return 1;
}
/** @brief Parse one request and start its response.
 *  @param fd Connection descriptor.
 *  @return 1 when a response is queued, 0 when more data is needed, -1 on a
 *          fatal error.
 *
 *  @note This is the largest function in the file and the hot path of the
 *        server. It validates the request line, walks the header list,
 *        resolves the path, applies the cache validators, and starts the
 *        body. Every rejection path answers 400, and all connection state is
 *        reset here on entry.
 */
static int prepare(int fd) {
    struct connection *c=&con[fd];char *r=requests[fd],*end=0,*line,*method,*target,*version,*p;
    char path[URI_MAX],range[128];int i,host=0,cl=0,te=0,close_flag=0,keep_flag=0,have_range=0;
    const char *inm=0,*ims=0,*ifr=0;
    /* The buffer is NUL-terminated after every read, so strstr is safe here
     * and replaces a per-byte memcmp scan. */
    end=strstr(r,"\r\n\r\n");
    if(!end) {if(c->used==REQ_MAX-1)return error_response(fd,431,"Request Header Fields Too Large",0);return 0;}
    c->consumed=(int)(end-r)+4;
    for(i=0;i<c->consumed;i++)if((unsigned char)r[i]==0)return error_response(fd,400,"Bad Request",0);
    *end=0;c->head=0;c->keep=0;c->http11=0;c->has_meta=0;c->options=0;
    c->o_method=c->o_target=-1;allow_hdr=0;
    line=strstr(r,"\r\n");if(!line)line=end;
    *line=0;method=r;target=strchr(method,' ');
    if(!target)return error_response(fd,400,"Bad Request",0);
    *target++=0;
    c->head=equal(method,"HEAD");
    version=strchr(target,' ');if(!version)return error_response(fd,400,"Bad Request",0);*version++=0;
    c->o_method=(int)(method-r);c->o_target=(int)(target-r);
    if(equal(version,"HTTP/1.1"))c->http11=1;
    else if(!equal(version,"HTTP/1.0"))return error_response(fd,505,"HTTP Version Not Supported",0);
    if(equal(method,"OPTIONS"))c->options=1;
    else if(!equal(method,"GET")&&!c->head) {
        /* Known method, not allowed on a static resource: 405 + Allow. */
        if(equal(method,"POST")||equal(method,"PUT")||equal(method,"DELETE")||equal(method,"PATCH")) {
            allow_hdr=ALLOW_METHODS;
            return error_response(fd,405,"Method Not Allowed",0);
        }
        return error_response(fd,501,"Not Implemented",0);
    }
    if(c->options&&equal(target,"*"))path[0]='/',path[1]=0;
    else {
        int d=decode_path(path,target);
        if(d==-2)return error_response(fd,414,"URI Too Long",0);
        if(d<0)return error_response(fd,400,"Bad Request",0);
    }
    /* Request-target is ASCII URI syntax, not raw whitespace/control data. */
    for(p=target;*p;p++)if((unsigned char)*p<=32||(unsigned char)*p==127)return error_response(fd,400,"Bad Request",0);
    range[0]=0;
    p=line==end?end:line+2;
    while(p<end&&*p) {
        char *next=strstr(p,"\r\n"),*colon,*v,*tail;
        if(!next)next=end;
        *next=0;
        colon=strchr(p,':');if(!colon||colon==p)return error_response(fd,400,"Bad Request",0);
        *colon=0;
        for(v=p;*v;v++)if(!((*v>='a'&&*v<='z')||(*v>='A'&&*v<='Z')||(*v>='0'&&*v<='9')||strchr("!#$%&'*+-.^_`|~",*v)))return error_response(fd,400,"Bad Request",0);
        v=colon+1;while(*v==' '||*v=='\t')v++;tail=v+strlen(v);
        while(tail>v&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        for(tail=v;*tail;tail++)if(((unsigned char)*tail<32&&*tail!='\t')||(unsigned char)*tail==127)return error_response(fd,400,"Bad Request",0);
        if(iequal(p,"host")){if(++host>1||!*v)return error_response(fd,400,"Bad Request",0);}
        else if(iequal(p,"connection")){close_flag|=token(v,"close");keep_flag|=token(v,"keep-alive");}
        else if(iequal(p,"content-length")){if(cl++||!equal(v,"0"))return error_response(fd,400,"Bad Request",0);}
        else if(iequal(p,"transfer-encoding"))te=1;
        else if(iequal(p,"range")){if(have_range++||strlen(v)>=sizeof range)return error_response(fd,400,"Bad Request",0);strcpy(range,v);}
        else if(iequal(p,"if-none-match")){if(inm)return error_response(fd,400,"Bad Request",0);inm=v;}
        else if(iequal(p,"if-modified-since")){if(ims)return error_response(fd,400,"Bad Request",0);ims=v;}
        else if(iequal(p,"if-range")){if(ifr)return error_response(fd,400,"Bad Request",0);ifr=v;}
        p=next==end?end:next+2;
    }
    if(te||(c->http11&&host!=1))return error_response(fd,400,"Bad Request",0);
    c->keep=!close_flag&&(c->http11||keep_flag);
    if(c->options) {
        allow_hdr=ALLOW_METHODS;
        return header(fd,200,"OK","text/plain",0,0,0,0,0)<0?-1:1;
    }
    {
        int file=open_path(path);struct stat st;const char *type_path=path;
        if(file<0) {
            /* The same predicate picks both the code and the reason. */
            int denied=(errno==EACCES||errno==ELOOP||errno==ENOTDIR);
            return error_response(fd,denied?403:404,denied?"Forbidden":"Not Found",0);
        }
        if(fstat(file,&st)<0){close(file);return error_response(fd,500,"Internal Server Error",0);}
        if(S_ISDIR(st.st_mode)) {
            size_t len=strlen(path);int index;struct stat ist;
            if(path[len-1]!='/') {
                char location[URI_MAX*3+2];struct buffer b={location,0,(int)sizeof(location)-1,0};
                /* Always one leading slash: never emit a scheme-relative redirect. */
                const char *s=path;while(*s=='/')s++;putc_b(&b,'/');url(&b,s,1);putc_b(&b,'/');location[b.length]=0;close(file);
                return header(fd,301,"Moved Permanently","text/plain",0,location,0,0,0)<0?-1:1;
            }
            index=openat(file,index_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK,0);
            if(index>=0) {
                if(fstat(index,&ist)<0||!S_ISREG(ist.st_mode)){close(index);close(file);return error_response(fd,403,"Forbidden",0);}
                close(file);file=index;st=ist;type_path=index_name;
            } else if(errno==ENOENT) {
                if(no_listing){close(file);return error_response(fd,403,"Forbidden",0);}
                return directory_response(fd,file,path);
            }
            else {close(file);return error_response(fd,403,"Forbidden",0);}
        }
        if(!S_ISREG(st.st_mode)){close(file);return error_response(fd,403,"Forbidden",0);}
        if(st.st_size<0||(unsigned long long)st.st_size>((~0UL)>>1)){close(file);return error_response(fd,413,"Content Too Large",0);}
        c->file=file;c->position=0;c->end=(long)st.st_size;
        c->has_meta=1;c->mtime=(long)st.st_mtime;c->fsize=(long)st.st_size;
        /* Conditional GET: 304 keeps the connection open and sends no body.
         * If-None-Match takes precedence over If-Modified-Since. */
        if((inm&&valid_match(inm,c->mtime,c->fsize))||(ims&&!inm&&valid_match(ims,c->mtime,c->fsize))) {
            close(file);c->file=-1;
            return header(fd,304,"Not Modified","text/plain",0,0,0,0,0)<0?-1:1;
        }
        /* If-Range: a stale validator downgrades the request to a full 200. */
        if(ifr&&have_range&&!valid_match(ifr,c->mtime,c->fsize))have_range=0;
        if(have_range&&!c->head) {
            long first,last;int result=range_value(range,c->end,&first,&last);
            if(result<0)return error_response(fd,416,"Range Not Satisfiable",c->end);
            if(result>0) {
                c->position=first;c->end=last+1;
                return header(fd,206,"Partial Content",mime(type_path),(unsigned long)(last-first+1),0,first,last,(long)st.st_size)<0?-1:1;
            }
        }
        if(c->head){close(c->file);c->file=-1;}
        return header(fd,200,"OK",mime(type_path),(unsigned long)st.st_size,0,0,0,0)<0?-1:1;
    }
}
/** @brief End one response and decide whether to keep the connection.
 *  @param fd Connection descriptor.
 *  @return -1 to close, 0 to keep waiting, 1 when a pipelined request follows.
 */
static int finish(int fd) {
    struct connection *c=&con[fd];int rest;
    release_payload(fd);if(!c->keep)return -1;
    rest=c->used-c->consumed;if(rest<0)return -1;
    memmove(requests[fd],requests[fd]+c->consumed,(size_t)rest);c->used=rest;requests[fd][rest]=0;
    c->consumed=0;c->state=rest?READING:KEEPALIVE;c->head=0;c->progress=c->started=now_sec();
    return rest?prepare(fd):0;
}
/** @brief Advance the response state machine.
 *  @param fd Connection descriptor.
 *  @return -1 to close, 0 when the socket is not ready for more.
 *  @note Sends the header, then the body by sendfile or from a page slot.
 *        EAGAIN returns 0 so the loop can serve other connections; the
 *        position is kept and sending resumes on the next EPOLLOUT.
 */
static int pump(int fd) {
    struct connection *c=&con[fd];int budget=16;
    while(budget--) {
        long n;
        if(c->state==HEADER) {
            if(c->hoff<c->hlen) {
                n=write(fd,headers[fd]+c->hoff,(unsigned)(c->hlen-c->hoff));
                if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
                if(n<0&&errno==EINTR)continue;
                if(n<=0)return -1;
                c->hoff+=(int)n;c->progress=now_sec();continue;
            }
            if(c->file>=0)c->state=FILE_BODY;
            else if(c->page>=0)c->state=MEMORY_BODY;
            else {int r=finish(fd);if(r<0)return -1;if(!r)return 0;continue;}
        }
        if(c->state==FILE_BODY||c->state==MEMORY_BODY) {
            if((c->state==FILE_BODY&&c->position>=c->end)||(c->state==MEMORY_BODY&&c->position>=c->mlen)) {
                int r=finish(fd);if(r<0)return -1;if(!r)return 0;continue;
            }
            if(c->state==FILE_BODY) {
                off_t offset=(off_t)c->position;long count=c->end-c->position;if(count>1048576)count=1048576;
                n=sendfile(fd,c->file,&offset,(size_t)count);
            } else n=write(fd,pages[c->page]+c->position,(size_t)(c->mlen-c->position));
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
            if(n<0&&errno==EINTR)continue;
                if(n<=0)return -1;
            c->position+=n;c->progress=now_sec();
        } else if(c->state!=HEADER)return 0;
    }
    return 0;
}
/** @brief Parse a command-line number with a range limit.
 *  @param s   Text to parse.
 *  @param max Largest accepted value.
 *  @return Value, or -1 when empty, non-numeric or out of range.
 */
static int uint_arg(const char *s,int max) {
    int n=0;if(!*s)return -1;
    while(*s){if(*s<'0'||*s>'9'||n>(max-(*s-'0'))/10)return -1;n=n*10+(*s++-'0');}
    return n;
}
/** @brief Parse a dotted-quad IPv4 literal.
 *  @param s   Text to parse.
 *  @param out Receives four bytes.
 *  @return 0 on success, -1 on malformed input.
 */
static int ipv4(const char *s,unsigned char out[4]) {
    int i;
    for(i=0;i<4;i++) {
        int v=0,n=0;while(*s>='0'&&*s<='9'){if(++n>3)return -1;v=v*10+(*s++-'0');}
        if(!n||v>255)return -1;
        out[i]=(unsigned char)v;
        if(i<3){if(*s++!='.')return -1;}else if(*s)return -1;
    }
    return 0;
}
/** @brief Parse an IPv4 or IPv6 literal into 16 bytes.
 *  @param s   Text to parse.
 *  @param out Receives the address in network order.
 *  @return 0 on success, -1 on malformed input.
 *  @note IPv4 input becomes an IPv4-mapped IPv6 address, so one socket can
 *        serve both families.
 */
static int address(const char *s,unsigned char out[16]) {
    unsigned char tmp[16]={0},v4[4];int used=0,gap=-1;
    if(ipv4(s,v4)==0){memset(out,0,16);out[10]=out[11]=255;memcpy(out+12,v4,4);return 0;}
    if(*s==':'){if(s[1]!=':')return -1;gap=0;s+=2;}
    while(*s) {
        int v=0,n=0,h;
        while((h=hex(*s))>=0){if(++n>4)return -1;v=v*16+h;s++;}
        if(!n||used>14)return -1;
        tmp[used++]=(unsigned char)(v>>8);tmp[used++]=(unsigned char)v;
        if(!*s)break;
        if(*s++!=':')return -1;
        if(*s==':'){if(gap>=0)return -1;gap=used;s++;if(!*s)break;}
        else if(!*s)return -1;
    }
    if(gap>=0){int tail=used-gap;if(used==16)return -1;memmove(tmp+16-tail,tmp+gap,(size_t)tail);memset(tmp+gap,0,(size_t)(16-used));}
    else if(used!=16)return -1;
    memcpy(out,tmp,16);return 0;
}
/** @brief Write a string to a descriptor, ignoring the result.
 *  @param fd Target descriptor.
 *  @param s  Text to write.
 */
static void emit(int fd,const char *s) { write(fd,s,strlen(s)); }
/** @brief Print author, license and project URL.
 *  @note Shared by --help and --version, so the strings exist once.
 */
static void show_footer(void) {
    emit(1,"Author:  " WHITEHTTPD_AUTHOR "\n"
           "License: " WHITEHTTPD_LICENSE " (GNU AGPL v3 or later)\n"
           "Project: " WHITEHTTPD_URL "\n");
}
/** @brief Print the version banner and the shared footer.
 */
static void show_version(void) {
    emit(1,"whitehttpd " WHITEHTTPD_VERSION "\n"
           "Freestanding C static file server, no libc.\n");
    show_footer();
}
/** @brief Print usage, options, behaviour, limits, examples and footer.
 */
static void show_help(void) {
    emit(1,
"whitehttpd " WHITEHTTPD_VERSION " - static file server without libc\n"
"\n"
"Usage:\n"
"  whitehttpd [PATH] [PORT] [OPTIONS]\n"
"\n"
"Arguments:\n"
"  PATH                 document root (default: current directory)\n"
"  PORT                 listen port, same as --port (default: 8080)\n"
"\n"
"Options:\n"
"  -h, --help           show this help and exit\n"
"  -v, --version        show version, author and license, then exit\n"
"      --port N         listen port, 1..65535\n"
"      --addr ADDR      bind address, IPv4 or IPv6 (default: ::, dual-stack)\n"
"      --workers N      prefork workers, 1..16 (default: 4)\n"
"      --index NAME     directory index file (default: index.html)\n"
"      --no-listing     return 403 instead of a directory listing\n"
"      --daemon         detach from the terminal\n"
"      --pidfile FILE   write the master PID to FILE\n"
"      --log FILE       access and error log, Common Log Format\n"
"                       overwritten when it exceeds 256 KiB\n"
"\n"
"Behaviour:\n"
"  Methods GET, HEAD and OPTIONS. HTTP/1.0 and HTTP/1.1, keep-alive,\n"
"  pipelining, single Range with 206 and 416, ETag, Last-Modified,\n"
"  If-None-Match, If-Modified-Since, If-Range, Cache-Control.\n"
"  Hidden files (leading dot) return 403, except /.well-known/ for ACME.\n"
"\n"
"Not implemented: TLS, CGI, WebSocket, HTTP/2, gzip, authentication,\n"
"chroot, configuration file, multiple ranges.\n"
"\n"
"Examples:\n"
"  whitehttpd /var/www\n"
"      serve on port 8080 with four workers\n"
"  whitehttpd /var/www --port 80 --daemon --pidfile /run/w.pid\n"
"      production: port 80, detached, PID for init scripts\n"
"  whitehttpd /srv --addr 127.0.0.1 --workers 1 --log /tmp/w.log\n"
"      local only, single worker, access and error log\n"
"  whitehttpd /srv --index home.html --no-listing\n"
"      custom index, directory listing disabled\n");
    emit(1,"\n");
    show_footer();
}
/* Error channel: stderr always, plus the log file when --log is active.
 * With --daemon stderr goes to /dev/null, so the file copy is what survives. */
/** @brief Append an error line to the log file.
 *  @param s Message, with its own trailing newline.
 *  @note Write errors are ignored on purpose: losing a log line must never
 *        take the server down.
 */
static void log_note(const char *s) {
    if(log_fd<0)return;
    {char line[256];struct buffer b={line,0,(int)sizeof line-1,0};
     clf_date(&b,wall_sec());puts_b(&b," [error] ");puts_b(&b,s);
     if(!b.failed)write(log_fd,line,(unsigned)b.length);}
}
/** @brief Report a fatal error to stderr and the log, then return 2.
 *  @param s Message with its own trailing newline.
 *  @return Always 2, so callers can `return log_error(...)`.
 */
static int log_error(const char *s) {write(2,s,strlen(s));log_note(s);return 2;}
/** @brief Parse arguments, open the root, start workers, run the loop.
 *  @param argc Argument count.
 *  @param argv Argument vector.
 *  @return Process exit status; the loop itself does not return.
 */
int main(int argc,char **argv) {
    const char *root=".",*bind_address="::",*log_path=0,*pid_path=0;int port=8080,workers=4,daemon=0,root_set=0,i,listen_fd,ep;
    struct ws_sockaddr_in6 addr;struct ws_epoll_event ev,events[64];int one=1,zero=0;
    for(i=1;i<argc;i++) {
        if(equal(argv[i],"--version")||equal(argv[i],"-v")){show_version();return 0;}
        if(equal(argv[i],"--help")||equal(argv[i],"-h")){show_help();return 0;}
        if(equal(argv[i],"--log")){if(++i==argc)return log_error("missing option value\n");log_path=argv[i];continue;}
        if(equal(argv[i],"--index")){if(++i==argc)return log_error("missing option value\n");index_name=argv[i];continue;}
        if(equal(argv[i],"--pidfile")){if(++i==argc)return log_error("missing option value\n");pid_path=argv[i];continue;}
        if(equal(argv[i],"--no-listing")){no_listing=1;continue;}
        if(equal(argv[i],"--daemon")){daemon=1;continue;}
        if(equal(argv[i],"--port")||equal(argv[i],"--workers")||equal(argv[i],"--addr")) {
            const char *flag=argv[i];int v;if(++i==argc)return log_error("missing option value\n");
            if(equal(flag,"--addr")){bind_address=argv[i];continue;}
            v=uint_arg(argv[i],equal(flag,"--port")?65535:MAX_WORKERS);
            if(v<=0)return log_error("invalid numeric option\n");
            if(equal(flag,"--port"))port=v;else workers=v;continue;
        }
        if(argv[i][0]=='-')return log_error("unknown option\n");
        if(!root_set){root=argv[i];root_set=1;}
        else {int v=uint_arg(argv[i],65535);if(v<=0)return log_error("unexpected argument\n");port=v;}
    }
    memset(&addr,0,sizeof addr);addr.sin6_family=WS_AF_INET6;
    {unsigned short p=(unsigned short)port;unsigned char *b=(unsigned char *)&addr.sin6_port;b[0]=(unsigned char)(p>>8);b[1]=(unsigned char)p;}
    if(address(bind_address,addr.sin6_addr.b)<0)return log_error("invalid bind address\n");
    root_fd=open(root,O_RDONLY|O_DIRECTORY,0);if(root_fd<0)return log_error("cannot open document root\n");
    if(!now_sec())return log_error("CLOCK_MONOTONIC unavailable\n");
    if(ws_ignore_signal(WS_SIGPIPE)<0||ws_ignore_signal(WS_SIGCHLD)<0)return 3;
    listen_fd=ws_socket(WS_AF_INET6,WS_SOCK_STREAM|WS_SOCK_NONBLOCK,0);if(listen_fd<0)return 4;
    if(ws_setsockopt(listen_fd,WS_IPPROTO_IPV6,WS_IPV6_V6ONLY,&zero,sizeof zero)<0||
       ws_setsockopt(listen_fd,WS_SOL_SOCKET,WS_SO_REUSEADDR,&one,sizeof one)<0)return 4;
    if(ws_bind(listen_fd,&addr,sizeof addr)<0||ws_listen(listen_fd,128)<0)return 5;
    if(log_path) {
        log_fd=open(log_path,O_WRONLY|O_CREAT|O_APPEND,0644);
        if(log_fd<0)return log_error("cannot open log file\n");
        log_note("starting whitehttpd " WHITEHTTPD_VERSION "\n");
    }
    if(daemon) {
        long pid=fork();if(pid<0)return 6;if(pid>0)return 0;
        if(ws_syscall0(__NR_setsid)<0)return 6;
        {int nullfd=open("/dev/null",O_RDWR,0);if(nullfd<0)return 6;for(i=0;i<3;i++)if(ws_syscall2(__NR_dup2,nullfd,i)<0)return 6;if(nullfd>2)close(nullfd);}
    }
    if(pid_path) {
        char line[32];struct buffer b={line,0,(int)sizeof line-2,0};
        long pid=ws_syscall0(__NR_getpid);
        int pf;
        if(pid<0)return log_error("getpid failed\n");
        number_b(&b,(unsigned long)pid);putc_b(&b,'\n');
        pf=open(pid_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(pf<0)return log_error("cannot write pidfile\n");
        if(write(pf,line,(unsigned)b.length)<0){close(pf);return log_error("cannot write pidfile\n");}
        close(pf);
    }
    for(i=1;i<workers;i++){long p=fork();if(p<0)return 6;if(!p){prctl(PR_SET_PDEATHSIG,SIGTERM,0,0,0);break;}}
    ep=ws_epoll_create1(0);if(ep<0)return 7;
    ev.events=WS_EPOLLIN;ev.data=(unsigned long long)listen_fd;
    if(ws_epoll_ctl(ep,WS_EPOLL_CTL_ADD,listen_fd,&ev)<0)return 8;
    for(;;) {
        time_t now;int n=ws_epoll_wait(ep,events,64,1000);
        if(n<0){if(errno==EINTR)continue;return 9;}
        now=now_sec();
        for(i=0;i<MAXFD;i++)if(con[i].state!=FREE) {
            int expire=(con[i].state==KEEPALIVE)?KEEPALIVE_SEC:IDLE_SEC;
            if(now-con[i].progress>=expire||(con[i].state==READING&&now-con[i].started>=REQUEST_SEC))close_connection(ep,i);
        }
        for(i=0;i<n;i++) {
            int fd=(int)events[i].data;
            if(fd==listen_fd) {
                for(;;) {
                    struct ws_sockaddr_in6 sa;int slen=(int)sizeof sa;
                    int c=ws_accept4(listen_fd,&sa,&slen,WS_SOCK_NONBLOCK);
                    if(c<0){if(errno==EINTR)continue;break;}
                    if(c>=MAXFD){close(c);continue;}
                    memset(&con[c],0,sizeof con[c]);con[c].state=READING;con[c].file=con[c].page=-1;
                    memcpy(con[c].peer,sa.sin6_addr.b,16);con[c].o_method=con[c].o_target=-1;
                    con[c].progress=con[c].started=now_sec();
                    ws_setsockopt(c,WS_IPPROTO_TCP,WS_TCP_NODELAY,&one,sizeof one);
                    ev.events=WS_EPOLLIN;ev.data=(unsigned long long)c;
                    if(ws_epoll_ctl(ep,WS_EPOLL_CTL_ADD,c,&ev)<0){close(c);con[c].state=FREE;}
                }
                continue;
            }
            if(fd<0||fd>=MAXFD||con[fd].state==FREE)continue;
            if(events[i].events&WS_EPOLLERR){close_connection(ep,fd);continue;}
            if((events[i].events&WS_EPOLLIN)&&(con[fd].state==READING||con[fd].state==KEEPALIVE)) {
                long count;
                if(con[fd].state==KEEPALIVE){con[fd].state=READING;con[fd].started=now_sec();}
                count=read(fd,requests[fd]+con[fd].used,(unsigned)(REQ_MAX-1-con[fd].used));
                if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
                if(count<=0){close_connection(ep,fd);continue;}
                con[fd].used+=(int)count;requests[fd][con[fd].used]=0;con[fd].progress=now_sec();
                if(prepare(fd)<0){close_connection(ep,fd);continue;}
            }
            if(con[fd].state==HEADER||con[fd].state==FILE_BODY||con[fd].state==MEMORY_BODY) {
                if(pump(fd)<0){close_connection(ep,fd);continue;}
            } else if((events[i].events&WS_EPOLLHUP)&&!(events[i].events&WS_EPOLLIN)){close_connection(ep,fd);continue;}
            ev.events=(con[fd].state==READING||con[fd].state==KEEPALIVE)?WS_EPOLLIN:WS_EPOLLOUT;
            ev.data=(unsigned long long)fd;
            if(ws_epoll_ctl(ep,WS_EPOLL_CTL_MOD,fd,&ev)<0)close_connection(ep,fd);
        }
    }
}
