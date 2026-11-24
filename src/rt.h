/* rt.h: the small runtime the MaxEco tray uses instead of a C library.
 *
 * Default build (x86_64): no libc at all, raw Linux system calls, which keeps
 * the resident memory of the tray at a few dozen kilobytes.
 * Build with -DMAXECO_LIBC (make LIBC=1) on any other architecture: the same
 * functions then call the system C library.
 *
 * Every function returns a negative errno value on failure, like the kernel.
 */
#ifndef MAXECO_RT_H
#define MAXECO_RT_H

typedef unsigned long rt_size;
typedef long rt_ssize;

#define RT_AT_FDCWD (-100)
#define RT_O_RDONLY 0
#define RT_O_WRONLY 1
#define RT_O_CREAT 0100
#define RT_O_TRUNC 01000
#define RT_O_CLOEXEC 02000000
#define RT_AF_UNIX 1
#define RT_SOCK_STREAM 1
#define RT_SOCK_CLOEXEC 02000000
#define RT_MSG_DONTWAIT 0x40
#define RT_MSG_NOSIGNAL 0x4000
#define RT_POLLIN 0x001
#define RT_POLLERR 0x008
#define RT_POLLHUP 0x010
#define RT_WNOHANG 1
#define RT_LOCK_EX 2
#define RT_LOCK_NB 4
#define RT_SFD_NONBLOCK 04000
#define RT_SFD_CLOEXEC 02000000
#define RT_SIG_BLOCK 0
#define RT_SIG_SETMASK 2
#define RT_EINTR 4
#define RT_EAGAIN 11
#define RT_EEXIST 17
#define RT_SIGHUP 1
#define RT_SIGINT 2
#define RT_SIGPIPE 13
#define RT_SIGTERM 15
#define RT_SIGCHLD 17
#define RT_SIGKILL 9
#define RT_PROT_RW 3                /* PROT_READ | PROT_WRITE */
#define RT_MAP_ANON_PRIVATE 0x22    /* MAP_PRIVATE | MAP_ANONYMOUS */

struct rt_pollfd { int fd; short events, revents; };
struct rt_timespec { long tv_sec, tv_nsec; };
struct rt_sockaddr_un { unsigned short sun_family; char sun_path[108]; };
/* struct signalfd_siginfo is 128 bytes; only the signal number is used */
struct rt_sigfd_info { unsigned int signo; unsigned char rest[124]; };

static char **rt_envp;

#ifndef MAXECO_LIBC
/* ------------------------------------------------------------------ raw */
#if !defined(__x86_64__)
#error "the libc free build supports x86_64 only, build with: make LIBC=1"
#endif
#include <asm/unistd.h>

static inline long rt_sc(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define RT_SC0(n) rt_sc(n, 0, 0, 0, 0, 0, 0)
#define RT_SC1(n, a) rt_sc(n, (long)(a), 0, 0, 0, 0, 0)
#define RT_SC2(n, a, b) rt_sc(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define RT_SC3(n, a, b, c) rt_sc(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define RT_SC4(n, a, b, c, d) rt_sc(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define RT_SC5(n, a, b, c, d, e) rt_sc(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define RT_SC6(n, a, b, c, d, e, f) \
    rt_sc(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* gcc may emit calls to these four even in freestanding code */
void *memcpy(void *d, const void *s, rt_size n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, rt_size n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b) {
        while (n--) *a++ = *b++;
    } else {
        while (n--) a[n] = b[n];
    }
    return d;
}
void *memset(void *d, int c, rt_size n)
{
    unsigned char *a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void *x, const void *y, rt_size n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++)
        if (*a != *b) return *a - *b;
    return 0;
}

static __attribute__((noreturn)) void rt_exit(int code)
{
    for (;;) RT_SC1(__NR_exit_group, code);
}

int main(int argc, char **argv);

__attribute__((used, noreturn)) void rt_start_c(long *sp);
__attribute__((used, noreturn)) void rt_start_c(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    rt_envp = argv + argc + 1;
    rt_exit(main(argc, argv));
}

__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "  xor %rbp, %rbp\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call rt_start_c\n"
        "  hlt\n");

static rt_ssize rt_read(int fd, void *b, rt_size n) { return RT_SC3(__NR_read, fd, b, n); }
static rt_ssize rt_write(int fd, const void *b, rt_size n) { return RT_SC3(__NR_write, fd, b, n); }
static int rt_close(int fd) { return (int)RT_SC1(__NR_close, fd); }
static int rt_open(const char *p, int fl, int mode) { return (int)RT_SC4(__NR_openat, RT_AT_FDCWD, p, fl, mode); }
static int rt_mkdir(const char *p, int mode) { return (int)RT_SC3(__NR_mkdirat, RT_AT_FDCWD, p, mode); }
static int rt_unlink(const char *p) { return (int)RT_SC3(__NR_unlinkat, RT_AT_FDCWD, p, 0); }
static int rt_rename(const char *a, const char *b)
{
    return (int)RT_SC5(__NR_renameat2, RT_AT_FDCWD, a, RT_AT_FDCWD, b, 0);
}
static int rt_exists(const char *p) { return RT_SC3(__NR_faccessat, RT_AT_FDCWD, p, 0) == 0; }
static rt_ssize rt_readlink(const char *p, char *b, rt_size n)
{
    return RT_SC4(__NR_readlinkat, RT_AT_FDCWD, p, b, n);
}
static int rt_flock(int fd, int op) { return (int)RT_SC2(__NR_flock, fd, op); }
static unsigned rt_getuid(void) { return (unsigned)RT_SC0(__NR_getuid); }
static int rt_getpid(void) { return (int)RT_SC0(__NR_getpid); }
static int rt_socket_unix(void) { return (int)RT_SC3(__NR_socket, RT_AF_UNIX, RT_SOCK_STREAM | RT_SOCK_CLOEXEC, 0); }
static int rt_connect(int fd, const struct rt_sockaddr_un *a, int len) { return (int)RT_SC3(__NR_connect, fd, a, len); }
static rt_ssize rt_send(int fd, const void *b, rt_size n) { return RT_SC6(__NR_sendto, fd, b, n, RT_MSG_NOSIGNAL, 0, 0); }
static rt_ssize rt_recv(int fd, void *b, rt_size n, int fl) { return RT_SC6(__NR_recvfrom, fd, b, n, fl, 0, 0); }
static int rt_poll(struct rt_pollfd *f, int n, const struct rt_timespec *t)
{
    return (int)RT_SC5(__NR_ppoll, f, n, t, 0, 8);
}
static int rt_fork(void) { return (int)RT_SC0(__NR_fork); }
static int rt_execve(const char *p, char *const *argv, char *const *envp) { return (int)RT_SC3(__NR_execve, p, argv, envp); }
static int rt_wait(int pid, int *status, int fl) { return (int)RT_SC4(__NR_wait4, pid, status, fl, 0); }
static int rt_sigmask(int how, const unsigned long *set) { return (int)RT_SC4(__NR_rt_sigprocmask, how, set, 0, 8); }
static int rt_signalfd(const unsigned long *set) { return (int)RT_SC4(__NR_signalfd4, -1, set, 8, RT_SFD_NONBLOCK | RT_SFD_CLOEXEC); }
static int rt_pipe(int fd[2]) { return (int)RT_SC2(__NR_pipe2, fd, RT_O_CLOEXEC); }
static int rt_dup2(int a, int b) { return (int)RT_SC2(__NR_dup2, a, b); }
static int rt_kill(int pid, int sig) { return (int)RT_SC2(__NR_kill, pid, sig); }
/* anonymous memory that goes back to the kernel on rt_unmap, so a big
 * temporary buffer does not stay in the resident size */
static void *rt_map(rt_size n)
{
    long r = RT_SC6(__NR_mmap, 0, n, RT_PROT_RW, RT_MAP_ANON_PRIVATE, -1, 0);
    return (unsigned long)r > -4096UL ? 0 : (void *)r;
}
static void rt_unmap(void *p, rt_size n) { RT_SC2(__NR_munmap, p, n); }

#else
/* ------------------------------------------------------------------ libc */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static long rt_err(long r) { return r < 0 ? -errno : r; }
#define RT_ERR(x) rt_err((long)(x))
static __attribute__((noreturn)) void rt_exit(int code) { _exit(code); }
static rt_ssize rt_read(int fd, void *b, rt_size n) { return RT_ERR(read(fd, b, n)); }
static rt_ssize rt_write(int fd, const void *b, rt_size n) { return RT_ERR(write(fd, b, n)); }
static int rt_close(int fd) { return RT_ERR(close(fd)); }
static int rt_open(const char *p, int fl, int mode) { return RT_ERR(open(p, fl, mode)); }
static int rt_mkdir(const char *p, int mode) { return RT_ERR(mkdir(p, (mode_t)mode)); }
static int rt_unlink(const char *p) { return RT_ERR(unlink(p)); }
static int rt_rename(const char *a, const char *b) { return RT_ERR(rename(a, b)); }
static int rt_exists(const char *p) { return access(p, F_OK) == 0; }
static rt_ssize rt_readlink(const char *p, char *b, rt_size n) { return RT_ERR(readlink(p, b, n)); }
static int rt_flock(int fd, int op) { return RT_ERR(flock(fd, op)); }
static unsigned rt_getuid(void) { return (unsigned)getuid(); }
static int rt_getpid(void) { return (int)getpid(); }
static int rt_socket_unix(void) { return RT_ERR(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)); }
static int rt_connect(int fd, const struct rt_sockaddr_un *a, int len)
{
    return RT_ERR(connect(fd, (const struct sockaddr *)a, (socklen_t)len));
}
static rt_ssize rt_send(int fd, const void *b, rt_size n) { return RT_ERR(send(fd, b, n, MSG_NOSIGNAL)); }
static rt_ssize rt_recv(int fd, void *b, rt_size n, int fl) { return RT_ERR(recv(fd, b, n, fl)); }
static int rt_poll(struct rt_pollfd *f, int n, const struct rt_timespec *t)
{
    return RT_ERR(ppoll((struct pollfd *)f, (nfds_t)n, (const struct timespec *)t, NULL));
}
static int rt_fork(void) { return RT_ERR(fork()); }
static int rt_execve(const char *p, char *const *argv, char *const *envp) { return RT_ERR(execve(p, argv, envp)); }
static int rt_wait(int pid, int *status, int fl) { return RT_ERR(waitpid(pid, status, fl)); }
static int rt_sigmask(int how, const unsigned long *set)
{
    sigset_t s;
    sigemptyset(&s);
    for (int i = 1; i < 64; i++)
        if (*set & (1UL << (i - 1))) sigaddset(&s, i);
    return RT_ERR(sigprocmask(how, &s, NULL));
}
static int rt_signalfd(const unsigned long *set)
{
    sigset_t s;
    sigemptyset(&s);
    for (int i = 1; i < 64; i++)
        if (*set & (1UL << (i - 1))) sigaddset(&s, i);
    return RT_ERR(signalfd(-1, &s, SFD_NONBLOCK | SFD_CLOEXEC));
}
static int rt_pipe(int fd[2]) { return RT_ERR(pipe2(fd, O_CLOEXEC)); }
static int rt_dup2(int a, int b) { return RT_ERR(dup2(a, b)); }
static int rt_kill(int pid, int sig) { return RT_ERR(kill(pid, sig)); }
static void *rt_map(rt_size n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
static void rt_unmap(void *p, rt_size n) { munmap(p, n); }
extern char **environ;
int maxeco_main(int argc, char **argv);
int main(int argc, char **argv)
{
    rt_envp = environ;
    return maxeco_main(argc, argv);
}
#define main maxeco_main
#endif

/* ------------------------------------------------------------- strings */
static rt_size rt_strlen(const char *s)
{
    rt_size n = 0;
    while (s[n]) n++;
    return n;
}
static int rt_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
static int rt_startswith(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++) return 0;
    return 1;
}
static const char *rt_getenv(const char *name)
{
    rt_size n = rt_strlen(name);
    for (char **e = rt_envp; e && *e; e++)
        if (rt_startswith(*e, name) && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

/* Bounded string builder: never overflows, sets ok = 0 when truncated. */
struct rt_sb { char *b; rt_size cap, len; int ok; };
static void sb_init(struct rt_sb *s, char *b, rt_size cap)
{
    s->b = b, s->cap = cap, s->len = 0, s->ok = 1;
    b[0] = 0;
}
static void sb_add(struct rt_sb *s, const char *t)
{
    while (*t) {
        if (s->len + 1 >= s->cap) {
            s->ok = 0;
            break;
        }
        s->b[s->len++] = *t++;
    }
    s->b[s->len] = 0;
}
static void sb_num(struct rt_sb *s, unsigned long v)
{
    char t[24];
    int i = 23;
    t[i] = 0;
    do t[--i] = (char)('0' + v % 10); while ((v /= 10) && i);
    sb_add(s, t + i);
}

static int rt_write_all(int fd, const void *b, rt_size n)
{
    const char *p = b;
    while (n) {
        rt_ssize r = rt_write(fd, p, n);
        if (r == -RT_EINTR) continue;
        if (r <= 0) return -1;
        p += r, n -= (rt_size)r;
    }
    return 0;
}
static void rt_log(const char *a, const char *b)
{
    rt_write_all(2, "maxeco: ", 8);
    rt_write_all(2, a, rt_strlen(a));
    if (b) rt_write_all(2, b, rt_strlen(b));
    rt_write_all(2, "\n", 1);
}

#endif
