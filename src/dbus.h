/* dbus.h: a minimal D-Bus client for the MaxEco tray.
 *
 * Wire protocol version 1, little endian, unix stream sockets, EXTERNAL
 * authentication. It covers what a StatusNotifierItem needs and nothing more:
 * sending calls, replies, errors and signals, reading incoming messages with
 * full bounds checking, and skipping any value it does not care about.
 */
#ifndef MAXECO_DBUS_H
#define MAXECO_DBUS_H

#include "rt.h"

#define DB_CALL 1
#define DB_RETURN 2
#define DB_ERROR 3
#define DB_SIGNAL 4
#define DB_NO_REPLY 0x1
#define DB_IN_BUF 16384
#define DB_MAX_DEPTH 32

/* ------------------------------------------------------------- writer */
struct db_w { unsigned char *b; unsigned cap, len; int err; };

static void dw_init(struct db_w *w, unsigned char *b, unsigned cap)
{
    w->b = b, w->cap = cap, w->len = 0, w->err = 0;
}
static void dw_put(struct db_w *w, const void *p, unsigned n)
{
    if (w->err || n > w->cap - w->len) {
        w->err = 1;
        return;
    }
    memcpy(w->b + w->len, p, n);
    w->len += n;
}
static void dw_pad(struct db_w *w, unsigned a)
{
    static const unsigned char zero[8];
    unsigned p = (w->len + a - 1) & ~(a - 1);
    dw_put(w, zero, p - w->len);
}
static void dw_u8(struct db_w *w, unsigned v)
{
    unsigned char c = (unsigned char)v;
    dw_put(w, &c, 1);
}
static void dw_u32(struct db_w *w, unsigned v)
{
    unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    dw_pad(w, 4);
    dw_put(w, c, 4);
}
static void dw_str(struct db_w *w, const char *s)
{
    unsigned n = (unsigned)rt_strlen(s);
    dw_u32(w, n);
    dw_put(w, s, n + 1);
}
static void dw_sig(struct db_w *w, const char *s)
{
    unsigned n = (unsigned)rt_strlen(s);
    dw_u8(w, n);
    dw_put(w, s, n + 1);
}
/* Arrays: dw_abegin returns where the length lives, dw_aend patches it. */
static unsigned dw_abegin(struct db_w *w, unsigned elem_align)
{
    dw_u32(w, 0);
    unsigned at = w->err ? 0 : w->len - 4;
    dw_pad(w, elem_align);
    return at;
}
static void dw_aend(struct db_w *w, unsigned at, unsigned elem_align)
{
    if (w->err) return;
    unsigned start = (at + 4 + elem_align - 1) & ~(elem_align - 1);
    unsigned n = w->len - start;
    w->b[at] = (unsigned char)n, w->b[at + 1] = (unsigned char)(n >> 8);
    w->b[at + 2] = (unsigned char)(n >> 16), w->b[at + 3] = (unsigned char)(n >> 24);
}
static void dw_var_s(struct db_w *w, const char *v) { dw_sig(w, "s"), dw_str(w, v); }
static void dw_var_o(struct db_w *w, const char *v) { dw_sig(w, "o"), dw_str(w, v); }
static void dw_var_i(struct db_w *w, int v) { dw_sig(w, "i"), dw_u32(w, (unsigned)v); }
static void dw_var_b(struct db_w *w, int v) { dw_sig(w, "b"), dw_u32(w, v ? 1 : 0); }
static void dw_empty_pixmaps(struct db_w *w)
{
    unsigned a = dw_abegin(w, 8);
    dw_aend(w, a, 8);
}

/* ------------------------------------------------------------- reader */
struct db_r { const unsigned char *b; unsigned len, pos; int err; };

static int dr_pad(struct db_r *r, unsigned a)
{
    unsigned p = (r->pos + a - 1) & ~(a - 1);
    if (r->err || p > r->len) {
        r->err = 1;
        return -1;
    }
    r->pos = p;
    return 0;
}
static unsigned dr_u8(struct db_r *r)
{
    if (r->err || r->pos >= r->len) {
        r->err = 1;
        return 0;
    }
    return r->b[r->pos++];
}
static unsigned dr_u32(struct db_r *r)
{
    if (dr_pad(r, 4) || r->len - r->pos < 4) {
        r->err = 1;
        return 0;
    }
    const unsigned char *p = r->b + r->pos;
    r->pos += 4;
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static const char *dr_bytes_str(struct db_r *r, unsigned n)
{
    if (r->err || n >= r->len - r->pos || r->b[r->pos + n] != 0) {
        r->err = 1;
        return "";
    }
    const char *s = (const char *)r->b + r->pos;
    if (rt_strlen(s) != n) { /* embedded nul */
        r->err = 1;
        return "";
    }
    r->pos += n + 1;
    return s;
}
static const char *dr_str(struct db_r *r)
{
    unsigned n = dr_u32(r);
    return r->err ? "" : dr_bytes_str(r, n);
}
static const char *dr_sig(struct db_r *r)
{
    unsigned n = dr_u8(r);
    return r->err ? "" : dr_bytes_str(r, n);
}

/* One complete type in a signature: returns the position right after it. */
static const char *sig_next(const char *s, int depth)
{
    if (depth > DB_MAX_DEPTH) return 0;
    switch (*s) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x':
    case 't': case 'd': case 'h': case 's': case 'o': case 'g': case 'v':
        return s + 1;
    case 'a':
        return sig_next(s + 1, depth + 1);
    case '(':
        if (s[1] == ')') return 0;
        s++;
        while (*s != ')') {
            s = sig_next(s, depth + 1);
            if (!s) return 0;
        }
        return s + 1;
    case '{':
        s = sig_next(s + 1, depth + 1);
        if (!s) return 0;
        s = sig_next(s, depth + 1);
        if (!s || *s != '}') return 0;
        return s + 1;
    default:
        return 0;
    }
}
static unsigned sig_align(char c)
{
    switch (c) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 4;
    }
}
/* Skip the value of the complete type at s; returns the type's end. */
static const char *dr_skip(struct db_r *r, const char *s, int depth)
{
    if (r->err || depth > DB_MAX_DEPTH) goto bad;
    switch (*s) {
    case 'y':
        dr_u8(r);
        break;
    case 'b': case 'i': case 'u': case 'h':
        dr_u32(r);
        break;
    case 'n': case 'q':
        if (dr_pad(r, 2) || r->len - r->pos < 2) goto bad;
        r->pos += 2;
        break;
    case 'x': case 't': case 'd':
        if (dr_pad(r, 8) || r->len - r->pos < 8) goto bad;
        r->pos += 8;
        break;
    case 's': case 'o':
        dr_str(r);
        break;
    case 'g':
        dr_sig(r);
        break;
    case 'v': {
        const char *vs = dr_sig(r);
        if (r->err) goto bad;
        const char *e = dr_skip(r, vs, depth + 1);
        if (!e || *e) goto bad;
        break;
    }
    case 'a': {
        unsigned n = dr_u32(r);
        const char *e = sig_next(s + 1, depth + 1);
        if (r->err || !e || dr_pad(r, sig_align(s[1])) || n > r->len - r->pos) goto bad;
        r->pos += n;
        return e;
    }
    case '(': case '{': {
        char close = *s == '(' ? ')' : '}';
        if (dr_pad(r, 8)) goto bad;
        s++;
        while (*s && *s != close) {
            s = dr_skip(r, s, depth + 1);
            if (!s) return 0;
        }
        if (*s != close) goto bad;
        return s + 1;
    }
    default:
        goto bad;
    }
    if (r->err) goto bad;
    return s + 1;
bad:
    r->err = 1;
    return 0;
}

/* ------------------------------------------------------------ messages */
struct db_msg {
    unsigned type, flags, serial, reply_serial;
    const char *path, *iface, *member, *error, *dest, *sender, *sig;
    const unsigned char *body;
    unsigned body_len;
};

struct db_conn {
    int fd;
    unsigned serial, inlen;
    unsigned long skip;
    unsigned char in[DB_IN_BUF];
};

static unsigned db_le32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* Parses one complete message of total bytes; returns 0 if it is valid. */
static int db_parse(const unsigned char *b, unsigned total, struct db_msg *m)
{
    unsigned flen = db_le32(b + 12);
    struct db_r r = {b, 16 + flen, 16, 0};
    memset(m, 0, sizeof *m);
    m->type = b[1], m->flags = b[2], m->serial = db_le32(b + 8);
    m->sig = "";
    while (r.pos < r.len) {
        if (dr_pad(&r, 8) || r.pos >= r.len) break;
        unsigned code = dr_u8(&r);
        const char *t = dr_sig(&r);
        if (r.err) return -1;
        const char *want = code == 1 ? "o" : code == 5 || code == 9 ? "u" : code == 8 ? "g" : "s";
        if (code >= 1 && code <= 9 && !rt_streq(t, want)) return -1;
        switch (code) {
        case 1: m->path = dr_str(&r); break;
        case 2: m->iface = dr_str(&r); break;
        case 3: m->member = dr_str(&r); break;
        case 4: m->error = dr_str(&r); break;
        case 5: m->reply_serial = dr_u32(&r); break;
        case 6: m->dest = dr_str(&r); break;
        case 7: m->sender = dr_str(&r); break;
        case 8: m->sig = dr_sig(&r); break;
        case 9: dr_u32(&r); return -1; /* never asked for fds */
        default: {
            const char *e = dr_skip(&r, t, 0);
            if (!e || *e) return -1;
        }
        }
        if (r.err) return -1;
    }
    unsigned body_at = (16 + flen + 7) & ~7u;
    m->body = b + body_at;
    m->body_len = total - body_at;
    if (m->type == DB_CALL && (!m->path || !m->member)) return -1;
    if (m->type == DB_SIGNAL && (!m->path || !m->iface || !m->member)) return -1;
    if ((m->type == DB_RETURN || m->type == DB_ERROR) && !m->reply_serial) return -1;
    if (m->type < 1 || m->type > 4) return -1;
    return 0;
}

typedef void (*db_handler)(struct db_conn *, struct db_msg *);

/* Reads what is available; returns -1 when the connection is gone. */
static int db_read(struct db_conn *c, db_handler handle)
{
    for (;;) {
        rt_ssize n = rt_recv(c->fd, c->in + c->inlen, DB_IN_BUF - c->inlen, RT_MSG_DONTWAIT);
        if (n == -RT_EINTR) continue;
        if (n == -RT_EAGAIN) return 0;
        if (n <= 0) return -1;
        c->inlen += (unsigned)n;
        for (;;) {
            unsigned use;
            if (c->skip) { /* dropping a message larger than the buffer */
                use = c->skip < c->inlen ? (unsigned)c->skip : c->inlen;
                c->skip -= use;
            } else {
                if (c->inlen < 16) break;
                const unsigned char *b = c->in;
                if (b[0] != 'l' || b[3] != 1) return -1;
                unsigned long flen = db_le32(b + 12), blen = db_le32(b + 4);
                if (flen > 65536 || blen > 134217728UL) return -1;
                unsigned long total = ((16 + flen + 7) & ~7UL) + blen;
                if (total > DB_IN_BUF) {
                    c->skip = total;
                    continue;
                }
                if (total > c->inlen) break;
                struct db_msg m;
                if (db_parse(b, (unsigned)total, &m) == 0) handle(c, &m); /* else: ignore it */
                use = (unsigned)total;
            }
            memmove(c->in, c->in + use, c->inlen - use);
            c->inlen -= use;
            if (!c->inlen) break;
        }
    }
}

static unsigned char db_out[9216];

static void dw_field(struct db_w *w, unsigned code, const char *type, const char *v)
{
    if (!v) return;
    dw_pad(w, 8);
    dw_u8(w, code);
    dw_sig(w, type);
    if (*type == 'g') dw_sig(w, v);
    else dw_str(w, v);
}

/* Sends one message; returns its serial or 0 on failure. */
static unsigned db_send(struct db_conn *c, unsigned type, unsigned flags, const char *dest,
                        const char *path, const char *iface, const char *member,
                        unsigned reply_serial, const char *error, const char *sig,
                        const struct db_w *body)
{
    struct db_w h;
    if (c->fd < 0 || (body && body->err)) return 0;
    dw_init(&h, db_out, sizeof db_out);
    unsigned serial = ++c->serial;
    dw_u8(&h, 'l'), dw_u8(&h, type), dw_u8(&h, flags), dw_u8(&h, 1);
    dw_u32(&h, body ? body->len : 0);
    dw_u32(&h, serial);
    unsigned a = dw_abegin(&h, 8);
    dw_field(&h, 1, "o", path);
    dw_field(&h, 2, "s", iface);
    dw_field(&h, 3, "s", member);
    dw_field(&h, 4, "s", error);
    if (reply_serial) {
        dw_pad(&h, 8), dw_u8(&h, 5), dw_sig(&h, "u"), dw_u32(&h, reply_serial);
    }
    dw_field(&h, 6, "s", dest);
    if (sig && *sig) dw_field(&h, 8, "g", sig);
    dw_aend(&h, a, 8);
    dw_pad(&h, 8);
    if (body) dw_put(&h, body->b, body->len);
    if (h.err) return 0;
    const unsigned char *p = h.b;
    unsigned left = h.len;
    while (left) {
        rt_ssize n = rt_send(c->fd, p, left);
        if (n == -RT_EINTR) continue;
        if (n <= 0) return 0;
        p += n, left -= (unsigned)n;
    }
    return serial;
}

static unsigned db_call(struct db_conn *c, unsigned flags, const char *dest, const char *path,
                        const char *iface, const char *member, const char *sig,
                        const struct db_w *body)
{
    return db_send(c, DB_CALL, flags, dest, path, iface, member, 0, 0, sig, body);
}

/* ---------------------------------------------------------- connecting */
static int db_unhex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* First usable unix:path= or unix:abstract= entry of a bus address list. */
static int db_parse_address(const char *a, struct rt_sockaddr_un *sa, int *len)
{
    while (*a) {
        int is_unix = rt_startswith(a, "unix:");
        const char *p = is_unix ? a + 5 : a;
        while (is_unix && *p && *p != ';') {
            int abstract = rt_startswith(p, "abstract=");
            int path = rt_startswith(p, "path=");
            if (abstract || path) {
                const char *v = p + (abstract ? 9 : 5);
                unsigned n = abstract ? 1 : 0;
                memset(sa, 0, sizeof *sa);
                sa->sun_family = RT_AF_UNIX;
                while (*v && *v != ',' && *v != ';') {
                    char ch = *v++;
                    if (ch == '%') {
                        int h1 = db_unhex(v[0]), h2 = h1 < 0 ? -1 : db_unhex(v[1]);
                        if (h2 < 0) return -1;
                        ch = (char)(h1 * 16 + h2);
                        v += 2;
                    }
                    if (n >= sizeof sa->sun_path - 1) return -1;
                    sa->sun_path[n++] = ch;
                }
                *len = (int)(2 + n + (abstract ? 0 : 1));
                return 0;
            }
            while (*p && *p != ',' && *p != ';') p++;
            if (*p == ',') p++;
        }
        while (*a && *a != ';') a++;
        if (*a == ';') a++;
    }
    return -1;
}

static int db_wait_in(int fd, long ms)
{
    struct rt_pollfd f = {fd, RT_POLLIN, 0};
    struct rt_timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    int r;
    do r = rt_poll(&f, 1, &t); while (r == -RT_EINTR);
    return r > 0 ? 0 : -1;
}

/* Connects, authenticates and sends Hello. Returns 0 on success. */
static int db_open(struct db_conn *c, const char *address)
{
    struct rt_sockaddr_un sa;
    int salen;
    c->fd = -1, c->serial = 0, c->inlen = 0, c->skip = 0;
    if (!address || db_parse_address(address, &sa, &salen) < 0) return -1;
    int fd = rt_socket_unix();
    if (fd < 0) return -1;
    if (rt_connect(fd, &sa, salen) < 0) goto fail;

    char line[128], uid[16];
    struct rt_sb s, u;
    static const char hex[] = "0123456789abcdef";
    sb_init(&u, uid, sizeof uid);
    sb_num(&u, rt_getuid());
    sb_init(&s, line, sizeof line);
    sb_add(&s, "AUTH EXTERNAL ");
    for (const char *q = uid; *q; q++) {
        char two[3] = {hex[(unsigned char)*q >> 4], hex[*q & 15], 0};
        sb_add(&s, two);
    }
    sb_add(&s, "\r\n");
    if (rt_send(fd, "", 1) != 1 || rt_send(fd, line, s.len) != (rt_ssize)s.len) goto fail;

    unsigned got = 0;
    for (;;) {
        if (db_wait_in(fd, 5000) < 0) goto fail;
        rt_ssize n = rt_recv(fd, line + got, sizeof line - 1 - got, 0);
        if (n == -RT_EINTR) continue;
        if (n <= 0) goto fail;
        got += (unsigned)n;
        line[got] = 0;
        if (got >= 2 && line[got - 2] == '\r' && line[got - 1] == '\n') break;
        if (got >= sizeof line - 1) goto fail;
    }
    if (!rt_startswith(line, "OK ")) goto fail;
    if (rt_send(fd, "BEGIN\r\n", 7) != 7) goto fail;
    c->fd = fd;
    if (!db_call(c, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                 "Hello", "", 0)) {
        c->fd = -1;
        goto fail;
    }
    return 0;
fail:
    rt_close(fd);
    return -1;
}

#endif
