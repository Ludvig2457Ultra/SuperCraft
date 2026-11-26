#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <unwind.h>
#include <link.h>
#include <libunwind.h>
#include <ucontext.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#undef getc
#undef fgetc
#undef clearerr
#undef fseeko
#undef ftello
#undef fgetpos
#undef fsetpos

/* Bionic stores its three standard streams in one array at a 0x98-byte
 * stride, while glibc's FILE is 0xe0 bytes. Only the last slot holds a
 * complete glibc handle; handing slots 0/1 to glibc aborts in
 * _IO_vtable_check with "invalid stdio handle". Map them to the real
 * glibc streams before any stdio call. */
extern unsigned char __sF[];
#define BIONIC_SFILE_SIZE 0x98u

static FILE *bionic_stream(FILE *fp) {
    if (!fp) return fp;
    uintptr_t base = (uintptr_t)__sF;
    uintptr_t v = (uintptr_t)fp;
    if (v < base || v - base >= 3u * BIONIC_SFILE_SIZE) return fp;
    switch ((v - base) / BIONIC_SFILE_SIZE) {
        case 0: return stdin;
        case 1: return stdout;
        default: return stderr;
    }
}

typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef void *EGLClientBuffer;
typedef void *EGLNativeWindowType;
typedef void *EGLNativeDisplayType;

enum {
    EGL_SUCCESS = 0x3000,
    EGL_BAD_DISPLAY = 0x300D,
    EGL_NO_CONTEXT = 0x0000,
    EGL_DEFAULT_DISPLAY = 0x0000,
    EGL_WINDOW_BIT = 0x0004,
    EGL_PBUFFER_BIT = 0x0001,
    EGL_RED_SIZE = 0x3024,
    EGL_GREEN_SIZE = 0x3023,
    EGL_BLUE_SIZE = 0x3022,
    EGL_ALPHA_SIZE = 0x3021,
    EGL_DEPTH_SIZE = 0x3025,
    EGL_STENCIL_SIZE = 0x3026,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_OPENGL_ES2_BIT = 0x0004,
    EGL_OPENGL_ES3_BIT = 0x0040,
    EGL_SURFACE_TYPE = 0x3033,
    EGL_CONFIG_CAVEAT = 0x3027,
    EGL_NONE = 0x3038,
    EGL_HEIGHT = 0x3056,
    EGL_WIDTH = 0x3057,
    EGL_CONTEXT_MAJOR_VERSION = 0x3098,
    EGL_CONTEXT_MINOR_VERSION = 0x30FB,
};

typedef void *(*glXGetProcAddr_fn)(const char *);

static glXGetProcAddr_fn g_glx_getproc;
static int g_glx_ready;

void sup_gl_init(void) {
    if (g_glx_ready) return;
    g_glx_getproc = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    if (!g_glx_getproc) g_glx_getproc = dlsym(RTLD_DEFAULT, "glXGetProcAddress");
    g_glx_ready = 1;
    fprintf(stderr, "GL> glXGetProcAddressARB = %p\n", (void *)g_glx_getproc);
    fflush(stderr);
}

void *sup_gl_alias_lookup(const char *name);

void *sup_gl_resolve(const char *name) {
    sup_gl_init();
    if (!g_glx_getproc) return NULL;
    /* Заглушки Mesa для glGetQueryObject*EXT/ARB: подменяем на рабочие. */
    void *alias = sup_gl_alias_lookup(name);
    if (alias) return alias;
    void *r = (void *)g_glx_getproc(name);
    if (name && strstr(name, "glGetShaderPrecision"))
        fprintf(stderr, "GLR> sup_gl_resolve(%s) -> %p (proc=%p)\n",
                name, r, (void *)g_glx_getproc);
    fflush(stderr);
    return r;
}

static void *real_dlsym(const char *name) {
    static void *(*fn)(void *, const char *);
    if (!fn) fn = dlsym(RTLD_NEXT, "dlsym");
    return fn(RTLD_DEFAULT, name);
}

/* ---- glGetQueryObject*: EXT/ARB-варианты в Mesa — no-op ------------------
 * Игра (libminecraftpe) через EGL-загрузчик просит glGetQueryObjectivEXT
 * для проверки готовности timer-query. Mesa отдаёт заглушку, которая не
 * пишет в params: игра читает мусор из стека, считает запрос «неготовым»
 * и не двигает курсор кольца рендер-команд. Кольцо переполняется (ёмкость
 * 1024) и рендер встаёт на ~1023 кадрах. Отдаём рабочий glGetQueryObjectuiv.
 * 64-битные варианты намеренно НЕ подменяем: их вызов из этого контекста
 * роняет игру (проверено). */
typedef void (*sup_q_uiv_fn)(unsigned, unsigned, unsigned *);
static sup_q_uiv_fn g_q_uiv;

static void sup_q_init(void) {
    if (g_q_uiv) return;
    /* берём настоящий символ, минуя собственные алиасы */
    g_q_uiv = (sup_q_uiv_fn)real_dlsym("glGetQueryObjectuiv");
    fprintf(stderr, "QFIX> uiv=%p\n", (void *)g_q_uiv);
    fflush(stderr);
}

void sup_glGetQueryObjectuivEXT(unsigned id, unsigned pname, unsigned *params) {
    sup_q_init();
    unsigned v = 0;
    if (g_q_uiv) g_q_uiv(id, pname, &v);
    if (params) *params = v;
}

void sup_glGetQueryObjectivEXT(unsigned id, unsigned pname, int *params) {
    sup_q_init();
    unsigned v = 0;
    if (g_q_uiv) g_q_uiv(id, pname, &v);
    if (params) *params = (int)v;
}

static const struct { const char *name; void *fn; } g_q_alias[] = {
    { "glGetQueryObjectivEXT",  (void *)sup_glGetQueryObjectivEXT },
    { "glGetQueryObjectuivEXT", (void *)sup_glGetQueryObjectuivEXT },
    { "glGetQueryObjectivARB",  (void *)sup_glGetQueryObjectivEXT },
    { "glGetQueryObjectuivARB", (void *)sup_glGetQueryObjectuivEXT },
};

void *sup_gl_alias_lookup(const char *name) {
    if (!name) return NULL;
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("SUP_QALIAS");
        off = (e && !strcmp(e, "0"));
    }
    if (off) return NULL;
    for (unsigned i = 0; i < sizeof(g_q_alias) / sizeof(g_q_alias[0]); ++i)
        if (strcmp(name, g_q_alias[i].name) == 0) {
            fprintf(stderr, "QFIX> alias %s -> %p\n", name, g_q_alias[i].fn);
            fflush(stderr);
            return g_q_alias[i].fn;
        }
    return NULL;
}

void *dlopen(const char *filename, int flags) {
    static void *(*real)(const char *, int);
    if (!real) real = (void *(*)(const char *, int))real_dlsym("dlopen");
    if (filename) {
        const char *known[] = {
            "libEGL.so", "libGLESv2.so", "libGLESv1_CM.so", "libandroid.so",
            "liblog.so", "libdl.so", "libc.so", "libm.so", "libz.so",
            "libc++_shared.so", "libstdc++.so", "libfmod.so", "libOpenSLES.so",
            NULL };
        for (int i = 0; known[i]; ++i)
            if (strcmp(filename, known[i]) == 0) return (void *)(uintptr_t)(0x1000 + i);
    }
    return real(filename, flags);
}

int dlclose(void *handle) {
    static int (*real)(void *);
    if (!real) real = (int (*)(void *))real_dlsym("dlclose");
    if (handle && (uintptr_t)handle >= 0x1000 && (uintptr_t)handle < 0x2000) return 0;
    return real(handle);
}

/* ---- перехват dlsym для графики --------------------------------------
 * Игра делает dlopen("libEGL.so") -> получает от нас фейковый handle
 * (0x1000+i, см. dlopen выше), потом dlsym(handle, "eglGetDisplay").
 * Без перехвата этот запрос уходит в RTLD_DEFAULT и достаёт настоящую
 * Mesa, минуя наш мост: поэтому EGL> в трейсе не появлялось вовсе,
 * а рендер шёл мимо. Здесь отдаём адреса собственных хуков.
 * Хуки определены ниже по файлу, поэтому нужны forward declarations. */

EGLDisplay eglGetDisplay(EGLNativeDisplayType);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglTerminate(EGLDisplay);
EGLBoolean eglBindAPI(unsigned int);
EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
EGLContext eglGetCurrentContext(void);
EGLSurface eglGetCurrentSurface(EGLint);
EGLDisplay eglGetCurrentDisplay(void);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
EGLBoolean eglReleaseThread(void);
void *eglGetProcAddress(const char *);
const char *eglQueryString(EGLDisplay, EGLint);
EGLint eglGetError(void);
EGLBoolean eglWaitClient(void);

/* Собственный handle нужен, чтобы резолвить НАШИ символы через настоящий
 * dlsym. Таблица fn[] выше инициализируется релокацией R_X86_64_64, которую
 * ld.so резолвит в глобальном scope: libEGL.so.1 грузится в loader.c с
 * RTLD_GLOBAL РАНЬШЕ libsupcompat, поэтому в таблицу попадали адреса Mesa,
 * и GOT игры уходил в настоящую libEGL мимо моста. dlsym(свой_handle, name)
 * ищет сначала в самом объекте и возвращает наше определение. */
static void *g_egl_self;

static void *sup_egl_self(void) {
    if (!g_egl_self) {
        Dl_info info;
        if (dladdr((const void *)sup_egl_self, &info) && info.dli_fname) {
            g_egl_self = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            fprintf(stderr, "EGLSELF> self=%p file=%s\n", g_egl_self,
                    info.dli_fname);
            fflush(stderr);
        }
    }
    return g_egl_self;
}

static void *sup_local_egl_symbol(const char *s) {
    static const struct { const char *name; void *fn; } tab[] = {
        { "eglGetDisplay", (void *)eglGetDisplay },
        { "eglInitialize", (void *)eglInitialize },
        { "eglTerminate", (void *)eglTerminate },
        { "eglBindAPI", (void *)eglBindAPI },
        { "eglGetConfigs", (void *)eglGetConfigs },
        { "eglChooseConfig", (void *)eglChooseConfig },
        { "eglGetConfigAttrib", (void *)eglGetConfigAttrib },
        { "eglCreateContext", (void *)eglCreateContext },
        { "eglDestroyContext", (void *)eglDestroyContext },
        { "eglCreateWindowSurface", (void *)eglCreateWindowSurface },
        { "eglCreatePbufferSurface", (void *)eglCreatePbufferSurface },
        { "eglDestroySurface", (void *)eglDestroySurface },
        { "eglMakeCurrent", (void *)eglMakeCurrent },
        { "eglSwapBuffers", (void *)eglSwapBuffers },
        { "eglSwapInterval", (void *)eglSwapInterval },
        { "eglGetCurrentContext", (void *)eglGetCurrentContext },
        { "eglGetCurrentSurface", (void *)eglGetCurrentSurface },
        { "eglGetCurrentDisplay", (void *)eglGetCurrentDisplay },
        { "eglQuerySurface", (void *)eglQuerySurface },
        { "eglReleaseThread", (void *)eglReleaseThread },
        { "eglGetProcAddress", (void *)eglGetProcAddress },
        { "eglQueryString", (void *)eglQueryString },
        { "eglGetError", (void *)eglGetError },
        { "eglWaitClient", (void *)eglWaitClient },
        { NULL, NULL }
    };
    static void *cache[24];
    for (int i = 0; tab[i].name; ++i) {
        if (strcmp(s, tab[i].name) != 0) continue;
        if (!cache[i]) {
            void *self = sup_egl_self();
            void *fn = self ? dlsym(self, s) : NULL;
            if (!fn) fn = tab[i].fn;
            if (fn != tab[i].fn) {
                fprintf(stderr, "EGLSELF> %s local=%p (table=%p)\n", s, fn,
                        tab[i].fn);
                fflush(stderr);
            }
            cache[i] = fn;
        }
        return cache[i];
    }
    return NULL;
}

void *sup_gl_slot_lookup(const char *name);

void *sup_local_egl_symbol_pub(const char *s) { return sup_local_egl_symbol(s); }

void *dlsym(void *handle, const char *symbol) {
    static void *(*real)(void *, const char *);
    if (!real) real = (void *(*)(void *, const char *))real_dlsym("dlsym");
    if (symbol) {
        void *alias = sup_gl_alias_lookup(symbol);
        if (alias) return alias;
        void *local = sup_local_egl_symbol(symbol);
        if (local) {
            fprintf(stderr, "DLSYM> %s -> bridge hook %p\n", symbol, local);
            fflush(stderr);
            return local;
        }
        /* gl* идут через слоты: адрес должен соответствовать текущему
         * GLES-контексту, поэтому берём из уже забинженных слотов. */
        if (strncmp(symbol, "gl", 2) == 0 &&
            strncmp(symbol, "glX", 3) != 0 &&
            strncmp(symbol, "glx", 3) != 0) {
            void *slot = sup_gl_slot_lookup(symbol);
            if (slot) {
                fprintf(stderr, "DLSYM> %s -> gl slot %p\n", symbol, slot);
                fflush(stderr);
                return slot;
            }
        }
    }
    if (symbol && (strncmp(symbol, "FMOD", 4) == 0 || strncmp(symbol, "_ZN", 3) == 0)) {
        fprintf(stderr, "DLSYM> %s from %p\n", symbol, __builtin_return_address(0));
    }
    void *r;
    if (handle && (uintptr_t)handle >= 0x1000 && (uintptr_t)handle < 0x2000)
        r = dlsym(RTLD_DEFAULT, symbol);
    else
        r = real(handle ? handle : RTLD_DEFAULT, symbol);
    if (symbol && strncmp(symbol, "FMOD", 4) == 0)
        fprintf(stderr, "DLSYM>  -> %s = %p\n", symbol, r);
    return r;
}

void *dlvsym(void *handle, const char *symbol, const char *version) {
    static void *(*real)(void *, const char *, const char *);
    if (!real) real = (void *(*)(void *, const char *, const char *))real_dlsym("dlvsym");
    if (handle && (uintptr_t)handle >= 0x1000 && (uintptr_t)handle < 0x2000)
        return dlsym(RTLD_DEFAULT, symbol);
    return real(handle ? handle : RTLD_DEFAULT, symbol, version);
}

int dladdr(const void *addr, Dl_info *info) {
    static int (*real)(const void *, Dl_info *);
    if (!real) real = (int (*)(const void *, Dl_info *))real_dlsym("dladdr");
    return real(addr, info);
}

char *dlerror(void) {
    static char *(*real)(void);
    if (!real) real = (char *(*)(void))real_dlsym("dlerror");
    return real();
}

typedef unsigned long sup_pthread_t;
typedef void *(*sup_start_fn)(void *);

/* === Диагностика остановки рендера ===
 * Свапы уходят в первые секунды и больше не приходят. Чтобы увидеть, ГДЕ
 * спит каждый поток в этот момент, по таймеру шлём SIGURG всем потокам
 * процесса; обработчик печатает свой стек только через write(2) (без
 * malloc/flockfile — безопасно в контексте прерванного потока). */
static volatile long g_last_swap_t;
static volatile unsigned g_swap_seen;
static volatile long g_swap_tid;
static int g_tdump_n;
static long g_tids[128];
static int g_ntids;
static volatile int g_tid_lock;   /* спинлок: pthread.h невозможен (interpose) */
/* Настоящий pthread_mutex_lock из libc (наш interpose его перекрывает). */
static void *g_real_pml;
static long g_main_tid;
static volatile int g_mutex_timed;

static void tid_lock(void) {
    while (__atomic_exchange_n(&g_tid_lock, 1, __ATOMIC_ACQUIRE)) { }
}
static void tid_unlock(void) { __atomic_store_n(&g_tid_lock, 0, __ATOMIC_RELEASE); }

void sup_note_swap(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_last_swap_t = ts.tv_sec;
    __atomic_add_fetch(&g_swap_seen, 1U, __ATOMIC_RELAXED);
}

static void tput(char *b, int *p, const char *s) {
    while (*s && *p < 4000) b[(*p)++] = *s++;
}
static void tput_dec(char *b, int *p, long v) {
    char t[24]; int n = 0;
    if (v < 0) { tput(b, p, "-"); v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    while (n && *p < 4000) b[(*p)++] = t[--n];
}
static void tput_hex(char *b, int *p, unsigned long v) {
    static const char hx[] = "0123456789abcdef";
    tput(b, p, "0x");
    int started = 0;
    for (int i = 15; i >= 0; --i) {
        unsigned d = (unsigned)((v >> (i * 4)) & 0xf);
        if (d || started || i == 0) { started = 1; b[(*p)++] = hx[d]; }
    }
}

typedef struct { unsigned long *buf; int n, max; } UwCtx;
static _Unwind_Reason_Code uw_cb(struct _Unwind_Context *ctx, void *arg) {
    UwCtx *c = (UwCtx *)arg;
    unsigned long ip = (unsigned long)_Unwind_GetIP(ctx);
    if (ip && c->n < c->max) c->buf[c->n++] = ip;
    return _URC_NO_REASON;
}

/* Дамп кольцевой очереди рендер-команд: только чтение памяти — GL из
 * обработчика сигнала звать нельзя (сигнал приходит в произвольный поток,
 * контекст может быть не текущим). */
static void sup_probe_ring(char *buf, int *p, unsigned long rbx) {
    volatile unsigned *q = (volatile unsigned *)(rbx + 0xb818);
    unsigned cap = q[0], f1c = q[1], curA = q[2], curB = q[3];
    tput(buf, p, " q{cap=");
    tput_dec(buf, p, (long)cap);
    tput(buf, p, " curA=");
    tput_dec(buf, p, (long)curA);
    tput(buf, p, " curB=");
    tput_dec(buf, p, (long)curB);
    tput(buf, p, " f1c=");
    tput_dec(buf, p, (long)f1c);
    tput(buf, p, "}");
    if (!cap || cap > 1u << 20) return;
    unsigned idx[2] = { curB, curA };
    for (int k = 0; k < 2; ++k) {
        unsigned i = idx[k];
        volatile unsigned *s = (volatile unsigned *)(rbx + 0x1818 + (unsigned long)i * 40);
        tput(buf, p, k ? " slotA{" : " slotB{");
        tput_dec(buf, p, (long)i);
        tput(buf, p, " id=");
        tput_dec(buf, p, (long)s[0]);
        tput(buf, p, " q=");
        tput_dec(buf, p, (long)s[1]);
        tput(buf, p, " f0=");
        tput_dec(buf, p, (long)s[2]);
        tput(buf, p, " f1=");
        tput_dec(buf, p, (long)s[3]);
        tput(buf, p, " ready=");
        tput_dec(buf, p, (long)(*(volatile unsigned char *)(rbx + 0x1828 + (unsigned long)i * 40)));
        tput(buf, p, "}");
    }
}

/* Диапазон .text игры, чтобы вытаскивать из стека свои code-адреса:
 * символов у игры нет, но смещения сравнимы между прогонами. */
static unsigned long g_dbg_gbase, g_dbg_gend;
void sup_dbg_set_game(unsigned long base, unsigned long size) {
    g_dbg_gbase = base;
    g_dbg_gend = base + size;
}

/* Таблица загруженных модулей (для символизации адресов в обработчике
 * сигнала без dladdr — его спиновые блокировки опасны в контексте
 * прерванного потока). Заполняется один раз в конструкторе. */
#define SUP_MAXMOD 48
static struct { unsigned long lo, hi; char name[56]; } g_mods[SUP_MAXMOD];
static int g_nmods;

static int sup_mod_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (g_nmods >= SUP_MAXMOD) return 1;
    unsigned long lo = ~0UL, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        unsigned long a = (unsigned long)info->dlpi_addr + ph->p_vaddr;
        unsigned long b = a + ph->p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (lo >= hi) return 0;
    const char *n = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "runner";
    const char *slash = strrchr(n, '/');
    if (slash) n = slash + 1;
    g_mods[g_nmods].lo = lo;
    g_mods[g_nmods].hi = hi;
    snprintf(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "%s", n);
    ++g_nmods;
    return 0;
}

/* Регистрация вручную загруженных модулей (game/deps) и их .eh_frame.
 * Нужно, чтобы libgcc умел раскручивать кадры игры, которую видит только
 * наш ELF-лоадер (dl_iterate_phdr её не знает). */
void sup_dbg_add_module(unsigned long lo, unsigned long hi, const char *path) {
    if (!lo || hi <= lo) return;
    for (int m = 0; m < g_nmods; ++m)
        if (g_mods[m].lo <= lo && g_mods[m].hi >= hi) return;
    if (g_nmods >= SUP_MAXMOD) return;
    const char *n = path ? path : "?";
    const char *slash = strrchr(n, '/');
    if (slash) n = slash + 1;
    g_mods[g_nmods].lo = lo;
    g_mods[g_nmods].hi = hi;
    snprintf(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "%s", n);
    ++g_nmods;
}

extern void __register_frame(void *);
void sup_dbg_register_frame(void *eh) {
    if (!eh) return;
    unsigned len = *(unsigned *)eh;
    if (len == 0 || len > 0x1000000u) {
        fprintf(stderr, "EHF> bad .eh_frame %p len=%u — пропуск\n", eh, len);
        fflush(stderr);
        return;
    }
    __register_frame(eh);
    fprintf(stderr, "EHF> __register_frame(%p) ok (first CIE len=%u)\n", eh, len);
    fflush(stderr);
}

static const char *sup_mod_name(unsigned long ip, unsigned long *off) {
    for (int m = 0; m < g_nmods; ++m) {
        if (ip >= g_mods[m].lo && ip < g_mods[m].hi) {
            *off = ip - g_mods[m].lo;
            return g_mods[m].name;
        }
    }
    return NULL;
}

/* Полный анвинд прерванного контекста через libunwind: стартуем прямо с
 * ucontext сигнальной рамки (UNW_INIT_SIGNAL_FRAME), поэтому видны все
 * кадры игры, а не 2-5, которые давал _Unwind_Backtrace. */
static void sup_dump_uw(void *ucv) {
    char b[320];
    unw_cursor_t cur;
    unw_context_t *uc = (unw_context_t *)ucv;
    if (!uc || unw_init_local2(&cur, uc, UNW_INIT_SIGNAL_FRAME) < 0) {
        (void)!write(2, "  uv: init failed\n", 18);
        return;
    }
    for (int i = 0; i < 48; ++i) {
        unw_word_t ip = 0, sp = 0;
        if (unw_get_reg(&cur, UNW_REG_IP, &ip) < 0 || !ip) break;
        (void)unw_get_reg(&cur, UNW_REG_SP, &sp);
        int p = 0;
        tput(b, &p, "  uv ");
        tput_dec(b, &p, i);
        tput(b, &p, " ip=");
        tput_hex(b, &p, (unsigned long)ip);
        tput(b, &p, " sp=");
        tput_hex(b, &p, (unsigned long)sp);
        unsigned long off = 0;
        const char *mn = sup_mod_name((unsigned long)ip, &off);
        if (mn) {
            tput(b, &p, " ");
            tput(b, &p, mn);
            tput(b, &p, "+");
            tput_hex(b, &p, off);
        }
        b[p++] = '\n';
        (void)!write(2, b, (size_t)p);
        if (unw_step(&cur) <= 0) break;
    }
}

static void sup_tdump_sig(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    char buf[4096];
    int p = 0;
    long tid = (long)syscall(186);
    tput(buf, &p, "THREADDUMP> tid=");
    tput_dec(buf, &p, tid);
    /* Состояние кольцевой очереди задач движка: rbx указывает на объект,
     * поля +0xb818(cap) +0xb820(curA) +0xb824(curB). В момент стопора
     * рендер-поток крутится в этом цикле — узнаём, пуста ли очередь. */
    if (tid == g_swap_tid && ucv) {
        ucontext_t *uc = (ucontext_t *)ucv;
        unsigned long rbx = (unsigned long)uc->uc_mcontext.gregs[REG_RBX];
        tput(buf, &p, " rbx=");
        tput_hex(buf, &p, rbx);
        if (rbx > 0x10000) sup_probe_ring(buf, &p, rbx);
    }
    if (ucv) {
        ucontext_t *uc = (ucontext_t *)ucv;
        tput(buf, &p, " rip=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RIP]);
        tput(buf, &p, " rsp=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
        /* Регистры нужны, чтобы понять, на каком мьютексе/объекте висим. */
        tput(buf, &p, " rax=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RAX]);
        tput(buf, &p, " rbx=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RBX]);
        tput(buf, &p, " rcx=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RCX]);
        tput(buf, &p, " rdx=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RDX]);
        tput(buf, &p, " rsi=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RSI]);
        tput(buf, &p, " rdi=");
        tput_hex(buf, &p, (unsigned long)uc->uc_mcontext.gregs[REG_RDI]);
        tput(buf, &p, "|");
        /* Сырой скан стека: адреса, попадающие в .text игры. */
        if (g_dbg_gbase) {
            unsigned long rsp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];
            for (int i = 0; i < 128; ++i) {
                unsigned long v = *(unsigned long *)(rsp + (unsigned long)i * 8);
                if (v >= g_dbg_gbase && v < g_dbg_gend) {
                    tput(buf, &p, " g+");
                    tput_hex(buf, &p, v - g_dbg_gbase);
                }
            }
        }
    }
    tput(buf, &p, " стек:\n");
    (void)!write(2, buf, (size_t)p);
    /* Если поток стоит в pthread_mutex_lock, печатаем состояние мьютекса:
     * +0 lock, +8 owner(tid), +12 nusers, +16 kind. Это отвечает на вопрос
     * «кто держит блокировку» — в т.ч. не сам ли поток (самодедлок). */
    if (ucv) {
        ucontext_t *uc = (ucontext_t *)ucv;
        unsigned long pc = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
        unsigned long m = (unsigned long)uc->uc_mcontext.gregs[REG_RDI];
        /* Внутренний futex-wait (__lll_lock_wait_private) лежит ниже
         * pthread_mutex_lock на ~0x700 байт, поэтому окно охватывает и его. */
        if (g_real_pml && pc >= (unsigned long)g_real_pml - 0x10000 &&
            pc < (unsigned long)g_real_pml + 0x600 &&
            /* rdi — &mutex->__lock (4-байтовая, не 8-байтовая выравнивка). */
            m > 0x10000 && !(m & 3)) {
            unsigned char vec[2] = { 0, 0 };
            if (mincore((void *)(m & ~(unsigned long)0xfff), 0x1000, vec) == 0) {
                char mb[240];
                int q = 0;
                long me = (long)syscall(186);
                int owner = *(int *)(m + 8);
                tput(mb, &q, "  MUTEX> ");
                tput_hex(mb, &q, m);
                tput(mb, &q, " lock=");
                tput_dec(mb, &q, (long)*(unsigned int *)m);
                tput(mb, &q, " owner_tid=");
                tput_dec(mb, &q, (long)owner);
                tput(mb, &q, " nusers=");
                tput_dec(mb, &q, (long)*(unsigned int *)(m + 12));
                tput(mb, &q, " kind=");
                tput_dec(mb, &q, (long)*(int *)(m + 16));
                tput(mb, &q, owner == (int)me ? " SELF-DEADLOCK\n" : " (чужой)\n");
                (void)!write(2, mb, (size_t)q);
            }
        }
    }
    sup_dump_uw(ucv);
    /* libgcc-анвинд: после __register_frame(.eh_frame игры) видит и её кадры. */
    {
        static __thread unsigned long bts[64];
        UwCtx c = { bts, 0, 64 };
        _Unwind_Backtrace(uw_cb, &c);
        for (int i = 0; i < c.n; ++i) {
            char lb[200];
            int q = 0;
            tput(lb, &q, "  bt ");
            tput_dec(lb, &q, i);
            tput(lb, &q, " ip=");
            tput_hex(lb, &q, bts[i]);
            unsigned long off = 0;
            const char *mn = sup_mod_name(bts[i], &off);
            if (mn) {
                tput(lb, &q, " ");
                tput(lb, &q, mn);
                tput(lb, &q, "+");
                tput_hex(lb, &q, off);
            }
            lb[q++] = '\n';
            (void)!write(2, lb, (size_t)q);
        }
    }
}

__attribute__((constructor)) static void sup_tdump_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sup_tdump_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGURG, &sa, NULL);
    g_real_pml = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    g_main_tid = syscall(SYS_gettid);
    g_mutex_timed = getenv("SUP_MUTEX_TIMED") != NULL;
    fprintf(stderr, "PML> real pthread_mutex_lock=%p timed=%d\n",
            g_real_pml, g_mutex_timed);
    fflush(stderr);
    /* Модули и кэш libunwind готовим заранее: в обработчике сигнала
     * dl_iterate_phdr/malloc уже нельзя. */
    dl_iterate_phdr(sup_mod_cb, NULL);
    {
        unw_context_t c;
        unw_cursor_t cur;
        if (unw_getcontext(&c) == 0 && unw_init_local(&cur, &c) == 0) {
            for (int i = 0; i < 4; ++i)
                if (unw_step(&cur) <= 0) break;
        }
    }
}

void sup_render_watch(void) {
    if (g_tdump_n >= 3 || !g_swap_seen) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long need = g_tdump_n ? 30 : 3;
    if (ts.tv_sec - g_last_swap_t < need) return;
    g_tdump_n++;
    char buf[160];
    int p = 0;
    tput(buf, &p, "THREADDUMP> свапы стоят ");
    tput_dec(buf, &p, ts.tv_sec - g_last_swap_t);
    tput(buf, &p, "s (последний #");
    tput_dec(buf, &p, (long)g_swap_seen);
    tput(buf, &p, "), дамп потоков (попытка ");
    tput_dec(buf, &p, g_tdump_n);
    tput(buf, &p, "); рендер-поток tid=");
    tput_dec(buf, &p, g_swap_tid);
    tput(buf, &p, "\n");
    (void)!write(2, buf, (size_t)p);
    /* Сохраняем карту памяти ровно в этот момент — для оффлайн-декодирования
     * адресов стеков (ASLR делает адреса уникальными для каждого прогона). */
    {
        int mf = open("/proc/self/maps", O_RDONLY);
        int of = open("/tmp/opencode/maps_tdump.txt",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mf >= 0 && of >= 0) {
            char c[4096];
            ssize_t r;
            while ((r = read(mf, c, sizeof c)) > 0) {
                ssize_t off = 0;
                while (off < r) {
                    ssize_t w = write(of, c + off, (size_t)(r - off));
                    if (w <= 0) break;
                    off += w;
                }
            }
        }
        if (mf >= 0) close(mf);
        if (of >= 0) close(of);
    }
    long me = (long)syscall(186);
    tid_lock();
    int n = g_ntids;
    long copy[128];
    memcpy(copy, g_tids, (size_t)n * sizeof(long));
    tid_unlock();
    for (int i = 0; i < n; ++i) {
        if (copy[i] == me) continue;
        syscall(234 /*tgkill*/, (long)getpid(), copy[i], SIGURG);
        usleep(25000);
    }
}

typedef struct { sup_start_fn fn; void *arg; } sup_targ;

static void *sup_tstart(void *raw) {
    sup_targ t = *(sup_targ *)raw;
    free(raw);
    long tid = (long)syscall(186);
    tid_lock();
    if (g_ntids < 128) g_tids[g_ntids++] = tid;
    tid_unlock();
    return t.fn(t.arg);
}

int pthread_create(sup_pthread_t *thr, void *attr, sup_start_fn start, void *arg) {
    static int (*real)(sup_pthread_t *, void *, sup_start_fn, void *);
    if (!real) real = (int (*)(sup_pthread_t *, void *, sup_start_fn, void *))
                          real_dlsym("pthread_create");
    void *caller = __builtin_return_address(0);
    fprintf(stderr, "PC> pthread_create start=%p arg=%p caller=%p stderr_tid=%ld\n",
            (void *)start, arg, caller, (long)syscall(186));
    fflush(stderr);
    sup_targ *t = (sup_targ *)malloc(sizeof *t);
    if (!t) return real(thr, attr, start, arg);
    t->fn = start;
    t->arg = arg;
    int rc = real(thr, attr, sup_tstart, t);
    if (rc != 0) free(t);
    return rc;
}

/* NVIDIA EGL ждёт special event от Xwayland, удерживая игровой мьютекс;
 * главный поток встаёт на этот мьютекс и загрузка замирает. Env-режим
 * SUP_XCB_NO_WAIT возвращает «события нет» сразу — Deadlock снимается.
 * Оставшийся упрёк в 43% был не здесь: в каталоге версии не было ассетов
 * и нативных библиотек из APK, поэтому AAssetManager отдавал пустые листинги.
 * tools/run-runner.sh раскладывает их и стартует до EGL/рендера. */
void *xcb_wait_for_special_event(void *c, void *se) {
    (void)c; (void)se;
    static void *(*real)(void *, void *);
    if (!getenv("SUP_XCB_NO_WAIT")) {
        if (!real) {
            real = (void *(*)(void *, void *))
                real_dlsym("xcb_wait_for_special_event");
            if (!real) {
                real = (void *(*)(void *, void *))
                    dlsym(RTLD_NEXT, "xcb_wait_for_special_event");
            }
        }
        if (real) return real(c, se);
    }
    /* Или события нет, или ждать нельзя — возвращаем «нет special event». */
    return NULL;
}

int pthread_mutex_lock(sup_pthread_t *m) {
    static int (*real)(sup_pthread_t *);
    static int (*timed)(sup_pthread_t *, const struct timespec *);
    if (!real) real = (int (*)(sup_pthread_t *))real_dlsym("pthread_mutex_lock");
    if (g_mutex_timed) {
        if (!timed) timed = (int (*)(sup_pthread_t *, const struct timespec *))
            real_dlsym("pthread_mutex_timedlock");
        if (timed) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int r = timed(m, &ts);
            if (r == ETIMEDOUT) {
                fprintf(stderr, "MUTEXBYPASS> %p\n", (void *)m);
                fflush(stderr);
                return 0;
            }
            return r;
        }
    }
    return real(m);
}

static void sup_log_open(const char *what, const char *path, int flags) {
    if (!path) return;
    const char *wh = strstr(path, "d.google");
    const char *st = getenv("SUP_TRACE_OPEN");
    if (!wh && !st) return;
    fprintf(stderr, "OPEN> %s(%s, flags=%#x) caller=%p path=\"%s\"\n",
            what, path, flags, __builtin_return_address(0), path);
    if (wh) {
        fprintf(stderr, "OPEN>  *** contains d.google marker ***\n");
        fflush(stderr);
    }
}

/* Должны передать variadic mode в реальный open(), иначе при O_CREAT
 * файл создаётся со случайными правами из мусорного регистра. */
static int sup_open_call(int (*real)(const char *, int, ...),
                         const char *path, int flags, va_list ap) {
    if (flags & O_CREAT) {
        int mode = va_arg(ap, int);
        return real(path, flags, mode);
    }
    return real(path, flags);
}

int open64(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = (int (*)(const char *, int, ...))real_dlsym("open64");
    sup_log_open("open64", path, flags);
    va_list ap;
    va_start(ap, flags);
    int fd = sup_open_call(real, path, flags, ap);
    va_end(ap);
    return fd;
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = (int (*)(const char *, int, ...))real_dlsym("open");
    sup_log_open("open", path, flags);
    va_list ap;
    va_start(ap, flags);
    int fd = sup_open_call(real, path, flags, ap);
    va_end(ap);
    return fd;
}

/* Трассировка read(): какие fd и сколько читает игра. Логируем каждый
 * 1000-й вызов (иначе 10k строк/с), только под SUP_TRACE_READ. */
ssize_t read(int fd, void *buf, size_t n) {
    static ssize_t (*real)(int, void *, size_t);
    static unsigned long cnt;
    if (!real) real = (ssize_t (*)(int, void *, size_t))real_dlsym("read");
    ssize_t r = real(fd, buf, n);
    if (r > 0 && (++cnt % 1000) == 0 && getenv("SUP_TRACE_READ")) {
        char lb[64], tg[256];
        char line[400];
        int l = snprintf(lb, sizeof lb, "/proc/self/fd/%d", fd);
        ssize_t tl = -1;
        if (l > 0) tl = readlink(lb, tg, sizeof tg - 1);
        if (tl > 0) tg[tl] = 0; else snprintf(tg, sizeof tg, "?");
        int m = snprintf(line, sizeof line, "RD> #%lu fd=%d n=%zu -> %s\n",
                         cnt, fd, (size_t)r, tg);
        if (m > 0) (void)!write(2, line, (size_t)m);
    }
    return r;
}

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = (FILE *(*)(const char *, const char *))real_dlsym("fopen");
    if (path) {
        const char *wh = strstr(path, "d.google");
        const char *st = getenv("SUP_TRACE_OPEN");
        if (wh || st) {
            fprintf(stderr, "OPEN> fopen(%s, \"%s\") caller=%p\n",
                    path, mode, __builtin_return_address(0));
            if (wh) {
                fprintf(stderr, "OPEN>  *** contains d.google marker ***\n");
                fflush(stderr);
            }
        }
    }
    return real(path, mode);
}

void free(void *p) {
    static void (*real)(void *) = NULL;
    uintptr_t v = (uintptr_t)p;
    if (v >= 0x1000 && v < 0x400000) {
        fprintf(stderr, "WG> free(%p) guarded, caller=%p\n", p,
                __builtin_return_address(0));
        return;
    }
    if (!real) real = (void (*)(void *))real_dlsym("free");
    real(p);
}

void *realloc(void *p, size_t n) {
    static void *(*real)(void *, size_t) = NULL;
    uintptr_t v = (uintptr_t)p;
    if (v >= 0x1000 && v < 0x400000) {
        fprintf(stderr, "WG> realloc(%p, %zu) -> fresh, caller=%p\n", p, n,
                __builtin_return_address(0));
        return real_dlsym("malloc") ? ((void *(*)(size_t))real_dlsym("malloc"))(n) : NULL;
    }
    if (!real) real = (void *(*)(void *, size_t))real_dlsym("realloc");
    return real(p, n);
}

static int stdio_ok(FILE *fp) {
    uintptr_t v = (uintptr_t)fp;
    if (!fp || v < 0x4096 || v > (uintptr_t)0x7fffffffffff) return 0;
    /* mincore() требует адрес, выровненный по странице. Раньше он вызывался
     * с самим FILE* (например 0x...a4a0), всегда возвращал EINVAL — и каждый
     * настоящий поток glibc считался «чужим», поэтому vfprintf/fputc молча
     * теряли весь вывод. Проверяем страницу, содержащую START и END FILE. */
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t page = v & ~(uintptr_t)(ps - 1);
    unsigned char vec = 0;
    if (mincore((void *)page, (size_t)ps, &vec) != 0 || (vec & 1) == 0) return 0;
    if (v + sizeof(FILE) > page + (uintptr_t)ps) {
        vec = 0;
        if (mincore((void *)(page + (uintptr_t)ps), (size_t)ps, &vec) != 0 ||
            (vec & 1) == 0) return 0;
    }
    /* _flags — 32-битное поле; старшие 16 бит — магическое число 0xfbad. */
    uint32_t flags = *(uint32_t *)fp;
    return (flags & 0xffff0000u) == 0xfbad0000u;
}

static void stdio_log(const char *fn, FILE *fp) {
    if (getenv("SUP_IOSTRACE"))
        fprintf(stderr, "IO> %s(%p) bypassed, caller=%p\n", fn, (void *)fp,
                __builtin_return_address(0));
}

int getc(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return EOF;
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("getc");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> getc(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(fp);
}

int fgetc(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return EOF;
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("fgetc");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fgetc(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(fp);
}

char *fgets(char *s, int n, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp || n <= 0) return NULL;
    static char *(*real)(char *, int, FILE *) = NULL;
    if (!real) real = (char *(*)(char *, int, FILE *))real_dlsym("fgets");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fgets(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(s, n, fp);
}

int ungetc(int c, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return EOF;
    static int (*real)(int, FILE *) = NULL;
    if (!real) real = (int (*)(int, FILE *))real_dlsym("ungetc");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> ungetc(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(c, fp);
}

int fflush(FILE *fp) {
    fp = bionic_stream(fp);
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("fflush");
    if (fp == NULL) return real(fp);
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fflush(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(fp);
}

int feof(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return 1;
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("feof");
    if (!stdio_ok(fp)) return real(fp);
    return real(fp);
}

int ferror(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return 0;
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("ferror");
    return real(fp);
}

void clearerr(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return;
    static void (*real)(FILE *) = NULL;
    if (!real) real = (void (*)(FILE *))real_dlsym("clearerr");
    real(fp);
}

size_t fread(void *p, size_t s, size_t n, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp || !p) return 0;
    static size_t (*real)(void *, size_t, size_t, FILE *) = NULL;
    if (!real) real = (size_t (*)(void *, size_t, size_t, FILE *))real_dlsym("fread");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fread(%p, size=%zu, count=%zu) non-glibc stream. caller=%p\n", (void *)fp, s, n, __builtin_return_address(0));
    }
    return real(p, s, n, fp);
}

size_t fwrite(const void *p, size_t s, size_t n, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp || !p) return 0;
    static size_t (*real)(const void *, size_t, size_t, FILE *) = NULL;
    if (!real) real = (size_t (*)(const void *, size_t, size_t, FILE *))real_dlsym("fwrite");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fwrite(%p, size=%zu, count=%zu) non-glibc stream. caller=%p\n", (void *)fp, s, n, __builtin_return_address(0));
    }
    return real(p, s, n, fp);
}

int fputc(int c, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return EOF;
    /* Тот же bypass, что у vfprintf: битый/чужой FILE* не отдаём в glibc,
     * иначе __libc_fatal("invalid stdio handle") убивает процесс. */
    if (!stdio_ok(fp)) {
        if (getenv("SUP_IOSTRACE")) {
            char b[160];
            int L = snprintf(b, sizeof b,
                "IO> fputc(%p) non-glibc — bypassed, caller=%p\n",
                (void *)fp, __builtin_return_address(0));
            if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
        }
        return c;  /* имитируем успех, не трогая glibc */
    }
    static int (*real)(int, FILE *) = NULL;
    if (!real) real = (int (*)(int, FILE *))real_dlsym("fputc");
    return real(c, fp);
}

int fputs(const char *s, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp || !s) return EOF;
    static int (*real)(const char *, FILE *) = NULL;
    if (!real) real = (int (*)(const char *, FILE *))real_dlsym("fputs");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fputs(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(s, fp);
}

int fseek(FILE *fp, long off, int whence) {
    fp = bionic_stream(fp);
    if (!fp) return -1;
    static int (*real)(FILE *, long, int) = NULL;
    if (!real) real = (int (*)(FILE *, long, int))real_dlsym("fseek");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fseek(%p, off=%ld) non-glibc stream. caller=%p\n", (void *)fp, off, __builtin_return_address(0));
    }
    return real(fp, off, whence);
}

int fseeko(FILE *fp, off_t off, int whence) {
    fp = bionic_stream(fp);
    if (!fp) return -1;
    static int (*real)(FILE *, off_t, int) = NULL;
    if (!real) real = (int (*)(FILE *, off_t, int))real_dlsym("fseeko");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fseeko(%p, off=%lld) non-glibc stream. caller=%p\n", (void *)fp, (long long)off, __builtin_return_address(0));
    }
    return real(fp, off, whence);
}

long ftell(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return -1;
    static long (*real)(FILE *) = NULL;
    if (!real) real = (long (*)(FILE *))real_dlsym("ftell");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> ftell(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(fp);
}

off_t ftello(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return -1;
    static off_t (*real)(FILE *) = NULL;
    if (!real) real = (off_t (*)(FILE *))real_dlsym("ftello");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> ftello(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    return real(fp);
}

void rewind(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return;
    static void (*real)(FILE *) = NULL;
    if (!real) real = (void (*)(FILE *))real_dlsym("rewind");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> rewind(%p) non-glibc stream.\n", (void *)fp);
    }
    real(fp);
}

int fclose(FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp) return 0;
    static int (*real)(FILE *) = NULL;
    if (!real) real = (int (*)(FILE *))real_dlsym("fclose");
    if (!stdio_ok(fp) && getenv("SUP_IOSTRACE")) {
        fprintf(stderr, "IO> fclose(%p) non-glibc stream. caller=%p\n", (void *)fp, __builtin_return_address(0));
    }
    /* Двойной fclose() на один FILE* рушит внутренний список потоков glibc
     * -> следующий stdio-вызов падает с "invalid stdio handle" (SIGABRT,
     * который glibc поднимает со сброшенным обработчиком). Игра зовёт
     * fclose дважды с одинаковым адресом (libminecraftpe, видно в
     * SUP_IOSTRACE=1). Прогоняем второй вызов в никуда. */
    static FILE *closed[64];
    static int nclosed;
    for (int i = 0; i < nclosed; ++i)
        if (closed[i] == fp) {
            if (getenv("SUP_IOSTRACE")) {
                char b[128];
                int L = snprintf(b, sizeof b,
                    "IO> fclose(%p) ПОВТОРНЫЙ — пропущен, caller=%p\n",
                    (void *)fp, __builtin_return_address(0));
                if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
            }
            return 0;
        }
    if (nclosed < 64) closed[nclosed++] = fp;
    else { memmove(closed, closed + 1, 63 * sizeof(FILE *));
           closed[63] = fp; }
    return real(fp);
}

int fgetpos(FILE *fp, fpos_t *pos) {
    fp = bionic_stream(fp);
    if (!fp || !pos) return -1;
    static int (*real)(FILE *, fpos_t *) = NULL;
    if (!real) real = (int (*)(FILE *, fpos_t *))real_dlsym("fgetpos");
    return real(fp, pos);
}

int fsetpos(FILE *fp, const fpos_t *pos) {
    fp = bionic_stream(fp);
    if (!fp || !pos) return -1;
    static int (*real)(FILE *, const fpos_t *) = NULL;
    if (!real) real = (int (*)(FILE *, const fpos_t *))real_dlsym("fsetpos");
    return real(fp, pos);
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    fp = bionic_stream(fp);
    if (!fp || !fmt) return 0;
    if (!stdio_ok(fp)) {
        if (getenv("SUP_IOSTRACE"))
            fprintf(stderr, "IO> vfprintf(non-glibc fp=%p) bypassed\n", (void*)fp);
        return 0;  /* не вызываем glibc, избегаем __libc_fatal */
    }
    static int (*real)(FILE *, const char *, va_list) = NULL;
    if (!real) real = (int (*)(FILE *, const char *, va_list))real_dlsym("vfprintf");
    return real(fp, fmt, ap);
}

ssize_t getline(char **lp, size_t *np, FILE *fp) {
    fp = bionic_stream(fp);
    if (!fp || !lp || !np) return -1;
    static ssize_t (*real)(char **, size_t *, FILE *) = NULL;
    if (!real) real = (ssize_t (*)(char **, size_t *, FILE *))real_dlsym("getline");
    return real(lp, np, fp);
}

/* ---- getaddrinfo: конвертация результата glibc в раскладку bionic ----
 * Игра (Android/NDK) читает из struct addrinfo поля по смещениям:
 *   ai_family@4, ai_socktype@8, ai_protocol@0xc, ai_addrlen@0x10 (u32),
 *   ai_addr@0x20, ai_next@0x28; поле @0x18 игрой не читается.
 * glibc же кладёт ai_addr по 0x18, а по 0x20 — ai_canonname (NULL, так
 * как AI_CANONNAME не запрашивается) → краш bias 0xdf9dbbf:
 *   df9dbbb: mov 0x20(%rdi),%rax   (rax = ai_canonname = NULL)
 *   df9dbbf: movups (%rax),%xmm0    SEGV addr (nil)
 * Отдаём узлы своего формата: sockaddr лежит inline сразу за заголовком,
 * указатель продублирован и в 0x18 (позиция glibc), и в 0x20 (позиция
 * игры), ai_next@0x28 совпадает у обеих раскладок, ai_addrlen (u32)
 * совпадает тоже — поэтому чужие freeaddrinfo (и наше, ниже) освобождают
 * такие узлы корректно: один блок malloc на узел. */

struct sup_addrinfo {
    int      ai_flags;
    int      ai_family;
    int      ai_socktype;
    int      ai_protocol;
    uint32_t ai_addrlen;
    uint32_t _pad;
    void    *ai_addr_glibc;  /* +0x18: позиция ai_addr в glibc */
    void    *ai_addr;        /* +0x20: позиция ai_addr в bionic/игре */
    void    *ai_next;        /* +0x28: совпадает у glibc и bionic */
};

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    static int (*real)(const char *, const char *,
                       const struct addrinfo *, struct addrinfo **);
    if (!res) return EAI_FAIL;
    *res = NULL;
    if (!real) real = (int (*)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **))
                      real_dlsym("getaddrinfo");
    if (!real) return EAI_FAIL;
    struct addrinfo *r = NULL;
    int rc = real(node, service, hints, &r);
    if (rc != 0 || !r) return rc;

    struct sup_addrinfo *head = NULL, *tail = NULL;
    for (struct addrinfo *p = r; p; p = p->ai_next) {
        size_t slen = p->ai_addrlen;
        if (slen > sizeof(struct sockaddr_storage))
            slen = sizeof(struct sockaddr_storage);
        struct sup_addrinfo *n = calloc(1, sizeof(*n) + slen);
        if (!n) {
            while (head) {
                struct sup_addrinfo *nx = head->ai_next;
                free(head);
                head = nx;
            }
            void (*rf)(struct addrinfo *) = (void (*)(struct addrinfo *))
                                            real_dlsym("freeaddrinfo");
            if (rf) rf(r);
            return EAI_MEMORY;
        }
        n->ai_flags = p->ai_flags;
        n->ai_family = p->ai_family;
        n->ai_socktype = p->ai_socktype;
        n->ai_protocol = p->ai_protocol;
        n->ai_addrlen = (uint32_t)p->ai_addrlen;
        n->ai_addr_glibc = (char *)n + sizeof(*n);
        n->ai_addr = n->ai_addr_glibc;
        if (p->ai_addr && slen)
            memcpy((char *)n + sizeof(*n), p->ai_addr, slen);
        if (tail)
            tail->ai_next = n;
        else
            head = n;
        tail = n;
    }
    void (*rf)(struct addrinfo *) = (void (*)(struct addrinfo *))
                                    real_dlsym("freeaddrinfo");
    if (rf) rf(r);
    *res = (struct addrinfo *)head;

    static int gai_log_count;
    if (gai_log_count < 16) {
        ++gai_log_count;
        fprintf(stderr,
                "GAI> getaddrinfo(%s%s%s, fam=%d, st=%d) -> %d, node fam=%d "
                "st=%d proto=%d len=%u addr=%p caller=%p\n",
                node ? node : "(null)",
                service ? ":" : "", service ? service : "",
                hints ? hints->ai_family : -1,
                hints ? hints->ai_socktype : -1,
                rc,
                head->ai_family, head->ai_socktype, head->ai_protocol,
                (unsigned)head->ai_addrlen, head->ai_addr,
                __builtin_return_address(0));
        fflush(stderr);
    }
    return rc;
}

void freeaddrinfo(struct addrinfo *ai) {
    while (ai) {
        struct sup_addrinfo *n = (struct sup_addrinfo *)ai;
        struct sup_addrinfo *next = n->ai_next;
        free(n);
        ai = (struct addrinfo *)next;
    }
}

int timer_create(clockid_t cid, struct sigevent *evp, timer_t *out) {
    static int (*real)(clockid_t, struct sigevent *, timer_t *);
    if (!real) real = (int (*)(clockid_t, struct sigevent *, timer_t *))
                          real_dlsym("timer_create");
    fprintf(stderr, "TIMER> timer_create(cid=%d notify=%d sig=%d)\n",
            (int)cid, evp ? evp->sigev_notify : -1,
            evp && (evp->sigev_notify == SIGEV_SIGNAL ||
                    evp->sigev_notify == SIGEV_THREAD) ? evp->sigev_signo : -1);
    if (evp && evp->sigev_notify == SIGEV_THREAD) {
        fprintf(stderr, "TIMER>   + SIGEV_THREAD func=%p params=%p caller=%p\n",
                evp->sigev_notify_function, evp->sigev_notify_attributes,
                __builtin_return_address(0));
    }
    return real(cid, evp, out);
}

int timer_settime(timer_t t, int flags, const struct itimerspec *new0,
                  struct itimerspec *old) {
    static int (*real)(timer_t, int, const struct itimerspec *,
                       struct itimerspec *);
    if (!real) real = (int (*)(timer_t, int, const struct itimerspec *,
                               struct itimerspec *))real_dlsym("timer_settime");
    if (new0) {
        fprintf(stderr, "TIMER> timer_settime(t=%p period=%lld.%09dns)\n",
                (void *)t, (long long)new0->it_interval.tv_sec,
                (int)new0->it_interval.tv_nsec);
    }
    return real(t, flags, new0, old);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [T%ld] ", tag ? tag : "?", (long)syscall(186));
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    fprintf(stderr, "[%s] [T%ld] ", tag ? tag : "?", (long)syscall(186));
    vfprintf(stderr, fmt, ap);
    return 0;
}

int __android_log_write(int prio, const char *tag, const char *msg) {
    (void)prio;
    fprintf(stderr, "[%s] [T%ld] %s\n", tag ? tag : "?", (long)syscall(186), msg);
    return 0;
}

int android_get_application_target_sdk_version(void) { return 33; }

int __system_property_get(const char *name, char *value) {
    const char *v = getenv(name);
    if (!v) { value[0] = 0; return 0; }
    strncpy(value, v, 92);
    value[92] = 0;
    return (int)strlen(value);
}

/* ---- real EGL via dlopen ---- */

static void *g_real_egl;
static EGLDisplay (*real_eglGetDisplay)(EGLNativeDisplayType);
static EGLBoolean (*real_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*real_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext (*real_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLSurface (*real_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLBoolean (*real_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*real_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*real_eglTerminate)(EGLDisplay);
static EGLBoolean (*real_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static const char *(*real_eglQueryString)(EGLDisplay, EGLint);
static EGLint (*real_eglGetError)(void);

/* Счётчики реальных входов в EGL-хуки. Нужны потому, что текстовый трейс
 * можно обойти (игра зовёт символы мимо наших fprintf), а счётчик
 * инкрементится в самой точке входа и не обойдётся. */
unsigned long sup_egl_counts[24];
static const char *sup_egl_names[24] = {
    "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglCreateContext",
    "eglCreateWindowSurface", "eglMakeCurrent", "eglSwapBuffers",
    "eglDestroySurface", "eglDestroyContext", "eglTerminate",
    "eglGetConfigs[stub]", "eglGetConfigAttrib[stub]", "eglBindAPI[stub]",
    "eglSwapInterval[stub]", "eglQueryString[stub]",
    "eglGetCurrentContext[stub]", "eglGetCurrentSurface[stub]",
    "eglGetCurrentDisplay[stub]", "eglQuerySurface[stub]",
    "eglCreatePbufferSurface", "eglGetError[stub]",
    "eglGetProcAddress[stub]", "eglReleaseThread", "(free23)"
};
#define EGL_CTR(i) __atomic_add_fetch(&sup_egl_counts[i], 1UL, __ATOMIC_RELAXED)

void sup_egl_dump_counts(void) {
    fprintf(stderr, "EGLCTR> === прямые счётчики входа в EGL-хуки ===\n");
    unsigned long total = 0;
    for (int i = 0; i < 24; ++i) {
        if (!sup_egl_counts[i]) continue;
        fprintf(stderr, "EGLCTR> %-24s = %lu\n", sup_egl_names[i],
                sup_egl_counts[i]);
        total += sup_egl_counts[i];
    }
    fprintf(stderr, "EGLCTR> ВСЕГО вызовов EGL через мост = %lu\n", total);
    if (!total)
        fprintf(stderr, "EGLCTR> ВЫВОД: движок НЕ заходил в EGL ни разу\n");
    fflush(stderr);
}

static void sup_egl_init(void) {
    if (g_real_egl) return;
    g_real_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_real_egl) {
        fprintf(stderr, "EGL> no libEGL.so.1, using stubs\n");
        fflush(stderr);
        return;
    }
    *(void **)&real_eglGetDisplay = dlsym(g_real_egl, "eglGetDisplay");
    *(void **)&real_eglInitialize = dlsym(g_real_egl, "eglInitialize");
    *(void **)&real_eglChooseConfig = dlsym(g_real_egl, "eglChooseConfig");
    *(void **)&real_eglCreateContext = dlsym(g_real_egl, "eglCreateContext");
    *(void **)&real_eglCreateWindowSurface = dlsym(g_real_egl, "eglCreateWindowSurface");
    *(void **)&real_eglMakeCurrent = dlsym(g_real_egl, "eglMakeCurrent");
    *(void **)&real_eglSwapBuffers = dlsym(g_real_egl, "eglSwapBuffers");
    *(void **)&real_eglDestroySurface = dlsym(g_real_egl, "eglDestroySurface");
    *(void **)&real_eglDestroyContext = dlsym(g_real_egl, "eglDestroyContext");
    *(void **)&real_eglTerminate = dlsym(g_real_egl, "eglTerminate");
    *(void **)&real_eglGetConfigs = dlsym(g_real_egl, "eglGetConfigs");
    *(void **)&real_eglQueryString = dlsym(g_real_egl, "eglQueryString");
    *(void **)&real_eglGetError = dlsym(g_real_egl, "eglGetError");
    fprintf(stderr, "EGL> real libEGL loaded: display=%p init=%p\n",
            (void *)real_eglGetDisplay, (void *)real_eglInitialize);
    fflush(stderr);
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType d) {
    EGL_CTR(0);
    sup_egl_init();
    if (real_eglGetDisplay) {
        EGLDisplay r = real_eglGetDisplay(d);
        fprintf(stderr, "EGL> eglGetDisplay -> %p\n", r);
        fflush(stderr);
        return r;
    }
    return (EGLDisplay)1;
}

EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) {
    EGL_CTR(1);
    sup_egl_init();
    if (real_eglInitialize) {
        EGLBoolean r = real_eglInitialize(d, major, minor);
        fprintf(stderr, "EGL> eglInitialize -> %d (major=%d minor=%d)\n", r,
                major ? *major : -1, minor ? *minor : -1);
        fflush(stderr);
        return r;
    }
    if (major) *major = 1;
    if (minor) *minor = 5;
    return 1;
}

EGLBoolean eglTerminate(EGLDisplay d) {
    EGL_CTR(9);
    sup_egl_init();
    if (real_eglTerminate) return real_eglTerminate(d);
    return 1;
}

EGLint eglGetError(void) { EGL_CTR(20); return EGL_SUCCESS; }

EGLBoolean eglBindAPI(unsigned int api) { EGL_CTR(12); return 1; }

const char *eglQueryString(EGLDisplay d, EGLint name) {
    EGL_CTR(14);
    /* Коды EGL/egl.h: 0x3053 VENDOR, 0x3054 VERSION, 0x3055 EXTENSIONS,
     * 0x308D CLIENT_APIS. Игра проверяет наличие EGL_ANDROID_recordable
     * через strstr по строке EXTENSIONS — раньше мы возвращали одну
     * строку на все коды, и проверка гарантированно проваливалась. */
    const char *r;
    switch (name) {
    case 0x3053: r = "Mesa Project"; break;
    case 0x3054: r = "1.5"; break;
    case 0x3055:
        /* Минимальный ответ: только то, что движок реально проверяет
         * (strstr по "EGL_ANDROID_recordable"). Длинная строка ранее
         * вызывала std::out_of_range при разборе. */
        r = "EGL_ANDROID_recordable";
        break;
    case 0x308D: r = "OpenGL ES OpenGL ES2 OpenGL ES3"; break;
    default:
        r = "";
        break;
    }
    { char b[600]; int L = snprintf(b, sizeof b,
            "EGLQ> eglQueryString(name=0x%x dpy=%p) len=%d\n", name, (void *)d, (int)strlen(r));
      if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; } }
    return r;
}

EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *c, EGLint size, EGLint *n) {
    EGL_CTR(10);
    if (n) *n = 0;
    return 1;
}

static EGLConfig g_cfg = (EGLConfig)1;

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *
    attrs, EGLConfig *cfg,
                           EGLint size, EGLint *n) {
    { char b[160]; int L = snprintf(b, sizeof b,
            "RAWHOOK> eglChooseConfig enter dpy=%p attrs=%p cfg=%p size=%d n=%p\n",
            (void *)d, (const void *)attrs, (void *)cfg, (int)size, (void *)n);
      if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; } }
    if (attrs) {
        char ab[700]; int p = 0, k;
        p += snprintf(ab + p, sizeof ab - p, "ATTRS>");
        for (k = 0; k < 40 && p < (int)sizeof ab - 40; ++k) {
            EGLint key = attrs[2 * k], val = attrs[2 * k + 1];
            p += snprintf(ab + p, sizeof ab - p, " %x=%x", key, val);
            if (key == 0x3038) break;
        }
        if (p < (int)sizeof ab - 2) { ab[p++] = '\n'; }
        if (p > 0) { ssize_t w = write(2, ab, (size_t)p); (void)w; }
    }
    EGL_CTR(2);
    sup_egl_init();
    if (real_eglChooseConfig) {
        EGLBoolean r = real_eglChooseConfig(d, attrs, cfg, size, n);
        fprintf(stderr, "EGL> eglChooseConfig -> %d (n=%d, cfg=%p)\n", r,
                n ? *n : -1, (void *)(cfg ? cfg[0] : NULL));
        fflush(stderr);
        if (!r || (n && *n == 0)) {
            EGLint total = -1;
            if (real_eglGetConfigs) real_eglGetConfigs(d, NULL, 0, &total);
            EGLint err = real_eglGetError ? real_eglGetError() : -1;
            fprintf(stderr, "EGLDIAG> vendor=%s version=%s total_configs=%d eglerr=%d(0x%x)\n",
                    (real_eglQueryString && d) ? real_eglQueryString(d, 0x3053) : "?",
                    (real_eglQueryString && d) ? real_eglQueryString(d, 0x3054) : "?",
                    (int)total, (int)err, (unsigned)err);
            fflush(stderr);
            /* Пробы: 1=без ANDROID-атрибута 0x3142, 2=ещё без RENDERABLE_TYPE. */
            for (int var = 1; var <= 2 && (!n || *n == 0); ++var) {
                EGLint filt[64]; int m = 0, k;
                if (attrs) {
                    for (k = 0; k < 40 && m < 60; ++k) {
                        EGLint key = attrs[2 * k], val = attrs[2 * k + 1];
                        if (key == 0x3038) break;
                        if (var >= 1 && key == 0x3142) continue;
                        if (var >= 2 && key == 0x3040) continue;
                        filt[m++] = key; filt[m++] = val;
                    }
                }
                filt[m++] = 0x3038; filt[m++] = 0;
                EGLint n2 = -1;
                EGLBoolean r2 = real_eglChooseConfig(d, filt, cfg, size, &n2);
                fprintf(stderr, "EGLDIAG> probe var=%d r=%d n=%d cfg=%p\n",
                        var, (int)r2, (int)n2, (void *)(cfg && n2 > 0 ? cfg[0] : NULL));
                fflush(stderr);
                if (r2 && n2 > 0 && n) *n = n2;
            }
            if (n && *n > 0) return 1;
        }
        return r;
    }
    if (n) *n = 0;
    if (cfg && n && size > 0) { cfg[0] = g_cfg; *n = 1; }
    return 1;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v) {
    switch (a) {
        case EGL_RED_SIZE: *v = 8; break;
        case EGL_GREEN_SIZE: *v = 8; break;
        case EGL_BLUE_SIZE: *v = 8; break;
        case EGL_ALPHA_SIZE: *v = 8; break;
        case EGL_DEPTH_SIZE: *v = 24; break;
        case EGL_STENCIL_SIZE: *v = 8; break;
        case EGL_SURFACE_TYPE: *v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
        case EGL_RENDERABLE_TYPE: *v = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
        default: *v = 0; break;
    }
    return 1;
}

EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext
    share,
                            const EGLint *attrs) {
    EGL_CTR(3);
    sup_egl_init();
    if (real_eglCreateContext) {
        EGLContext r = real_eglCreateContext(d, c, share, attrs);
        fprintf(stderr, "EGL> eglCreateContext -> %p\n", r);
        fflush(stderr);
        return r;
    }
    return (EGLContext)0x51;
}

EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext ctx) {
    EGL_CTR(8);
    sup_egl_init();
    if (real_eglDestroyContext) return real_eglDestroyContext(d, ctx);
    return 1;
}

/* X11-окно из runner'а. Передаётся сеттером (а не extern-ссылкой),
 * потому что runner не линкуется с -rdynamic и его символы не видны
 * при dlopen(libsupcompat, RTLD_NOW). */
static void *g_x11_display;
static unsigned long g_x11_window;
static void *g_fake_anative_window;

void sup_set_x11_window(void *dpy, unsigned long win, void *fake) {
    g_x11_display = dpy;
    g_x11_window = win;
    g_fake_anative_window = fake;
    fprintf(stderr, "EGL> sup_set_x11_window dpy=%p win=%#lx fake=%p\n",
            dpy, win, fake);
    fflush(stderr);
}

EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType
    w,
                                  const EGLint *attrs) {
    EGL_CTR(4);
    sup_egl_init();
    EGLNativeWindowType w2 = w;
    /* Игра отдаёт fake ANativeWindow* (заглушку из runner'а), а десктопный
     * EGL ожидает XID. Меняем на реальное окно. */
    if (w && g_x11_window && (void *)w == g_fake_anative_window) {
        w2 = (EGLNativeWindowType)g_x11_window;
        fprintf(stderr, "EGL> conv fake ANativeWindow %p -> XID %#lx\n",
                (void *)w, g_x11_window);
        fflush(stderr);
    }
    if (attrs) {
        char ab[400]; int p = 0, k;
        p += snprintf(ab + p, sizeof ab - p, "SATTRS>");
        for (k = 0; k < 20 && p < (int)sizeof ab - 40; ++k) {
            EGLint key = attrs[k];
            p += snprintf(ab + p, sizeof ab - p, " %x", (unsigned)key);
            if (key == 0x3038) break;
        }
        if (p < (int)sizeof ab - 2) ab[p++] = '\n';
        if (p > 0) { ssize_t wr = write(2, ab, (size_t)p); (void)wr; }
    }
    if (real_eglCreateWindowSurface) {
        EGLSurface r = real_eglCreateWindowSurface(d, c, w2, attrs);
        fprintf(stderr, "EGL> eglCreateWindowSurface(dpy=%p, cfg=%p, win=%p) -> %p\n",
                d, c, w2, r);
        fflush(stderr);
        return r;
    }
    return (EGLSurface)w2;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attrs) {
    EGL_CTR(19);
    return (EGLSurface)0x52;
}

EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) {
    EGL_CTR(7);
    sup_egl_init();
    if (real_eglDestroySurface) return real_eglDestroySurface(d, s);
    return 1;
}

EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    EGL_CTR(5);
    sup_egl_init();
    if (real_eglMakeCurrent) {
        EGLBoolean r = real_eglMakeCurrent(d, draw, read, ctx);
        fprintf(stderr, "EGL> eglMakeCurrent -> %d\n", r);
        fflush(stderr);
        return r;
    }
    return 1;
}

/* Визуальная верификация: снимаем задний буфер первых свапов в PPM
 * (X11-захват под композитингом KWin для root даёт чёрное). */
static void sup_dump_frame(int seq) {
    typedef void *(*EGPA_t)(const char *);
    static EGPA_t egpa;
    static void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    static void (*glGetIntegerv)(unsigned, int *);
    if (!egpa && g_real_egl)
        *(void **)&egpa = dlsym(g_real_egl, "eglGetProcAddress");
    if (!glReadPixels && egpa) {
        glReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
                egpa("glReadPixels");
        glGetIntegerv = (void (*)(unsigned, int *))egpa("glGetIntegerv");
    }
    if (!glReadPixels || !glGetIntegerv) {
        fprintf(stderr, "FRAME> #%d: glReadPixels недоступен\n", seq);
        return;
    }
    int vp[4] = {0, 0, 0, 0};
    glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fprintf(stderr, "FRAME> #%d: буфер %dx%d — пропуск\n", seq, w, h);
        return;
    }
    unsigned char *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
    char path[128];
    snprintf(path, sizeof path, "/tmp/opencode/frame_%d.ppm", seq);
    FILE *f = fopen(path, "wb");
    if (f) {
        double sum = 0;
        long n = 0;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y)   /* GL: строка 0 снизу */
            for (int x = 0; x < w; ++x) {
                unsigned char *p = px + ((size_t)y * (size_t)w + x) * 4;
                fwrite(p, 1, 3, f);
                sum += p[0] + p[1] + p[2];
                n += 3;
            }
        fclose(f);
        fprintf(stderr, "FRAME> #%d %dx%d -> %s mean=%.2f\n", seq, w, h, path,
                n ? sum / n : 0.0);
    }
    free(px);
}

EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    EGL_CTR(6);
    static int count = 0;
    long tid = (long)syscall(186);
    g_swap_tid = tid;
    if (count < 10) {
        fprintf(stderr, "EGL> eglSwapBuffers(dpy=%p, surf=%p) tid=%ld\n", d, s, tid);
        fflush(stderr);
    }
    if (count == 0) {
        char path[64];
        int l = snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
        if (l > 0) {
            int f = open(path, O_RDONLY);
            if (f >= 0) {
                char nm[64];
                ssize_t r = read(f, nm, sizeof nm - 1);
                if (r > 0) {
                    nm[r] = 0;
                    for (int k = 0; k < r; ++k) if (nm[k] == '\n') nm[k] = 0;
                    fprintf(stderr, "SWAPCALL> имя рендер-потока tid=%ld: \"%s\"\n", tid, nm);
                    fflush(stderr);
                }
                close(f);
            }
        }
        void *bt[32];
        int n = backtrace(bt, 32);
        char **sy = backtrace_symbols(bt, n);
        fprintf(stderr, "SWAPCALL> стек первого eglSwapBuffers (tid=%ld):\n", tid);
        for (int i = 0; i < n; ++i) fprintf(stderr, "  %s\n", sy[i]);
        free(sy);
        fflush(stderr);
    }
    count++;
    if (count % 10000 == 0) {
        long rss = 0;
        FILE *st = fopen("/proc/self/statm", "r");
        if (st) {
            long a = 0, b = 0;
            if (fscanf(st, "%ld %ld", &a, &b) == 2) rss = b;
            fclose(st);
        }
        fprintf(stderr, "SWAPS> %d кадров, rss=%ld КБ, tid=%ld\n", count,
                rss * 4, tid);
        fflush(stderr);
    }
    sup_egl_init();
    sup_note_swap();
    if (count <= 10 || count % 100000 == 0) sup_dump_frame(count);
    /* Дамп по требованию: touch /tmp/opencode/dump_req — снимем ближайший
     * (в пределах 1000 кадров) кадр, чтобы глазами проверить картинку. */
    if (count % 1000 == 0 && access("/tmp/opencode/dump_req", F_OK) == 0) {
        unlink("/tmp/opencode/dump_req");
        sup_dump_frame(count);
    }
    if (real_eglSwapBuffers) return real_eglSwapBuffers(d, s);
    return 1;
}

EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { EGL_CTR(13); return 1; }

EGLContext eglGetCurrentContext(void) { EGL_CTR(15); return (EGLContext)0x51; }
EGLSurface eglGetCurrentSurface(EGLint t) { EGL_CTR(16); return (EGLSurface)0x52; }
EGLDisplay eglGetCurrentDisplay(void) { EGL_CTR(17); return (EGLDisplay)1; }

EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
    EGL_CTR(18);
    switch (a) {
        case EGL_WIDTH: *v = 1280; break;
        case EGL_HEIGHT: *v = 720; break;
        default: *v = 0; break;
    }
    return 1;
}

EGLBoolean eglReleaseThread(void) {
    EGL_CTR(22); return 1; }

void *eglGetProcAddress(const char *name) {
    EGL_CTR(21);
    if (name) {
        void *local = sup_local_egl_symbol(name);
        if (local) {
            fprintf(stderr, "EGLPA> %s -> bridge hook %p\n", name, local);
            fflush(stderr);
            return local;
        }
    }
    return sup_gl_resolve(name); }

/* ---- перехват __cxa_throw: печатаем тип исключения --------------------
 * Игра бросает std::out_of_range, он не пойман, libc++ зовёт
 * android_set_abort_message("terminating") -> abort. Без перехвата мы не
 * видим ни тип, ни место броска. */
extern void __cxa_throw(void *, void *, void (*)(void *));
/* Оригинал берём из libc++_shared через лоадер (см. loader.c). Сам через
 * dlsym(RTLD_NEXT) его достать нельзя: libsupcompat от libc++_shared не
 * зависит, а RTLD_DEFAULT вернул бы нас самих -> бесконечная рекурсия. */
void *sup_cxa_throw_real_pub;
void sup_set_cxa_throw_pub(void *p) {
    if (p) sup_cxa_throw_real_pub = p;
}
void __cxa_throw(void *thrown, void *tinfo, void (*dtor)(void *)) {
    static void (*real)(void *, void *, void (*)(void *));
    if (!real)
        real = (void (*)(void *, void *, void (*)(void *)))sup_cxa_throw_real_pub;
    if (!real) _exit(97);
    void *(*cur)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT,
                                                 "__cxa_current_exception_type");
    char *(*dem)(const char *, int *, void *, void *) =
        (char *(*)(const char *, int *, void *, void *))dlsym(RTLD_DEFAULT,
                                                              "__cxa_demangle");
    void *ti = tinfo ? tinfo : (cur ? cur() : 0);
    const char *nm = ti ? *(const char *const *)ti : "?";
    int st = 0;
    char *dn = dem ? dem(nm, 0, 0, &st) : 0;
    char b[512];
    int L = snprintf(b, sizeof b, "THROW> исключение: %s\n", dn ? dn : nm);
    if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    if (dn) free(dn);
    void *bt[16];
    int n = backtrace(bt, 16);
    Dl_info di;
    for (int i = 1; i < n && i < 8; ++i) {
        const char *sn = 0;
        if (dladdr(bt[i], &di)) sn = di.dli_sname ? di.dli_sname : di.dli_fname;
        L = snprintf(b, sizeof b, "THROW>   %p <%s>\n", bt[i], sn ? sn : "?");
        if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    }
    real(thrown, tinfo, dtor);
}

/* ---- перехват __throw_out_of_range ---------------------------------
 * Игра инстанцирует std::vector::at() у себя и зовёт этот symbol из
 * libc++_shared через PLT -> лоадер резолвит его здесь. Печатаем стек
 * (через write(2), stdio битый), затем вызываем оригинал. */
void *sup_oor_real_pub;
void sup_set_oor_pub(void *p) {
    if (p) sup_oor_real_pub = p;
}
void sup_throw_oor_hook(void) __asm__("_ZNKSt6__ndk120__vector_base_commonILb1EE20__throw_out_of_rangeEv");
void sup_throw_oor_hook(void) {
    char b[256];
    int L = snprintf(b, sizeof b, "OOR> __throw_out_of_range вызван\n");
    if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    void *bt[20];
    int n = backtrace(bt, 20);
    Dl_info di;
    for (int i = 1; i < n && i < 14; ++i) {
        const char *sn = 0; const char *fn = 0;
        if (dladdr(bt[i], &di)) { sn = di.dli_sname; fn = di.dli_fname; }
        L = snprintf(b, sizeof b, "OOR>   %p <%s> %s\n", bt[i],
                     sn ? sn : "?", fn ? fn : "");
        if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    }
    fflush(stderr);
    void (*real)(void) = (void (*)(void))sup_oor_real_pub;
    if (real) real();
    _exit(96);
}
