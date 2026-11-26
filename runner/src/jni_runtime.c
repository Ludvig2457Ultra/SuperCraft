#include "jni_min.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

void sup_render_watch(void);   /* compat.c: дамп стеков при остановке свапов */

static int g_trace;
static int g_exc;
static char g_exc_msg[256];

/* Игровой цикл зовёт tick()/getFreeMemory() и GetEnv тысячи раз в секунду.
 * Печать каждого вызова давала >500K строк за 90 с: диск и CPU отбирались
 * у рендера. Теперь каждое уникальное сочетание name+sig печатается один
 * раз; повторы считаются, а итог пишется редко. */
static void sup_unbound_log(const char *name, const char *sig, void *obj) {
    static struct {
        char name[64];
        char sig[48];
        unsigned long n;
    } seen[64];
    static int nseen;
    char nb[64], sb[48];
    int i;

    snprintf(nb, sizeof nb, "%s", name ? name : "?");
    snprintf(sb, sizeof sb, "%s", sig ? sig : "?");
    for (i = 0; i < nseen; ++i) {
        if (!strcmp(seen[i].name, nb) && !strcmp(seen[i].sig, sb)) {
            ++seen[i].n;
            if ((seen[i].n & 0xffff) == 0)
                fprintf(stderr,
                        "JNI: unbound %s %s: %lu calls (already reported)\n",
                        nb, sb, seen[i].n);
            return;
        }
    }
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        snprintf(seen[nseen].name, sizeof seen[nseen].name, "%s", nb);
        snprintf(seen[nseen].sig, sizeof seen[nseen].sig, "%s", sb);
        seen[nseen].n = 1;
        ++nseen;
    }
    fprintf(stderr, "JNI: *** UNBOUND native call: %s %s (this=%p) ***\n",
            nb, sb, obj);
    fflush(stderr);
}

static void sup_getenv_log(JavaVM *vm, JNIEnv **env, jint ver) {
    static unsigned long n;
    ++n;
    if (n <= 3 || (n & 0xffff) == 0)
        fprintf(stderr, "JNI: fnGetEnv(vm=%p env=%p ver=%#x n=%lu)\n",
                (void *)vm, (void *)env, (unsigned)ver, n);
}

#define TRACE(...)                                                  \
    do {                                                            \
        if (g_trace) fprintf(stderr, "JNI: " __VA_ARGS__);          \
    } while (0)

#define MAXCLASS 256
#define MAXMETH 96

typedef struct JMethod {
    char *name;
    char *sig;
    void *fn;
} JMethod;

typedef struct JClass {
    const char *name;
    JMethod m[MAXMETH];
    size_t nm;
} JClass;

static JClass *g_classes[MAXCLASS];
static volatile size_t *g_nclass_map;
#define g_nclass (*g_nclass_map)
static uint64_t g_canary = 0xDEADBEEFCAFEBABEULL;

typedef enum {
    K_CLASS,
    K_UTF8,
    K_CHARS,
    K_BYTES,
    K_BOOLS,
    K_SHORTS,
    K_INTS,
    K_LONGS,
    K_FLOATS,
    K_DOUBLES,
    K_OBJECTS,
    K_DIRECT,
    K_INSTANCE
} JKind;

typedef struct JRef {
    JKind kind;
    size_t len;
    void *data;
    JClass *clazz;
} JRef;

static JClass *find_or_make_class(const char *name);
static jobject game_virtual(JNIEnv *e, jobject o, jmethodID mid);
static JClass g_stub_class;
static JNIEnv_ *g_real_env;
static JavaVM_ *g_real_vm;

static void *g_unknown_field = (void *)1;
static JRef *g_last_utf;
static const char *g_last_find_class;

static jstring fnNewStringUTF(JNIEnv *e, const char *u);

static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "JNI: OOM\n");
        abort();
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static JRef *as_ref(jobject o, JKind want) {
    if (!o) return NULL;
    uintptr_t first = *(uintptr_t *)o;
    if (first < 16) {
        JRef *r = (JRef *)o;
        return r->kind == want ? r : NULL;
    }
    return NULL;
}

static int dbg_count = 0;

static JClass *find_or_make_class(const char *name) {
    if (!g_nclass_map) {
        void *pg = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        fprintf(stderr, "JNI: nclass raw mmap pg=%p errno=%d(%s)\n",
                (void *)pg, errno, strerror(errno));
        if (pg == MAP_FAILED) {
            fprintf(stderr, "JNI: nclass mmap failed: %s\n", strerror(errno));
            pg = calloc(1, 4096);
        }
        g_nclass_map = (volatile size_t *)pg;
    }
    if (g_canary != 0xDEADBEEFCAFEBABEULL) {
        fprintf(stderr, "FMC> !!! CANARY CORRUPTED: g_canary=%#lx (expected 0xDEADBEEFCAFEBABE)\n",
                (unsigned long)g_canary);
        fflush(stderr);
        _exit(1);
    }
    if (g_nclass > MAXCLASS) {
        fprintf(stderr, "FMC> !!! g_nclass=%zu > MAXCLASS=%d — CORRUPTION!\n",
                g_nclass, MAXCLASS);
        fflush(stderr);
        _exit(1);
    }
    if (!name) return NULL;
    if (dbg_count < 50) {
        fprintf(stderr, "FMC> &g_classes=%p &g_nclass=%p &g_last_utf=%p &g_last_find_class=%p &g_exc_msg=%p &g_unknown_field=%p\n",
                (void*)g_classes, (void*)&g_nclass, (void*)&g_last_utf,
                (void*)&g_last_find_class, (void*)g_exc_msg, (void*)&g_unknown_field);
        fprintf(stderr, "FMC> find_or_make_class(name=%p '%s') g_nclass=%zu\n",
                (void*)name, name, g_nclass);
        for (size_t i = 0; i < g_nclass && i < 5; ++i) {
            fprintf(stderr, "FMC>   [%zu] %p name=%p '%s'\n",
                    i, (void*)g_classes[i], (void*)g_classes[i]->name,
                    g_classes[i]->name ? g_classes[i]->name : "(null)");
        }
        fflush(stderr);
        dbg_count++;
    }
    for (size_t i = 0; i < g_nclass; ++i) {
        JClass *c = g_classes[i];
        if (!c) continue;
        if (!c->name) continue;
        if (strcmp(c->name, name) == 0) return c;
    }
    if (g_nclass >= MAXCLASS) return NULL;
    JClass *c = xmalloc(sizeof(*c));
    c->name = xstrdup(name);
    g_classes[g_nclass++] = c;
    return c;
}

/* Возвращаемый объектный дескриптор вида "Lcom/mojang/.../Store;" ->
 * имя класса для JClass-заглушки. Без этого все unbound-объекты стали
 * java/lang/Object, и engine звал store-методы не на том классе. */
static const char *class_from_ret(const char *rd, char *buf, size_t cap) {
    if (!rd || rd[0] != 'L' || !cap) return "java/lang/Object";
    const char *semi = strchr(rd + 1, ';');
    size_t n = semi ? (size_t)(semi - (rd + 1)) : 0;
    if (!n || n >= cap) return "java/lang/Object";
    memcpy(buf, rd + 1, n);
    buf[n] = '\0';
    return buf;
}

static JClass *objclass_of(jobject o) {
    if (!o) return NULL;

    uintptr_t first = *(uintptr_t *)o;

    if (first == K_INSTANCE) {
        return ((JRef *)o)->clazz;
    }

    if (first >= 16) return (JClass *)o;
    return NULL;
}

static JMethod *class_find_method(JClass *c, const char *name, const char *sig) {
    for (size_t i = 0; i < c->nm; ++i) {
        if (strcmp(c->m[i].name, name) == 0 &&
            (!sig || strcmp(c->m[i].sig, sig) == 0))
            return &c->m[i];
    }
    return NULL;
}

/* Заглушки для методов, которые движок зовёт, но которые Android-рантайм не
 * дал. Ключевой из них — runNativeCallbackOnUiThread(long): движок передаёт
 * туда нативный callback-указатель и ждёт его исполнения. Пока задание не
 * выполняется, инициализация не доходит до рендерера (EGL=0). Звать указатель
 * вслепую нельзя — сначала проверяем, что он попадает в исполняемый сегмент
 * одного из загруженных объектов (см. sup_ptr_in_exec в loader.c). */

extern int sup_ptr_in_exec(const void *p);

/* Проверяем читаемость блока, не полагаясь на SIGSEGV-ловушку. */
static int mincore_ok(void *p, size_t len) {
    if (!p) return 0;
    unsigned char vec[64];
    size_t pages = (len + 4095) / 4096;
    if (pages > sizeof(vec)) pages = sizeof(vec);
    if (mincore((void *)((uintptr_t)p & ~(uintptr_t)4095), pages, vec) != 0)
        return 0;
    for (size_t i = 0; i < pages; ++i)
        if (!(vec[i] & 1)) return 0;
    return 1;
}

/* Очередь задач "UI-потока". На Android runNativeCallbackOnUiThread(long)
 * лишь ПОСТИТ задачу в Handler главного (UI) потока, который потом зовёт
 * нативный колбэк. Наш UI-поток — это тот, что вызвал
 * ANativeActivity_onCreate (main в loader.c); он свободен и крутит
 * hold-loop, поэтому именно он и должен исполнять задачи. */
#define SUP_UI_QMAX 256
static pthread_mutex_t g_ui_mu = PTHREAD_MUTEX_INITIALIZER;
/* Копируем структуру задачи целиком: исходный объект может быть освобождён
 * к моменту, когда UI-поток доберётся до очереди (раньше хранили только
 * указатель и видели мусор вместо cb[0]). */
#define SUP_UI_TASK_SZ 128
static unsigned char g_ui_q[SUP_UI_QMAX][SUP_UI_TASK_SZ];
static int g_ui_head, g_ui_tail;
static volatile int g_ui_pumping;

void sup_ui_pump(void) {
    if (g_ui_pumping) return;
    for (;;) {
        unsigned char task[SUP_UI_TASK_SZ];
        int got = 0;
        pthread_mutex_lock(&g_ui_mu);
        if (g_ui_head != g_ui_tail) {
            memcpy(task, g_ui_q[g_ui_head], SUP_UI_TASK_SZ);
            g_ui_head = (g_ui_head + 1) % SUP_UI_QMAX;
            got = 1;
        }
        pthread_mutex_unlock(&g_ui_mu);
        if (!got) break;
        uintptr_t fnptr = 0;
        memcpy(&fnptr, task, sizeof(fnptr));
        if (!fnptr) {
            fprintf(stderr, "JNI: UI-PUMP task без callable — пропуск\n");
            fflush(stderr);
            continue;
        }
        fprintf(stderr, "JNI: UI-PUMP fn=0x%lx in_exec=%d\n",
                (unsigned long)fnptr, sup_ptr_in_exec((const void *)fnptr));
        fflush(stderr);
        if (sup_ptr_in_exec((const void *)fnptr)) {
            g_ui_pumping = 1;
            ((void (*)(void *))fnptr)(task);
            g_ui_pumping = 0;
            fprintf(stderr, "JNI: UI-PUMP cb done\n");
            fflush(stderr);
        }
    }
}

static int ui_queue_push(uintptr_t cb) {
    int ok = 0;
    if (!mincore_ok((void *)cb, SUP_UI_TASK_SZ)) return 0;
    unsigned char task[SUP_UI_TASK_SZ];
    memcpy(task, (void *)cb, SUP_UI_TASK_SZ);
    pthread_mutex_lock(&g_ui_mu);
    int nt = (g_ui_tail + 1) % SUP_UI_QMAX;
    if (nt != g_ui_head) {
        memcpy(g_ui_q[g_ui_tail], task, SUP_UI_TASK_SZ);
        g_ui_tail = nt;
        ok = 1;
    }
    pthread_mutex_unlock(&g_ui_mu);
    return ok;
}

static void fnRunNativeCallbackOnUiThread(JNIEnv *e, jobject o, jlong cb) {
    (void)e; (void)o;
    uintptr_t v = (uintptr_t)cb;
    int ok = sup_ptr_in_exec((const void *)v);
    fprintf(stderr, "JNI: runNativeCallbackOnUiThread cb=0x%lx in_exec=%d\n",
            (unsigned long)v, ok);
    fflush(stderr);
    if (getenv("SUP_UI_PUMP")) {
        if (!ok) fprintf(stderr, "JNI: UI-Q push cb=0x%lx -> %d\n",
                         (unsigned long)v, ui_queue_push(v));
        fflush(stderr);
        return;
    }

    /* Дамп 64 байт по cb: выясняем, дескриптор это или всё-таки указатель.
     * Каждое слово печатаем с проверкой sup_ptr_in_exec(), чтобы сразу увидеть
     * спрятанный адрес игрового коллбэка, если он есть. */
    {
        unsigned char *p = (unsigned char *)v;
        int readable = 0;
        fprintf(stderr, "JNI: --- cb dump ---\n");
        fflush(stderr);
        if (p && mincore_ok(p, 64)) {
            readable = 1;
            for (int i = 0; i < 8; ++i) {
                uintptr_t w;
                memcpy(&w, p + i * 8, sizeof(w));
                fprintf(stderr, "JNI: cb+0x%02x = 0x%016lx  in_exec=%d\n",
                        i * 8, (unsigned long)w, sup_ptr_in_exec((const void *)w));
            }
        }
        fprintf(stderr, "JNI: --- cb dump %s ---\n", readable ? "ok" : "UNREADABLE");
        fflush(stderr);
    }
    /* Расшифровано дампом: cb — указатель на структуру-задачу, а cb[0] хранит
     * адрес нативного коллбэка игры (проверено: in_exec=1, внутри .text
     * libminecraftpe.so). Это НЕ дескриптор Handler/Looper — вызывать cb
     * напрямую нельзя (in_exec=0), нужно брать поле +0x00. */
    if (!ok) {
        uintptr_t fnptr = 0;
        if (mincore_ok((void *)v, sizeof(uintptr_t))) {
            memcpy(&fnptr, (void *)v, sizeof(fnptr));
        }
        /* ВЫЗОВ ОТКЛЮЧЁН. Декодирование оставляем: cb[0] — валидная функция
         * .text игры, но структура-задача к этому моменту не инициализирована
         * (краш на чтении cb+0x240). Экспериментально подтверждено: вызов
         * роняет прогон с 20 тиков до 2. Выполнять задачу должен сам UI-очередь
         * Android-моста, которого у нас нет. Не раскомментировать без
         * полноценной очереди задач. */
        if (0 && sup_ptr_in_exec((const void *)fnptr)) {
            /* Сигнатура восстановлена по прологу: первые же инструкции читают
             * (%rdi) и 0x8(%rdi), значит функция принимает один
             * аргумент-указатель на структуру задачи. Раньше звали как
             * void(*)(void) -> rdi был мусором и падали на третьей
             * инструкции (bias 0xe4a5baa). Передаём сам cb. */
            void (*fn)(void *) = (void (*)(void *))fnptr;
            fprintf(stderr, "JNI: *** вызываю UI-callback cb[0]=0x%lx (arg=cb=0x%lx)\n",
                    (unsigned long)fnptr, (unsigned long)v);
            fflush(stderr);
            fn((void *)v);
            fprintf(stderr, "JNI: *** UI-callback отработал\n");
            fflush(stderr);
        } else if (!sup_ptr_in_exec((const void *)fnptr)) {
            fprintf(stderr, "JNI: cb[0]=0x%lx не исполняемый — задача пропущена\n",
                    (unsigned long)fnptr);
        } else {
            fprintf(stderr, "JNI: cb[0]=0x%lx декодирован, вызов отключён\n",
                    (unsigned long)fnptr);
        }
    }
}

static void fnNoopVoid(JNIEnv *e, jobject o, jmethodID m) {
    (void)e; (void)o; (void)m;
}

typedef struct StubEntry { const char *name; const char *sig; void *fn; } StubEntry;

static const StubEntry g_stubs[] = {
    { "runNativeCallbackOnUiThread", "(J)V", (void *)fnRunNativeCallbackOnUiThread },
    /* initializeAppExitInfoHelper() возвращает объект — его намеренно НЕ
     * биндим в void-заглушку: пусть отработает generic obj-return в
     * call_method, он отдаёт настоящий JClass-заглушку, который проходит
     * objclass_of(). void-функция вернула бы мусор вместо указателя. */
    { "readyForAppExitInfo",         "()V", (void *)fnNoopVoid },
    { "setKeepScreenOnFlag",         "(Z)V", (void *)fnNoopVoid },
};

static void *stub_lookup(const char *name, const char *sig) {
    if (!name || !sig) return NULL;
    for (size_t i = 0; i < sizeof(g_stubs) / sizeof(g_stubs[0]); ++i)
        if (!strcmp(g_stubs[i].name, name) && !strcmp(g_stubs[i].sig, sig))
            return g_stubs[i].fn;
    return NULL;
}

static JMethod *class_add_method(JClass *c, const char *name, const char *sig, void *fn) {
    JMethod *m = class_find_method(c, name, sig);
    if (!m) {
        if (c->nm >= MAXMETH) return NULL;
        m = &c->m[c->nm++];
        m->name = xstrdup(name);
        m->sig = xstrdup(sig);
    }
    if (!fn) {
        void *s = stub_lookup(name, sig);
        if (s) {
            fprintf(stderr, "JNI: stub bind %s%s -> %p\n", name, sig, s);
            fflush(stderr);
            fn = s;
        }
    }
    m->fn = fn;
    return m;
}

/* ---- basic ---- */

static jint fnGetVersion(JNIEnv *e) {
    fprintf(stderr, "JNI: fnGetVersion()\n");
    fflush(stderr);
    (void)e;
    return JNI_VERSION_1_6;
}

static jclass fnDefineClass(JNIEnv *e, const char *n, jobject l, const jbyte *b, jsize nb) {
    (void)e; (void)l; (void)b; (void)nb;
    return (jclass)find_or_make_class(n ? n : "unknown");
}

static jclass fnFindClass(JNIEnv *e, const char *name) {
    fprintf(stderr, "JNI: fnFindClass(%s)\n", name ? name : "(null)");
    fflush(stderr);
    (void)e;
    if (name) g_last_find_class = name;
    return (jclass)find_or_make_class(name ? name : "unknown");
}

static jmethodID fnFromReflectedMethod(JNIEnv *e, jobject o) {
    (void)e; (void)o;
    return NULL;
}

static jfieldID fnFromReflectedField(JNIEnv *e, jobject o) {
    (void)e; (void)o;
    return (jfieldID)g_unknown_field;
}

static jobject fnToReflectedMethod(JNIEnv *e, jclass c, jmethodID m, jboolean b) {
    (void)e; (void)c; (void)m; (void)b;
    return NULL;
}

static jclass fnGetSuperclass(JNIEnv *e, jclass c) {
    (void)e; (void)c;
    return NULL;
}

static jboolean fnIsAssignableFrom(JNIEnv *e, jclass a, jclass b) {
    (void)e; (void)a; (void)b;
    return JNI_TRUE;
}

static jobject fnToReflectedField(JNIEnv *e, jclass c, jfieldID f, jboolean b) {
    (void)e; (void)c; (void)f; (void)b;
    return (jobject)g_unknown_field;
}

/* ---- exceptions ---- */

static jint fnThrow(JNIEnv *e, jthrowable t) {
    (void)e;
    JClass *c = objclass_of(t);
    snprintf(g_exc_msg, sizeof(g_exc_msg), "%s", c ? c->name : "unknown");
    g_exc = 1;
    return JNI_OK;
}

static jint fnThrowNew(JNIEnv *e, jclass c, const char *msg) {
    (void)e; (void)c;
    snprintf(g_exc_msg, sizeof(g_exc_msg), "%s", msg ? msg : "(null)");
    g_exc = 1;
    TRACE("ThrowNew(%s)\n", g_exc_msg);
    return JNI_OK;
}

static jthrowable fnExceptionOccurred(JNIEnv *e) {
    (void)e;
    return g_exc ? (jthrowable)(void *)&g_stub_class : NULL;
}

static void fnExceptionDescribe(JNIEnv *e) {
    (void)e;
    if (g_exc) fprintf(stderr, "JNI exception: %s\n", g_exc_msg);
}

static void fnExceptionClear(JNIEnv *e) {
    (void)e;
    g_exc = 0;
    g_exc_msg[0] = 0;
}

static void fnFatalError(JNIEnv *e, const char *msg) {
    (void)e;
    fprintf(stderr, "JNI FatalError: %s\n", msg ? msg : "(null)");
    exit(1);
}

/* ---- refs ---- */

static jint fnPushLocalFrame(JNIEnv *e, jint n) {
    (void)e; (void)n;
    return JNI_OK;
}

static jobject fnPopLocalFrame(JNIEnv *e, jobject o) {
    (void)e;
    return o;
}

static jobject fnNewGlobalRef(JNIEnv *e, jobject o) {
    (void)e;
    return o;
}

static void fnDeleteGlobalRef(JNIEnv *e, jobject o) {
    (void)e; (void)o;
}

static void fnDeleteLocalRef(JNIEnv *e, jobject o) {
    (void)e; (void)o;
}

static jboolean fnIsSameObject(JNIEnv *e, jobject a, jobject b) {
    (void)e;
    return a == b ? JNI_TRUE : JNI_FALSE;
}

static jobject fnNewLocalRef(JNIEnv *e, jobject o) {
    (void)e;
    return o;
}

static jint fnEnsureLocalCapacity(JNIEnv *e, jint n) {
    (void)e; (void)n;
    return JNI_OK;
}

static jobject fnAllocObject(JNIEnv *e, jclass c) {
    (void)e;
    JClass *cl = objclass_of((jobject)c);
    if (!cl) return NULL;

    JRef *obj = xmalloc(sizeof(*obj));
    obj->kind = K_INSTANCE;
    obj->clazz = cl;
    return (jobject)obj;
}

static jobject fnNewObject(JNIEnv *e, jclass c, jmethodID mid, ...) {
    (void)e; (void)mid;
    JClass *cl = objclass_of((jobject)c);

    fprintf(stderr, "JNI: NewObject cls=%p %s\n", (void *)c,
            cl && cl->name ? cl->name : "(null)");
    fflush(stderr);

    if (!cl) return NULL;

    JRef *obj = xmalloc(sizeof(*obj));
    obj->kind = K_INSTANCE;
    obj->clazz = cl;
    return (jobject)obj;
}

static jobject fnNewObjectV(JNIEnv *e, jclass c, jmethodID mid, va_list a) {
    (void)a;
    return fnNewObject(e, c, mid);
}

static jobject fnNewObjectA(JNIEnv *e, jclass c, jmethodID mid, const jvalue *a) {
    (void)a;
    return fnNewObject(e, c, mid);
}

static jclass fnGetObjectClass(JNIEnv *e, jobject o) {
    JClass *c = objclass_of(o);
    (void)e;
    return (jclass)(c ? c : find_or_make_class("java/lang/Object"));
}

static jboolean fnIsInstanceOf(JNIEnv *e, jobject o, jclass c) {
    (void)e; (void)o; (void)c;
    return JNI_TRUE;
}

static jmethodID fnGetMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    fprintf(stderr, "JNI: fnGetMethodID(cls=%p name=%s sig=%s)\n",
            (void*)c, name ? name : "(null)", sig ? sig : "(null)");
    fflush(stderr);
    JClass *cl = objclass_of((jobject)c);
    (void)e;
    if (!cl) cl = find_or_make_class("unknown");
    JMethod *m = class_find_method(cl, name, sig);
    if (!m) m = class_add_method(cl, name, sig, NULL);
    return (jmethodID)m;
}

static jmethodID fnGetStaticMethodID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    fprintf(stderr, "JNI: fnGetStaticMethodID(cls=%p name=%s sig=%s)\n",
            (void*)c, name ? name : "(null)", sig ? sig : "(null)");
    fflush(stderr);
    return fnGetMethodID(e, c, name, sig);
}

/* ---- method dispatch ---- */

static int parse_sig(const char *sig, char *types, int max, int *ret) {
    int n = 0;
    const char *p = sig;

    if (*p == '(') ++p;

    while (*p && *p != ')' && n < max) {
        if (*p == '[') {
            types[n++] = '[';
            while (*p == '[') ++p;
            if (*p == 'L') {
                while (*p && *p != ';') ++p;
                if (*p == ';') ++p;
            } else if (*p) {
                ++p;
            }
        } else if (*p == 'L') {
            types[n++] = 'L';
            while (*p && *p != ';') ++p;
            if (*p == ';') ++p;
        } else {
            types[n++] = *p++;
        }
    }

    *ret = 'V';
    if (*p == ')') {
        ++p;
        if (*p == '[') *ret = '[';
        else if (*p == 'L') *ret = 'L';
        else if (*p) *ret = *p;
    }

    return n;
}

static void read_args(int n, const char *types, va_list ap, jvalue *out) {
    for (int i = 0; i < n; ++i) {
        switch (types[i]) {
            case 'Z': out[i].z = (jboolean)va_arg(ap, int); break;
            case 'B': out[i].b = (jbyte)va_arg(ap, int); break;
            case 'C': out[i].c = (jchar)va_arg(ap, int); break;
            case 'S': out[i].s = (jshort)va_arg(ap, int); break;
            case 'I': out[i].i = va_arg(ap, jint); break;
            case 'J': out[i].j = va_arg(ap, jlong); break;
            case 'F': out[i].f = (jfloat)va_arg(ap, double); break;
            case 'D': out[i].d = va_arg(ap, double); break;
            case 'L':
            case '[': out[i].l = va_arg(ap, jobject); break;
        }
    }
}

typedef long long (*f2)(void *, void *);
typedef long long (*f3)(void *, void *, long long);
typedef long long (*f4)(void *, void *, long long, long long);
typedef long long (*f5)(void *, void *, long long, long long, long long);
typedef long long (*f6)(void *, void *, long long, long long, long long, long long);
typedef long long (*f7)(void *, void *, long long, long long, long long, long long, long long);
typedef long long (*f8)(void *, void *, long long, long long, long long, long long, long long, long long);

static jvalue call_method(JNIEnv *e, jobject obj, jmethodID id, jvalue *args, int n, const char *types) {
    jvalue ret;
    memset(&ret, 0, sizeof(ret));
    JMethod *m = (JMethod *)id;

    /* --- void/primitive методы MainActivity, которые не идут через game_virtual --- */
    if (m && m->name) {
        if (strcmp(m->name, "setCachedDeviceId") == 0) {
            fprintf(stderr, "JNI: call_method.setCachedDeviceId() -> noop\n");
            fflush(stderr);
            return ret;   /* void */
        }
        if (strcmp(m->name, "initializeCrashManager") == 0) {
            fprintf(stderr, "JNI: call_method.initializeCrashManager() -> stub CrashManager\n");
            fflush(stderr);
            ret.l = (jobject)find_or_make_class("com/mojang/minecraftpe/CrashManager");
            return ret;
        }
        if (strcmp(m->name, "getInternalStoragePath") == 0) {
            const char *p = getenv("SUP_NATIVE_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact";
            fprintf(stderr, "JNI: call_method.getInternalStoragePath() -> %s\n", p);
            fflush(stderr);
            ret.l = (jobject)fnNewStringUTF(e, p);
            return ret;
        }
        if (strcmp(m->name, "getExternalStoragePath") == 0) {
            const char *p = getenv("SUP_EXTERNAL_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact_ext";
            fprintf(stderr, "JNI: call_method.getExternalStoragePath() -> %s\n", p);
            fflush(stderr);
            ret.l = (jobject)fnNewStringUTF(e, p);
            return ret;
        }
        if (strcmp(m->name, "getLegacyDeviceID") == 0) {
            fprintf(stderr, "JNI: call_method.getLegacyDeviceID() -> fixed\n");
            fflush(stderr);
            ret.l = (jobject)fnNewStringUTF(e, "00000000-0000-0000-0000-000000000000");
            return ret;
        }
        if (strcmp(m->name, "createUUID") == 0) {
            fprintf(stderr, "JNI: call_method.createUUID() -> fixed\n");
            fflush(stderr);
            ret.l = (jobject)fnNewStringUTF(e, "11111111-1111-1111-1111-111111111111");
            return ret;
        }

        /* Платформа и железо. Раньше эти методы уходили в unbound и
         * возвращали 0, из-за чего движок не видел размера окна и не
         * доходил до создания графического контекста. */
        {
            const char *nm = m->name;
            long v = 0;
            int hit = 0;
            if (!strcmp(nm, "getDisplayWidth") || !strcmp(nm, "getScreenWidth")) { v = 1280; hit = 1; }
            else if (!strcmp(nm, "getDisplayHeight") || !strcmp(nm, "getScreenHeight")) { v = 720; hit = 1; }
            else if (!strcmp(nm, "getNumCores")) { v = 4; hit = 1; }
            else if (!strcmp(nm, "getNumClusters")) { v = 1; hit = 1; }
            else if (!strcmp(nm, "getPerformanceCoreCount")) { v = 2; hit = 1; }
            else if (!strcmp(nm, "getTotalMemory")) { v = 4096LL * 1024 * 1024; hit = 1; }
            else if (!strcmp(nm, "getMemoryLimit")) { v = 4096LL * 1024 * 1024; hit = 1; }
            else if (!strcmp(nm, "getIsRooted")) { v = 0; hit = 1; }
            else if (!strcmp(nm, "isChromebook")) { v = 0; hit = 1; }
            else if (!strcmp(nm, "isTablet")) { v = 0; hit = 1; }
            else if (!strcmp(nm, "hasWriteExternalStoragePermission")) { v = 1; hit = 1; }
            /* Мониторы: getBatteryMonitor()/getThermalMonitor() возвращают
             * ОБЪЕКТ, поэтому их нельзя отдавать как v (100 / 0) — они
             * падают в generic obj-return ветку ниже, которая возвращает
             * настоящий JClass-заглушку. Здесь держим только их методы. */
            else if (!strcmp(nm, "getBatteryLevel")) { v = 100; hit = 1; }
            else if (!strcmp(nm, "getBatteryScale")) { v = 100; hit = 1; }
            else if (!strcmp(nm, "getBatteryStatus")) { v = 1; hit = 1; }   /* charging */
            else if (!strcmp(nm, "getBatteryTemperature")) { v = 250; hit = 1; } /* 25.0C */
            else if (!strcmp(nm, "getLowPowerModeEnabled")) { v = 0; hit = 1; }
            if (hit) {
                fprintf(stderr, "JNI: call_method.%s%s -> %ld\n", nm, m->sig, v);
                fflush(stderr);
                if (strchr(m->sig, 'Z')) ret.z = (jboolean)(v != 0);
                else if (strchr(m->sig, 'J')) ret.j = (jlong)v;
                else if (strchr(m->sig, 'I')) ret.i = (jint)v;
                else ret.j = (jlong)v;
                return ret;
            }
        }
    }

    if (!m || !m->fn) {
        sup_unbound_log(m ? m->name : "?", m ? m->sig : "?", obj);
        sup_render_watch();
        /* Метод с объектным возвратом нельзя отдавать как NULL: игра сразу
         * вызывает метод на результате и падает на разыменовании NULL
         * (rip=0x4). Возвращаем настоящий JClass-заглушку — он проходит
         * objclass_of(), и любой вызов на нём уходит в эту же ветку. */
        if (m && m->sig) {
            const char *close = strchr(m->sig, ')');
            /* Верификация лицензии: наша заглушка подписей не умеет
             * RSA/SHA256, честно не проверяем — просто пускаем. */
            if (m->name && strcmp(m->name, "verify") == 0 &&
                close && close[1] == 'Z') {
                fprintf(stderr, "JNI: unbound Signature.verify -> true\n");
                fflush(stderr);
                ret.z = JNI_TRUE;
                return ret;
            }
            /* Arrays.copyOfRange: игра вырезает диапазон байт из публичного
             * ключа. Возвращаем настоящую копию, иначе сегменты пустые. */
            if (m->name && strcmp(m->name, "copyOfRange") == 0 &&
                close && close[1] == '[' && n >= 3 && args[0].l) {
                JRef *src = (JRef *)args[0].l;
                jsize from = args[1].i, to = args[2].i;
                if (src->kind == K_BYTES && from >= 0 && to > from &&
                    (size_t)to <= src->len && (size_t)from <= src->len) {
                    size_t c = (size_t)(to - from);
                    unsigned char *cp = xmalloc(c);
                    memcpy(cp, (const unsigned char *)src->data + from, c);
                    JRef *a = xmalloc(sizeof(*a));
                    a->kind = K_BYTES;
                    a->len = c;
                    a->data = cp;
                    fprintf(stderr, "JNI: unbound Arrays.copyOfRange -> %zu bytes\n", c);
                    fflush(stderr);
                    ret.l = (jobject)a;
                    return ret;
                }
                fprintf(stderr, "JNI: Arrays.copyOfRange BAD args\n");
                fflush(stderr);
            }
            /* getFreeMemory: движок зовёт его каждый тик и по нему решает,
             * есть ли смысл грузить дальше. Заглушка 0 = «памяти нет» —
             * загрузка встаёт на ~43%. Отдаём реально свободную память. */
            if (m->name && strcmp(m->name, "getFreeMemory") == 0 &&
                close && close[1] == 'J') {
                unsigned long long kb = 0;
                FILE *mf = fopen("/proc/meminfo", "r");
                if (mf) {
                    char line[256];
                    while (fgets(line, sizeof line, mf)) {
                        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
                            break;
                    }
                    fclose(mf);
                }
                if (!kb) kb = 4ULL * 1024 * 1024; /* запасной вариант: 4 ГБ */
                ret.j = (jlong)(kb * 1024ULL);
                static int gm_n;
                if (gm_n++ % 2000 == 0) {
                    fprintf(stderr, "JNI: getFreeMemory -> %llu МБ\n",
                            (unsigned long long)(kb / 1024));
                    fflush(stderr);
                }
                return ret;
            }
            if (close && close[1] == '[') {
                /* Возврат массива: NULL не годится — игра падает (getImageData)
                 * или получает "Unexpected decoded key length". Для
                 * Base64.decode декодируем K_UTF8-строку в байты. */
                JKind mk = K_BYTES;
                switch (close[2]) {
                    case 'Z': mk = K_BOOLS; break;
                    case 'B': mk = K_BYTES; break;
                    case 'C': mk = K_CHARS; break;
                    case 'S': mk = K_SHORTS; break;
                    case 'I': mk = K_INTS; break;
                    case 'J': mk = K_LONGS; break;
                    case 'F': mk = K_FLOATS; break;
                    case 'D': mk = K_DOUBLES; break;
                    default: mk = K_OBJECTS; break;
                }
                size_t len = 0;
                void *data = NULL;
                if (m->name && strcmp(m->name, "decode") == 0 &&
                    n >= 2 && args[0].l) {
                    JRef *s = (JRef *)args[0].l;
                    if (s->kind == K_UTF8 && s->data) {
                        const unsigned char *src =
                            (const unsigned char *)s->data;
                        size_t in = s->len;
                        unsigned char *out = xmalloc((in * 3) / 4 + 4);
                        size_t on = 0;
                        uint32_t v = 0;
                        int bits = 0;
                        for (size_t i = 0; i < in; ++i) {
                            int c = src[i];
                            int val;
                            if (c >= 'A' && c <= 'Z') val = c - 'A';
                            else if (c >= 'a' && c <= 'z') val = c - 'a' + 26;
                            else if (c >= '0' && c <= '9') val = c - '0' + 52;
                            else if (c == '+') val = 62;
                            else if (c == '/') val = 63;
                            else continue;
                            v = (v << 6) | (uint32_t)val;
                            bits += 6;
                            if (bits >= 8) {
                                bits -= 8;
                                out[on++] = (unsigned char)(v >> bits);
                            }
                        }
                        len = on;
                        data = out;
                    }
                    fprintf(stderr, "JNI: unbound Base64.decode -> %zu bytes\n", len);
                    fflush(stderr);
                }
                if (!data) data = xmalloc(8);
                JRef *a = xmalloc(sizeof(*a));
                a->kind = mk;
                a->len = len;
                a->data = data;
                /* Сетевые запросы: даём легитимный массив с 127.0.0.1, чтобы
                 * сетевой стек не прыгал по пустой/мусорной памяти. */
                if (mk == K_OBJECTS && m->name &&
                    (strstr(m->name, "IPAddress") ||
                     strstr(m->name, "BroadcastAddress"))) {
                    const char *ip = "127.0.0.1";
                    JRef *s = xmalloc(sizeof(*s));
                    s->kind = K_UTF8;
                    s->len = strlen(ip);
                    s->data = xstrdup(ip);
                    a->len = 1;
                    a->data = xmalloc(sizeof(jobject));
                    ((jobject *)a->data)[0] = (jobject)s;
                    fprintf(stderr, "JNI: unbound %s -> [%s]\n", m->name, ip);
                    fflush(stderr);
                }
                ret.l = (jobject)a;
                return ret;
            }
            if (close && close[1] == 'L') {
                /* Строка: движок потом зовёт GetStringUTFChars, поэтому нужен
                 * настоящий jstring с осмысленным содержимым, а не JClass.
                 * Тип возврата лежит ПОСЛЕ ')': раньше здесь искали
                 * "Ljava/lang/String;" во всей сигнатуре, и метод
                 * createGooglePlayStore(String,StoreListener)Store
                 * возвращал "Linux" вместо объекта. Движок звал на этой
                 * строке getRealmsSkuPrefix и падал на rip=0x4. */
                if (strcmp(close + 1, "Ljava/lang/String;") == 0) {
                    const char *val = "Linux";
                    if (strstr(m->name, "Model")) val = "SuperCraft PC";
                    else if (strstr(m->name, "CPUType")) val = "x86_64";
                    else if (strstr(m->name, "CPUNa") || strstr(m->name, "CPU")) val = "x86_64";
                    else if (strstr(m->name, "Features")) val = "";
                    /* Пути хранилища: игра берёт их как CurrentFileStoragePath,
                     * поэтому нужен реальный каталог, а не слово "Linux". */
                    else if (strstr(m->name, "LegacyExternal")) val = "user_data/bedrock";
                    else if (strstr(m->name, "StoragePath") || strstr(m->name, "getPath"))
                        val = "user_data/bedrock/nativeact";
                    else if (strstr(m->name, "Installer")) val = "com.mojang.minecraftpe";
                    else if (strstr(m->name, "SecureId")) val = "00000000-0000-0000-0000-000000000000";
                    else if (strstr(m->name, "Serial")) val = "SUPERCRFT0001";
                    else if (strstr(m->name, "Board")) val = "x86_64";
                    else if (strstr(m->name, "AndroidVersion")) val = "33";
                    else if (strstr(m->name, "Locale")) val = "en_US";
                    else if (strstr(m->name, "Signatures")) val = "";
                    JRef *s = xmalloc(sizeof(*s));
                    s->kind = K_UTF8;
                    s->len = strlen(val);
                    s->data = xstrdup(val);
                    fprintf(stderr, "JNI: unbound String-return %s -> \"%s\" %p\n",
                            m->name, val, (void *)s);
                    fflush(stderr);
                    ret.l = (jobject)s;
                } else {
                    char cbuf[256];
                    const char *cname = class_from_ret(close + 1, cbuf, sizeof cbuf);
                    JClass *stub = find_or_make_class(cname);
                    fprintf(stderr, "JNI: unbound obj-return %s -> %s %p\n",
                            m->name ? m->name : "?", cname, (void *)stub);
                    fflush(stderr);
                    ret.l = (jobject)stub;
                }
            }
        }
        return ret;
    }
    for (int i = 0; i < n; ++i)
        if (types[i] == 'F' || types[i] == 'D') {
            fprintf(stderr, "JNI: FP arg unsupported: %s %s\n", m->name, m->sig);
            fflush(stderr);
            return ret;
        }
    if (n > 6) {
        fprintf(stderr, "JNI: too many args (%d) for %s %s\n", n, m->name, m->sig);
        fflush(stderr);
        return ret;
    }
    long long g[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < n; ++i) g[i] = args[i].j;
    fprintf(stderr, "JNI: call %s %s (n=%d this=%p)\n", m->name, m->sig, n, obj);
    fflush(stderr);
    long long r = 0;
    switch (n) {
        case 0: r = ((f2)(void *)m->fn)((void *)e, (void *)obj); break;
        case 1: r = ((f3)(void *)m->fn)((void *)e, (void *)obj, g[0]); break;
        case 2: r = ((f4)(void *)m->fn)((void *)e, (void *)obj, g[0], g[1]); break;
        case 3: r = ((f5)(void *)m->fn)((void *)e, (void *)obj, g[0], g[1], g[2]); break;
        case 4: r = ((f6)(void *)m->fn)((void *)e, (void *)obj, g[0], g[1], g[2], g[3]); break;
        case 5: r = ((f7)(void *)m->fn)((void *)e, (void *)obj, g[0], g[1], g[2], g[3], g[4]); break;
        case 6: r = ((f8)(void *)m->fn)((void *)e, (void *)obj, g[0], g[1], g[2], g[3], g[4], g[5]); break;
    }
    ret.j = r;
    return ret;
}

#define CALL_DISPATCH(CName, CType, field)                                   \
    static CType fnCall##CName##Method(JNIEnv *e, jobject o, jmethodID m, ...) { \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallStatic##CName##Method(JNIEnv *e, jclass c, jmethodID m, ...) { \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, (jobject)c, m, args, n, types);            \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallNonvirtual##CName##Method(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) { \
        (void)c;                                                             \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }

/* Object-варианты: сначала game_virtual, потом call_method */
#define CALL_DISPATCH_OBJ(CName, CType, field)                               \
    static CType fnCall##CName##Method(JNIEnv *e, jobject o, jmethodID m, ...) { \
        jobject g = game_virtual(e, o, m);                                   \
        if (g) return (CType)g;                                              \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallStatic##CName##Method(JNIEnv *e, jclass c, jmethodID m, ...) { \
        jobject g = game_virtual(e, (jobject)c, m);                          \
        if (g) return (CType)g;                                              \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, (jobject)c, m, args, n, types);            \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallNonvirtual##CName##Method(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) { \
        (void)c;                                                             \
        jobject g = game_virtual(e, o, m);                                   \
        if (g) return (CType)g;                                              \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        va_end(ap);                                                          \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }

CALL_DISPATCH_OBJ(Object, jobject, l)
CALL_DISPATCH(Boolean, jboolean, z)
CALL_DISPATCH(Byte, jbyte, b)
CALL_DISPATCH(Char, jchar, c)
CALL_DISPATCH(Short, jshort, s)
CALL_DISPATCH(Int, jint, i)
CALL_DISPATCH(Long, jlong, j)
CALL_DISPATCH(Float, jfloat, f)
CALL_DISPATCH(Double, jdouble, d)

#undef CALL_DISPATCH
#undef CALL_DISPATCH_OBJ

/* ---- game-driven virtual dispatch (Call<X>MethodV/A families) ---- */

static jobject game_virtual(JNIEnv *e, jobject o, jmethodID mid) {
    JMethod *m = (JMethod *)mid;
    if (m && m->name) {
        if (strcmp(m->name, "getClassLoader") == 0) {
            fprintf(stderr, "JNI: Virtual.getClassLoader() -> java/lang/ClassLoader\n");
            fflush(stderr);
            return (jobject)find_or_make_class("java/lang/ClassLoader");
        }
        if (strcmp(m->name, "findClass") == 0) {
            const char *n = (g_last_utf && g_last_utf->data) ? (const char *)g_last_utf->data : NULL;
            if (!n) n = g_last_find_class;
            fprintf(stderr, "JNI: Virtual.findClass(%s)\n", n ? n : "(null)");
            fflush(stderr);
            return n ? (jobject)find_or_make_class(n) : NULL;
        }
        if (strcmp(m->name, "getSystemService") == 0) {
            fprintf(stderr, "JNI: Virtual.getSystemService(%s) -> java/lang/Object\n", m->sig);
            fflush(stderr);
            return (jobject)find_or_make_class("java/lang/Object");
        }

        /* --- MainActivity storage paths --- */
        if (strcmp(m->name, "getInternalStoragePath") == 0) {
            const char *p = getenv("SUP_NATIVE_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact";
            fprintf(stderr, "JNI: Virtual.getInternalStoragePath() -> %s\n", p);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, p);
        }
        if (strcmp(m->name, "getExternalStoragePath") == 0) {
            const char *p = getenv("SUP_EXTERNAL_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact_ext";
            fprintf(stderr, "JNI: Virtual.getExternalStoragePath() -> %s\n", p);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, p);
        }
        if (strcmp(m->name, "getExternalFilesDir") == 0) {
            const char *p = getenv("SUP_EXTERNAL_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact_ext";
            fprintf(stderr, "JNI: Virtual.getExternalFilesDir() -> %s\n", p);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, p);
        }
        if (strcmp(m->name, "getFilesDir") == 0) {
            const char *p = getenv("SUP_NATIVE_DIR");
            if (!p || !*p) p = "user_data/bedrock/nativeact";
            fprintf(stderr, "JNI: Virtual.getFilesDir() -> %s\n", p);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, p);
        }
        if (strcmp(m->name, "getCacheDir") == 0) {
            fprintf(stderr, "JNI: Virtual.getCacheDir() -> user_data/bedrock/cache\n");
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "user_data/bedrock/cache");
        }

        /* --- device / UUID --- */
        if (strcmp(m->name, "getLegacyDeviceID") == 0) {
            fprintf(stderr, "JNI: Virtual.getLegacyDeviceID() -> fixed\n");
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "00000000-0000-0000-0000-000000000000");
        }
        if (strcmp(m->name, "getDeviceId") == 0) {
            fprintf(stderr, "JNI: Virtual.getDeviceId() -> fixed\n");
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "00000000-0000-0000-0000-000000000000");
        }
        if (strcmp(m->name, "createUUID") == 0) {
            fprintf(stderr, "JNI: Virtual.createUUID() -> fixed\n");
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "11111111-1111-1111-1111-111111111111");
        }

        /* --- crash manager --- */
        if (strcmp(m->name, "initializeCrashManager") == 0) {
            fprintf(stderr, "JNI: Virtual.initializeCrashManager(%s) -> stub CrashManager\n", m->sig);
            fflush(stderr);
            return (jobject)find_or_make_class("com/mojang/minecraftpe/CrashManager");
        }

        /* --- misc MainActivity, которые игра может дёргать --- */
        if (strcmp(m->name, "getPackageName") == 0) {
            fprintf(stderr, "JNI: Virtual.getPackageName() -> com.mojang.minecraftpe\n");
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "com.mojang.minecraftpe");
        }
        if (strcmp(m->name, "getApplicationContext") == 0) {
            fprintf(stderr, "JNI: Virtual.getApplicationContext() -> this\n");
            fflush(stderr);
            return o;
        }
        if (strcmp(m->name, "getAssets") == 0) {
            fprintf(stderr, "JNI: Virtual.getAssets() -> stub AssetManager\n");
            fflush(stderr);
            return (jobject)find_or_make_class("android/content/res/AssetManager");
        }
        if (strcmp(m->name, "getResources") == 0) {
            fprintf(stderr, "JNI: Virtual.getResources() -> stub Resources\n");
            fflush(stderr);
            return (jobject)find_or_make_class("android/content/res/Resources");
        }
        if (strcmp(m->name, "getWindowManager") == 0) {
            fprintf(stderr, "JNI: Virtual.getWindowManager() -> stub WindowManager\n");
            fflush(stderr);
            return (jobject)find_or_make_class("android/view/WindowManager");
        }
        if (strcmp(m->name, "getSystemService") == 0) {
            fprintf(stderr, "JNI: Virtual.getSystemService(%s) -> stub\n", m->sig);
            fflush(stderr);
            return (jobject)find_or_make_class("java/lang/Object");
        }
        if (strcmp(m->name, "getClass") == 0) {
            fprintf(stderr, "JNI: Virtual.getClass() -> java/lang/Class\n");
            fflush(stderr);
            return (jobject)find_or_make_class("java/lang/Class");
        }

        /* --- crash upload URI stubs (emulator) --- */
        if (strcmp(m->name, "getCrashUploadURI") == 0 ||
            strcmp(m->name, "getExceptionUploadURI") == 0) {
            fprintf(stderr, "JNI: Virtual.%s(%s) -> empty URI\n", m->name, m->sig);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "");
        }
        if (strcmp(m->name, "uploadCrashFile") == 0) {
            fprintf(stderr, "JNI: Virtual.uploadCrashFile(%s) -> uploaded\n", m->sig);
            fflush(stderr);
            return (jobject)fnNewStringUTF(e, "uploaded");
        }
    }
    fprintf(stderr, "JNI: Virtual: unhandled %s %s (this=%p)\n",
            m ? m->name : "?", m ? m->sig : "?", o);
    fflush(stderr);
    return NULL;
}

#define JNI_NULL_METHOD_V(CName, CType)                                     \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in Call" #CName            \
                    "MethodV (this=%p)\n", o);                               \
            fflush(stderr);                                                  \
            return (CType)0;                                                 \
        }

#define JNI_NULL_METHOD_A(CName, CType)                                     \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in Call" #CName            \
                    "MethodA (this=%p)\n", o);                               \
            fflush(stderr);                                                  \
            return (CType)0;                                                 \
        }

#define CALL_VA(CName, CType, field)                                         \
    static CType fnCall##CName##MethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { \
        JNI_NULL_METHOD_V(CName, CType)                                             \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCall##CName##MethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *args) { \
        JNI_NULL_METHOD_A(CName, CType)                                             \
        char types[32]; jvalue a[32];                                        \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        for (int i = 0; i < n && i < 32; ++i) a[i] = args[i];                \
        jvalue v = call_method(e, o, m, a, n, types);                        \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallStatic##CName##MethodV(JNIEnv *e, jclass c, jmethodID m, va_list ap) { \
        return fnCall##CName##MethodV(e, (jobject)c, m, ap);                 \
    }                                                                        \
    static CType fnCallStatic##CName##MethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *args) { \
        return fnCall##CName##MethodA(e, (jobject)c, m, args);               \
    }                                                                        \
    static CType fnCallNonvirtual##CName##MethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, va_list ap) { \
        (void)c; return fnCall##CName##MethodV(e, o, m, ap);                 \
    }                                                                        \
    static CType fnCallNonvirtual##CName##MethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *args) { \
        (void)c; return fnCall##CName##MethodA(e, o, m, args);               \
    }

#define CALL_VA_OBJ(CName, CType, field)                                     \
    static CType fnCall##CName##MethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { \
        JNI_NULL_METHOD_V(CName, CType)                                             \
        jobject g = game_virtual(e, o, m);                                   \
        if (g) return (CType)g;                                              \
        char types[32]; jvalue args[32];                                     \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        read_args(n, types, ap, args);                                       \
        jvalue v = call_method(e, o, m, args, n, types);                     \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCall##CName##MethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *args) { \
        JNI_NULL_METHOD_A(CName, CType)                                             \
        jobject g = game_virtual(e, o, m);                                   \
        if (g) return (CType)g;                                              \
        char types[32]; jvalue a[32];                                        \
        int n, ret;                                                          \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                 \
        for (int i = 0; i < n && i < 32; ++i) a[i] = args[i];                \
        jvalue v = call_method(e, o, m, a, n, types);                        \
        return (CType)v.field;                                               \
    }                                                                        \
    static CType fnCallStatic##CName##MethodV(JNIEnv *e, jclass c, jmethodID m, va_list ap) { \
        return fnCall##CName##MethodV(e, (jobject)c, m, ap);                 \
    }                                                                        \
    static CType fnCallStatic##CName##MethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *args) { \
        return fnCall##CName##MethodA(e, (jobject)c, m, args);               \
    }                                                                        \
    static CType fnCallNonvirtual##CName##MethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, va_list ap) { \
        (void)c; return fnCall##CName##MethodV(e, o, m, ap);                 \
    }                                                                        \
    static CType fnCallNonvirtual##CName##MethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *args) { \
        (void)c; return fnCall##CName##MethodA(e, o, m, args);               \
    }

CALL_VA_OBJ(Object, jobject, l)
CALL_VA(Boolean, jboolean, z)
CALL_VA(Int, jint, i)
CALL_VA(Long, jlong, j)
#undef CALL_VA
#undef CALL_VA_OBJ

#define CALL_VOID(tag)                                                     \
    static void fnCall##tag##VoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) { \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in Call" #tag               \
                    "VoidMethod (this=%p)\n", o);                             \
            fflush(stderr);                                                  \
            return;                                                          \
        }                                                                    \
        va_list ap;                                                        \
        va_start(ap, m);                                                   \
        char types[32]; jvalue args[32];                                   \
        int n, ret;                                                        \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);               \
        read_args(n, types, ap, args);                                     \
        va_end(ap);                                                        \
        call_method(e, o, m, args, n, types);                              \
    }                                                                      \
    static void fnCallStatic##tag##VoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) { \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in CallStatic" #tag         \
                    "VoidMethod (this=%p)\n", (void *)c);                    \
            fflush(stderr);                                                  \
            return;                                                          \
        }                                                                    \
        va_list ap;                                                        \
        va_start(ap, m);                                                   \
        char types[32]; jvalue args[32];                                   \
        int n, ret;                                                        \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);               \
        read_args(n, types, ap, args);                                     \
        va_end(ap);                                                        \
        call_method(e, (jobject)c, m, args, n, types);                     \
    }                                                                      \
    static void fnCallNonvirtual##tag##VoidMethod(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) { \
        (void)c;                                                           \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in CallNonvirtual" #tag    \
                    "VoidMethod (this=%p)\n", o);                            \
            fflush(stderr);                                                  \
            return;                                                          \
        }                                                                    \
        va_list ap;                                                        \
        va_start(ap, m);                                                   \
        char types[32]; jvalue args[32];                                   \
        int n, ret;                                                        \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);               \
        read_args(n, types, ap, args);                                     \
        va_end(ap);                                                        \
        call_method(e, o, m, args, n, types);                              \
    }

CALL_VOID()
#undef CALL_VOID

#define CALL_VOID_VA()                                                      \
    static void fnCallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in CallVoidMethodV"        \
                    " (this=%p)\n", o);                                       \
            fflush(stderr);                                                  \
            return;                                                          \
        }                                                                    \
        char types[32]; jvalue args[32];                                    \
        int n, ret;                                                         \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                \
        read_args(n, types, ap, args);                                      \
        call_method(e, o, m, args, n, types);                               \
    }                                                                       \
    static void fnCallVoidMethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *args) { \
        if (!m) {                                                            \
            fprintf(stderr, "JNI: NULL jmethodID in CallVoidMethodA"        \
                    " (this=%p)\n", o);                                       \
            fflush(stderr);                                                  \
            return;                                                          \
        }                                                                    \
        char types[32]; jvalue a[32];                                       \
        int n, ret;                                                         \
        n = parse_sig(((JMethod *)m)->sig, types, 32, &ret);                \
        for (int i = 0; i < n && i < 32; ++i) a[i] = args[i];               \
        call_method(e, o, m, a, n, types);                                  \
    }                                                                       \
    static void fnCallStaticVoidMethodV(JNIEnv *e, jclass c, jmethodID m, va_list ap) { \
        fnCallVoidMethodV(e, (jobject)c, m, ap);                            \
    }                                                                       \
    static void fnCallStaticVoidMethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *args) { \
        fnCallVoidMethodA(e, (jobject)c, m, args);                          \
    }                                                                       \
    static void fnCallNonvirtualVoidMethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, va_list ap) { \
        (void)c; fnCallVoidMethodV(e, o, m, ap);                            \
    }                                                                       \
    static void fnCallNonvirtualVoidMethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *args) { \
        (void)c; fnCallVoidMethodA(e, o, m, args);                          \
    }

CALL_VOID_VA()
#undef CALL_VOID_VA

/* ---- fields ---- */

static jfieldID fnGetFieldID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e; (void)c;
    TRACE("GetFieldID(%s %s)\n", name, sig);
    return (jfieldID)g_unknown_field;
}

#define FIELD_GET(CName, CType, defval)                                    \
    static CType fnGet##CName##Field(JNIEnv *e, jobject o, jfieldID f) { \
        (void)e; (void)o; (void)f;                                        \
        return (CType)(defval);                                           \
    }                                                                     \
    static CType fnGetStatic##CName##Field(JNIEnv *e, jclass c, jfieldID f) { \
        (void)e; (void)c; (void)f;                                        \
        return (CType)(defval);                                           \
    }

FIELD_GET(Object, jobject, NULL)
FIELD_GET(Boolean, jboolean, 0)
FIELD_GET(Byte, jbyte, 0)
FIELD_GET(Char, jchar, 0)
FIELD_GET(Short, jshort, 0)
FIELD_GET(Int, jint, 0)
FIELD_GET(Long, jlong, 0)
FIELD_GET(Float, jfloat, 0)
FIELD_GET(Double, jdouble, 0)

#undef FIELD_GET

#define FIELD_SET(CName, CType)                                    \
    static void fnSet##CName##Field(JNIEnv *e, jobject o, jfieldID f, CType v) { \
        (void)e; (void)o; (void)f; (void)v;                        \
    }                                                              \
    static void fnSetStatic##CName##Field(JNIEnv *e, jclass c, jfieldID f, CType v) { \
        (void)e; (void)c; (void)f; (void)v;                        \
    }

FIELD_SET(Object, jobject)
FIELD_SET(Boolean, jboolean)
FIELD_SET(Byte, jbyte)
FIELD_SET(Char, jchar)
FIELD_SET(Short, jshort)
FIELD_SET(Int, jint)
FIELD_SET(Long, jlong)
FIELD_SET(Float, jfloat)
FIELD_SET(Double, jdouble)

#undef FIELD_SET

static jfieldID fnGetStaticFieldID(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e; (void)c;
    TRACE("GetStaticFieldID(%s %s)\n", name, sig);
    return (jfieldID)g_unknown_field;
}

/* ---- strings ---- */

static jstring fnNewString(JNIEnv *e, const jchar *u, jsize len) {
    (void)e;
    JRef *r = xmalloc(sizeof(*r));
    r->kind = K_CHARS;
    r->len = (size_t)len;
    r->data = xmalloc(sizeof(jchar) * (size_t)(len + 1));
    if (len) memcpy(r->data, u, sizeof(jchar) * (size_t)len);
    ((jchar *)r->data)[len] = 0;
    return (jstring)r;
}

static jsize fnGetStringLength(JNIEnv *e, jstring s) {
    JRef *r = as_ref(s, K_CHARS);
    (void)e;
    return r ? (jsize)r->len : 0;
}

static const jchar *fnGetStringChars(JNIEnv *e, jstring s, jboolean *isCopy) {
    JRef *r = as_ref(s, K_CHARS);
    (void)e;
    if (isCopy) *isCopy = JNI_FALSE;
    return r ? (const jchar *)r->data : NULL;
}

static void fnReleaseStringChars(JNIEnv *e, jstring s, const jchar *c) {
    (void)e; (void)s; (void)c;
}

static jstring fnNewStringUTF(JNIEnv *e, const char *u) {
    fprintf(stderr, "JNI: fnNewStringUTF(\"%s\")\n", u ? u : "(null)");
    fflush(stderr);
    (void)e;
    JRef *r = xmalloc(sizeof(*r));
    r->kind = K_UTF8;
    r->len = u ? strlen(u) : 0;
    r->data = xstrdup(u ? u : "");
    g_last_utf = r;
    return (jstring)r;
}

static jsize fnGetStringUTFLength(JNIEnv *e, jstring s) {
    JRef *r = as_ref(s, K_UTF8);
    (void)e;
    return r ? (jsize)r->len : 0;
}

static const char *fnGetStringUTFChars(JNIEnv *e, jstring s, jboolean *isCopy) {
    JRef *r = as_ref(s, K_UTF8);
    const char *result = r ? (const char *)r->data : NULL;
    fprintf(stderr, "JNI: fnGetStringUTFChars(%p) -> %s\n", (void*)s, result ? result : "(null)");
    fflush(stderr);
    (void)e;
    if (isCopy) *isCopy = JNI_FALSE;
    return result;
}

static void fnReleaseStringUTFChars(JNIEnv *e, jstring s, const char *c) {
    (void)e; (void)s; (void)c;
}

static void fnGetStringRegion(JNIEnv *e, jstring s, jsize off, jsize len, jchar *buf) {
    JRef *r = as_ref(s, K_CHARS);
    (void)e;
    if (!r || off < 0 || len < 0 || off + len > (jsize)r->len) return;
    memcpy(buf, (const jchar *)r->data + off, sizeof(jchar) * (size_t)len);
}

static void fnGetStringUTFRegion(JNIEnv *e, jstring s, jsize off, jsize len, char *buf) {
    JRef *r = as_ref(s, K_UTF8);
    (void)e;
    if (!r || off < 0 || len < 0 || off + len > (jsize)r->len) return;
    memcpy(buf, (const char *)r->data + off, (size_t)len);
    buf[len] = 0;
}

/* ---- arrays ---- */

#define NEW_ARRAY(kindval, CName, CType)                                     \
    static CType fnNew##CName##Array(JNIEnv *e, jsize len) {                 \
        (void)e;                                                             \
        JRef *r = xmalloc(sizeof(*r));                                       \
        r->kind = kindval;                                                   \
        r->len = (size_t)len;                                                \
        r->data = xmalloc((size_t)len * 8 + 8);                              \
        return (CType)r;                                                     \
    }

NEW_ARRAY(K_BYTES, Byte, jbyteArray)
NEW_ARRAY(K_BOOLS, Boolean, jbooleanArray)
NEW_ARRAY(K_CHARS, Char, jcharArray)
NEW_ARRAY(K_SHORTS, Short, jshortArray)
NEW_ARRAY(K_INTS, Int, jintArray)
NEW_ARRAY(K_LONGS, Long, jlongArray)
NEW_ARRAY(K_FLOATS, Float, jfloatArray)
NEW_ARRAY(K_DOUBLES, Double, jdoubleArray)

#undef NEW_ARRAY

static jsize fnGetArrayLength(JNIEnv *e, jarray a) {
    (void)e;
    return a ? (jsize)((JRef *)a)->len : 0;
}

static jobjectArray fnNewObjectArray(JNIEnv *e, jsize len, jclass c, jobject init) {
    (void)e; (void)c; (void)init;
    JRef *r = xmalloc(sizeof(*r));
    r->kind = K_OBJECTS;
    r->len = (size_t)len;
    r->data = xmalloc((size_t)len * sizeof(jobject));
    for (jsize i = 0; i < len; ++i) ((jobject *)r->data)[i] = init;
    return (jobjectArray)r;
}

static jobject fnGetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i) {
    JRef *r = as_ref((jobject)a, K_OBJECTS);
    (void)e;
    if (!r || i < 0 || (size_t)i >= r->len) return NULL;
    return ((jobject *)r->data)[i];
}

static void fnSetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i, jobject v) {
    JRef *r = as_ref((jobject)a, K_OBJECTS);
    (void)e;
    if (!r || i < 0 || (size_t)i >= r->len) return;
    ((jobject *)r->data)[i] = v;
}

#define ELEMS_GET(kindval, elemtype, CName, CType)                             \
    static elemtype *fnGet##CName##ArrayElements(JNIEnv *e, CType a, jboolean *isCopy) { \
        JRef *r = as_ref((jobject)a, kindval); (void)e;                        \
        if (isCopy) *isCopy = JNI_FALSE;                                       \
        return r ? (elemtype *)r->data : NULL;                                 \
    }                                                                          \
    static void fnRelease##CName##ArrayElements(JNIEnv *e, CType a, elemtype *p, jint m) { \
        (void)e; (void)a; (void)p; (void)m;                                    \
    }

ELEMS_GET(K_BYTES, jbyte, Byte, jbyteArray)
ELEMS_GET(K_BOOLS, jboolean, Boolean, jbooleanArray)
ELEMS_GET(K_CHARS, jchar, Char, jcharArray)
ELEMS_GET(K_SHORTS, jshort, Short, jshortArray)
ELEMS_GET(K_INTS, jint, Int, jintArray)
ELEMS_GET(K_LONGS, jlong, Long, jlongArray)
ELEMS_GET(K_FLOATS, jfloat, Float, jfloatArray)
ELEMS_GET(K_DOUBLES, jdouble, Double, jdoubleArray)

#undef ELEMS_GET

#define REGION_GET(kindval, elemtype, CName, CType)                             \
    static void fnGet##CName##ArrayRegion(JNIEnv *e, CType a, jsize off, jsize len, elemtype *buf) { \
        JRef *r = as_ref((jobject)a, kindval); (void)e;                         \
        if (!r || off < 0 || len < 0 || off + len > (jsize)r->len) return;      \
        memcpy(buf, (elemtype *)r->data + off, sizeof(elemtype) * (size_t)len); \
    }                                                                           \
    static void fnSet##CName##ArrayRegion(JNIEnv *e, CType a, jsize off, jsize len, const elemtype *buf) { \
        JRef *r = as_ref((jobject)a, kindval); (void)e;                         \
        if (!r || off < 0 || len < 0 || off + len > (jsize)r->len) return;      \
        memcpy((elemtype *)r->data + off, buf, sizeof(elemtype) * (size_t)len); \
    }

REGION_GET(K_BOOLS, jboolean, Boolean, jbooleanArray)
REGION_GET(K_BYTES, jbyte, Byte, jbyteArray)
REGION_GET(K_CHARS, jchar, Char, jcharArray)
REGION_GET(K_SHORTS, jshort, Short, jshortArray)
REGION_GET(K_INTS, jint, Int, jintArray)
REGION_GET(K_LONGS, jlong, Long, jlongArray)
REGION_GET(K_FLOATS, jfloat, Float, jfloatArray)
REGION_GET(K_DOUBLES, jdouble, Double, jdoubleArray)

#undef REGION_GET

/* ---- natives ---- */

static JClass g_stub_class = { "java/lang/Object", {{0}}, 0 };

typedef struct RNMethod {
    char name[64];
    char sig[96];
    void *fn;
} RNMethod;

static RNMethod g_rm[8];
static size_t g_nrm;

static jint fnRegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint count) {
    JClass *cl = objclass_of((jobject)c);
    (void)e;
    if (!cl) cl = &g_stub_class;
    for (jint i = 0; i < count; ++i) {
        fprintf(stderr, "JNI: RegisterNatives %s . %s %s fn=%p\n",
                cl->name, m[i].name, m[i].signature, m[i].fnPtr);
        fflush(stderr);

        class_add_method(cl, m[i].name, m[i].signature, m[i].fnPtr);

        if ((strstr(m[i].name, "RunMain") || strstr(m[i].name, "runMain") ||
             strstr(m[i].name, "nativeRun") || strstr(m[i].name, "NativeRun")) &&
            g_nrm < 8) {
            fprintf(stderr, "JNI: *** MAIN CANDIDATE %s %s fn=%p\n",
                    m[i].name, m[i].signature, m[i].fnPtr);
            fflush(stderr);
            g_rm[g_nrm].fn = m[i].fnPtr;
            snprintf(g_rm[g_nrm].name, sizeof(g_rm[g_nrm].name), "%s", m[i].name);
            snprintf(g_rm[g_nrm].sig, sizeof(g_rm[g_nrm].sig), "%s", m[i].signature);
            ++g_nrm;
        }
    }
    return JNI_OK;
}

static jint fnUnregisterNatives(JNIEnv *e, jclass c) {
    (void)e; (void)c;
    return JNI_OK;
}

static jint fnMonitorEnter(JNIEnv *e, jobject o) {
    (void)e; (void)o;
    return JNI_OK;
}

static jint fnMonitorExit(JNIEnv *e, jobject o) {
    (void)e; (void)o;
    return JNI_OK;
}

static jint fnGetJavaVM(JNIEnv *e, JavaVM **vm) {
    (void)e;
    if (vm) *vm = (const JavaVM_ **)(void *)g_real_vm;
    return JNI_OK;
}

static void *fnGetPrimitiveArrayCritical(JNIEnv *e, jarray a, jboolean *isCopy) {
    (void)e;
    if (isCopy) *isCopy = JNI_FALSE;
    return a ? ((JRef *)a)->data : NULL;
}

static void fnReleasePrimitiveArrayCritical(JNIEnv *e, jarray a, void *p, jint m) {
    (void)e; (void)a; (void)p; (void)m;
}

static const jchar *fnGetStringCritical(JNIEnv *e, jstring s, jboolean *isCopy) {
    JRef *r = as_ref(s, K_CHARS);
    if (!r) r = as_ref(s, K_UTF8);
    (void)e;
    if (isCopy) *isCopy = JNI_FALSE;
    return r ? (const jchar *)r->data : NULL;
}

static void fnReleaseStringCritical(JNIEnv *e, jstring s, const jchar *c) {
    (void)e; (void)s; (void)c;
}

static jweak fnNewWeakGlobalRef(JNIEnv *e, jobject o) {
    (void)e;
    return (jweak)o;
}

static void fnDeleteWeakGlobalRef(JNIEnv *e, jweak o) {
    (void)e; (void)o;
}

static jboolean fnExceptionCheck(JNIEnv *e) {
    (void)e;
    return g_exc ? JNI_TRUE : JNI_FALSE;
}

static jobject fnNewDirectByteBuffer(JNIEnv *e, void *addr, jlong cap) {
    (void)e;
    JRef *r = xmalloc(sizeof(*r));
    r->kind = K_DIRECT;
    r->len = (size_t)cap;
    r->data = addr;
    return (jobject)r;
}

static void *fnGetDirectBufferAddress(JNIEnv *e, jobject b) {
    JRef *r = as_ref(b, K_DIRECT);
    (void)e;
    return r ? r->data : NULL;
}

static jlong fnGetDirectBufferCapacity(JNIEnv *e, jobject b) {
    JRef *r = as_ref(b, K_DIRECT);
    (void)e;
    return r ? (jlong)r->len : -1;
}

static jobject fnGetObjectRefType(JNIEnv *e, jobject o) {
    (void)e; (void)o;
    return (jobject)(void *)(uintptr_t)JNILocalRefType;
}

/* ---- invoke ---- */

static jint fnDestroyJavaVM(JavaVM *vm) {
    (void)vm;
    return JNI_OK;
}

static jint fnAttachCurrentThread(JavaVM *vm, JNIEnv **env, void *res) {
    (void)vm; (void)res;
    if (env) *env = (const JNIEnv_ **)(void *)g_real_env;
    return JNI_OK;
}

static jint fnDetachCurrentThread(JavaVM *vm) {
    (void)vm;
    return JNI_OK;
}

void *fnGameInvoke(void *thiz, void *receiver, jmethodID mid, void *va) {
    (void)thiz;
    JMethod *m = (JMethod *)mid;
    if (m && m->name && strcmp(m->name, "getClassLoader") == 0) {
        TRACE("gInvoke: getClassLoader() -> java/lang/ClassLoader\n");
        return find_or_make_class("java/lang/ClassLoader");
    }
    if (m && m->name && strcmp(m->name, "findClass") == 0) {
        const char *n = NULL;
        uintptr_t *w = (uintptr_t *)va;
        for (int i = 0; i < 8; ++i) {
            JRef *r = (JRef *)(void *)(uintptr_t)0;
            if (w && w[i] > 0x10000 && w[i] < (1ULL << 47))
                r = (JRef *)(void *)(uintptr_t)w[i];
            if (r && r->kind == K_UTF8 && r->data) { n = (const char *)r->data; break; }
        }
        if (!n && g_last_utf && g_last_utf->data) n = (const char *)g_last_utf->data;
        TRACE("gInvoke: findClass(%s)\n", n ? n : "(null)");
        return find_or_make_class(n ? n : "unknown");
    }
    TRACE("gInvoke: unhandled %s %s (recv=%p va=%p)\n",
          m ? m->name : "?", m ? m->sig : "?", receiver, va);
    return NULL;
}

static jint fnGetEnv(JavaVM *vm, JNIEnv **env, jint ver) {
    sup_getenv_log(vm, env, ver);
    (void)vm;
    if (env) *env = (const JNIEnv_ **)(void *)g_real_env;
    if (ver == JNI_VERSION_1_1 || ver == JNI_VERSION_1_2 ||
        ver == JNI_VERSION_1_4 || ver == JNI_VERSION_1_6)
        return JNI_OK;
    return JNI_EVERSION;
}

static jint fnAttachCurrentThreadAsDaemon(JavaVM *vm, JNIEnv **env, void *res) {
    (void)vm; (void)res;
    if (env) *env = (const JNIEnv_ **)(void *)g_real_env;
    return JNI_OK;
}

/* ---- tables ---- */

static JNINativeInterface g_native_table = {
    .GetVersion = fnGetVersion,
    .DefineClass = fnDefineClass,
    .FindClass = fnFindClass,
    .FromReflectedMethod = fnFromReflectedMethod,
    .FromReflectedField = fnFromReflectedField,
    .ToReflectedMethod = fnToReflectedMethod,
    .GetSuperclass = fnGetSuperclass,
    .IsAssignableFrom = fnIsAssignableFrom,
    .ToReflectedField = fnToReflectedField,
    .Throw = fnThrow,
    .ThrowNew = fnThrowNew,
    .ExceptionOccurred = fnExceptionOccurred,
    .ExceptionDescribe = fnExceptionDescribe,
    .ExceptionClear = fnExceptionClear,
    .FatalError = fnFatalError,
    .PushLocalFrame = fnPushLocalFrame,
    .PopLocalFrame = fnPopLocalFrame,
    .NewGlobalRef = fnNewGlobalRef,
    .DeleteGlobalRef = fnDeleteGlobalRef,
    .DeleteLocalRef = fnDeleteLocalRef,
    .IsSameObject = fnIsSameObject,
    .NewLocalRef = fnNewLocalRef,
    .EnsureLocalCapacity = fnEnsureLocalCapacity,
    .AllocObject = fnAllocObject,
    .NewObject = fnNewObject,
    .NewObjectV = fnNewObjectV,
    .NewObjectA = fnNewObjectA,
    .GetObjectClass = fnGetObjectClass,
    .IsInstanceOf = fnIsInstanceOf,
    .GetMethodID = fnGetMethodID,
.CallObjectMethod = fnCallObjectMethod,
    .CallObjectMethodV = fnCallObjectMethodV,
    .CallObjectMethodA = fnCallObjectMethodA,
    .CallBooleanMethod = fnCallBooleanMethod,
    .CallBooleanMethodV = fnCallBooleanMethodV,
    .CallBooleanMethodA = fnCallBooleanMethodA,
    .CallByteMethod = fnCallByteMethod,
    .CallCharMethod = fnCallCharMethod,
    .CallShortMethod = fnCallShortMethod,
    .CallIntMethod = fnCallIntMethod,
    .CallIntMethodV = fnCallIntMethodV,
    .CallIntMethodA = fnCallIntMethodA,
    .CallLongMethod = fnCallLongMethod,
    .CallLongMethodV = fnCallLongMethodV,
    .CallLongMethodA = fnCallLongMethodA,
    .CallFloatMethod = fnCallFloatMethod,
    .CallDoubleMethod = fnCallDoubleMethod,
    .CallVoidMethod = fnCallVoidMethod,
    .CallVoidMethodV = fnCallVoidMethodV,
    .CallVoidMethodA = fnCallVoidMethodA,
    .CallNonvirtualObjectMethod = fnCallNonvirtualObjectMethod,
    .CallNonvirtualObjectMethodV = fnCallNonvirtualObjectMethodV,
    .CallNonvirtualObjectMethodA = fnCallNonvirtualObjectMethodA,
    .CallNonvirtualBooleanMethod = fnCallNonvirtualBooleanMethod,
    .CallNonvirtualBooleanMethodV = fnCallNonvirtualBooleanMethodV,
    .CallNonvirtualBooleanMethodA = fnCallNonvirtualBooleanMethodA,
    .CallNonvirtualByteMethod = fnCallNonvirtualByteMethod,
    .CallNonvirtualCharMethod = fnCallNonvirtualCharMethod,
    .CallNonvirtualShortMethod = fnCallNonvirtualShortMethod,
    .CallNonvirtualIntMethod = fnCallNonvirtualIntMethod,
    .CallNonvirtualIntMethodV = fnCallNonvirtualIntMethodV,
    .CallNonvirtualIntMethodA = fnCallNonvirtualIntMethodA,
    .CallNonvirtualLongMethod = fnCallNonvirtualLongMethod,
    .CallNonvirtualLongMethodV = fnCallNonvirtualLongMethodV,
    .CallNonvirtualLongMethodA = fnCallNonvirtualLongMethodA,
    .CallNonvirtualFloatMethod = fnCallNonvirtualFloatMethod,
    .CallNonvirtualDoubleMethod = fnCallNonvirtualDoubleMethod,
    .CallNonvirtualVoidMethod = fnCallNonvirtualVoidMethod,
    .CallNonvirtualVoidMethodV = fnCallNonvirtualVoidMethodV,
    .CallNonvirtualVoidMethodA = fnCallNonvirtualVoidMethodA,
    .GetFieldID = fnGetFieldID,
    .GetObjectField = fnGetObjectField,
    .GetBooleanField = fnGetBooleanField,
    .GetByteField = fnGetByteField,
    .GetCharField = fnGetCharField,
    .GetShortField = fnGetShortField,
    .GetIntField = fnGetIntField,
    .GetLongField = fnGetLongField,
    .GetFloatField = fnGetFloatField,
    .GetDoubleField = fnGetDoubleField,
    .SetObjectField = fnSetObjectField,
    .SetBooleanField = fnSetBooleanField,
    .SetByteField = fnSetByteField,
    .SetCharField = fnSetCharField,
    .SetShortField = fnSetShortField,
    .SetIntField = fnSetIntField,
    .SetLongField = fnSetLongField,
    .SetFloatField = fnSetFloatField,
    .SetDoubleField = fnSetDoubleField,
    .GetStaticMethodID = fnGetStaticMethodID,
    .CallStaticObjectMethod = fnCallStaticObjectMethod,
    .CallStaticObjectMethodV = fnCallStaticObjectMethodV,
    .CallStaticObjectMethodA = fnCallStaticObjectMethodA,
    .CallStaticBooleanMethod = fnCallStaticBooleanMethod,
    .CallStaticBooleanMethodV = fnCallStaticBooleanMethodV,
    .CallStaticBooleanMethodA = fnCallStaticBooleanMethodA,
    .CallStaticByteMethod = fnCallStaticByteMethod,
    .CallStaticCharMethod = fnCallStaticCharMethod,
    .CallStaticShortMethod = fnCallStaticShortMethod,
    .CallStaticIntMethod = fnCallStaticIntMethod,
    .CallStaticIntMethodV = fnCallStaticIntMethodV,
    .CallStaticIntMethodA = fnCallStaticIntMethodA,
    .CallStaticLongMethod = fnCallStaticLongMethod,
    .CallStaticLongMethodV = fnCallStaticLongMethodV,
    .CallStaticLongMethodA = fnCallStaticLongMethodA,
    .CallStaticFloatMethod = fnCallStaticFloatMethod,
    .CallStaticDoubleMethod = fnCallStaticDoubleMethod,
    .CallStaticVoidMethod = fnCallStaticVoidMethod,
    .CallStaticVoidMethodV = fnCallStaticVoidMethodV,
    .CallStaticVoidMethodA = fnCallStaticVoidMethodA,
    .GetStaticFieldID = fnGetStaticFieldID,
    .GetStaticObjectField = fnGetStaticObjectField,
    .GetStaticBooleanField = fnGetStaticBooleanField,
    .GetStaticByteField = fnGetStaticByteField,
    .GetStaticCharField = fnGetStaticCharField,
    .GetStaticShortField = fnGetStaticShortField,
    .GetStaticIntField = fnGetStaticIntField,
    .GetStaticLongField = fnGetStaticLongField,
    .GetStaticFloatField = fnGetStaticFloatField,
    .GetStaticDoubleField = fnGetStaticDoubleField,
    .SetStaticObjectField = fnSetStaticObjectField,
    .SetStaticBooleanField = fnSetStaticBooleanField,
    .SetStaticByteField = fnSetStaticByteField,
    .SetStaticCharField = fnSetStaticCharField,
    .SetStaticShortField = fnSetStaticShortField,
    .SetStaticIntField = fnSetStaticIntField,
    .SetStaticLongField = fnSetStaticLongField,
    .SetStaticFloatField = fnSetStaticFloatField,
    .SetStaticDoubleField = fnSetStaticDoubleField,
    .NewString = fnNewString,
    .GetStringLength = fnGetStringLength,
    .GetStringChars = fnGetStringChars,
    .ReleaseStringChars = fnReleaseStringChars,
    .NewStringUTF = fnNewStringUTF,
    .GetStringUTFLength = fnGetStringUTFLength,
    .GetStringUTFChars = fnGetStringUTFChars,
    .ReleaseStringUTFChars = fnReleaseStringUTFChars,
    .GetArrayLength = fnGetArrayLength,
    .NewObjectArray = fnNewObjectArray,
    .GetObjectArrayElement = fnGetObjectArrayElement,
    .SetObjectArrayElement = fnSetObjectArrayElement,
    .NewBooleanArray = fnNewBooleanArray,
    .NewByteArray = fnNewByteArray,
    .NewCharArray = fnNewCharArray,
    .NewShortArray = fnNewShortArray,
    .NewIntArray = fnNewIntArray,
    .NewLongArray = fnNewLongArray,
    .NewFloatArray = fnNewFloatArray,
    .NewDoubleArray = fnNewDoubleArray,
    .GetBooleanArrayElements = fnGetBooleanArrayElements,
    .GetByteArrayElements = fnGetByteArrayElements,
    .GetCharArrayElements = fnGetCharArrayElements,
    .GetShortArrayElements = fnGetShortArrayElements,
    .GetIntArrayElements = fnGetIntArrayElements,
    .GetLongArrayElements = fnGetLongArrayElements,
    .GetFloatArrayElements = fnGetFloatArrayElements,
    .GetDoubleArrayElements = fnGetDoubleArrayElements,
    .ReleaseBooleanArrayElements = fnReleaseBooleanArrayElements,
    .ReleaseByteArrayElements = fnReleaseByteArrayElements,
    .ReleaseCharArrayElements = fnReleaseCharArrayElements,
    .ReleaseShortArrayElements = fnReleaseShortArrayElements,
    .ReleaseIntArrayElements = fnReleaseIntArrayElements,
    .ReleaseLongArrayElements = fnReleaseLongArrayElements,
    .ReleaseFloatArrayElements = fnReleaseFloatArrayElements,
    .ReleaseDoubleArrayElements = fnReleaseDoubleArrayElements,
    .GetBooleanArrayRegion = fnGetBooleanArrayRegion,
    .GetByteArrayRegion = fnGetByteArrayRegion,
    .GetCharArrayRegion = fnGetCharArrayRegion,
    .GetShortArrayRegion = fnGetShortArrayRegion,
    .GetIntArrayRegion = fnGetIntArrayRegion,
    .GetLongArrayRegion = fnGetLongArrayRegion,
    .GetFloatArrayRegion = fnGetFloatArrayRegion,
    .GetDoubleArrayRegion = fnGetDoubleArrayRegion,
    .SetBooleanArrayRegion = fnSetBooleanArrayRegion,
    .SetByteArrayRegion = fnSetByteArrayRegion,
    .SetCharArrayRegion = fnSetCharArrayRegion,
    .SetShortArrayRegion = fnSetShortArrayRegion,
    .SetIntArrayRegion = fnSetIntArrayRegion,
    .SetLongArrayRegion = fnSetLongArrayRegion,
    .SetFloatArrayRegion = fnSetFloatArrayRegion,
    .SetDoubleArrayRegion = fnSetDoubleArrayRegion,
    .RegisterNatives = fnRegisterNatives,
    .UnregisterNatives = fnUnregisterNatives,
    .MonitorEnter = fnMonitorEnter,
    .MonitorExit = fnMonitorExit,
    .GetJavaVM = fnGetJavaVM,
    .GetStringRegion = fnGetStringRegion,
    .GetStringUTFRegion = fnGetStringUTFRegion,
    .GetPrimitiveArrayCritical = fnGetPrimitiveArrayCritical,
    .ReleasePrimitiveArrayCritical = fnReleasePrimitiveArrayCritical,
    .GetStringCritical = fnGetStringCritical,
    .ReleaseStringCritical = fnReleaseStringCritical,
    .NewWeakGlobalRef = fnNewWeakGlobalRef,
    .DeleteWeakGlobalRef = fnDeleteWeakGlobalRef,
    .ExceptionCheck = fnExceptionCheck,
    .NewDirectByteBuffer = fnNewDirectByteBuffer,
    .GetDirectBufferAddress = fnGetDirectBufferAddress,
    .GetDirectBufferCapacity = fnGetDirectBufferCapacity,
    .GetObjectRefType = fnGetObjectRefType,
};

static JNIInvokeInterface g_invoke_table = {
    .DestroyJavaVM = fnDestroyJavaVM,
    .AttachCurrentThread = fnAttachCurrentThread,
    .DetachCurrentThread = fnDetachCurrentThread,
    .GetEnv = fnGetEnv,
    .AttachCurrentThreadAsDaemon = fnAttachCurrentThreadAsDaemon,
};

void sup_jni_boot(void) {
    fprintf(stderr, "JNI: sup_jni_boot called\n");
    fflush(stderr);
    static int done = 0;
    if (done) return;
    done = 1;
    g_trace = getenv("SUP_JNI_TRACE") ? 1 : 0;
    if (!g_real_env || !g_real_vm) {
        void *ep = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *vp = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ep == MAP_FAILED || vp == MAP_FAILED) {
            fprintf(stderr, "JNI: could not mmap env/vm nodes\n");
            _exit(1);
        }
        g_real_env = (JNIEnv_ *)ep;
        g_real_vm = (JavaVM_ *)vp;
    }
    g_real_env->functions = &g_native_table;
    g_real_vm->functions = &g_invoke_table;
    fprintf(stderr, "JNI: &g_vm_node=%p &g_invoke_table=%p vm.functions=%p\n",
            (void *)g_real_vm, (void *)&g_invoke_table, (void *)g_real_vm->functions);
    fprintf(stderr, "JNI: &g_env_node=%p &g_native_table=%p env.functions=%p\n",
            (void *)g_real_env, (void *)&g_native_table, (void *)g_real_env->functions);
    fflush(stderr);
    find_or_make_class("com/mojang/minecraftpe/MainActivity");
    find_or_make_class("com/mojang/minecraftpe/MinecraftActivity");
    find_or_make_class("com/mojang/minecraftpe/CrashManager");
    find_or_make_class("com/mojang/minecraftpe/WorldRecovery");
    find_or_make_class("java/lang/Object");
    find_or_make_class("java/lang/ClassLoader");
    find_or_make_class("java/lang/Class");
    find_or_make_class("unknown");
}

void sup_arm_nclass_guard(void) {
    void *pg = (void *)g_nclass_map;
    fprintf(stderr, "JNI: arm: &g_nclass_map=%p pg=%p\n", (void *)&g_nclass_map, pg);
    if (pg && ((uintptr_t)pg & 0xfff) == 0) {
        /* g_nclass живёт на этой странице и законно пишется в
         * find_or_make_class, поэтому PROT_READ на всю страницу убивал
         * движок на g_classes[g_nclass++] = c. Частично прикрыть страницу
         * тоже нельзя: mprotect требует page-aligned адрес, а pg+0x40
         * не выровнен (EINVAL). Оставляем страницу RW. */
        fprintf(stderr, "JNI: nclass page stays RW (counter at %p)\n", pg);
        fflush(stderr);
    } else {
        fprintf(stderr, "JNI: nclass guard not page-aligned (%p) - can't arm\n",
                (void *)pg);
        fflush(stderr);
    }
    if (g_real_env)
        fprintf(stderr, "JNI: env/vm node pages stay RW (%p %p)\n",
                (void *)g_real_env, (void *)g_real_vm);
}

JavaVM *sup_jni_vm(void) {
    fprintf(stderr, "JNI: sup_jni_vm() -> vm=%p\n", (void *)g_real_vm);
    fflush(stderr);
    return (JavaVM *)g_real_vm;
}

JNIEnv *sup_jni_env(void) {
    fprintf(stderr, "JNI: sup_jni_env() -> env=%p\n", (void *)g_real_env);
    fflush(stderr);
    return (JNIEnv *)g_real_env;
}

void *sup_jni_find_class(const char *name) {
    return (void *)find_or_make_class(name ? name : "com/mojang/minecraftpe/MainActivity");
}

int sup_jni_run_main(void) {
    if (!g_nrm) {
        fprintf(stderr, "JNI: no main-loop candidate registered\n");
        fflush(stderr);
        return 0;
    }
    size_t pick = 0;
    for (size_t i = 0; i < g_nrm; ++i) {
        if (strcmp(g_rm[i].name, "nativeRunMain") == 0) pick = i;
    }

    const char *assets_dir = getenv("SUP_ASSETS_DIR");
    if (!assets_dir || !*assets_dir)
        assets_dir = "user_data/bedrock/versions/1.21.70.03/assets";
    fprintf(stderr, "JNI: assets_dir = %s\n", assets_dir);
    fflush(stderr);

    JRef *str = xmalloc(sizeof(*str));
    str->kind = K_UTF8;
    str->len = strlen(assets_dir);
    str->data = xstrdup(assets_dir);

    JClass *cl = find_or_make_class("com/mojang/minecraftpe/MainActivity");
    if (!cl) cl = &g_stub_class;

    typedef void (*run_t)(JNIEnv *, jobject, jobject, jobject,
                          jobject, jobject, jobject, jobject, jobject);
    run_t fn = (run_t)g_rm[pick].fn;

    fprintf(stderr, "JNI: calling main loop %s %s fn=%p env=%p cl=%p str=%p\n",
            g_rm[pick].name, g_rm[pick].sig, g_rm[pick].fn,
            (void *)sup_jni_env(), (void *)cl, (void *)str);
    fflush(stderr);

    fn(sup_jni_env(), (jobject)cl, (jobject)cl, (jobject)str,
       (jobject)str, (jobject)str, (jobject)str, (jobject)str, (jobject)str);

    fprintf(stderr, "JNI: main loop returned\n");
    fflush(stderr);
    return 1;
}