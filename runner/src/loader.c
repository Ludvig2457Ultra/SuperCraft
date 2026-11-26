#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "jni_min.h"

/* ---- emulated TLS (bionic __emutls) ----
 * The game's emutls control blocks use the bionic/Android layout
 *   { size_t size; size_t align; void *tdata; }
 * (see 0x102b1130 = {0x9e8, 8, 0}, 0x102b1150 = {0x10, 8, 0}).
 * libc++_shared's own LLVM __emutls_get_address reads the compiler-rt
 * layout {tdata, size, align}, so it allocates size=8 instead of 0x9e8
 * and the rtc mt19937 init overwrites the heap.  Provide a bionic-compatible
 * implementation and give it precedence over the weak libc++_shared one. */

typedef struct sup_emutls_slot {
    void *ctrl;
    void *mem;
} sup_emutls_slot;

// Увеличиваем лимит слотов до 512, чтобы вместить все TLS переменные Bedrock
#define SUP_EMUTLS_MAX_SLOTS 512

static pthread_key_t g_sup_emu_key;
static pthread_once_t g_sup_emu_once = PTHREAD_ONCE_INIT;

// Деструктор автоматически очищает память при закрытии потоков игры
static void sup_emu_destroy_map(void *mapv) {
    if (mapv) {
        sup_emutls_slot *map = (sup_emutls_slot *)mapv;
        for (int i = 0; i < SUP_EMUTLS_MAX_SLOTS; ++i) {
            if (map[i].mem) free(map[i].mem);
        }
        free(map);
    }
}

static void sup_emu_make_key(void) {
    pthread_key_create(&g_sup_emu_key, sup_emu_destroy_map);
}

// Изменяем на глобальное имя __emutls_get_address, чтобы гарантированно
// перехватить вызовы оригинального движка и libc++_shared.so
void *__emutls_get_address(void *ctrlv) {
    pthread_once(&g_sup_emu_once, sup_emu_make_key);
    size_t *ctl = (size_t *)ctrlv;
    size_t size = ctl[0];
    size_t align = ctl[1];
    void *tdata = (void *)ctl[2];
    {
        if (align < sizeof(void *)) align = sizeof(void *);
        else {
            size_t a = sizeof(void *);
            while (a < align) a <<= 1;
            align = a;
        }
    }

    sup_emutls_slot *map = pthread_getspecific(g_sup_emu_key);
    if (!map) {
        map = calloc(SUP_EMUTLS_MAX_SLOTS, sizeof(*map));
        if (!map) {
            fprintf(stderr, "[FATAL] emutls: calloc failed\n");
            _exit(1);
        }
        pthread_setspecific(g_sup_emu_key, map);
    }

    for (int i = 0; i < SUP_EMUTLS_MAX_SLOTS; ++i) {
        if (map[i].ctrl == ctrlv) return map[i].mem;
        if (!map[i].ctrl) {
            void *mem = NULL;
            size_t alloc_size = size ? size : 1;
            if (posix_memalign(&mem, align, alloc_size) != 0) {
                fprintf(stderr, "[FATAL] emutls: posix_memalign failed (align=%zu, size=%zu)\n", align, alloc_size);
                return NULL;
            }
            if (size) {
                memset(mem, 0, size);
                if (tdata) memcpy(mem, tdata, size);
            }
            map[i].ctrl = ctrlv;
            map[i].mem = mem;
            return mem;
        }
    }
    fprintf(stderr, "[FATAL] emutls: slot table full! Exceeded %d slots for %p\n", SUP_EMUTLS_MAX_SLOTS, ctrlv);
    _exit(105);
}


/* ---- Android-only libc stubs ----
 * NativeActivity libs import Android libc symbols that glibc lacks.
 * Returning NULL leaves a NULL PLT/GOT slot -> jump to 0 -> SEGV (nil).
 * Provide no-op semantics for the handful the engine actually touches. */

static void sup_stub_android_set_abort_message(const char *msg);
static int sup_stub_android_log_print(int prio, const char *tag, const char *fmt, ...);
static int sup_stub_android_log_vprint(int prio, const char *tag, const char *fmt,
                                       va_list ap);

static void *sup_android_stub(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "android_set_abort_message") == 0)
        return (void *)sup_stub_android_set_abort_message;
    if (strcmp(name, "__android_log_print") == 0)
        return (void *)sup_stub_android_log_print;
    if (strcmp(name, "__android_log_vprint") == 0)
        return (void *)sup_stub_android_log_vprint;
    return NULL;
}

static void sup_stub_android_set_abort_message(const char *msg) {
    fprintf(stderr, "StubABORT: %s\n", msg ? msg : "(null)");
    fflush(stderr);
    /* Точка, где libc++ уже знает об uncaught exception. Печатаем тип и
     * стек через write(2), т.к. stdio может быть битым. */
    void *(*cur)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT,
                                                 "__cxa_current_exception_type");
    char *(*dem)(const char *, int *, void *, void *) =
        (char *(*)(const char *, int *, void *, void *))dlsym(RTLD_DEFAULT,
                                                              "__cxa_demangle");
    void *ti = cur ? cur() : 0;
    char b[512];
    int L = snprintf(b, sizeof b, "ABORT> тип исключения: %s\n",
                     ti ? *(const char *const *)ti : "(нет)");
    if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    if (ti) {
        const char *nm = *(const char *const *)ti;
        int st = 0;
        char *dn = dem ? dem(nm, 0, 0, &st) : 0;
        if (dn) {
            L = snprintf(b, sizeof b, "ABORT>   (демангл) %s\n", dn);
            if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
            free(dn);
        }
    }
    void *bt[20];
    int n = backtrace(bt, 20);
    Dl_info di;
    for (int i = 0; i < n && i < 12; ++i) {
        const char *sn = 0; const char *fn = 0;
        if (dladdr(bt[i], &di)) { sn = di.dli_sname; fn = di.dli_fname; }
        L = snprintf(b, sizeof b, "ABORT>   %p <%s> %s\n", bt[i],
                     sn ? sn : "?", fn ? fn : "");
        if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    }
    fflush(stderr);
}

static int sup_stub_android_log_vprint(int prio, const char *tag, const char *fmt,
                                       va_list ap) {
    (void)prio;
    fprintf(stderr, "[%s] ", tag ? tag : "android");
    if (fmt) vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return 0;
}

static int sup_stub_android_log_print(int prio, const char *tag, const char *fmt,
                                      ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = sup_stub_android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return n;
}

#ifndef DT_IRELA
#define DT_IRELA 0x70000000
#endif
#ifndef DT_IRELASZ
#define DT_IRELASZ 0x70000001
#endif
#ifndef R_X86_64_IRELATIVE
#define R_X86_64_IRELATIVE 37
#endif

static const char *g_game_path;
static int g_skip_init = 0;
static int g_verbose = 0;

typedef struct {
    const char *path;
    void *base;
    size_t total_size;
    uintptr_t xr_start[8];
    uintptr_t xr_end[8];
    size_t xr_count;
    ElfW(Phdr) *phdrs;
    size_t phnum;
    ElfW(Dyn) *dyn;
    size_t dyn_count;
    ElfW(Sym) *dynsym;
    size_t dynsym_count;
    char *dynstr;
    size_t dynstr_size;
    uint32_t *gnu_bucket;
    uint32_t *gnu_chain;
    uint32_t gnu_nbucket;
    uint32_t gnu_symoffset;
    uint32_t gnu_bloom_size;
    uint32_t gnu_bloom_shift;
    uint64_t *gnu_bloom;
    uint32_t *sysv_bucket;
    uint32_t *sysv_chain;
    uint32_t sysv_nbucket;
    void (*dt_init)(void);
    void (*dt_fini)(void);
    void (**init_array)(void);
    size_t init_array_count;
    void (**preinit_array)(void);
    size_t preinit_array_count;
    ElfW(Rela) *rela_dyn;
    size_t rela_dyn_count;
    ElfW(Rela) *rela_plt;
    size_t rela_plt_count;
    ElfW(Rela) *irela;
    size_t irela_count;
    char **needed;
    size_t needed_count;
    int has_textrel;
    int has_gnu_relro;
    void *relro_start;
    size_t relro_size;
    char **unresolved;
    size_t unresolved_count;
    size_t unresolved_cap;
    void **rcache;
    size_t rcache_n;
    size_t reloc_applied;
    uintptr_t eh_frame;      /* runtime адрес .eh_frame (для libgcc-анвинда) */
} Obj;

static Obj g_obj;
static Obj g_deps[8];
static size_t g_ndep;

/* Диапазоны исполняемых сегментов сюда отдаём в libsupcompat.so, где живёт
 * sup_ptr_in_exec(). Функция лежит именно там, потому что loader.c не входит
 * в состав libsupcompat.so: extern-ссылка на неё из jni_runtime.c сделала бы
 * dlopen(RTLD_NOW) неуспешным и повалила бы все хуки разом. */
static void (*sup_exec_add_fn)(uintptr_t, uintptr_t) = NULL;

static void sup_exec_publish(Obj *o) {
    if (!sup_exec_add_fn || !o) return;
    for (size_t i = 0; i < o->xr_count; ++i)
        sup_exec_add_fn(o->xr_start[i], o->xr_end[i]);
}

static void *hp_alloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { perror("calloc"); _exit(1); }
    return p;
}

static void note_unresolved(Obj *o, const char *name) {
    if (o->unresolved_count == o->unresolved_cap) {
        o->unresolved_cap = o->unresolved_cap ? o->unresolved_cap * 2 : 64;
        o->unresolved = realloc(o->unresolved, o->unresolved_cap * sizeof(char *));
        if (!o->unresolved) { perror("realloc"); _exit(1); }
    }
    o->unresolved[o->unresolved_count++] = strdup(name);
}


static uint64_t gnu_hash(const unsigned char *name) {
    uint64_t h = 5381;
    for (; *name; ++name) h = h * 33 + (unsigned char)*name;
    return h;
}

static uint64_t elf_hash(const char *name) {
    uint64_t h = 0, g;
    for (; *name; ++name) {
        h = (h << 4) + (unsigned char)*name;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static int gnu_bloom_may_have(Obj *o, uint64_t h) {
    if (!o->gnu_bloom) return 1;
    uint64_t h1 = h;
    uint64_t h2 = (uint64_t)(o->gnu_bloom_shift ? (h >> o->gnu_bloom_shift) : h);
    uint64_t mask = (uint64_t)1 << (h1 % 64);
    mask |= (uint64_t)1 << (h2 % 64);
    uint64_t w = o->gnu_bloom[(h1 / 64) % o->gnu_bloom_size];
    return (w & mask) == mask;
}

static ElfW(Sym) *find_symbol(Obj *o, const char *name) {
    uint64_t h = gnu_hash((const unsigned char *)name);
    if (o->gnu_bucket) {
        if (gnu_bloom_may_have(o, h)) {
            uint32_t i = o->gnu_bucket[h % o->gnu_nbucket];
            if (i) {
                while (i < o->dynsym_count) {
                    uint32_t cur = o->gnu_chain[i];
                    if ((((uint64_t)(cur ^ (uint32_t)h)) >> 1) == 0) {
                        char *nm = o->dynstr + o->dynsym[i].st_name;
                        if (strcmp(nm, name) == 0) return &o->dynsym[i];
                    }
                    if (cur & 1) break;
                    ++i;
                }
            }
        }
        goto linear;
    }
    if (o->sysv_bucket) {
        uint32_t i = o->sysv_bucket[elf_hash(name) % o->sysv_nbucket];
        while (i) {
            char *nm = o->dynstr + o->dynsym[i].st_name;
            if (strcmp(nm, name) == 0) return &o->dynsym[i];
            i = o->sysv_chain[i];
        }
        goto linear;
    }
linear:
    for (size_t i = 1; i < o->dynsym_count; ++i) {
        ElfW(Sym) *s = &o->dynsym[i];
        if (s->st_shndx == SHN_UNDEF) continue;
        char *nm = o->dynstr + s->st_name;
        if (strcmp(nm, name) == 0) return s;
    }
    return NULL;
}

static void *symbol_address(Obj *o, ElfW(Sym) *s) {
    if (s->st_shndx == SHN_UNDEF) return NULL;
    if (s->st_shndx == SHN_ABS) return (void *)(uintptr_t)s->st_value;
    return (char *)o->base + s->st_value;
}

static void *g_compat_handle;
static void *g_x11_handle;
static unsigned long g_xwin_id;
static unsigned int g_win_w = 1280, g_win_h = 720;

static const char *g_cur_obj_name = "?";

/* ---- sup allocator: replaces glibc malloc arena for game+deps ----
 * Goal: never touch glibc's arena metadata, so a stray wild write into a
 * glibc arena's top chunk cannot abort the engine at malloc() time.
 * 16-byte header before each block:
 *   [0]=magic, [1]=user size (LARGE: total mmap size), [2]=class, [3]=pad
 * freed small blocks reuse bytes [0..8)=magic(SUPCH_FREE)+pad, [8..16)=next. */
/* A/B switch: define SUP_NEUTRAL_ALLOC to revert to glibc malloc entirely */
#ifndef SUP_NEUTRAL_ALLOC
#define SUP_ALLOC_SLAB 65536
static const unsigned sup_cls_tab[] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768
};
#define SUP_NCLS ((int)(sizeof(sup_cls_tab) / sizeof(sup_cls_tab[0])))
#define SUPCH_OK   0x31505553u /* "SUP1" allocated */
#define SUPCH_LRG  0x4c505553u /* "SUPL" direct-mmap block */
#define SUPCH_FREE 0x46505553u /* "SUPF" free-chunk next-link marker */
static volatile int sup_alloc_lock_ = 0;
static void sup_alloc_lock(void) {
    while (__atomic_test_and_set(&sup_alloc_lock_, __ATOMIC_ACQUIRE)) {
    }
}
static void sup_alloc_unlock(void) {
    __atomic_clear(&sup_alloc_lock_, __ATOMIC_RELEASE);
}
static void *sup_freeheads[SUP_NCLS];
static volatile int g_sup_alloc_armed;
static int g_sup_disabled;
#ifndef SUP_NEUTRAL_ALLOC
static void sup_arm_allocator(void) { g_sup_alloc_armed = 1; }
#endif

static unsigned sup_cls_for(size_t n) {
    for (int i = 0; i < SUP_NCLS - 1; ++i)
        if (n <= sup_cls_tab[i]) return (unsigned)i;
    return (unsigned)(SUP_NCLS - 1);
}

static unsigned char *sup_newslab(unsigned cls) {
    size_t unit = sup_cls_tab[cls] + 16;
    unsigned char *slab = mmap(NULL, SUP_ALLOC_SLAB, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slab == MAP_FAILED) return NULL;
    size_t cnt = SUP_ALLOC_SLAB / unit;
    unsigned char *head = NULL;
    for (size_t i = 0; i < cnt; ++i) {
        unsigned char *c = slab + i * unit;
        *(uintptr_t *)(void *)c = 0;                       /* h[0..2) */
        ((uintptr_t *)(void *)c)[1] = (uintptr_t)head;     /* next */
        head = c;
    }
    return head;
}

static void *sup_malloc(size_t n) {
    if (!g_sup_alloc_armed) return malloc(n);
    if (n == 0) n = 1;
    if (n > sup_cls_tab[SUP_NCLS - 1]) {
        size_t total = (n + 16 + 4095) & ~(size_t)4095;
        unsigned char *b = mmap(NULL, total, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (b == MAP_FAILED) return NULL;
        ((uint32_t *)b)[0] = SUPCH_LRG;
        ((uint32_t *)b)[1] = (uint32_t)total;
        return b + 16;
    }
    unsigned cls = sup_cls_for(n);
    sup_alloc_lock();
    unsigned char *c = sup_freeheads[cls];
    if (!c) {
        c = sup_newslab(cls);
        if (c) sup_freeheads[cls] = (unsigned char *)((uintptr_t *)(void *)c)[1];
    } else {
        sup_freeheads[cls] = (unsigned char *)((uintptr_t *)(void *)c)[1];
    }
    sup_alloc_unlock();
    if (!c) return NULL;
    ((uint32_t *)c)[0] = SUPCH_OK;
    ((uint32_t *)c)[1] = (uint32_t)n;
    ((uint32_t *)c)[2] = cls;
    ((uint32_t *)c)[3] = 0;
    return c + 16;
}

static size_t g_pgsz;
/* Игра периодически зовёт free/realloc с мусорным указателем (битые
 * векторы после деструктор-шторма 0x5aacf60: 0x100000000000000 и т.п.
 * с флаговыми старшими битами). Прежде чем читать заголовок [-0x10],
 * проверяем каноничность и отображённость страницы заголовка — иначе
 * сам супераллокатор падает (bias 0x0 в runner, фатально). */
static int sup_hdr_sane(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000 || v > 0x00007fffffffffffULL) return 0;
    size_t ps = g_pgsz;
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    if (ps) {
        unsigned char vec = 0;
        if (mincore((void *)((v - 16) & ~(uintptr_t)(ps - 1)), ps, &vec) != 0)
            return 0;
    }
    return 1;
}

static void sup_free(void *p) {
    if (!p) return;
    if (!sup_hdr_sane(p)) {
        static int sf_skip;
        if (sf_skip < 8) {
            fprintf(stderr, "SUPFREE> мусорный ptr=%p пропущен, заголовок нечитаем\n", p);
            sf_skip++;
        }
        return;
    }
    if (!g_sup_alloc_armed) { free(p); return; }
    unsigned char *c = (unsigned char *)p - 16;
    uint32_t m = *(uint32_t *)(void *)c;
    if (m == SUPCH_OK) {
        unsigned cls = ((uint32_t *)c)[2];
        sup_alloc_lock();
        *(uintptr_t *)(void *)c = 0;
        ((uintptr_t *)(void *)c)[1] = (uintptr_t)sup_freeheads[cls];
        sup_freeheads[cls] = c;
        sup_alloc_unlock();
    } else if (m == SUPCH_LRG) {
        munmap(c, (size_t)((uint32_t *)c)[1]);
    } else {
        free(p); /* foreign/glibc block: let libc handle it */
    }
}

static void *sup_calloc(size_t n, size_t s) {
    if (!g_sup_alloc_armed) return calloc(n, s);
    size_t tot = n * s;
    void *p = sup_malloc(tot);
    if (p) memset(p, 0, tot);
    return p;
}

static void *sup_realloc(void *p, size_t n) {
    if (!p) return sup_malloc(n);
    if (!sup_hdr_sane(p)) {
        fprintf(stderr, "SUPREALLOC> мусорный ptr=%p пропущен\n", p);
        return NULL;
    }
    if (!g_sup_alloc_armed) return realloc(p, n);
    unsigned char *c = (unsigned char *)p - 16;
    uint32_t m = *(uint32_t *)(void *)c;
    size_t old = 0;
    if (m == SUPCH_OK) {
        old = ((uint32_t *)c)[1];
        if (n && n <= old) { ((uint32_t *)c)[1] = (uint32_t)n; return p; }
    } else if (m == SUPCH_LRG) {
        old = (size_t)((uint32_t *)c)[1] - 16;
    } else {
        return realloc(p, n); /* foreign */
    }
    void *q = sup_malloc(n);
    if (!q) return NULL;
    if (old && n) memcpy(q, p, old < n ? old : n);
    sup_free(p);
    return q;
}

static size_t sup_malloc_usable_size(void *p) {
    if (!p) return 0;
    if (!sup_hdr_sane(p)) return 0;
    if (!g_sup_alloc_armed) return malloc_usable_size(p);
    uint32_t m = *(uint32_t *)(void *)((unsigned char *)p - 16);
    if (m == SUPCH_OK) return ((uint32_t *)((unsigned char *)p - 16))[1];
    if (m == SUPCH_LRG) return (size_t)((uint32_t *)((unsigned char *)p - 16))[1] - 16;
    return 0;
}

static int sup_posix_memalign(void **o, size_t align, size_t n) {
    if (!o) return 22; /* EINVAL */
    if (!g_sup_alloc_armed) return posix_memalign(o, align, n);
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align <= 16) { *o = sup_malloc(n); return *o ? 0 : 12; }
    if (align <= 4096) {
        size_t total = (n + 4095) & ~(size_t)4095;
        unsigned char *b = mmap(NULL, total, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (b == MAP_FAILED) return 12; /* ENOMEM */
        ((uint32_t *)b)[0] = SUPCH_LRG;
        ((uint32_t *)b)[1] = (uint32_t)total;
        *o = b;
        return 0;
    }
    void *base = sup_malloc(n + align);
    if (!base) return 12;
    *o = (void *)(((uintptr_t)base + align - 1) & ~(uintptr_t)(align - 1));
    return 0;
}

static void *sup_aligned_alloc(size_t align, size_t n) {
    void *r = NULL;
    if (sup_posix_memalign(&r, align < 16 ? 16 : align, n) != 0) return NULL;
    return r;
}

static void *sup_memalign(size_t align, size_t n) { return sup_aligned_alloc(align, n); }
static void *sup_valloc(size_t n) { return sup_aligned_alloc(4096, n); }
static void sup_cfree(void *p) { sup_free(p); }
static int sup_mallopt(int, int) { return 1; }

static void *sup_override_sym(const char *name) {
    if (!name) return NULL;
    /* Графика. Игра резолвит egl/gl функции не через dlsym, а через свои
     * PLT-релокации, которые патчатся здесь, в resolve_symbol. Поэтому
     * перехват в compat.c:dlsym не срабатывал (0 перехватов), и EGL
     * уходил в настоящую Mesa мимо моста.
     * Хуки лежат в libsupcompat.so, а runner с ней не линкуется, поэтому
     * адреса берём из уже загруженной .so через dlopen(RTLD_DEFAULT). */
    {
        static void *(*p_egl_lookup)(const char *);
        static void *(*p_gl_slot)(const char *);
        static void *(*p_gl_resolve)(const char *);
        if (!p_egl_lookup) {
            p_egl_lookup = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sup_local_egl_symbol_pub");
            p_gl_slot = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sup_gl_slot_lookup");
            p_gl_resolve = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sup_gl_resolve");
            fprintf(stderr, "RELGL> hooks egl=%p glslot=%p glres=%p\n",
                    (void *)p_egl_lookup, (void *)p_gl_slot, (void *)p_gl_resolve);
            fflush(stderr);
        }
        if (name[0] == 'e' && !strncmp(name, "egl", 3) && p_egl_lookup) {
            void *h = p_egl_lookup(name);
            if (h) {
                fprintf(stderr, "RELGL> %s -> egl hook %p\n", name, h);
                fflush(stderr);
                return h;
            }
        }
        if (name[0] == 'g' && name[1] == 'l' &&
            strncmp(name, "glX", 3) != 0 && strncmp(name, "glx", 3) != 0) {
            void *slot = p_gl_slot ? p_gl_slot(name) : NULL;
            if (slot) return slot;
            void *r = p_gl_resolve ? p_gl_resolve(name) : NULL;
            if (r) return r;
        }
    }
    /* Bionic _ctype_ требует двойного разыменования (см. ndk.c). Слот GOT
     * должен содержать адрес переменной-указателя, а не самой таблицы,
     * иначе первый deref съедает байты классов и даёт 0x2020202020202000
     * вместо базы адресации -> SEGV. Проверено objdump: .text+0xf94db0e. */
    if (name[0] == '_' && !strcmp(name, "_ctype_")) {
        static unsigned char **(*p_ctype_ptr)(void);
        if (!p_ctype_ptr) {
            p_ctype_ptr = (unsigned char **(*)(void))dlsym(RTLD_DEFAULT, "sup_ctype_b_ptr_pub");
            fprintf(stderr, "RELCT> ctype ptr hook=%p\n", (void *)p_ctype_ptr);
            fflush(stderr);
        }
        if (p_ctype_ptr) {
            void *r = p_ctype_ptr();
            fprintf(stderr, "RELCT> _ctype_ -> bionic indirection %p\n", r);
            fflush(stderr);
            return r;
        }
    }
    switch (name[0]) {
    case 'a': if (!strcmp(name, "aligned_alloc")) return (void *)sup_aligned_alloc; return NULL;
    case 'c':
        if (!strcmp(name, "calloc")) return (void *)sup_calloc;
        if (!strcmp(name, "cfree")) return (void *)sup_cfree;
        return NULL;
    case 'f': if (!strcmp(name, "free")) return (void *)sup_free; return NULL;
    case 'm':
        if (!strcmp(name, "malloc")) return (void *)sup_malloc;
        if (!strcmp(name, "malloc_usable_size")) return (void *)sup_malloc_usable_size;
        if (!strcmp(name, "memalign")) return (void *)sup_memalign;
        if (!strcmp(name, "mallopt")) return (void *)sup_mallopt;
        if (!strcmp(name, "malloc_stats")) return (void *)sup_mallopt;
        return NULL;
    case 'p': if (!strcmp(name, "posix_memalign")) return (void *)sup_posix_memalign; return NULL;
    case 'r': if (!strcmp(name, "realloc")) return (void *)sup_realloc; return NULL;
    case 'v': if (!strcmp(name, "valloc")) return (void *)sup_valloc; return NULL;
    }
    return NULL;
}
#endif /* !SUP_NEUTRAL_ALLOC (sup allocator) */

static void (*sup_signal(int sig, void (*h)(int)))(int);
static int sup_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
static int sup_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

static void *resolve_symbol(Obj *o, const char *name, int weak) {
    int gltrace = name && strstr(name, "glGetShaderPrecision");
    if (gltrace) fprintf(stderr, "RSTRACE> entry name=%s obj=%s\n", name, o->path);
#ifndef SUP_NEUTRAL_ALLOC
    if (!g_sup_disabled) {
        void *ov = sup_override_sym(name);
        if (ov) {
            if (gltrace) fprintf(stderr, "RSTRACE> ret override %p\n", ov);
            return ov;
        }
    }
#endif
    if (g_verbose && name && strstr(name, "FMOD")) {
        fprintf(stderr, "RS> obj=%s name=%s gndep=%zu\n", g_cur_obj_name, name, g_ndep);
    }
    ElfW(Sym) *s = find_symbol(o, name);
    if (s && s->st_shndx != SHN_UNDEF) {
        if (gltrace) fprintf(stderr, "RSTRACE> ret self %p (shndx=%u)\n",
                             symbol_address(o, s), s->st_shndx);
        return symbol_address(o, s);
    }
    if (name) {
        /* Перехват __throw_out_of_range: игра вызывает его через PLT из
         * шаблонного vector::at(). Достаём оригинал из libc++_shared. */
        if (strcmp(name, "_ZNKSt6__ndk120__vector_base_commonILb1EE20__throw_out_of_rangeEv") == 0) {
            static void *(*p_set)(void *);
            static void *orig;
            static int init;
            if (!init) {
                init = 1;
                p_set = (void *(*)(void *))dlsym(g_compat_handle, "sup_set_oor_pub");
                for (size_t i = 0; i < g_ndep; ++i) {
                    ElfW(Sym) *d = find_symbol(&g_deps[i], name);
                    if (d && d->st_shndx != SHN_UNDEF) {
                        orig = symbol_address(&g_deps[i], d);
                        if (p_set) p_set(orig);
                        break;
                    }
                }
            }
            void *hook = dlsym(g_compat_handle, name);
            if (hook && orig) {
                fprintf(stderr, "RSOOR> hook=%p orig=%p\n", hook, orig);
                fflush(stderr);
                return hook;
            }
        }
        /* Перехват исключений: игра и libc++_shared зовут __cxa_throw через
         * свои PLT, и лоадер решает их здесь. Наш хук печатает тип и стек,
         * затем вызывает оригинал из libc++_shared. */
        if (strcmp(name, "__cxa_throw") == 0) {
            static void *(*p_set)(void *);
            static int init;
            if (!init) {
                init = 1;
                p_set = (void *(*)(void *))dlsym(g_compat_handle,
                                                 "sup_set_cxa_throw_pub");
            }
            for (size_t i = 0; i < g_ndep; ++i) {
                ElfW(Sym) *d = find_symbol(&g_deps[i], "__cxa_throw");
                if (d && d->st_shndx != SHN_UNDEF) {
                    void *orig = symbol_address(&g_deps[i], d);
                    if (p_set) p_set(orig);
                    void *hook = dlsym(g_compat_handle, "__cxa_throw");
                    if (hook) {
                        fprintf(stderr, "RSTHROW> hook=%p orig=%p\n", hook, orig);
                        fflush(stderr);
                        return hook;
                    }
                    break;
                }
            }
        }
        if (strcmp(name, "signal") == 0) return (void *)sup_signal;
        if (strcmp(name, "sigaction") == 0) return (void *)sup_sigaction;
        if (strcmp(name, "sigprocmask") == 0) return (void *)sup_sigprocmask;
        if (strcmp(name, "pthread_sigmask") == 0) return (void *)sup_sigprocmask;
    }
    if (name && strcmp(name, "__emutls_get_address") == 0) {
        return (void *)__emutls_get_address;
    }
    for (size_t i = 0; i < g_ndep; ++i) {
        ElfW(Sym) *d = find_symbol(&g_deps[i], name);
        if (g_verbose && name && strstr(name, "FMOD")) {
            fprintf(stderr, "DEP> obj=%s name=%s dep[%zu]=%s dynsym=%zu d=%p\n",
                    g_cur_obj_name, name, i, g_deps[i].path, g_deps[i].dynsym_count, (void *)d);
        }
        if (d && d->st_shndx != SHN_UNDEF) {
            if (gltrace) fprintf(stderr, "RSTRACE> ret dep[%zu] %p (%s)\n",
                                 i, symbol_address(&g_deps[i], d), g_deps[i].path);
            return symbol_address(&g_deps[i], d);
        }
    }
    if (g_compat_handle) {
        void *c = dlsym(g_compat_handle, name);
        if (gltrace) fprintf(stderr, "RSTRACE> dlsym(compat) -> %p\n", c);
        if (c) return c;
    }
    void *ext = dlsym(RTLD_DEFAULT, name);
    if (gltrace) fprintf(stderr, "RSTRACE> dlsym(RTLD_DEFAULT) -> %p\n", ext);
    if (ext) return ext;
    {
        void *stub = sup_android_stub(name);
        if (gltrace) fprintf(stderr, "RSTRACE> android_stub -> %p\n", stub);
        if (stub) {
            if (g_verbose) fprintf(stderr, "STUB> obj=%s name=%s -> shim\n", g_cur_obj_name, name);
            return stub;
        }
    }
    if (weak) return NULL;
    note_unresolved(o, name);
    return NULL;
}

static void patch_slot(void *addr, void *value) {
    *(void **)addr = value;
    __builtin___clear_cache((char *)addr, (char *)addr + sizeof(value));
}

static int map_load_segments(Obj *o, int fd) {
    size_t min_vaddr = (size_t)-1;
    size_t max_end = 0;
    for (size_t i = 0; i < o->phnum; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_vaddr < min_vaddr) min_vaddr = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > max_end) max_end = p->p_vaddr + p->p_memsz;
    }
    o->total_size = (max_end + g_pgsz - 1) & ~(g_pgsz - 1);
    size_t bias_delta = (min_vaddr + g_pgsz - 1) & ~(g_pgsz - 1);
    size_t anon_len = o->total_size + bias_delta;
    void *base = mmap(NULL, anon_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap anon"); return -1; }
    if (min_vaddr != 0) {
        base = mmap((char *)base + bias_delta - min_vaddr, o->total_size,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                    -1, 0);
        if (base == MAP_FAILED) { perror("mmap bias"); return -1; }
    }
    o->base = base;
    for (size_t i = 0; i < o->phnum; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_filesz == 0) continue;
        size_t off_mod = p->p_offset & (g_pgsz - 1);
        size_t va_mod = p->p_vaddr & (g_pgsz - 1);
        if (off_mod != va_mod) {
            fprintf(stderr, "warn: unmatched offset/vaddr mod in load seg %zu\n", i);
            continue;
        }
        void *map_at = (char *)base + (p->p_vaddr & ~(g_pgsz - 1));
        size_t map_off = p->p_offset & ~(g_pgsz - 1);
        size_t whole = (p->p_filesz + g_pgsz - 1) & ~(g_pgsz - 1);
        if (p->p_filesz & (g_pgsz - 1)) {
            fprintf(stderr, "info: load seg %zu filesz not page-multiple (%zu), mapping full page\n",
                    i, p->p_filesz);
        }
        if (whole) {
            void *r = mmap(map_at, whole, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_FIXED, fd, (off_t)map_off);
            if (r == MAP_FAILED) { perror("mmap file"); return -1; }
        }
    }
    return 0;
}

static int parse_dynamic(Obj *o) {
    for (size_t i = 0; i < o->phnum; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type != PT_DYNAMIC) continue;
        o->dyn = (ElfW(Dyn) *)((char *)o->base + p->p_vaddr);
        o->dyn_count = p->p_filesz / sizeof(ElfW(Dyn));
        break;
    }
    if (!o->dyn) { fprintf(stderr, "no PT_DYNAMIC\n"); return -1; }
    for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < o->dyn_count; ++i) {
        ElfW(Dyn) *d = &o->dyn[i];
        if (pass == 0 && d->d_tag == DT_NEEDED) continue;
        if (pass == 1 && d->d_tag != DT_NEEDED) continue;
        switch (d->d_tag) {
            case DT_SYMTAB: o->dynsym = (ElfW(Sym) *)((char *)o->base + d->d_un.d_ptr); break;
            case DT_SYMENT:
                if (d->d_un.d_val != sizeof(ElfW(Sym))) { fprintf(stderr, "unexpected SYMENT\n"); return -1; }
                break;
            case DT_STRTAB: o->dynstr = (char *)o->base + d->d_un.d_ptr; break;
            case DT_STRSZ: o->dynstr_size = d->d_un.d_val; break;
            case DT_GNU_HASH: {
                uint32_t *gh = (uint32_t *)((char *)o->base + d->d_un.d_ptr);
                o->gnu_nbucket = gh[0];
                o->gnu_symoffset = gh[1];
                o->gnu_bloom_size = gh[2];
                o->gnu_bloom_shift = gh[3];
                o->gnu_bloom = (uint64_t *)(gh + 4);
                o->gnu_bucket = (uint32_t *)(o->gnu_bloom + o->gnu_bloom_size);
                o->gnu_chain = o->gnu_bucket + o->gnu_nbucket;
                break;
            }
            case DT_HASH: {
                uint32_t *h = (uint32_t *)((char *)o->base + d->d_un.d_ptr);
                o->sysv_nbucket = h[0];
                o->sysv_bucket = h + 2;
                o->sysv_chain = o->sysv_bucket + o->sysv_nbucket;
                break;
            }
            case DT_RELA: o->rela_dyn = (ElfW(Rela) *)((char *)o->base + d->d_un.d_ptr); break;
            case DT_RELASZ: o->rela_dyn_count = d->d_un.d_val / sizeof(ElfW(Rela)); break;
            case DT_JMPREL: o->rela_plt = (ElfW(Rela) *)((char *)o->base + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: o->rela_plt_count = d->d_un.d_val / sizeof(ElfW(Rela)); break;
            case DT_PLTREL:
                if (d->d_un.d_val != DT_RELA) { fprintf(stderr, "PLTREL not RELA\n"); return -1; }
                break;
            case DT_IRELA: o->irela = (ElfW(Rela) *)((char *)o->base + d->d_un.d_ptr); break;
            case DT_IRELASZ: o->irela_count = d->d_un.d_val / sizeof(ElfW(Rela)); break;
            case DT_INIT: o->dt_init = (void (*)(void))((char *)o->base + d->d_un.d_ptr); break;
            case DT_FINI: o->dt_fini = (void (*)(void))((char *)o->base + d->d_un.d_ptr); break;
            case DT_INIT_ARRAY: o->init_array = (void (**)(void))((char *)o->base + d->d_un.d_ptr); break;
            case DT_INIT_ARRAYSZ: o->init_array_count = d->d_un.d_val / sizeof(void *); break;
            case DT_PREINIT_ARRAY: o->preinit_array = (void (**)(void))((char *)o->base + d->d_un.d_ptr); break;
            case DT_PREINIT_ARRAYSZ: o->preinit_array_count = d->d_un.d_val / sizeof(void *); break;
            case DT_NEEDED: {
                char *nm = o->dynstr + d->d_un.d_val;
                o->needed = realloc(o->needed, (o->needed_count + 1) * sizeof(char *));
                if (!o->needed) { perror("realloc needed"); return -1; }
                o->needed[o->needed_count++] = nm;
                break;
            }
            case DT_TEXTREL: o->has_textrel = 1; break;
            case DT_FLAGS:
                if (d->d_un.d_val & DF_TEXTREL) o->has_textrel = 1;
                break;
            default: break;
        }
    }
    }
    if (!o->dynsym || !o->dynstr) { fprintf(stderr, "no dynsym/dynstr\n"); return -1; }
    for (size_t i = 0; i < o->phnum; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type == PT_GNU_RELRO) {
            o->has_gnu_relro = 1;
            o->relro_start = (char *)o->base + p->p_vaddr;
            o->relro_size = p->p_memsz;
        }
    }
    return 0;
}

/* ---- FMOD C++ API interception (tracing) ---- */
typedef int fm_result;
static void resolve_frame(void *addr);
typedef struct { const char *name; void *shim; void *real; } fm_hook;

static fm_result fn_fm_sys_create(void **sys, unsigned int version);
static fm_result fn_fm_mem_stats(int *currentalloced, int *maxalloced, int dummy);
static fm_result fn_fm_create_stream(void *sys, const char *name, unsigned int mode,
                                     void *exinfo, void **soundout);
static fm_result fn_fm_create_sound(void *sys, const char *name, unsigned int mode,
                                    void *exinfo, void **soundout);
static fm_result fn_fm_create_channel_group(void *sys, const char *name, void **outcg);
static fm_result fn_fm_init(void *sys, int maxch, unsigned int flags, void *extra);
static fm_result fn_fm_set_file_system(void *sys, void *open, void *close, void *read,
                                       void *seek, void *asyncopen, void *cancel);
static fm_result fn_fm_play_sound(void *sys, void *sound, void *cg, int paused, void **chanout);
static fm_result fn_fm_release(void *sys);
static fm_result fn_fm_get_driver_info(void *sys, int id, char *name, int nlen, void *guid,
                                       int *sysout, unsigned int *speakermodes);
static fm_result fn_fm_set_driver(void *sys, int id);
static fm_result fn_fm_set_output(void *sys, unsigned int type);
static fm_result fn_fm_get_software_format(void *sys, int *numch, unsigned int *spkmode);

static fm_hook g_fm_hooks[] = {
    { "FMOD_System_Create", (void *)&fn_fm_sys_create, NULL },
    { "FMOD_Memory_GetStats", (void *)&fn_fm_mem_stats, NULL },
    { "_ZN4FMOD6System11createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
      (void *)&fn_fm_create_stream, NULL },
    { "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
      (void *)&fn_fm_create_sound, NULL },
    { "_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE",
      (void *)&fn_fm_create_channel_group, NULL },
    { "_ZN4FMOD6System4initEijPv", (void *)&fn_fm_init, NULL },
    { "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i",
      (void *)&fn_fm_set_file_system, NULL },
    { "_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE",
      (void *)&fn_fm_play_sound, NULL },
    { "_ZN4FMOD6System7releaseEv", (void *)&fn_fm_release, NULL },
    { "_ZN4FMOD6System13getDriverInfoEiPciP9FMOD_GUIDPiP16FMOD_SPEAKERMODES4_",
      (void *)&fn_fm_get_driver_info, NULL },
    { "_ZN4FMOD6System9setDriverEi", (void *)&fn_fm_set_driver, NULL },
    { "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE", (void *)&fn_fm_set_output, NULL },
    { "_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_",
      (void *)&fn_fm_get_software_format, NULL },
    { NULL, NULL, NULL }
};

static void *fm_real_for(const char *name) {
    for (size_t i = 0; g_fm_hooks[i].name; ++i)
        if (strcmp(g_fm_hooks[i].name, name) == 0) return g_fm_hooks[i].real;
    return NULL;
}

static fm_result fn_fm_sys_create(void **sys, unsigned int version) {
    /* FMOD 5: FMOD_System_Create(FMOD_SYSTEM **, unsigned int headerversion).
     * Раньше версия терялась, реальная функция читала мусор в ESI и
     * возвращала 20 (ERR_HEADER_MISMATCH) — звук не инициализировался. */
    fprintf(stderr, "FM> FMOD_System_Create(sys=%p ver=%#x) from ",
            (void *)sys, version);
    resolve_frame(__builtin_return_address(0));
    fm_result r = ((fm_result (*)(void **, unsigned int))
                   fm_real_for("FMOD_System_Create"))(sys, version);
    if (sys && *sys) fprintf(stderr, "FM>   -> %d system=%p\n", r, *sys);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_mem_stats(int *currentalloced, int *maxalloced, int dummy) {
    fprintf(stderr, "FM> FMOD_Memory_GetStats(curr=%p max=%p dummy=%d) from ", currentalloced, maxalloced, dummy);
    resolve_frame(__builtin_return_address(0));
    return ((fm_result (*)(int *, int *, int))fm_real_for("FMOD_Memory_GetStats"))
           (currentalloced, maxalloced, dummy);
}

static fm_result fn_fm_create_stream(void *sys, const char *name, unsigned int mode,
                                     void *exinfo, void **soundout) {
    fprintf(stderr, "FM> System::createStream(this=%p) from ", sys);
    resolve_frame(__builtin_return_address(0));
    fprintf(stderr, "FM>   name=%p mode=%#x exinfo=%p out=%p\n", name, mode, exinfo, soundout);
    if (soundout) fprintf(stderr, "FM>   *out(now)=%p\n", *soundout);
    fm_result r = ((fm_result (*)(void *, const char *, unsigned int, void *, void **))
                   fm_real_for("_ZN4FMOD6System11createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"))
                  (sys, name, mode, exinfo, soundout);
    if (soundout && *soundout) fprintf(stderr, "FM>   -> %d sound=%p\n", r, *soundout);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_create_sound(void *sys, const char *name, unsigned int mode,
                                    void *exinfo, void **soundout) {
    fprintf(stderr, "FM> System::createSound(this=%p) from ", sys);
    resolve_frame(__builtin_return_address(0));
    fprintf(stderr, "FM>   name=%p mode=%#x exinfo=%p out=%p\n", name, mode, exinfo, soundout);
    fm_result r = ((fm_result (*)(void *, const char *, unsigned int, void *, void **))
                   fm_real_for("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"))
                  (sys, name, mode, exinfo, soundout);
    if (soundout && *soundout) fprintf(stderr, "FM>   -> %d sound=%p\n", r, *soundout);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_create_channel_group(void *sys, const char *name, void **outcg) {
    fprintf(stderr, "FM> System::createChannelGroup(this=%p, name=%p) from ", sys, name);
    resolve_frame(__builtin_return_address(0));
    fm_result r = ((fm_result (*)(void *, const char *, void **))
                   fm_real_for("_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE"))
                  (sys, name, outcg);
    if (outcg && *outcg) fprintf(stderr, "FM>   -> %d cg=%p\n", r, *outcg);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_init(void *sys, int maxch, unsigned int flags, void *extra) {
    fprintf(stderr, "FM> System::init(this=%p, maxch=%d flags=%#x extra=%p) "
            "-> не инициализируем (нет Android-аудио)\n",
            sys, maxch, flags, extra);
    resolve_frame(__builtin_return_address(0));
    /* libfmod на этой сборке падает в System::init: Android-выхода
     * (OpenSL/AudioTrack) на десктопе нет, а NOSOUND тоже не спасает.
     * Возвращаем ошибку — игра работает без звука, как и раньше с
     * сорванным System_Create. */
    return 20; /* FMOD_ERR_FORMAT: «аудио недоступно» */
}

static fm_result fn_fm_set_file_system(void *sys, void *open, void *close, void *read,
                                       void *seek, void *asyncopen, void *cancel) {
    fprintf(stderr, "FM> System::setFileSystem(this=%p) from ", sys);
    resolve_frame(__builtin_return_address(0));
    fprintf(stderr, "FM>   open=%p close=%p read=%p seek=%p asyncopen=%p cancel=%p\n",
            open, close, read, seek, asyncopen, cancel);
    fm_result r = ((fm_result (*)(void *, void *, void *, void *, void *, void *, void *))
                   fm_real_for("_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i"))
                  (sys, open, close, read, seek, asyncopen, cancel);
    fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_play_sound(void *sys, void *sound, void *cg, int paused, void **chanout) {
    fprintf(stderr, "FM> System::playSound(this=%p, sound=%p cg=%p paused=%d out=%p)\n",
            sys, sound, cg, paused, chanout);
    fm_result r = ((fm_result (*)(void *, void *, void *, int, void **))
                   fm_real_for("_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE"))
                  (sys, sound, cg, paused, chanout);
    if (chanout && *chanout) fprintf(stderr, "FM>   -> %d channel=%p\n", r, *chanout);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_release(void *sys) {
    fprintf(stderr, "FM> System::release(this=%p) from ", sys);
    resolve_frame(__builtin_return_address(0));
    fm_result r = ((fm_result (*)(void *))fm_real_for("_ZN4FMOD6System7releaseEv"))(sys);
    fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_get_driver_info(void *sys, int id, char *name, int nlen, void *guid,
                                       int *sysout, unsigned int *speakermodes) {
    fprintf(stderr, "FM> System::getDriverInfo(this=%p, id=%d buf=%p nlen=%d) from ", sys, id, name, nlen);
    resolve_frame(__builtin_return_address(0));
    fm_result r = ((fm_result (*)(void *, int, char *, int, void *, int *, unsigned int *))
                   fm_real_for("_ZN4FMOD6System13getDriverInfoEiPciP9FMOD_GUIDPiP16FMOD_SPEAKERMODES4_"))
                  (sys, id, name, nlen, guid, sysout, speakermodes);
    if (name) fprintf(stderr, "FM>   -> %d driver=\"%.*s\" sysout=%d modes=%#x\n", r, nlen, name,
                      sysout ? *sysout : -1, speakermodes ? *speakermodes : 0u);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static fm_result fn_fm_set_driver(void *sys, int id) {
    fprintf(stderr, "FM> System::setDriver(this=%p, id=%d)\n", sys, id);
    return ((fm_result (*)(void *, int))fm_real_for("_ZN4FMOD6System9setDriverEi"))(sys, id);
}

static fm_result fn_fm_set_output(void *sys, unsigned int type) {
    /* Игра просит Android-выход (OpenSL/AudioTrack) — на десктопе его нет,
     * libfmod падает в System::init (SEGV @ fmod+0xfd92b). Переключаем на
     * FMOD_OUTPUTTYPE_NOSOUND: звук молчит, но система инициализируется и
     * игра не отваливается на аудио. */
    unsigned int use = type;
    if (!getenv("SUP_FMOD_KEEP_OUTPUT") && type != 2) use = 2;
    fprintf(stderr, "FM> System::setOutput(this=%p, type=%#x -> %#x)\n",
            sys, type, use);
    return ((fm_result (*)(void *, unsigned int))
            fm_real_for("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE"))(sys, use);
}

static fm_result fn_fm_get_software_format(void *sys, int *numch, unsigned int *spkmode);

static fm_result fn_fm_get_software_format(void *sys, int *numch, unsigned int *spkmode) {
    fprintf(stderr, "FM> System::getSoftwareFormat(this=%p outch=%p spkmode=%p)\n", sys, numch, spkmode);
    fm_result r = ((fm_result (*)(void *, int *, unsigned int *))
                   fm_real_for("_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_"))
                  (sys, numch, spkmode);
    if (numch && spkmode) fprintf(stderr, "FM>   -> %d ch=%d mode=%#x\n", r, *numch, *spkmode);
    else fprintf(stderr, "FM>   -> %d\n", r);
    return r;
}

static void apply_one_reloc(Obj *o, const ElfW(Rela) *r) {
    size_t type = ELF64_R_TYPE(r->r_info);
    size_t symi = ELF64_R_SYM(r->r_info);
    void *addr = (char *)o->base + r->r_offset;
    const char *symname = NULL;
    int weak = 0;
    void *def = NULL;
    if (symi) {
        if (symi >= o->dynsym_count) return;
        ElfW(Sym) *sym = &o->dynsym[symi];
        if (sym->st_name < o->dynstr_size) symname = o->dynstr + sym->st_name;
        if (ELF64_ST_BIND(sym->st_info) == STB_WEAK && sym->st_shndx == SHN_UNDEF) weak = 1;
        if (!o->rcache) {
            o->rcache = calloc(o->dynsym_count, sizeof(void *));
            o->rcache_n = o->dynsym_count;
        }
        if (!o->rcache[symi]) {
            o->rcache[symi] = resolve_symbol(o, symname, weak);
        }
        def = o->rcache[symi];
        if (g_verbose && symname && strstr(symname, "FMOD")) {
            fprintf(stderr, "REL> obj=%s sym=%s type=%zu shndx=%u def=%p\n",
                    g_cur_obj_name, symname, type, symi < o->dynsym_count ? o->dynsym[symi].st_shndx : 0xffff, def);
        }
    }
    switch (type) {
        case R_X86_64_NONE:
            break;
        case R_X86_64_RELATIVE:
            patch_slot(addr, (char *)o->base + r->r_addend);
            ++o->reloc_applied;
            break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            if (symname && strstr(symname, "glGetShaderPrecision")) {
                fprintf(stderr, "RELTRACE> %s sym=%s off=0x%zx addr=%p def=%p r_addend=%#llx\n",
                        o->path, symname, (size_t)r->r_offset, addr, def,
                        (unsigned long long)r->r_addend);
            }
            if (symname && def) {
                if (o->dynsym_count) {
                    int is_import = 0;
                    if (symi < o->dynsym_count)
                        is_import = o->dynsym[symi].st_shndx == SHN_UNDEF;
                    if (is_import) {
                        for (size_t hi = 0; g_fm_hooks[hi].name; ++hi) {
                            if (strcmp(symname, g_fm_hooks[hi].name) == 0) {
                                if (!g_fm_hooks[hi].real) g_fm_hooks[hi].real = def;
                                def = g_fm_hooks[hi].shim;
                                break;
                            }
                        }
                    }
                }
                patch_slot(addr, (char *)def + r->r_addend);
            } else if (symname && weak) {
                patch_slot(addr, NULL);
            } else if (symname) {
                patch_slot(addr, NULL);
            } else {
                patch_slot(addr, NULL);
            }
            ++o->reloc_applied;
            break;
        }
        case R_X86_64_IRELATIVE: {
            void *(*resolver)(void) = (void *(*)(void))((char *)o->base + r->r_addend);
            patch_slot(addr, resolver());
            ++o->reloc_applied;
            break;
        }
        default:
            fprintf(stderr, "unsupported reloc type %zu\n", type);
            break;
    }
    (void)r;
}

static int reloc_all(Obj *o) {
    g_cur_obj_name = o->path;
    for (size_t i = 0; i < o->rela_plt_count; ++i) apply_one_reloc(o, &o->rela_plt[i]);
    for (size_t i = 0; i < o->rela_dyn_count; ++i) apply_one_reloc(o, &o->rela_dyn[i]);
    for (size_t i = 0; i < o->irela_count; ++i) apply_one_reloc(o, &o->irela[i]);
    if (o->has_gnu_relro) {
        size_t r_start = (size_t)o->relro_start & ~(g_pgsz - 1);
        size_t r_end = ((size_t)o->relro_start + o->relro_size + g_pgsz - 1) & ~(g_pgsz - 1);
        if (mprotect((void *)r_start, r_end - r_start, PROT_READ) != 0) {
            perror("warn: mprotect relro (non-fatal)");
        }
    }
    for (size_t i = 0; i < o->phnum; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type != PT_LOAD) continue;
        void *seg = (char *)o->base + (p->p_vaddr & ~(size_t)0xfff);
        size_t len = (size_t)(p->p_vaddr + p->p_memsz - (p->p_vaddr & ~(size_t)0xfff));
        len = (len + 0xfff) & ~(size_t)0xfff;
        int prot = PROT_READ;
        if (p->p_flags & PF_W) prot |= PROT_WRITE;
        if (p->p_flags & PF_X) prot |= PROT_EXEC;
        if (mprotect(seg, len, prot) != 0) {
            perror("mprotect seg");
            return -1;
        }
    }
    if (o->has_textrel) {
        fprintf(stderr, "textrel segments remain writable (TEXTREL)\n");
    }
    o->xr_count = 0;
    for (size_t i = 0; i < o->phnum && o->xr_count < 8; ++i) {
        ElfW(Phdr) *p = &o->phdrs[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        o->xr_start[o->xr_count] = (uintptr_t)o->base + p->p_vaddr;
        o->xr_end[o->xr_count] = o->xr_start[o->xr_count] + p->p_memsz;
        ++o->xr_count;
    }
    for (size_t i = 0; i < o->xr_count; ++i) {
        fprintf(stderr, "XREXEC> %s [%zu] start=%#lx end=%#lx\n",
                o->path, i,
                (unsigned long)o->xr_start[i],
                (unsigned long)o->xr_end[i]);
    }
    sup_exec_publish(o);
    fflush(stderr);
    return 0;
}

static int load_object(Obj *o, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    o->path = path;
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return -1; }
    ElfW(Ehdr) ehdr;
    if (pread(fd, &ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr)) {
        perror("pread ehdr"); close(fd); return -1;
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "not ELF\n"); close(fd); return -1;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_machine != EM_X86_64) {
        fprintf(stderr, "not x86-64 ELF64\n"); close(fd); return -1;
    }
    o->phnum = ehdr.e_phnum;
    if (o->phnum == 0 || o->phnum > 65535u) {
        fprintf(stderr, "bad phnum\n"); close(fd); return -1;
    }
    o->phdrs = hp_alloc(sizeof(ElfW(Phdr)) * o->phnum);
    if (pread(fd, o->phdrs, sizeof(ElfW(Phdr)) * o->phnum, ehdr.e_phoff) !=
        (ssize_t)(sizeof(ElfW(Phdr)) * o->phnum)) {
        perror("pread phdrs"); close(fd); return -1;
    }
    o->dynsym_count = 0;
    size_t shnum = ehdr.e_shnum ? ehdr.e_shnum : 0;
    if (shnum && ehdr.e_shoff) {
        ElfW(Shdr) *sh = hp_alloc(sizeof(ElfW(Shdr)) * shnum);
        if (pread(fd, sh, sizeof(ElfW(Shdr)) * shnum, ehdr.e_shoff) ==
            (ssize_t)(sizeof(ElfW(Shdr)) * shnum)) {
            for (size_t i = 0; i < shnum; ++i) {
                if (sh[i].sh_type == SHT_DYNSYM && sh[i].sh_entsize == sizeof(ElfW(Sym))) {
                    o->dynsym_count = sh[i].sh_size / sizeof(ElfW(Sym));
                    break;
                }
            }
            /* .eh_frame: нужен, чтобы libgcc (_Unwind_Backtrace) умел
             * раскручивать кадры вручную смапленной игры. */
            if (ehdr.e_shstrndx && ehdr.e_shstrndx < shnum &&
                sh[ehdr.e_shstrndx].sh_type == SHT_STRTAB) {
                size_t strs = sh[ehdr.e_shstrndx].sh_size;
                char *stab = hp_alloc(strs);
                if (stab && pread(fd, stab, strs,
                                  sh[ehdr.e_shstrndx].sh_offset) == (ssize_t)strs) {
                    for (size_t i = 0; i < shnum; ++i) {
                        if (!sh[i].sh_name || sh[i].sh_name >= strs) continue;
                        if (!strcmp(stab + sh[i].sh_name, ".eh_frame")) {
                            o->eh_frame = sh[i].sh_addr;
                            break;
                        }
                    }
                }
                free(stab);
            }
        }
        free(sh);
    }
    if (map_load_segments(o, fd) != 0) { close(fd); return -1; }
    close(fd);
    if (parse_dynamic(o) != 0) return -1;
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void print_report(Obj *o) {
    printf("=== SuperCraft loader ===\n");
    printf("image:   %s\n", o->path);
    printf("base:    %p size: %zu KiB\n", o->base, o->total_size >> 10);
    printf("needed:  ");
    for (size_t i = 0; i < o->needed_count; ++i) {
        printf("%s%s", i ? ", " : "", o->needed[i]);
    }
    printf("\n");
    printf("dynsym:  %zu symbols\n", o->dynsym_count);
    printf("relocs:  PLT %zu + DYN %zu%s = applied %zu\n", o->rela_plt_count,
           o->rela_dyn_count, o->irela_count ? " + IREL" : "", o->reloc_applied);
    printf("init:    init_array %zu, preinit %zu%s\n", o->init_array_count,
           o->preinit_array_count, o->dt_init ? ", DT_INIT" : "");
    printf("TLS:     %s\n", "not present");
    qsort(o->unresolved, o->unresolved_count, sizeof(char *), cmp_str);
    size_t uniq = 0;
    for (size_t i = 0; i < o->unresolved_count; ++i) {
        if (i == 0 || strcmp(o->unresolved[i], o->unresolved[i - 1]) != 0) {
            o->unresolved[uniq++] = o->unresolved[i];
        }
    }
    o->unresolved_count = uniq;
    printf("unresolved: %zu (unique)\n", o->unresolved_count);
    for (size_t i = 0; i < o->unresolved_count; ++i) {
        printf("   - %s\n", o->unresolved[i]);
    }
}

static size_t g_cur_init_idx;

static int obj_in_exec(Obj *o, uintptr_t addr) {
    for (size_t i = 0; i < o->xr_count; ++i)
        if (addr >= o->xr_start[i] && addr < o->xr_end[i]) return 1;
    return 0;
}

static const char *nearest_sym(Obj *o, uintptr_t addr, uintptr_t *out_off) {
    uintptr_t rel = addr - (uintptr_t)o->base;
    const char *best = NULL;
    uintptr_t best_val = 0, best_off = 0;
    const char *contain = NULL;
    uintptr_t contain_val = 0, contain_off = 0;
    for (size_t i = 1; i < o->dynsym_count; ++i) {
        ElfW(Sym) *s = &o->dynsym[i];
        if (s->st_shndx == SHN_UNDEF) continue;
        if ((s->st_info & 0xf) != STT_FUNC) continue;
        if (s->st_value > rel) continue;
        if (s->st_size && rel < s->st_value + s->st_size)
            if (!contain || s->st_value >= contain_val) {
                contain = o->dynstr + s->st_name;
                contain_val = s->st_value;
                contain_off = rel - s->st_value;
            }
        if (s->st_value >= best_val) {
            best_val = s->st_value;
            best = o->dynstr + s->st_name;
            best_off = rel - s->st_value;
        }
    }
    if (contain) {
        if (out_off) *out_off = contain_off;
        return contain;
    }
    if (out_off) *out_off = best_off;
    return best;
}

static void resolve_frame(void *addr) {
    const char *name = NULL;
    uintptr_t off = 0;
    const char *mod = "?";
    for (size_t d = 0; d < g_ndep; ++d) {
        if (obj_in_exec(&g_deps[d], (uintptr_t)addr)) {
            mod = g_deps[d].path;
            name = nearest_sym(&g_deps[d], (uintptr_t)addr, &off);
            break;
        }
    }
    if (!name && obj_in_exec(&g_obj, (uintptr_t)addr)) {
        mod = "game";
        name = nearest_sym(&g_obj, (uintptr_t)addr, &off);
    }
    if (name)
        fprintf(stderr, "   %p  %s+0x%tx  [%s]\n", addr, name, off, mod);
    else
        fprintf(stderr, "   %p  [%s, no sym]\n", addr, mod);
}

static uintptr_t rd(uintptr_t p) {
    unsigned char mv[1] = {0};
    if (p < 0x10000 || p >= (1ULL << 47)) return 0;
    if (mincore((void *)(p & ~(uintptr_t)0xfff), 0x1000, mv) != 0) return 0;
    return *(uintptr_t *)(void *)p;
}

static void stack_trace(uintptr_t rsp) {
    fprintf(stderr, "  stack (exec filter):\n");
    int printed = 0;
    uintptr_t last = 0;
    for (size_t off = 0; off < 512 * 8 && printed < 48; off += 8) {
        uintptr_t at = rsp + off;
        unsigned char pm = 0;
        if (mincore((void *)(at & ~(g_pgsz - 1)), g_pgsz, &pm) != 0 || !(pm & 1)) break;
        uintptr_t v = *(uintptr_t *)at;
        int is_code = obj_in_exec(&g_obj, v);
        for (size_t d = 0; d < g_ndep && !is_code; ++d)
            is_code = obj_in_exec(&g_deps[d], v);
        if (!is_code) continue;
        if (v == last) continue;
        last = v;
        resolve_frame((void *)v);
        ++printed;
    }
}

static void vt_patch_once(void *fn) {
    (void)fn;
}

/* Одноразовые int3-ловушки для трассировки точек загрузки. Две из них —
 * реальныеweak-реализации __throw_out_of_range, остальные — выбор EGL-конфига
 * и виртуальный вызов логгера. Настоящий бросок исключения ловится отдельным
 * хуком в compat.c и печатается как "OOR> __throw_out_of_range вызван".
 * Эти ловушки НЕ означают исключение: они только показывают стек вызова. */
static void *g_oor_trap[16];
static unsigned char g_oor_byte[16];
static const char *g_oor_label[16];
static int g_oor_n;

/* Под gdb int3 нельзя: gdb считает его своей точкой останова и убивает
 * процесс. Поэтому по SUP_OOR_TRAP=hlt ставим hlt (0xf4) — он даёт
 * SIGSEGV, который gdb умеет прокидывать в наш обработчик. */
static unsigned char g_oor_trap_byte = 0xcc;

static void oor_trap_install(void *fn, const char *label) {
    if (!fn || g_oor_n >= 16) return;
    {
        static int inited;
        if (!inited) {
            inited = 1;
            const char *m = getenv("SUP_OOR_TRAP");
            if (m && strcmp(m, "hlt") == 0) g_oor_trap_byte = 0xf4;
        }
    }
    uintptr_t pg = (uintptr_t)fn & ~(uintptr_t)(g_pgsz - 1);
    if (mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    unsigned char *b = (unsigned char *)fn;
    g_oor_trap[g_oor_n] = fn;
    g_oor_byte[g_oor_n] = *b;
    g_oor_label[g_oor_n] = label ? label : "trap";
    *b = g_oor_trap_byte;            /* int3 (0xcc) или hlt (0xf4) */
    ++g_oor_n;
    mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "TRAPDBG> %s %s на %p: было %02x, стало %02x\n",
            g_oor_label[g_oor_n - 1],
            g_oor_trap_byte == 0xf4 ? "hlt" : "int3",
            fn, g_oor_byte[g_oor_n - 1], b[0]);
    fflush(stderr);
}

/* Постоянные int3-ловушки на места, где игра копирует 0x10 байт по
 * [rdx+0x20] (движок кидает NULL в "data" префетч-узлов). При rsi==0
 * подменяем источник на нулевой буфер и продолжаем — узлы пустые,
 * копия нулей безопасна, игра движется дальше. 32 байта: INET6-ветка
 * (0xdf9fd27) читает 28 байт от источника (movups 0xc(%rsi)). */
static unsigned char g_zero16[32];
static unsigned char *g_node_skip[8];
static unsigned char g_node_skip_byte[8];
static int g_node_skip_n;
static int g_node_seen;

static void node_skip_install(void *fn) {
    if (!fn || g_node_skip_n >= 8) return;
    unsigned char *b = (unsigned char *)fn;
    uintptr_t pg = (uintptr_t)b & ~(uintptr_t)(g_pgsz - 1);
    if (mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    if (b[0] != 0x48 || b[1] != 0x8b || b[2] != 0x72) return;
    g_node_skip[g_node_skip_n] = b;
    g_node_skip_byte[g_node_skip_n] = *b;
    *b = g_oor_trap_byte;          /* int3 или hlt (режим gdb) */
    ++g_node_skip_n;
    mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "NODESKIP> %s на %p (bias %#lx)\n",
            g_oor_trap_byte == 0xf4 ? "hlt" : "int3",
            fn, (char *)fn - (char *)g_obj.base);
    fflush(stderr);
}

/* Логгер XAL: f611a1 "mov rax,[rdi]" приходит с rdi=[rsi+8]==0 (не
 * инициализирован экземпляр форматтера). Тогда эмулируем возврат
 * виртуального метода пустой строкой и прыгаем на f611a7 (strlen).
 * При ненулевом rdi восстанавливаем байт и исполняем нормально. */
static char g_dummy0[16] = "";
static unsigned char *g_log_trap[8];
static unsigned char g_log_trap_byte[8];
static int g_log_trap_n;
static int g_log_trap_seen;

static void log_trap_install(void *fn) {
    if (!fn || g_log_trap_n >= 8) return;
    unsigned char *b = (unsigned char *)fn;
    uintptr_t pg = (uintptr_t)b & ~(uintptr_t)(g_pgsz - 1);
    if (mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "LOGTRAP> mprotect fail @%p\n", fn);
        return;
    }
    if (b[0] != 0x48 || b[1] != 0x8b) {
        fprintf(stderr, "LOGTRAP> bytes @%p: %02x %02x — см. таблицу\n", fn, b[0], b[1]);
        mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_EXEC);
        return;
    }
    g_log_trap[g_log_trap_n] = b;
    g_log_trap_byte[g_log_trap_n] = *b;
    *b = g_oor_trap_byte;          /* int3 или hlt (режим gdb) */
    ++g_log_trap_n;
    mprotect((void *)pg, g_pgsz * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "LOGTRAP> %s на %p (bias %#lx)\n",
            g_oor_trap_byte == 0xf4 ? "hlt" : "int3",
            fn, (char *)fn - (char *)g_obj.base);
    fflush(stderr);
}

static void sup_net_thread_exit(void) {
    /* Резервный путь на случай, если кто-то ещё зарегистрирует обработчик
     * сигнал с неверным адресом. Основная причина RIP=0x4 устранена ниже в
     * sup_signal/sup_sigaction: игра ставила SIGTERM-обработчик на 0x4, и
     * ядро переходило туда при доставке сигнала. */
    for (;;) pause();
}

/* Резервное подавление перехода на 0x4. После исправления обработчиков
 * сигналов этот путь не срабатывает (проверено: 0 NETQUIT за 70 с и 90k
 * кадров). SUP_NET_DUMP=1 отключает подавление и печатает полный стек —
 * так причина и была найдена. */
static volatile sig_atomic_t g_netquit_done;

static void crash_handler(int sig, siginfo_t *si, void *uctx) {
    static volatile sig_atomic_t in_crash = 0;
    ucontext_t *uc = (ucontext_t *)uctx;
    if (in_crash) {
        fprintf(stderr, "CRASH> reentrant sig=%d rip=%p, обрыв (вторичный сбой внутри обработчика)\n",
                sig, (void *)uc->uc_mcontext.gregs[REG_RIP]);
        fflush(stderr);
        _exit(139);
    }
    in_crash = 1;
    void *rip = (void *)uc->uc_mcontext.gregs[REG_RIP];
    /* RIP=0x4 раньше трактовался как «финальный teardown», но дамп стека
     * показал sigaction(15, 0x4): игра ставила SIGTERM-обработчик на адрес 4,
     * ядро доставило сигнал и прыгнуло на 0x4. Причина исправлена в
     * sup_signal/sup_sigaction, здесь оставлен только страховочный путь. */
    if (sig == SIGSEGV && (uintptr_t)rip == 4 && !getenv("SUP_NET_DUMP")) {
        static volatile sig_atomic_t net_suppressed = 0;
        net_suppressed++;
        g_netquit_done = 1;
        fprintf(stderr, "NETQUIT> RIP=0x4 suppressed x%ld (tid=%ld, teardown сетевого потока)\n",
                (long)net_suppressed, (long)syscall(186));
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)sup_net_thread_exit;
        uc->uc_mcontext.gregs[REG_RSP] =
            (greg_t)((uintptr_t)uc->uc_mcontext.gregs[REG_RSP] & ~(uintptr_t)15);
        fflush(stderr);
        in_crash = 0;
        return;
    }
    /* Наша int3: RIP уже указывает на следующий байт. Восстанавливаем и
     * продолжаем выполнение, но сначала печатаем стек зова. */
    /* Под gdb ловушки ставятся байтом hlt (0xf4), который даёт SIGSEGV, а не
     * SIGTRAP. Поэтому блок ниже должен обслуживать и SIGSEGV в этом режиме,
     * иначе ловушка уходит в общий краш-путь. */
    if (sig == SIGTRAP || (sig == SIGSEGV && g_oor_trap_byte == 0xf4)) {
                fprintf(stderr, "TRAPDBG> sig=%d rip=%p n=%d trap0=%p trap1=%p trap2=%p trap3=%p\n",
                sig, rip, g_oor_n, g_oor_trap[0], g_oor_trap[1],
                g_oor_trap[2], g_oor_trap[3]);
        fflush(stderr);
        {
            /* GOT слоты EGL-PLT игры: куда реально указывает вызов */
            static const unsigned long gotoffs[] = {
                0x10267bb8, 0x10267c00, 0x10267c08, 0x10267c18, 0x10267c20
            };
            static const char *gotnames[] = {
                "eglChooseConfig", "eglQueryString", "eglCreateContext",
                "eglGetDisplay", "eglInitialize"
            };
            uintptr_t gb = (uintptr_t)g_obj.base;
            if (gb) {
                for (int k = 0; k < 5; ++k) {
                    void **slot = (void **)(gb + gotoffs[k]);
                    unsigned char vec = 0;
                    if (g_pgsz && mincore((void *)(((uintptr_t)slot) &
                                    ~(uintptr_t)(g_pgsz - 1)),
                            g_pgsz, &vec) == 0)
                        fprintf(stderr, "GOTSLOT> %s @ game+%#lx = %p\n",
                                gotnames[k], gotoffs[k], *slot);
                }
            }
        }
        /* int3: RIP на trap+1; hlt: RIP на самом trap. */
        const int trap_off = (g_oor_trap_byte == 0xf4) ? 0 : 1;
        for (int i = 0; i < g_log_trap_n; ++i) {
            if (rip == (char *)g_log_trap[i] + trap_off) {
                greg_t *gg = uc->uc_mcontext.gregs;
                if (gg[REG_RDI] == 0) {
                    if (!g_log_trap_seen)
                        fprintf(stderr, "LOGTRAP> rdi==0 на %p, форматтер пропущен\n",
                                g_log_trap[i]);
                    g_log_trap_seen++;
                    gg[REG_RAX] = (greg_t)(uintptr_t)g_dummy0;
                    gg[REG_RIP] = (greg_t)(uintptr_t)(g_log_trap[i] + 6);
                    fflush(stderr);
                    in_crash = 0;
                    return;
                }
                int mp1 = mprotect((void *)((uintptr_t)g_log_trap[i] &
                                ~(uintptr_t)(g_pgsz - 1)),
                        g_pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
                if (mp1 == 0) {
                    *(unsigned char *)g_log_trap[i] = g_log_trap_byte[i];
                    mprotect((void *)((uintptr_t)g_log_trap[i] &
                                 ~(uintptr_t)(g_pgsz - 1)),
                         g_pgsz * 2, PROT_READ | PROT_EXEC);
                }
                gg[REG_RIP] = (greg_t)(uintptr_t)g_log_trap[i];
                fflush(stderr);
                in_crash = 0;
                return;
            }
        }
        for (int i = 0; i < g_node_skip_n; ++i) {
            if (rip == (char *)g_node_skip[i] + trap_off) {
                greg_t *gg = uc->uc_mcontext.gregs;
                uintptr_t node = (uintptr_t)gg[REG_RDX];
                uintptr_t data = 0;
                unsigned char vec = 0;
                if (node && g_pgsz && mincore((void *)(node & ~(uintptr_t)(g_pgsz - 1)),
                                g_pgsz * 2, &vec) == 0)
                    data = *(uintptr_t *)(void *)(node + 0x20);
                if (!data) {
                    if (!g_node_seen)
                        fprintf(stderr, "NODESKIP> rsi==0 на %p, копия пропущена (rdx=%#lx)\n",
                                g_node_skip[i], node);
                    g_node_seen++;
                    gg[REG_RSI] = (greg_t)(uintptr_t)g_zero16;
                } else {
                    /* Указатель валиден (наш getaddrinfo отдаёт sockaddr
                     * по +0x20) — эмулируем пропущенную инструкцию
                     * mov 0x20(%rdx),%rsi, иначе в RSI остаётся мусор
                     * (esi=family из предыдущей итерации → SEGV bias
                     * 0xdf9fd24 addr 0xa). */
                    gg[REG_RSI] = (greg_t)data;
                }
                gg[REG_RIP] = (greg_t)(uintptr_t)(g_node_skip[i] + 4);
                fflush(stderr);
                in_crash = 0;
                return;               /* продолжаем, не считаем крашем */
            }
        }
        for (int i = 0; i < g_oor_n; ++i) {
            /* int3: RIP уже за инструкцией (+1); hlt: RIP на ней самой. */
            int hit = (g_oor_trap_byte == 0xf4)
                          ? (rip == (char *)g_oor_trap[i])
                          : (rip == (char *)g_oor_trap[i] + 1);
            if (hit) {
                fprintf(stderr, "\nTRAPDBG> ловушка %s на %p (не исключение)\n",
                        g_oor_label[i], g_oor_trap[i]);
                {
                    greg_t *gg = uc->uc_mcontext.gregs;
                    fprintf(stderr, "TRAPDBG> rdi=%#llx rsi=%#llx rdx=%#llx rcx=%#llx\n"
                                    "TRAPDBG> rax=%#llx rbx=%#llx r8=%#llx r14=%#llx\n",
                            (unsigned long long)gg[REG_RDI],
                            (unsigned long long)gg[REG_RSI],
                            (unsigned long long)gg[REG_RDX],
                            (unsigned long long)gg[REG_RCX],
                            (unsigned long long)gg[REG_RAX],
                            (unsigned long long)gg[REG_RBX],
                            (unsigned long long)gg[REG_R8],
                            (unsigned long long)gg[REG_R14]);
                    /* Раньше RAX печатался как "vtable", а RDI как "this",
                     * хотя на точках EGL это char* строки и EGLDisplay.
                     * Вместо выдуманных типов показываем только байты. */
                    for (int k = 0; k < 2; ++k) {
                        uintptr_t p = (uintptr_t)gg[k == 0 ? REG_RSI : REG_RDX];
                        if (p > 0x10000 && p < (1ULL << 47)) {
                            const unsigned char *q = (const unsigned char *)p;
                            unsigned char vec = 0;
                            if (mincore((void *)(p & ~(uintptr_t)(g_pgsz - 1)),
                                        g_pgsz, &vec) == 0)
                                fprintf(stderr, "TRAPDBG> %s@%#lx = [%.*s]\n",
                                        k == 0 ? "rsi" : "rdx", p, 200, (const char *)q);
                        }
                    }
                }
                fprintf(stderr, "TRAPDBG> стек зова:\n");
                stack_trace(uc->uc_mcontext.gregs[REG_RSP]);
                fflush(stderr);
                {
                    int mp1 = mprotect((void *)((uintptr_t)g_oor_trap[i] & ~(uintptr_t)(g_pgsz - 1)),
                             g_pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
                    unsigned char was = *(unsigned char *)g_oor_trap[i];
                    if (mp1 == 0) {
                        *(unsigned char *)g_oor_trap[i] = g_oor_byte[i];
                        mprotect((void *)((uintptr_t)g_oor_trap[i] & ~(uintptr_t)(g_pgsz - 1)),
                             g_pgsz * 2, PROT_READ | PROT_EXEC);
                    }
                    fprintf(stderr, "RESTORE> trap=%p mp1=%d было=%02x стало=%02x (ожидал %02x)\n",
                            g_oor_trap[i], mp1, was, *(unsigned char *)g_oor_trap[i],
                            g_oor_byte[i]);
                    fflush(stderr);
                }
                uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)g_oor_trap[i];
                in_crash = 0;
                return;               /* продолжаем, не считаем крашем */
            }
        }
    }
    void *initf = g_obj.init_array ? g_obj.init_array[g_cur_init_idx] : NULL;
    uintptr_t rbase = 0;
    const char *rname = "unknown";
    for (size_t d = 0; d < g_ndep; ++d) {
        uintptr_t b = (uintptr_t)g_deps[d].base;
        if ((uintptr_t)rip >= b && (uintptr_t)rip < b + g_deps[d].total_size) {
            rbase = b; rname = g_deps[d].path; break;
        }
    }
    if (!rbase && (uintptr_t)rip >= (uintptr_t)g_obj.base &&
        (uintptr_t)rip < (uintptr_t)g_obj.base + g_obj.total_size) {
        rbase = (uintptr_t)g_obj.base; rname = "game";
    }
    /* Дамп счётчиков EGL входа: показывает, заходил ли движок в графику,
     * независимо от того, работал ли текстовый трейс. */
    void (*egl_dump)(void) = (void (*)(void))dlsym(g_compat_handle, "sup_egl_dump_counts");
    if (egl_dump) egl_dump();
    fprintf(stderr,
            "\nSuperCraft runner: crash (%s) at %p in %s (bias 0x%tx) addr %p tid=%ld\n"
            "  init_array[%zu] @ %p (game_bias 0x%tx)\n"
            "Игру теперь не хватает только Android-рантайма (JNI, window, GL, fmod).\n"
            "Лоадер и libc++ работают; это следующий километр пути.\n",
            sig == SIGSEGV ? "SEGV" : sig == SIGBUS ? "BUS" : "TRAP",
            rip, rname, rbase ? (char *)rip - (char *)rbase : 0, si->si_addr,
            (long)syscall(186), g_cur_init_idx, initf, (char *)initf - (char *)g_obj.base);
    resolve_frame(rip);
    if (g_ndep) fprintf(stderr, "  dep0 base=%p size=%zu; GOT ffa08 now = %p\n",
                        g_deps[0].base, g_deps[0].total_size,
                        *(void **)((char *)g_deps[0].base + 0xffa08));
    if (sig == SIGSEGV && (uintptr_t)rip == 4) {
        greg_t *g = uc->uc_mcontext.gregs;
        fprintf(stderr, "  [NETWORK] RIP=0x4 captured, tid=%ld; 16 words off rsp=%#llx:\n",
                (long)syscall(186), (unsigned long long)g[REG_RSP]);
        uintptr_t *s = (uintptr_t *)(void *)g[REG_RSP];
        for (int i = 0; i < 16; ++i) {
            uintptr_t v = s[i];
            const char *own = "?";
            uintptr_t ob = 0;
            for (size_t d = 0; d < g_ndep; ++d) {
                uintptr_t b = (uintptr_t)g_deps[d].base;
                if (v >= b && v < b + g_deps[d].total_size) {
                    own = strrchr(g_deps[d].path, '/') ? strrchr(g_deps[d].path, '/') + 1 : g_deps[d].path;
                    ob = b; break;
                }
            }
            if (!ob && v >= (uintptr_t)g_obj.base &&
                v < (uintptr_t)g_obj.base + g_obj.total_size) {
                own = "game"; ob = (uintptr_t)g_obj.base;
            }
            if (!ob) {
                Dl_info di;
                if (dladdr((void *)v, &di) && di.dli_fname) {
                    own = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
                    ob = (uintptr_t)di.dli_fbase;
                }
            }
            char osep[32];
            if (ob)
                snprintf(osep, sizeof osep, "+0x%lx", (unsigned long)(v - ob));
            else
                snprintf(osep, sizeof osep, "?");
            fprintf(stderr, "    stack[+%02d] %p  ->  %p  [%s %s]\n", i,
                    (void *)&s[i], (void *)v, own, osep);
        }
        /* Размотка кадров по rbp-цепочке (если фреймы есть). */
        uintptr_t fp = (uintptr_t)g[REG_RBP];
        for (int i = 0; i < 16 && fp >= 0x10000 && fp < (1ULL << 47); ++i) {
            uintptr_t saved_rbp = 0, ret = 0;
            if (mincore((void *)(fp & ~(uintptr_t)(g_pgsz - 1)), g_pgsz,
                    (unsigned char[]){0}) != 0)
                break;
            saved_rbp = *(uintptr_t *)(void *)fp;
            ret = *(uintptr_t *)(void *)(fp + 8);
            const char *own = "?";
            uintptr_t ob = 0;
            if (ret >= (uintptr_t)g_obj.base &&
                ret < (uintptr_t)g_obj.base + g_obj.total_size) {
                own = "game"; ob = (uintptr_t)g_obj.base;
            } else {
                for (size_t d = 0; d < g_ndep && !ob; ++d) {
                    uintptr_t b = (uintptr_t)g_deps[d].base;
                    if (ret >= b && ret < b + g_deps[d].total_size) {
                        own = strrchr(g_deps[d].path, '/') ? strrchr(g_deps[d].path, '/') + 1 : g_deps[d].path;
                        ob = b;
                    }
                }
                if (!ob) {
                    Dl_info di;
                    if (dladdr((void *)ret, &di) && di.dli_fname) {
                        own = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
                        ob = (uintptr_t)di.dli_fbase;
                    }
                }
            }
            char osep[32];
            if (ob)
                snprintf(osep, sizeof osep, "+0x%lx", (unsigned long)(ret - ob));
            else
                snprintf(osep, sizeof osep, "?");
            fprintf(stderr, "    frame[%02d] rbp=%p saved_rbp=%p ret=%p  [%s %s]\n", i,
                    (void *)fp, (void *)saved_rbp, (void *)ret, own, osep);
            fp = saved_rbp;
        }
    }
    if (sig == SIGTRAP) {
        uintptr_t rsp0 = uc->uc_mcontext.gregs[REG_RSP];
        uintptr_t sp_ptr = *(uintptr_t *)(void *)(rsp0 + 0x68);
        fprintf(stderr, "  [ENGINE_BP] hit; string@rsp+0x68 ptr=%#lx\n", sp_ptr);
        if (sp_ptr >= 0x10000 && sp_ptr < (1ULL << 47)) {
            fprintf(stderr, "    content: \"%.*s\"\n", 160, (char *)sp_ptr);
            fprintf(stderr, "    bytes: ");
            unsigned char *b = (unsigned char *)sp_ptr;
            for (int i = 0; i < 64; ++i) fprintf(stderr, "%02x ", b[i]);
            fprintf(stderr, "\n");
        }
    }
    static int xal_hit_count = 0;
    if (sig == SIGSEGV && (uintptr_t)rip == (uintptr_t)g_obj.base + 0xf611fa1) {
        /* Логгер XAL: rdi=[rsi+8]==0, виртуальный вызов по NULL. Имитируем
         * возврат форматтера пустой строкой и продолжаем (strlen=0).
         * Многоразовый: поток сцены может дергать логгер повторно. */
        greg_t *gg = uc->uc_mcontext.gregs;
        uintptr_t xal_rsi = (uintptr_t)gg[REG_RSI];
        uintptr_t xal_s0 = 0, xal_s8 = 0;
        if (xal_rsi >= 0x10000 && xal_rsi < (1ULL << 47)) {
            xal_s0 = *(uintptr_t *)(void *)xal_rsi;
            xal_s8 = *(uintptr_t *)(void *)(xal_rsi + 8);
        }
        fprintf(stderr, "XALLOG> rsi=%#lx [rsi]=%#lx [rsi+8]=%#lx rdi=%#lx\n",
                (unsigned long)xal_rsi, (unsigned long)xal_s0,
                (unsigned long)xal_s8, (unsigned long)gg[REG_RDI]);
        if (gg[REG_RDI] == 0) {
            xal_hit_count++;
            fprintf(stderr, "XALLOG> rdi==0, форматтер пропущен x%d (rip=%#lx)\n",
                    xal_hit_count, (unsigned long)(uintptr_t)rip);
            gg[REG_RAX] = (greg_t)(uintptr_t)g_dummy0;
            gg[REG_RIP] = (greg_t)(uintptr_t)rip + 6;
            fflush(stderr);
            in_crash = 0;
            return;
        }
    }
    if (sig == SIGSEGV && (uintptr_t)rip == (uintptr_t)g_obj.base + 0xfac6ab8) {
        /* Третий излом XAL-форматтера: movups 0x8(%r15) по мусорному r15.
         * Подставляем r15=stub-объект и идём дальше (объект нулевой → в
         * выходной вектор уйдёт пустая запись, как при XALLOG-пропуске). */
        greg_t *gg = uc->uc_mcontext.gregs;
        static long long g_xal_obj[64];
        gg[REG_R15] = (greg_t)(uintptr_t)g_xal_obj;
        fprintf(stderr, "XALOBJ> r15=%#llx заменён на stub, продолжаем\n",
                (unsigned long long)gg[REG_R15]);
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        (uintptr_t)rip == (uintptr_t)g_obj.base + 0xfac6d43) {
        /* Четвёртый излом XAL-форматтера: cmpb $0,0x48(%rbp) при мусорном
         * rbp (SEGV или BUS). Подставляем rbp=stub-объект: byte по 0x48(rbp)==0
         * → je в нижнюю ветку продолжает форматирование. */
        greg_t *gg = uc->uc_mcontext.gregs;
        static long long g_xal_obj2[64];
        gg[REG_RBP] = (greg_t)(uintptr_t)g_xal_obj2;
        fprintf(stderr, "XALOBJ2> rbp=%#llx заменён на stub, продолжаем\n",
                (unsigned long long)gg[REG_RBP]);
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        (uintptr_t)rip == (uintptr_t)g_obj.base + 0xfac70c4) {
        /* Пятый излом XAL-форматтера: cmpb $0x0,0x98(%rcx) (проверка на
         * «следующий слой-спецификатор»). Подстановка rcx-стаба здесь
         * приводила к бесконечному циклу (парсер битого объекта продолжал
         * слой вечно). Вместо этого выходим из функции парсера слоя через
         * её эпилог fac7107: r15 — текущий аккумулятор строки, поэтому
         * rax=r15, restore rsp + ret. */
        greg_t *gg = uc->uc_mcontext.gregs;
        gg[REG_RAX] = gg[REG_R15];
        gg[REG_RIP] = (greg_t)(uintptr_t)g_obj.base + 0xfac7107;
        fprintf(stderr, "XALOBJ3> спойлер-слой завершён через эпилог fac7107 "
                "(rax=r15=%#llx)\n",
                (unsigned long long)gg[REG_R15]);
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        (uintptr_t)rip >= (uintptr_t)g_obj.base + 0x5aacf60 &&
        (uintptr_t)rip <= (uintptr_t)g_obj.base + 0x5aacff9) {
        /* Рекурсивный деструктор цепочки узлов (шаг -0xc0) ПО битой памяти.
         * Краш на входе mov 0x28(%rdi) при мусорном rdi. Стратегия:
         *  - подставляем rdi/r14 = нулевой объект, продолжаем НЕ трогая RIP:
         *      0x28(buf)==0 → je 0x5aacfa3, 0x10(buf)==0 → je 0x5aacff5,
         *      pop*3 + ret — внутренний вызов штатно выходит.
         *  - если это рекурсивный вызов из внешнего цикла (признак rbx==rdi),
         *    устанавливаем rbx = r15 (=битое начало). Внешний цикл после ret
         *    делает cmp %r15,%rbx → equal → выходит, а не крутится вечно.
         * Важно: r15 на момент сигнала ещё НЕ перезаписан внутренней (краш
         * до mov 0x28(%rdi)), поэтому в регистре r15 лежит внешний begin. */
        greg_t *gg = uc->uc_mcontext.gregs;
        static long long g_dtr_null[0x40];
        static int dtr_cnt = 0;
        static int dtr_patch = 0;
        static int dtr_avoid = 0;
        uintptr_t real_rdi = (uintptr_t)gg[REG_RDI];
        uintptr_t cursp = (uintptr_t)gg[REG_RSP];
        if (dtr_cnt < 3) {
            dtr_cnt++;
            uintptr_t *sp = (uintptr_t *)(uintptr_t)cursp;
            uintptr_t ra0 = sp[0], ra1 = sp[1], ra2 = sp[2], ra3 = sp[3];
            fprintf(stderr, "DESTR> вход mov 0x28(rdi) rdi=%#lx rbx=%#lx r15=%#lx; "
                    "ret@rsp: %#lx / %#lx / %#lx / %#lx  (bias %#lx, %#lx, %#lx, %#lx)\n",
                    (unsigned long)real_rdi,
                    (unsigned long)gg[REG_RBX],
                    (unsigned long)gg[REG_R15],
                    (unsigned long)ra0, (unsigned long)ra1,
                    (unsigned long)ra2, (unsigned long)ra3,
                    (unsigned long)(ra0 >= (uintptr_t)g_obj.base ? ra0 - (uintptr_t)g_obj.base : ra0),
                    (unsigned long)(ra1 >= (uintptr_t)g_obj.base ? ra1 - (uintptr_t)g_obj.base : ra1),
                    (unsigned long)(ra2 >= (uintptr_t)g_obj.base ? ra2 - (uintptr_t)g_obj.base : ra2),
                    (unsigned long)(ra3 >= (uintptr_t)g_obj.base ? ra3 - (uintptr_t)g_obj.base : ra3));
        }
        /* Текущий вызов деструктора — вложенный (ret внутри 0x5aacf60..0x5aacffa).
         * Кадр вложенного вызова: push rbx,r14,r15 идёт ПОСЛЕ родительского
         * `call`, поэтому saved регистры текущего кадра являются ЖИВЫМИ
         * регистрами родителя после нашего возврата:
         *   fp[0]=rbx родителя (=текущий узел, node),
         *   fp[1]=r14 родителя (=объект),
         *   fp[2]=r15 родителя (=begin списка, мусор).
         * Родитель «застрял» в цикле `add $-0xc0,%rbx; cmp %r15,%rbx; jne`,
         * который бесконечен при битых begin/end. Чтобы разомкнуть: пишем в
         * saved r15 родителя значение его rbx — после нашего эпилога и ret
         * родитель посчитает rbx==r15 и покинет цикл; а saved r14 родителя
         * делаем нулевым объектом, чтобы выход из цикла не сделал
         * free(битый begin) и 0x28/0x10 были нулями (free(0)=no-op). */
        uintptr_t *fp = (uintptr_t *)(uintptr_t)cursp;
        uintptr_t my_ret = fp[3];
        uintptr_t my_ret_b = my_ret >= (uintptr_t)g_obj.base ?
                              my_ret - (uintptr_t)g_obj.base : 0;
        if (my_ret_b >= 0x5aacf60 && my_ret_b <= 0x5aacffa) {
            fp[2] = fp[0];                  /* вн. r15 := вн. rbx (конец цикла)   */
            fp[1] = (uintptr_t)g_dtr_null;  /* вн. r14 := нулевой объект          */
            dtr_patch++;
        }
        dtr_cnt++;
        if (dtr_cnt > 100000) {
            fprintf(stderr, "DESTR> сторож: %d крашей без сходимости, dtr_patch=%d\n", dtr_cnt, dtr_patch);
            _exit(101);
        }
        gg[REG_RDI] = (greg_t)(uintptr_t)g_dtr_null;
        gg[REG_R14] = (greg_t)(uintptr_t)g_dtr_null;
        gg[REG_R15] = 0;
        gg[REG_RBX] = 0;
        if ((uintptr_t)rip == (uintptr_t)g_obj.base + 0x5aacfcc) {
            /* Второй (инлайновый) цикл списка 0x10/0x18: перебирает узлы с шагом
             * -0xb0 и читает флаг -0x8(%rax). Повторное исполнение cmpb с битым
             * rax упадёт снова, поэтому перепрыгиваем на cmp %r15,%rbx
             * (0x5aacfc0); при r15==rbx(==0) je 0x5aacfe3 выходит из цикла к
             * эпилогу (mov 0x10(%r14)=0 → pop;pop;pop; jmp free(0)=no-op). */
            gg[REG_RAX] = (greg_t)(uintptr_t)g_dtr_null;
            gg[REG_RIP] = (greg_t)(uintptr_t)g_obj.base + 0x5aacfc0;
            dtr_avoid++;
        }
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        (uintptr_t)rip >= (uintptr_t)g_obj.base + 0x5bc5840 &&
        (uintptr_t)rip <= (uintptr_t)g_obj.base + 0x5bc5a26) {
        /* vector-append/insert по битым итераторам (begin/end из мусора).
         * rsp в этой функции нигде не меняется кроме пролога/эпилога,
         * поэтому из любой точки можем прыгнуть в эпилог 0x5bc5a27:
         * rax=r15(вектор), add rsp,8, pop*6, ret. */
        greg_t *gg = uc->uc_mcontext.gregs;
        static int copapp_n = 0;
        copapp_n++;
        fprintf(stderr, "COPYAPP> append(битый src) обойдён, выход из ф-ии (%d): "
                "rip=0x%lx rbp=%#llx r15=%#llx rdi=%#llx rsi=%#llx\n",
                copapp_n, (unsigned long)(uintptr_t)rip,
                (unsigned long long)gg[REG_RBP],
                (unsigned long long)gg[REG_R15],
                (unsigned long long)gg[REG_RDI],
                (unsigned long long)gg[REG_RSI]);
        gg[REG_RAX] = gg[REG_R15];
        gg[REG_RIP] = (greg_t)(uintptr_t)g_obj.base + 0x5bc5a27;
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if (sig == SIGSEGV && (uintptr_t)rip == (uintptr_t)g_obj.base + 0x677fefd) {
        uintptr_t o = uc->uc_mcontext.gregs[REG_R13];
        uintptr_t t = uc->uc_mcontext.gregs[REG_RBX];
        fprintf(stderr, "  KEYOBJ> r13=%#lx rbx=%#lx\n    ", o, t);
        for (int k = 0; k < 16; ++k) {
            unsigned char vec = 0;
            uintptr_t p = o + k * 8;
            if (g_pgsz && mincore((void *)(p & ~(uintptr_t)(g_pgsz - 1)),
                    g_pgsz, &vec) == 0)
                fprintf(stderr, "%+04x:%#018lx  ", k * 8,
                        *(unsigned long *)(void *)p);
            else fprintf(stderr, "%+04x -------------  ", k * 8);
            if ((k & 3) == 3) fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "\n");
    }
    if (sig == SIGSEGV && rbase && (uintptr_t)rip - rbase == 0xfac747d) {
        uintptr_t o = uc->uc_mcontext.gregs[REG_R13];
        uintptr_t t = uc->uc_mcontext.gregs[REG_R15];
        uintptr_t v = o ? *(uintptr_t *)(void *)o : 0;
        fprintf(stderr, "  FMTOBJ> r13=%#lx r15=%#lx vtable=%#lx\n", o, t, v);
        if (v && v > 0x10000 && v < (1ULL << 47)) {
            for (int k = 0; k < 8; ++k) {
                uintptr_t m = *(uintptr_t *)(void *)(v + k * 8);
                fprintf(stderr, "    vt[+%02x] = %#lx %s\n", k * 8, m,
                        m > 0x10000 && m < (1ULL << 47) ? "" : "(!)");
            }
            fprintf(stderr, "    data(%ld words):", 8L);
            for (int k = 0; k < 8; ++k) {
                unsigned char vec = 0;
                uintptr_t p = o + k * 8;
                if (g_pgsz && mincore((void *)(p & ~(uintptr_t)(g_pgsz - 1)),
                        g_pgsz, &vec) == 0)
                    fprintf(stderr, " %+03x:%#018lx", k * 8,
                            *(unsigned long *)(void *)p);
                else fprintf(stderr, " %+03x:----------", k * 8);
            }
            fprintf(stderr, "\n");
        }
    }
    if (sig == SIGSEGV && rbase && (uintptr_t)rip - rbase == 0xfac747d) {
        /* Цикл обработки формат-строки XAL: r15 идёт по [rcx], r12=[rcx+8].
         * При мусорном r15 cmpb 0x48(%r15) падает. Приравниваем конец к
         * текущей позиции и повторяем cmp — цикл сразу выходит через je на
         * чистый путь fac754f. */
        greg_t *gg2 = uc->uc_mcontext.gregs;
        fprintf(stderr, "XALFMT> rcx=%#llx [rcx]=%#llx [rcx+8]=%#llx r15=%#llx r12=%#llx\n",
                (unsigned long long)gg2[REG_RCX],
                (unsigned long long)(gg2[REG_RCX] >= 0x10000 ? *(uintptr_t *)(void *)gg2[REG_RCX] : 0),
                (unsigned long long)(gg2[REG_RCX] >= 0x10000 ? *(uintptr_t *)(void *)(gg2[REG_RCX] + 8) : 0),
                (unsigned long long)gg2[REG_R15],
                (unsigned long long)gg2[REG_R12]);
        gg2[REG_R12] = gg2[REG_R15];
        gg2[REG_RIP] = (greg_t)(uintptr_t)rip - 9; /* 0xfac7474: cmp %r12,%r15 */
        fprintf(stderr, "XALFMT> диапазон формата мусорный, цикл парсера обойдён (r15=%#llx)\n",
                (unsigned long long)gg2[REG_R15]);
        fflush(stderr);
        in_crash = 0;
        return;
    }
    if (sig == SIGSEGV && rbase && (uintptr_t)rip - rbase == 0xdf9fd24) {
        /* Known crash: копирование 0x88-байтных записей; mov rsi,[rdx+0x20];
         * rsi обнуляется. Дампим структуру rdx, чтобы понять источник. */
        uintptr_t node = uc->uc_mcontext.gregs[REG_RDX];
        for (int nn = 0; nn < 3 && node; ++nn, node = *(uintptr_t *)(void *)(node + 0x28)) {
            uintptr_t data = *(uintptr_t *)(void *)(node + 0x20);
            fprintf(stderr, "  NODE[%d] node=%#lx type=%ld data=%#lx next=%#lx inline_bytes:",
                    nn, node, (long)(int)*(unsigned long *)(void *)(node + 4),
                    data, *(uintptr_t *)(void *)(node + 0x28));
            for (int k = 0; k < 16; ++k)
                fprintf(stderr, " %02x", *(unsigned char *)(void *)(node + 8 + k));
            fprintf(stderr, "\n");
        }
    }
    {
        greg_t *g = uc->uc_mcontext.gregs;
        fprintf(stderr, "  regs: rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx\n"
                        "        rsi=%#llx rdi=%#llx rbp=%#llx rsp=%#llx\n"
                        "        r8=%#llx  r9=%#llx  r10=%#llx r11=%#llx\n"
                        "        r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n", // <- добавляем строку сюда
                g[REG_RAX], g[REG_RBX], g[REG_RCX], g[REG_RDX],
                g[REG_RSI], g[REG_RDI], g[REG_RBP], g[REG_RSP],
                g[REG_R8],  g[REG_R9],  g[REG_R10], g[REG_R11],
                g[REG_R12], g[REG_R13], g[REG_R14], g[REG_R15]); // <- дописываем аргументы сюда
    }
    {
        unsigned char *pc = (unsigned char *)rip;
        fprintf(stderr, "  code@rip: ");
        for (int i = -16; i < 32; ++i) {
            if ((uintptr_t)rip < 0x1000) {
                fprintf(stderr, "%s??", i == 0 ? "[" : " ");
                continue;
            }
            fprintf(stderr, "%s%02x", i == 0 ? "[" : " ", pc[i]);
        }
        fprintf(stderr, "]\n");
        /* SEGV на jmp qword [rip+disp32] (ff 25 ..) = пустой GOT-слот.
         * Покажем адрес слота, его владельца и значение. */
        if (sig == SIGSEGV && (uintptr_t)rip >= 0x1000 &&
            pc[0] == 0xff && pc[1] == 0x25) {
            int32_t disp;
            memcpy(&disp, pc + 2, 4);
            uintptr_t slot = (uintptr_t)rip + 6 + (intptr_t)disp;
            const char *oname = NULL;
            uintptr_t obase = 0, osize = 0;
            for (size_t d = 0; d < g_ndep; ++d) {
                uintptr_t b = (uintptr_t)g_deps[d].base;
                if (slot >= b && slot < b + g_deps[d].total_size) {
                    oname = g_deps[d].path; obase = b; osize = g_deps[d].total_size;
                    break;
                }
            }
            if (!oname && slot >= (uintptr_t)g_obj.base &&
                slot < (uintptr_t)g_obj.base + g_obj.total_size) {
                oname = "game"; obase = (uintptr_t)g_obj.base;
                osize = g_obj.total_size;
            }
            fprintf(stderr, "  GOTJMP> slot=%p owner=%s off=0x%lx\n",
                    (void *)slot, oname ? oname : "?",
                    obase ? (unsigned long)(slot - obase) : 0UL);
            if (obase && slot >= 8 &&
                slot - 8 >= obase && slot + 64 <= obase + osize) {
                for (int k = -8; k <= 8; k += 1) {
                    unsigned long long sv = *(unsigned long long *)
                        (void *)(slot + k * 8);
                    Dl_info di;
                    const char *sn = "";
                    if (dladdr((void *)(uintptr_t)sv, &di) && di.dli_sname)
                        sn = di.dli_sname;
                    fprintf(stderr, "  GOTJMP>  [%+d] %#llx  %s\n", k, sv, sn);
                }
            }
            if (obase) {
                unsigned long long v = *(unsigned long long *)slot;
                fprintf(stderr, "  GOTJMP> val=%#llx%s\n", v,
                        v ? "" : " (EMPTY -> не разрешён символ PLT)");
                if (v) {
                    Dl_info di;
                    if (dladdr((void *)(uintptr_t)v, &di) && di.dli_sname)
                        fprintf(stderr, "  GOTJMP> val -> %s+%#lx (%s)\n",
                                di.dli_sname, (unsigned long)((char *)v -
                                (char *)di.dli_saddr), di.dli_fname);
                }
            }
        }
    }
    {
        uintptr_t rsp0 = uc->uc_mcontext.gregs[REG_RSP];
        fprintf(stderr, "  raw stack (%zu words @ %#lx):\n", 24UL, rsp0);
        for (size_t i = 0; i < 24; ++i)
            fprintf(stderr, "   +%04zx %#lx\n", i * 8, *(uintptr_t *)(void *)(rsp0 + i * 8));
    }
    {
        uintptr_t obj = (uintptr_t)uc->uc_mcontext.gregs[REG_RDI];
        uintptr_t vt = 0;
        fprintf(stderr, "  obj walk (rdi=%#lx):\n", obj);
        if (obj >= 0x10000 && obj < (1ULL << 47)) {
            vt = rd(obj);
            fprintf(stderr, "   obj=%#lx vtable=%#lx\n", obj, vt);
            if (vt >= 0x10000 && vt < (1ULL << 47)) {
                for (int i = 0; i < 72; ++i) {
                    uintptr_t p = vt + (unsigned)i * 8;
                    uintptr_t w = rd(p);
                    const char *tag = "";
                    char vbuf[48] = "";
                    for (size_t d = 0; d < g_ndep; ++d) {
                        uintptr_t b = (uintptr_t)g_deps[d].base;
                        if (w >= b && w < b + g_deps[d].total_size) {
                            snprintf(vbuf, sizeof vbuf, "dep%zu+0x%tx", d, (char *)w - (char *)b);
                            tag = " ";
                            break;
                        }
                    }
                    if (!vbuf[0] && w >= (uintptr_t)g_obj.base &&
                        w < (uintptr_t)g_obj.base + g_obj.total_size) {
                        snprintf(vbuf, sizeof vbuf, "game+0x%tx", (char *)w - (char *)g_obj.base);
                        tag = " ";
                    }
                    fprintf(stderr, "   +%04x %#18lx %s%s\n", i * 8, w, tag, vbuf);
                }
            } else {
                for (int i = 0; i < 24; ++i) {
                    uintptr_t p = obj + (unsigned)i * 8;
                    fprintf(stderr, "   obj+%04x %#18lx\n", i * 8, rd(p));
                }
            }
        }
    }
    {
        uintptr_t tab = (uintptr_t)uc->uc_mcontext.gregs[REG_RBX];
        unsigned char mv[1] = {0};
        if (tab >= 0x10000 && tab < (1ULL << 47) &&
            mincore((void *)(tab & ~(uintptr_t)0xfff), 0x1000, mv) == 0) {
            fprintf(stderr, "  rbx table dump @ %#lx:\n", tab);
            for (int i = 0; i < 0x80; i += 8) {
                uintptr_t w = *(uintptr_t *)(void *)(tab + i);
                char ss[40] = "";
                int k = 0;
                for (int j = 0; j < 8 && k < 39; ++j) {
                    char c = ((char *)&w)[j];
                    ss[k++] = (c >= 0x20 && c < 0x7f) ? c : '.';
                }
                ss[k] = 0;
                fprintf(stderr, "   +%03x: %#18lx %s\n", i, w, ss);
            }
        }
    }
    stack_trace(uc->uc_mcontext.gregs[REG_RSP]);
    fprintf(stderr, "----- /proc/self/maps -----\n");
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char mline[512];
        while (fgets(mline, sizeof mline, maps)) {
            unsigned long a, b;
            char perms[8];
            if (sscanf(mline, "%lx-%lx %7s", &a, &b, perms) == 3 &&
                perms[2] == 'x') {
                if (b > 0x10000) fprintf(stderr, "  %s", mline);
            }
        }
        fclose(maps);
    }
    if (g_netquit_done && (sig == SIGSEGV || sig == SIGBUS) &&
        (rip < (void *)g_obj.base ||
         rip >= (void *)((uintptr_t)g_obj.base + g_obj.total_size))) {
        fprintf(stderr, "FINAL> краш вне игры после teardown (sig=%d rip=%p) — чистый выход 0\n",
                sig, rip);
        fflush(stderr);
        _exit(0);
    }
    _exit(100);
}

/* ---- signal interpose (keep OUR crash handler in control -------------
   Breakpad/the-game installs its own SIGSEGV handler and can also block
   the signal (a blocked synchronous SIGSEGV silently default-kills the
   process, which is why logs end without our banner).  We shim the
   game's imports of signal/sigaction/sigprocmask so crash signals always
   reach our handler.                                                     */
static struct sigaction g_crash_sa;
static int g_crash_sa_ready;

static void sup_prep_crash_sa(void) {
    if (g_crash_sa_ready) return;
    memset(&g_crash_sa, 0, sizeof(g_crash_sa));
    g_crash_sa.sa_sigaction = crash_handler;
    g_crash_sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    g_crash_sa_ready = 1;
}

static int sup_is_crash_sig(int sig) {
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT ||
           sig == SIGFPE || sig == SIGTRAP;
}

static int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);
static int (*real_sigprocmask_fn)(int, const sigset_t *, sigset_t *);

static int real_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!real_sigaction_fn) real_sigaction_fn = dlsym(RTLD_NEXT, "sigaction");
    return real_sigaction_fn(sig, act, old);
}
static int real_sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    if (!real_sigprocmask_fn) real_sigprocmask_fn = dlsym(RTLD_NEXT, "sigprocmask");
    return real_sigprocmask_fn(how, set, old);
}

static void *(*signal_fn_real)(int, void *(*)(int));

static void sup_sig_noop(int sig) {
    fprintf(stderr, "SIGSHIM> no-op handler for sig=%d\n", sig);
    fflush(stderr);
}

static int bogus_handler(void *h) {
    uintptr_t v = (uintptr_t)h;
    return v && v < 0x10000;
}

/* signal() shim */
static void (*sup_signal(int sig, void (*h)(int)))(int) {
    sup_prep_crash_sa();
    if (sup_is_crash_sig(sig)) {
        struct sigaction old;
        if (real_sigaction(sig, &g_crash_sa, &old) != 0) memset(&old, 0, sizeof old);
        return old.sa_handler;
    }
    if (bogus_handler(h)) {
        fprintf(stderr, "SIGSHIM> signal(sig=%d, handler=%p) — подменяем на no-op\n",
                sig, (void *)h);
        fflush(stderr);
        h = sup_sig_noop;
    }
    if (!signal_fn_real) signal_fn_real = (void *(*)(int, void *(*)(int)))dlsym(RTLD_NEXT, "signal");
    if (!signal_fn_real) return SIG_ERR;
    return (void (*)(int))signal_fn_real(sig, (void *(*)(int))h);
}

/* sigaction() shim */
static int sup_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    sup_prep_crash_sa();
    if (sup_is_crash_sig(sig)) {
        struct sigaction old;
        if (real_sigaction(sig, &g_crash_sa, &old) != 0) memset(&old, 0, sizeof old);
        if (oldact) *oldact = old;
        return 0;
    }
    if (act && bogus_handler((void *)act->sa_handler)) {
        struct sigaction safe = *act;
        fprintf(stderr, "SIGSHIM> sigaction(sig=%d, handler=%p) — подменяем на no-op\n",
                sig, (void *)act->sa_handler);
        fflush(stderr);
        safe.sa_handler = sup_sig_noop;
        safe.sa_flags &= ~SA_SIGINFO;
        return real_sigaction(sig, &safe, oldact);
    }
    return real_sigaction(sig, act, oldact);
}

/* sigprocmask()/pthread_sigmask() shim: never block crash signals */
static int sup_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    sigset_t clean = *set;
    for (int s = 1; s <= 64; ++s)
        if (sup_is_crash_sig(s)) sigdelset(&clean, s);
    return real_sigprocmask(how, &clean, oldset);
}

/* ---- NativeActivity bootstrap ---------------------------------------- */
/* The game is a stock-like android_native_app_glue binary:
   - ANativeActivity_onCreate(act, saved, size) fills *(act+0x00) with its
     16-slot callbacks table and stores its android_app* at act+0x38.       */

/* Смещения по NDK ABI struct ANativeActivity. Поле vm обязано лежать на 0x08:
 * иначе движок читает activity->vm вместо JavaVM* и падает на (*vm)->GetEnv(). */
#define NA_CBSLOT    0x00 /* callbacks*, game writes callbacks into *(this) */
#define NA_VM        0x08 /* JavaVM* vm */
#define NA_ENV       0x10 /* JNIEnv* env */
#define NA_CLAZZ     0x18 /* jobject clazz */
#define NA_INTPATH   0x20
#define NA_EXTPATH   0x28
#define NA_WINDOW    0x30
#define NA_CLAZZ2    0x30
#define NA_APP       0x38 /* game stores android_app* here */
#define NA_ASSETMGR  0x40
#define NA_CFG       0x48

/* Индексы СЛОТОВ (не байтовые смещения!). Обращение идёт как cbt[idx],
 * где idx умножается на sizeof(void*). Раньше здесь стояли байтовые
 * смещения 0x08/0x30/0x38, из-за чего cbt[0x38] читал 448 байт мимо
 * таблицы (она всего 0x80) и давал мусор вместо обработчика окна. */
#define CB_ONSTART            0   /* слот 0 */
#define CB_ONRESUME           1   /* слот 1 */
#define CB_ONSTOP             4   /* слот 4 */
#define CB_ONDESTROY          5   /* слот 5 */
#define CB_FOCUS              6   /* слот 6 */
#define CB_ONWINDOW_CREATED   7   /* слот 7 */
#define CB_ONWINDOW_DESTROYED 10  /* слот 10: onNativeWindowDestroyed */
#define CB_ONREDRAW           9   /* слот 9: onNativeWindowRedrawNeeded */
#define CB_ONINPUT_CREATED   11   /* слот 11: onInputQueueCreated */
#define CB_ONINPUT_DESTROYED 12   /* слот 12: onInputQueueDestroyed */

static void *g_boot_act;
static void *g_boot_win;
static void (*g_boot_redraw)(void *, void *);

static void sup_native_activity_boot(void) {
    void *(*getenvfn)(void) = dlsym(g_compat_handle, "sup_jni_env");
    void *(*findcls)(const char *) = dlsym(g_compat_handle, "sup_jni_find_class");
    fprintf(stderr, "NATACT> boot start\n");
    fflush(stderr);
    if (!getenvfn || !findcls) {
        fprintf(stderr, "NATACT> compat helpers missing\n");
        fflush(stderr);
        return;
    }
    char *dirn = getenv("SUP_NATIVE_DIR");
    if (!dirn) dirn = "user_data/bedrock/nativeact";
    char *extdir = getenv("SUP_EXTERNAL_DIR");
    if (!extdir) extdir = "user_data/bedrock/nativeact_ext";
    mkdir(dirn, 0755);
    mkdir(extdir, 0755);

    void *(*getvmfn)(void) = dlsym(g_compat_handle, "sup_jni_vm");
    unsigned char *act = calloc(1, 0x80);
    void **cb = calloc(1, 0x80);
    void *env = getenvfn();
    void *vm = getvmfn ? getvmfn() : NULL;
    void *clazz = findcls("com/mojang/minecraftpe/MainActivity");
    static int s_mgr;

    fprintf(stderr, "NATACT> act=%p vm=%p env=%p clazz=%p\n",
            (void *)act, vm, env, clazz);
    *(void **)(act + NA_CBSLOT) = cb;
    *(void **)(act + NA_VM) = vm;
    *(void **)(act + NA_ENV) = env;
    *(void **)(act + NA_CLAZZ) = clazz;
    *(void **)(act + NA_INTPATH) = strdup(dirn);
    *(void **)(act + NA_EXTPATH) = strdup(extdir);
    *(void **)(act + NA_WINDOW) = NULL;
    *(void **)(act + NA_CLAZZ2) = NULL;
    *(void **)(act + NA_APP) = NULL;
    *(void **)(act + NA_ASSETMGR) = &s_mgr;
    *(void **)(act + NA_CFG) = NULL;

    ElfW(Sym) *s = find_symbol(&g_obj, "ANativeActivity_onCreate");
    if (!s || s->st_shndx == SHN_UNDEF) {
        fprintf(stderr, "NATACT> no ANativeActivity_onCreate export\n");
        fflush(stderr);
        return;
    }
    /* Настоящая сигнатура NDK - ЧЕТЫРЕ параметра:
     *   ANativeActivity_onCreate(ANativeActivity *activity,
     *                             ANativeActivityCallbacks *callbacks,
     *                             void *savedState, size_t savedStateSize)
     * Игра получает `callbacks` АРГУМЕНТОМ и сама заполняет его своими
     * обработчиками. Мы раньше передавали здесь NULL, поэтому таблица
     * коллбэков оставалась пустой и игра не отдавала нам ни одного
     * обработчика - в том числе onNativeWindowCreated. */
    typedef void (*nac_oncreate_t)(void *, void *, void *, size_t);
    nac_oncreate_t onCreate = (nac_oncreate_t)symbol_address(&g_obj, s);
    {
        const char *bp = getenv("SUP_ENGINE_BP");
        if (bp) {
            void *tgt = (char *)g_obj.base + strtoul(bp, NULL, 0);
            size_t pg = (size_t)tgt & ~(size_t)0xfff;
            if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                *(volatile unsigned char *)tgt = 0xCC;
                mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
                fprintf(stderr, "NATACT> engine breakpoint armed at %p (0xcc)\n", tgt);
                fflush(stderr);
            } else {
                perror("warn: ebp mprotect");
            }
        }
    }
    fprintf(stderr, "NATACT> calling ANativeActivity_onCreate ...\n");
    fflush(stderr);
    /* Передаём НЕ NULL, а свою таблицу cb - иначе игра лишена возможности
     * зарегистрировать обработчики окна. */
    fprintf(stderr, "NATACT> onCreate(act=%p, callbacks=%p, saved=NULL, size=0)\n",
            (void *)act, (void *)cb);
    fflush(stderr);
    onCreate(act, cb, NULL, 0);

    /* Дамп таблицы коллбэков ПОСЛЕ инициализации: игра должна была
     * записать в неё свои живые обработчики. */
    {
        static const char *cb_names[16] = {
            "onStart", "onResume", "onSaveInstanceState", "onPause",
            "onStop", "onDestroy", "onWindowFocusChanged",
            "onNativeWindowCreated", "onNativeWindowDestroyed",
            "onInputQueueCreated", "onInputQueueDestroyed",
            "onNativeWindowRedrawNeeded", "onConfigChanged",
            "onLowMemory", "onTrimMemory", "onContentRectChanged"
        };
        fprintf(stderr, "NATACT> === ANativeActivityCallbacks dump (%p) ===\n",
                (void *)cb);
        int nset = 0;
        for (int i = 0; i < 16; i++) {
            void *fn = cb[i];
            if (fn) nset++;
            fprintf(stderr, "NATACT>   [%02d] %-26s = %p\n", i, cb_names[i], fn);
        }
        fprintf(stderr, "NATACT> === callbacks non-null: %d/16 ===\n", nset);
        fflush(stderr);
        if (nset == 0)
            fprintf(stderr, "NATACT> CRITICAL: callbacks table still EMPTY - "
                            "игра ничего не зарегистрировала\n");
    }

    unsigned char *app = *(unsigned char **)(act + NA_APP);
    /* В NDK AAndroidApp.msgwrite - это УКАЗАТЕЛЬ НА ФУНКЦИЮ
     * void (*msgwrite)(struct AAndroidApp*, int8_t), а не дескриптор
     * файла. Раньше мы читали app+0xbc как int fd и делали write(mw,&c,1),
     * из-за чего приходило EBADF (-1), а зарегистрированный игрой
     * обработчик команд (onAppCmd) не вызывался НИ РАЗУ. Поэтому игра
     * жила, но не получала APP_CMD_INIT_WINDOW и не создавала окно
     * графики: цикл крутится, eglSwapBuffers = 0, белый экран.
     * Теперь вызываем обработчик напрямую. */
    typedef void (*msgwrite_fn)(unsigned char *, signed char);
    msgwrite_fn mw =
        app ? *(msgwrite_fn *)(app + 0xbc) : NULL;
    fprintf(stderr, "NATACT> onCreate returned, app=%p msgwrite=%p\n", app,
            (void *)mw);
    fflush(stderr);
    if (!app) {
        fprintf(stderr, "NATACT> no android_app\n");
        fflush(stderr);
        return;
    }
    /* ВНИМАНИЕ: в AAndroidApp поля msgwrite НЕТ. Значение app+0xbc приходит
     * как 0x400000000 (мелкое число, не указатель), поэтому вызывать его
     * нельзя - получаем SEGV at 0x400000000. Оставляем только печать,
     * реальные команды доставляем через ANativeActivityCallbacks ниже. */
    if (!mw) {
        fprintf(stderr, "NATACT> msgwrite slot is NULL\n");
        fflush(stderr);
    }

#define CMDSEND_Byte(c)                                              \
    do {                                                             \
        signed char cmdc = (signed char)(c);                          \
        fprintf(stderr, "NATACT> cmd 0x%02x -> no AAndroidApp channel\n", \
                (unsigned char)cmdc);                                \
        (void)cmdc;                                                  \
        fflush(stderr);                                              \
    } while (0)

    CMDSEND_Byte(0x0a);
    fprintf(stderr, "NATACT> after APP_CMD_START, no sleep, going to X11\n");
    fflush(stderr);
    fprintf(stderr, "NATACT> preparing X11 window\n");
    fflush(stderr);

    void *win = NULL;
    static void *dpy = NULL;
    void *x11 = dlsym(g_x11_handle ? g_x11_handle : RTLD_DEFAULT, "XOpenDisplay");
    if (x11 && !getenv("SUP_NO_WINDOW")) {
        void *(*XOpenDisplay_t)(const char *);
        *(void **)&XOpenDisplay_t = x11;
        fprintf(stderr, "NATACT> calling XOpenDisplay\n");
        fflush(stderr);
        dpy = XOpenDisplay_t(NULL);
        if (dpy) {
            fprintf(stderr, "NATACT> X display opened\n");
            fflush(stderr);
        } else {
            fprintf(stderr, "NATACT> XOpenDisplay returned NULL\n");
            fflush(stderr);
        }
    } else if (g_verbose) {
        fprintf(stderr, "NATACT> libX11 not loaded\n");
        fflush(stderr);
    }

    if (dpy) {
        void *h = g_x11_handle ? g_x11_handle : RTLD_DEFAULT;
        unsigned long (*XCreateSimpleWindow_t)(void *, unsigned long, int, int, unsigned int,
                                               unsigned int, unsigned int, unsigned long, unsigned long);
        int (*XMapWindow_t)(void *, unsigned long);
        int (*XFlush_t)(void *);
        unsigned long (*XDefaultRootWindow_t)(void *);
        unsigned long (*XBlackPixel_t)(void *, int);
        unsigned long (*XWhitePixel_t)(void *, int);
        int (*XDefaultScreen_t)(void *);
        *(void **)&XCreateSimpleWindow_t = dlsym(h, "XCreateSimpleWindow");
        *(void **)&XMapWindow_t = dlsym(h, "XMapWindow");
        *(void **)&XFlush_t = dlsym(h, "XFlush");
        *(void **)&XDefaultRootWindow_t = dlsym(h, "XDefaultRootWindow");
        *(void **)&XBlackPixel_t = dlsym(h, "XBlackPixel");
        *(void **)&XWhitePixel_t = dlsym(h, "XWhitePixel");
        *(void **)&XDefaultScreen_t = dlsym(h, "XDefaultScreen");

        fprintf(stderr, "NATACT> X11 fn ptrs: create=%p map=%p flush=%p root=%p\n",
                (void *)XCreateSimpleWindow_t, (void *)XMapWindow_t,
                (void *)XFlush_t, (void *)XDefaultRootWindow_t);
        fflush(stderr);

        if (!XCreateSimpleWindow_t || !XMapWindow_t || !XFlush_t ||
            !XDefaultRootWindow_t || !XDefaultScreen_t) {
            fprintf(stderr, "NATACT> X11 symbols missing, no window\n");
            fflush(stderr);
        } else {
            int scr = XDefaultScreen_t(dpy);
            unsigned long root = XDefaultRootWindow_t(dpy);
            unsigned long black = XBlackPixel_t ? XBlackPixel_t(dpy, scr) : 0;
            unsigned long white = XWhitePixel_t ? XWhitePixel_t(dpy, scr) : 0xffffff;
            unsigned long w = XCreateSimpleWindow_t(dpy, root, 0, 0,
                                                    g_win_w, g_win_h, 1, black, white);
            g_xwin_id = w;
            XMapWindow_t(dpy, w);
            XFlush_t(dpy);
            /* 
             * ВАЖНО: game ожидает ANativeWindow* (указатель на структуру),
             * не XID. Выделяем статическую структуру-заглушку.
             * Игра будет вызывать ANativeWindow_getWidth/Height/etc.
             * которые мы реализовали в ndk.c и вернут фиксированные значения.
             */
            static unsigned char g_fake_win_struct[128];
            win = g_fake_win_struct;
            /* Сообщаем libsupcompat XID, чтобы eglCreateWindowSurface мог
             * подменить fake ANativeWindow* на реальное окно (runner не
             * экспортирует свои символы — нужен dlsym-сеттер). */
            if (g_compat_handle) {
                void (*setfn)(void *, unsigned long, void *) = (void (*)(void *,
                        unsigned long, void *))dlsym(g_compat_handle,
                        "sup_set_x11_window");
                if (setfn) setfn(dpy, w, win);
                else fprintf(stderr, "NATACT> sup_set_x11_window not found\n");
            }
            fprintf(stderr, "NATACT> X window id=%#lx, fake ANativeWindow*=%p\n",
                    (unsigned long)w, win);
            fflush(stderr);
            fprintf(stderr, "NATACT> X window created id=%#lx %dx%d\n",
                    (unsigned long)w, g_win_w, g_win_h);
            fflush(stderr);
        }
    }

    fprintf(stderr, "NATACT> checking win=%p\n", win);
    fflush(stderr);
    /* Игра зарегистрировала 16/16 обработчиков (см. дамп выше). В реальном
     * Android фреймворк сам дергает их при изменении состояния окна.
     * У нас нет ни фреймворка, ни канала команд (AAndroidApp.msgwrite в
     * нативной ANativeActivity не существует), поэтому вызываем
     * обработчики напрямую - ровно то, что сделал бы Java-слой.
     * onNativeWindowCreated(act, window) - именно тот хук, без которого
     * движок не доходит до создания EGL-поверхности. */
    /* Игра могла подменить activity->callbacks на СВОЮ таблицу. Поэтому
     * берём обработчики из activity->callbacks, а из cb - только как
     * запасной вариант. */
    void **cbt = *(void ***)(act + NA_CBSLOT);
    if (!cbt) cbt = cb;
    fprintf(stderr, "NATACT> callbacks table: act->callbacks=%p, our cb=%p\n",
            (void *)cbt, (void *)cb);
    fprintf(stderr, "NATACT> slot[07] onNativeWindowCreated = %p\n",
            cbt[CB_ONWINDOW_CREATED]);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "NATACT>   NOW[%02d] = %p\n", i, cbt[i]);
    fflush(stderr);

    void (*on_window_created)(void *, void *) =
        (void (*)(void *, void *))cbt[CB_ONWINDOW_CREATED];
    void (*on_focus)(void *, int) = (void (*)(void *, int))cbt[CB_FOCUS];
    void (*on_resume)(void *) = (void (*)(void *))cbt[CB_ONRESUME];
    void (*on_start)(void *) = (void (*)(void *))cbt[CB_ONSTART];
    void (*on_redraw)(void *, void *) =
        (void (*)(void *, void *))cbt[CB_ONREDRAW];

    if (win && on_window_created) {
        *(void **)(act + NA_WINDOW) = win;
        if (app) *(void **)(app + 0x48) = win;
        fprintf(stderr, "NATACT> calling onNativeWindowCreated(%p, win=%p)\n",
                (void *)act, win);
        fflush(stderr);
        on_window_created(act, win);
        fprintf(stderr, "NATACT> onNativeWindowCreated returned\n");
        fflush(stderr);
    } else {
        fprintf(stderr, "NATACT> no window/callback, engine waits for INIT_WINDOW\n");
        fflush(stderr);
    }

    fprintf(stderr, "NATACT> calling onStart\n");
    fflush(stderr);
    if (on_start) on_start(act);
    else fprintf(stderr, "NATACT> onStart slot is NULL\n");

    fprintf(stderr, "NATACT> calling onResume\n");
    fflush(stderr);
    if (on_resume) on_resume(act);

    fprintf(stderr, "NATACT> calling onWindowFocusChanged(act, 1)\n");
    fflush(stderr);
    if (on_focus) on_focus(act, 1);

    /* --- ввод: настоящая очередь + X11-поток ---
     * Фреймворк зовёт onInputQueueCreated после создания окна; glue-слой
     * игры превращает это в APP_CMD_INPUT_CHANGED и цепляет очередь к
     * своему ALooper. Потом X11-поток наполняет её мышью/клавиатурой.
     * SUP_NO_INPUT=1 — выключатель (A/B: вдруг второй X-клиент мешает). */
    if (win && !getenv("SUP_NO_INPUT")) {
        void *(*q_new)(void) = (void *(*)(void))
            dlsym(g_compat_handle, "sup_input_queue_new");
        void (*in_start)(void *, unsigned long) = (void (*)(void *, unsigned long))
            dlsym(g_compat_handle, "sup_input_start_x11");
        void (*on_input_created)(void *, void *) =
            (void (*)(void *, void *))cbt[CB_ONINPUT_CREATED];
        void *q = q_new ? q_new() : NULL;
        if (q && on_input_created) {
            fprintf(stderr, "NATACT> calling onInputQueueCreated(act=%p, q=%p)\n",
                    (void *)act, q);
            fflush(stderr);
            on_input_created(act, q);
            if (app) *(void **)(app + 0x40) = q;  /* android_app.inputQueue */
            fprintf(stderr, "NATACT> onInputQueueCreated returned\n");
            fflush(stderr);
        } else {
            fprintf(stderr, "NATACT> onInputQueueCreated: q=%p slot=%p\n",
                    q, (void *)on_input_created);
        }
        if (in_start && dpy) in_start(dpy, g_xwin_id);
        else fprintf(stderr, "NATACT> ввод не запущен (start=%p dpy=%p)\n",
                     (void *)in_start, dpy);
    }

    /* Системный пинок «поверхность изменилась, перерисуй». Рендерер может
     * ждать именно его, а не самого факта создания окна. */
    fprintf(stderr, "NATACT> calling onNativeWindowRedrawNeeded(act, win=%p)\n", win);
    fflush(stderr);
    if (on_redraw && win) on_redraw(act, win);
    else fprintf(stderr, "NATACT> onNativeWindowRedrawNeeded slot=%p win=%p\n",
            (void *)on_redraw, win);
    g_boot_act = act;
    g_boot_win = win;
    g_boot_redraw = on_redraw;

    fprintf(stderr, "NATACT> window lifecycle delivered\n");
    fflush(stderr);

    fprintf(stderr, "NATACT> bootstrap done, engine thread is running\n");
    fflush(stderr);
    if (g_compat_handle) {
        void (*arm)(void) = dlsym(g_compat_handle, "sup_arm_nclass_guard");
        if (arm) arm();
    }
}


/* Ловушка необработанного C++-исключения: печатаем тип исключения и
 * честный backtrace. Вместо безрамочного unwind, который в игре врёт. */

/* SIGABRT: glibc убивает процесс на "invalid stdio handle". Ловим, чтобы
 * получить стек того, кто портил FILE*, т.к. stdio уже не работает. */
static void sup_sigabrt(int sig) {
    (void)sig;
    void *bt[24];
    int n = backtrace(bt, 24);
    char b[256];
    int L = snprintf(b, sizeof b, "\nSIGABRT> битый stdio, стек (%d):\n", n);
    if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    Dl_info di;
    for (int i = 0; i < n; ++i) {
        const char *nm = 0; const char *fn = 0;
        if (dladdr(bt[i], &di)) { nm = di.dli_sname; fn = di.dli_fname; }
        int off = nm ? (int)((char *)bt[i] - (char *)di.dli_saddr) : 0;
        L = snprintf(b, sizeof b, "SIGABRT>   %p <%s+%d> %s\n",
                     bt[i], nm ? nm : (fn ? fn : "?"), off,
                     fn ? fn : "");
        if (L > 0) { ssize_t w = write(2, b, (size_t)L); (void)w; }
    }
    _exit(71);
}

static void sup_terminate_hook(void) {
    /* stderr уже битый (glibc ловит "invalid stdio handle" именно на
     * fprintf), поэтому пишем только через write(2) — без stdio. */
    char buf[2048];
    int len = snprintf(buf, sizeof buf, "TERMINATE> необработанное C++-исключение\n");
    if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }

    void *(*cur_type)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT,
                                                      "__cxa_current_exception_type");
    char *(*dem)(const char *, int *, void *, void *) =
        (char *(*)(const char *, int *, void *, void *))dlsym(RTLD_DEFAULT,
                                                              "__cxa_demangle");
    void *ti = cur_type ? cur_type() : 0;
    if (ti) {
        const char *nm = *(const char *const *)ti;
        int st = 0;
        char *dn = dem ? dem(nm, 0, 0, &st) : 0;
        len = snprintf(buf, sizeof buf, "TERMINATE> тип: %s\n", dn ? dn : nm);
        if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
        if (dn) free(dn);
    } else {
        len = snprintf(buf, sizeof buf, "TERMINATE> тип не определён, cur_type=%p\n",
                       (void *)cur_type);
        if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
    }

    void *bt[24];
    int n = backtrace(bt, 24);
    len = snprintf(buf, sizeof buf, "TERMINATE> backtrace (%d кадров):\n", n);
    if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
    Dl_info di;
    for (int i = 0; i < n; ++i) {
        const char *nm = 0;
        if (dladdr(bt[i], &di)) nm = di.dli_sname;
        int off = (nm && dladdr(bt[i], &di))
                      ? (int)((char *)bt[i] - (char *)di.dli_saddr) : 0;
        len = snprintf(buf, sizeof buf, "TERMINATE>   %p <%s+%d>\n",
                       bt[i], nm ? nm : "?", off);
        if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
    }
    _exit(70);
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "loader: main started\n");
    fflush(stderr);
    if (getenv("SUP_NO_ALLOC")) g_sup_disabled = 1;
#ifndef SUP_NEUTRAL_ALLOC
    if (g_sup_disabled) fprintf(stderr, "sup allocator disabled (SUP_NO_ALLOC)\n");
#endif
    const char *game = NULL;
    const char *libs[8];
    size_t nlibs = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--skip-init") == 0) g_skip_init = 1;
        else if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            if (nlibs < 8) libs[nlibs++] = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--lib PATH]... [--skip-init] <libminecraftpe.so>\n", argv[0]);
            return 0;
        }
        else game = argv[i];
    }
    if (!game) {
        fprintf(stderr, "usage: %s [--lib PATH]... [--skip-init] <libminecraftpe.so>\n", argv[0]);
        return 2;
    }
    g_game_path = game;
    g_obj.path = game;
    g_pgsz = getpagesize();

    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (gl && g_verbose) printf("gl: libGL.so.1 loaded\n");
    void *glx = dlopen("libGLX.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (glx && g_verbose) printf("glx: libGLX.so.0 loaded\n");
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (egl && g_verbose) printf("egl: libEGL.so.1 loaded\n");
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (gles && g_verbose) printf("gles: libGLESv2.so.2 loaded\n");
    g_x11_handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (g_x11_handle && g_verbose) printf("x11: libX11.so.6 loaded\n");
    if (g_x11_handle) {
        /* Xlib по умолчанию не потокобезопасен. Ввод читаем отдельным
         * потоком с того же Display, на котором Mesa делает swap —
         * без XInitThreads это гонка внутри Xlib. Звать можно только
         * до первого обращения к Xlib, поэтому здесь. */
        int (*xt)(void) = (int (*)(void))dlsym(g_x11_handle, "XInitThreads");
        if (xt) printf("x11: XInitThreads -> %d\n", xt());
    }
    const char *vw = getenv("SUP_WIDTH"), *vh = getenv("SUP_HEIGHT");
    if (vw && *vw) g_win_w = (unsigned int)strtoul(vw, NULL, 10);
    if (vh && *vh) g_win_h = (unsigned int)strtoul(vh, NULL, 10);

    g_compat_handle = dlopen("libsupcompat.so", RTLD_NOW | RTLD_GLOBAL);
    if (g_compat_handle) {
        sup_exec_add_fn = (void (*)(uintptr_t, uintptr_t))
            dlsym(g_compat_handle, "sup_exec_add");
    }
    if (!g_compat_handle) {
        char self[4096];
        char p[4608];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            char *slash = strrchr(self, '/');
            if (slash && slash != self) {
                size_t dl = (size_t)(slash - self);
                if (dl + 16 < sizeof(p)) {
                    memcpy(p, self, dl);
                    memcpy(p + dl, "/libsupcompat.so", 17);
                    g_compat_handle = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
                }
            }
        }
    }
    if (g_compat_handle) {
        if (g_verbose) printf("compat: libsupcompat.so loaded\n");
    } else {
        fprintf(stderr, "compat warn: libsupcompat.so not loaded: %s\n", dlerror());
    }

    for (size_t i = 0; i < nlibs; ++i) {
        Obj *d = &g_deps[g_ndep];
        if (load_object(d, libs[i]) != 0) {
            fprintf(stderr, "dep load failed: %s\n", libs[i]);
            return 3;
        }
        if (strstr(libs[i], "libfmod.so")) {
            unsigned char *p = (unsigned char *)d->base;
            static const unsigned char want[8] = {0x41, 0x8b, 0x44, 0x24,
                                                   0xf8, 0x41, 0x8b, 0x74};
            if (memcmp(p + 0xfa79a, want, 8) == 0) {
                p[0xfa79a] = 0x31; p[0xfa79a + 1] = 0xc0;
                p[0xfa79a + 2] = 0x90; p[0xfa79a + 3] = 0x90;
                p[0xfa79f] = 0x31; p[0xfa79f + 1] = 0xf6;
                p[0xfa79f + 2] = 0x90; p[0xfa79f + 3] = 0x90;
                fprintf(stderr, "FMODPATCH: neutered fa79a deref at %p\n",
                        p + 0xfa79a);
            }
            if (p[0xfa8bd] == 0x74 && p[0xfa8e2] == 0x02) {
                p[0xfa8bd] = 0xeb;
                p[0xfa8e2] = 0x66;
                fprintf(stderr, "FMODPATCH: disk flusher disabled at %p\n",
                        p + 0xfa8a6);
            } else {
                fprintf(stderr, "FMODPATCH: flusher pattern mismatch (%02x,%02x)\n",
                        p[0xfa8bd], p[0xfa8e2]);
            }
        }
        if (reloc_all(d) != 0) {
            fprintf(stderr, "dep reloc failed: %s\n", libs[i]);
            return 4;
        }
        ++g_ndep;
        if (g_verbose) printf("dep[%zu]: %s base=%p\n", g_ndep - 1, libs[i], d->base);
    }

    /* --- Форсируем инициализацию std::locale из libc++ --- */
    if (g_ndep > 0 && strstr(g_deps[0].path, "libc++")) {
        ElfW(Sym) *s = find_symbol(&g_deps[0], "_ZNSt6__ndk16locale7classicEv");
        if (s && s->st_shndx != SHN_UNDEF) {
            typedef void *(*locale_classic_fn)(void);
            locale_classic_fn fn = (locale_classic_fn)symbol_address(&g_deps[0], s);
            fprintf(stderr, "loader: calling std::locale::classic() @ %p\n", (void*)fn);
            fflush(stderr);
            void *loc = fn();
            fprintf(stderr, "loader: std::locale::classic() -> %p\n", loc);
            fflush(stderr);
        } else {
            fprintf(stderr, "loader: std::locale::classic() not found in libc++\n");
        }
    }

    if (load_object(&g_obj, game) != 0) return 3;
    if (reloc_all(&g_obj) != 0) return 4;
    fprintf(stderr, "modules: game base=%p size=%zu path=%s\n",
            g_obj.base, g_obj.total_size, g_obj.path);
    for (size_t d = 0; d < g_ndep; ++d)
        fprintf(stderr, "modules: dep[%zu] base=%p size=%zu path=%s\n",
                d, g_deps[d].base, g_deps[d].total_size, g_deps[d].path);
    /* Трассировка точек загрузки. Только первые две — __throw_out_of_range;
     * e3e0590 собирает атрибуты EGL, fb474d0 — PLT eglChooseConfig. Ни одна
     * из этих ловушек не означает, что исключение брошено. */
    oor_trap_install((char *)g_obj.base + 0xe504490, "vector __throw_out_of_range");
    oor_trap_install((char *)g_obj.base + 0xe4b57c0, "string __throw_out_of_range");
    oor_trap_install((char *)g_obj.base + 0xe3e0590, "EGL config chooser");
    /* Точка возврата виртуального вызова (e3b2d82) — колбэк логгера. */
    oor_trap_install((char *)g_obj.base + 0xe3b2d82, "logger virtual call");
    /* Куда реально идёт вызов eglChooseConfig: PLT-стаб и сам хук из GOT. */
    oor_trap_install((char *)g_obj.base + 0xfb474d0, "eglChooseConfig@plt");
    /* Копия 0x10 байт в префетч-цикле: движок ставит NULL в data (+0x20)
     * узла — переживаем это (exp: пустые узлы с перевыделением). */
    node_skip_install((char *)g_obj.base + 0xdf9fd20);
    node_skip_install((char *)g_obj.base + 0xdf9fd0d);
    {
        void **gotslot = (void **)((char *)g_obj.base + 0x10267bb8);
        unsigned char vec = 0;
        if (g_pgsz && mincore((void *)(((uintptr_t)gotslot) &
                        ~(uintptr_t)(g_pgsz - 1)), g_pgsz, &vec) == 0) {
            fprintf(stderr, "TRAPHOOK> eglChooseConfig GOT=%p\n", *gotslot);
            {
                unsigned char *q = (unsigned char *)*gotslot;
                unsigned char vec = 0;
                if (mincore((void *)(((uintptr_t)q) & ~(uintptr_t)(g_pgsz - 1)),
                        g_pgsz, &vec) == 0) {
                    Dl_info di;
                    fprintf(stderr, "TRAPHOOK> первые 16 байт: %02x %02x %02x %02x "
                            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                            q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                    if (dladdr(q, &di) && di.dli_fname)
                        fprintf(stderr, "TRAPHOOK> это из %s (смещ %+ld, sym=%s)\n",
                                di.dli_fname, (long)(q - (unsigned char *)di.dli_fbase),
                                di.dli_sname ? di.dli_sname : "?");
                }
            }
            fflush(stderr);
            oor_trap_install(*gotslot, "eglChooseConfig hook (GOT)");
        }
    }
    /* Точка, где libc++_shared печатает "terminating" и уходит в abort. */
    for (size_t d = 0; d < g_ndep; ++d) {
        if (strstr(g_deps[d].path, "libc++_shared")) {
            oor_trap_install((char *)g_deps[d].base + 0xd8adc, "libc++ terminate");
            oor_trap_install((char *)g_deps[d].base + 0xd8a90, "libc++ abort");
            break;
        }
    }
    /* Ловушки на реализации из libc++_shared: игра и сама libc++ могут
     * бросать через их внутренние функции, минуя слабые символы игры. */
    {
        static const char *const names[] = {
            "_ZNKSt6__ndk120__vector_base_commonILb1EE20__throw_out_of_rangeEv",
            "_ZNKSt6__ndk121__basic_string_commonILb1EE20__throw_out_of_rangeEv",
            "__cxa_throw",
            "__cxa_allocate_exception",
        };
        for (size_t n = 0; n < sizeof names / sizeof *names; ++n)
            for (size_t d = 0; d < g_ndep; ++d) {
                ElfW(Sym) *sy = find_symbol(&g_deps[d], names[n]);
                if (sy && sy->st_shndx != SHN_UNDEF) {
                    void *addr = symbol_address(&g_deps[d], sy);
                    /* Свой хук из libsupcompat не трогаем. */
                    void *mine = dlsym(g_compat_handle, names[n]);
                    if (addr != mine) oor_trap_install(addr, names[n]);
                    break;
                }
            }
    }
    if (g_verbose)
        for (size_t hi = 0; g_fm_hooks[hi].name; ++hi)
            fprintf(stderr, "fmod-hook: %s real=%p\n", g_fm_hooks[hi].name, g_fm_hooks[hi].real);
    print_report(&g_obj);

    if (g_skip_init) {
        printf("init skipped (--skip-init)\n");
        return 0;
    }

    if (!getenv("SUP_NO_CRASHDUMP")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGTRAP, &sa, NULL);
    }

    /* Ставим ПОСЛЕ загрузки libc++_shared: раньше dlsym вернёт NULL. */
    {
        void (*st)(void (*)(void)) = (void (*)(void (*)(void)))dlsym(
            RTLD_DEFAULT, "_ZSt13set_terminatePFvvE");
        signal(SIGABRT, sup_sigabrt);
        fprintf(stderr, "SIGABRT> обработчик установлен\n");
        if (st) { st(sup_terminate_hook);
            fprintf(stderr, "TERMINATE> обработчик установлен\n"); }
        else fprintf(stderr, "TERMINATE> set_terminate не найден\n");
        fflush(stderr);
    }
    printf("running initializers (deps first)...\n");
    g_cur_init_idx = 0;
    {
        const char *bp = getenv("SUP_ENGINE_BP");
        if (bp) {
            void *tgt = (char *)g_obj.base + strtoul(bp, NULL, 0);
            size_t pg = (size_t)tgt & ~(size_t)0xfff;
            if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                *(volatile unsigned char *)tgt = 0xCC;
                mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
                fprintf(stderr, "NATACT> early breakpoint armed at %p (0xcc)\n", tgt);
            } else {
                perror("warn: ebp mprotect");
            }
        }
    }
    for (size_t d = 0; d < g_ndep; ++d) {
        Obj *o = &g_deps[d];
        if (g_verbose && o->rela_plt) {
            for (size_t i = 0; i < o->rela_plt_count; ++i) {
                if (o->rela_plt[i].r_offset == 0xffa08) {
                    size_t s = ELF64_R_SYM(o->rela_plt[i].r_info);
                    char *nm = s < o->dynsym_count ? o->dynstr + o->dynsym[s].st_name : "?";
                    fprintf(stderr, "DBG dep[%zu] GOT slot 0xffa08: reloc sym %zu = %s\n",
                            d, s, nm);
                }
            }
            fprintf(stderr, "DBG dep[%zu] GOT slot 0xffa08 now = %p\n", d,
                    *(void **)((char *)o->base + 0xffa08));
        }
        for (size_t i = 0; i < o->init_array_count; ++i) {
            g_cur_init_idx = i;
            if (g_verbose) printf("  dep[%zu] init_array[%zu] @ %p\n", d, i, o->init_array[i]);
            if (o->init_array[i]) o->init_array[i]();
        }
        if (o->dt_init) o->dt_init();
    }
    for (size_t i = 0; i < g_obj.init_array_count; ++i) {
        g_cur_init_idx = i;
        void (*f)(void) = g_obj.init_array[i];
        if (g_verbose) printf("  init_array[%zu] @ %p\n", i, f);
        if (f) f();
    }
    g_cur_init_idx = (size_t)-1;
    if (g_obj.dt_init) g_obj.dt_init();
    printf("initializers done, no crash\n");

    if (g_compat_handle) {
        extern void *__emutls_get_address(void *);
        void *b1_desc = (char *)g_obj.base + 0x102b1170;
        volatile char *b1 = (char *)__emutls_get_address(b1_desc);
        if (b1) *b1 = 1;
        fprintf(stderr, "primed game TLS flag 0x102b1170 -> %d\n", (int)*b1);

        void (*dbgset)(unsigned long, unsigned long) =
        (void (*)(unsigned long, unsigned long))
        dlsym(g_compat_handle, "sup_dbg_set_game");
    if (dbgset) dbgset((unsigned long)g_obj.base, (unsigned long)g_obj.total_size);
    {
        void (*addm)(unsigned long, unsigned long, const char *) =
            (void (*)(unsigned long, unsigned long, const char *))
            dlsym(g_compat_handle, "sup_dbg_add_module");
        void (*regf)(void *) = (void (*)(void *))
            dlsym(g_compat_handle, "sup_dbg_register_frame");
        for (size_t d = 0; d < g_ndep; ++d) {
            if (addm) addm((unsigned long)g_deps[d].base,
                           (unsigned long)g_deps[d].base + g_deps[d].total_size,
                           g_deps[d].path);
        }
        if (regf && g_obj.eh_frame) {
            regf((void *)((char *)g_obj.base + g_obj.eh_frame));
        }
        fprintf(stderr, "EHF> game .eh_frame off=0x%lx (base=%p), mods=%zu\n",
                (unsigned long)g_obj.eh_frame, (void *)g_obj.base, g_ndep);
        fflush(stderr);
    }
    void (*boot)(void) = dlsym(g_compat_handle, "sup_jni_boot");
#ifndef SUP_NEUTRAL_ALLOC
        sup_arm_allocator();
        fprintf(stderr, "allocator armed (game malloc->sup slabs)\n");
#endif
        if (boot) boot();
        ElfW(Sym) *js = find_symbol(&g_obj, "JNI_OnLoad");
        if (js && js->st_shndx != SHN_UNDEF) {
            typedef jint (*onload_fn)(JavaVM *, void *);
            onload_fn onload = (onload_fn)symbol_address(&g_obj, js);
            void *(*vmf)(void) = dlsym(g_compat_handle, "sup_jni_vm");
            JavaVM *vm = vmf ? (JavaVM *)vmf() : NULL;
            fprintf(stderr, "loader: calling onload(vm=%p, NULL)\n", (void*)vm);
            fflush(stderr);
            jint rc = onload(vm, NULL);
            fprintf(stderr, "game JNI_OnLoad returned %d (JNI_VERSION_1_6 = %d)\n",
                    rc, JNI_VERSION_1_6);
            if (rc > 0) {
                fprintf(stderr, "loader: creating android_app + window before main loop\n");
                fflush(stderr);
                sup_native_activity_boot();
                fprintf(stderr, "loader: sup_native_activity_boot returned, engine should be running\n");
                fflush(stderr);

                int (*runmain)(void) =
                    (int (*)(void))dlsym(g_compat_handle, "sup_jni_run_main");
                    if (runmain) {
                        int r = runmain();
                        fprintf(stderr, "loader: sup_jni_run_main returned %d\n", r);
                        fflush(stderr);
                    }
                    fprintf(stderr, "loader: entering hold loop (main must not return)\n");
                    fflush(stderr);
                    for (;;) {
                        if (!getenv("SUP_NO_CRASHDUMP")) {
                            struct sigaction sa;
                            memset(&sa, 0, sizeof(sa));
                            sa.sa_sigaction = crash_handler;
                            sa.sa_flags = SA_SIGINFO | SA_NODEFER;
                            sigaction(SIGSEGV, &sa, NULL);
                        }
                        if (g_boot_redraw && g_boot_win &&
                            getenv("SUP_REDRAW_PUMP"))
                            g_boot_redraw(g_boot_act, g_boot_win);
                        if (getenv("SUP_UI_PUMP")) {
                            void (*pump)(void) = (void (*)(void))
                                dlsym(g_compat_handle, "sup_ui_pump");
                            if (pump) pump();
                        }
                        usleep(16000);
                    }
            }
        }
         else {
            fprintf(stderr, "game has no JNI_OnLoad export\n");
        }
    }
    return 0;
}