#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>

typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;
typedef struct AAssetManager AAssetManager;
typedef struct AConfiguration AConfiguration;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct ALooper ALooper;
typedef struct ANativeActivity ANativeActivity;
typedef struct ANativeWindow ANativeWindow;
typedef void (*ALooper_callbackFunc)(int, int, void *);

typedef long off64_t;

#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct AAsset {
    unsigned char *buf;
    size_t len;
    size_t pos;
};

struct AAssetDir {
    char **names;
    size_t nn;
    size_t idx;
};

struct AAssetManager {
    char root[1024];
};

static AAssetManager g_mgr;

static const char *assets_root(void) {
    if (g_mgr.root[0]) return g_mgr.root;
    /* Корень ассетов: берём первый РЕАЛЬНО существующий каталог. Раньше
     * здесь жёстко подставлялся "user_data/..." — при запуске из
     * supercraft_runner такого пути нет (данные на уровень выше), из-за
     * чего AAssetManager_openDir возвращал NULL на КАЖДЫЙ каталог, и
     * загрузка игры замирала на 43% (листинг ресурсов пустой). */
    static char cand[4][1024];
    int n = 0;
    const char *ov = getenv("SUP_ASSETS_DIR");
    if (!ov || !*ov) ov = getenv("SUP_VDIR");
    if (ov && *ov) {
        snprintf(cand[n], sizeof cand[0], "%s/assets", ov);
        ++n;
    }
    snprintf(cand[n], sizeof cand[0],
             "user_data/bedrock/versions/1.21.70.03/assets");
    ++n;
    snprintf(cand[n], sizeof cand[0],
             "../user_data/bedrock/versions/1.21.70.03/assets");
    ++n;
    snprintf(cand[n], sizeof cand[0],
             "../../user_data/bedrock/versions/1.21.70.03/assets");
    ++n;
    int pick = n - 1;
    for (int i = 0; i < n; ++i) {
        if (access(cand[i], R_OK) == 0) { pick = i; break; }
    }
    snprintf(g_mgr.root, sizeof g_mgr.root, "%s", cand[pick]);
    /* Лаунчер раскладывает данные версии как <...>/1.21.70.03/assets/
     * assets/{bootstrap.json,resource_packs,...}. Настоящий корень
     * ассетов — ВНУТРЕННИЙ assets: именно от него движок отсчитывает
     * имена (resource_packs/..., fonts/...). open() это скрывал
     * фолбэком, а openDir() — нет, поэтому листинги каталогов пустые. */
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/assets/bootstrap.json", g_mgr.root);
    if (access(probe, R_OK) == 0) {
        snprintf(probe, sizeof probe, "%s/assets", g_mgr.root);
        snprintf(g_mgr.root, sizeof g_mgr.root, "%s", probe);
    }
    return g_mgr.root;
}

AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) {
    (void)m;
    (void)mode;
    if (!name || !*name) return NULL;
    char path[2048];
    int fd = -1;
    
    /* Попытка 1: $root/$name */
    snprintf(path, sizeof path, "%s/%s", assets_root(), name);
    fd = open(path, O_RDONLY);
    
    /* Попытка 2: $root/assets/$name */
    if (fd < 0) {
        snprintf(path, sizeof path, "%s/assets/%s", assets_root(), name);
        fd = open(path, O_RDONLY);
    }
    
    /* Попытка 3: $SUP_ASSETS_DIR/$name */
    if (fd < 0) {
        const char *ov = getenv("SUP_ASSETS_DIR");
        if (ov && *ov) {
            snprintf(path, sizeof path, "%s/%s", ov, name);
            fd = open(path, O_RDONLY);
        }
    }
    
    /* Попытка 4: $SUP_ASSETS_DIR/assets/$name */
    if (fd < 0) {
        const char *ov = getenv("SUP_ASSETS_DIR");
        if (ov && *ov) {
            snprintf(path, sizeof path, "%s/assets/%s", ov, name);
            fd = open(path, O_RDONLY);
        }
    }
    
    /* Попытка 5/6: runner стартует из supercraft_runner/, а данные игры лежат
     * уровнем выше — пробуем тот же путь с префиксом "../" */
    if (fd < 0) {
        static const char *const rel[] = {
            "../user_data/bedrock/versions/1.21.70.03/assets",
            "../user_data/bedrock/versions/1.21.70.03",
        };
        for (size_t i = 0; i < sizeof rel / sizeof rel[0] && fd < 0; ++i) {
            snprintf(path, sizeof path, "%s/%s", rel[i], name);
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                snprintf(path, sizeof path, "%s/assets/%s", rel[i], name);
                fd = open(path, O_RDONLY);
            }
        }
    }

    if (fd < 0) {
        if (getenv("SUP_ASSET_TRACE")) fprintf(stderr, "AAS> %s -> ENOENT\n", name);
        return NULL;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    unsigned char *buf = mmap(NULL, len ? len : 1, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) return NULL;
    AAsset *a = calloc(1, sizeof *a);
    a->buf = buf;
    a->len = len;
    a->pos = 0;
    if (getenv("SUP_ASSET_TRACE")) fprintf(stderr, "AAS> %s -> %zu bytes\n", path, len);
    return a;
}

AAssetDir *AAssetManager_openDir(AAssetManager *m, const char *dir) {
    (void)m;
    char path[2048];
    DIR *d = NULL;
    const char *roots[4];
    int nr = 0;
    roots[nr++] = assets_root();
    /* Запас: внешний корень (файлы вроде dexopt/) и относительные
     * варианты, если assets_root выбран неверно. */
    static char outer[1100];
    snprintf(outer, sizeof outer, "%s/..", assets_root());
    roots[nr++] = outer;
    roots[nr++] = "user_data/bedrock/versions/1.21.70.03/assets";
    roots[nr++] = "../user_data/bedrock/versions/1.21.70.03/assets";
    for (int i = 0; i < nr && !d; ++i) {
        if (dir && *dir && strcmp(dir, "/") != 0)
            snprintf(path, sizeof path, "%s/%s", roots[i], dir);
        else
            snprintf(path, sizeof path, "%s", roots[i]);
        d = opendir(path);
    }
    if (!d) {
        if (getenv("SUP_ASSET_TRACE"))
            fprintf(stderr, "AAD> %s -> NULL\n", dir ? dir : "");
        return NULL;
    }
    AAssetDir *ad = calloc(1, sizeof *ad);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && !e->d_name[1]) continue;
        if (!strcmp(e->d_name, "..")) continue;
        ad->names = realloc(ad->names, (ad->nn + 1) * sizeof(char *));
        ad->names[ad->nn++] = strdup(e->d_name);
    }
    closedir(d);
    if (getenv("SUP_ASSET_TRACE"))
        fprintf(stderr, "AAD> %s -> %zu [%s]\n", dir ? dir : "",
                ad->nn, path);
    return ad;
}

const char *AAssetDir_getNextFileName(AAssetDir *dir) {
    if (!dir || dir->idx >= dir->nn) return NULL;
    return dir->names[dir->idx++];
}
void AAssetDir_close(AAssetDir *dir) {
    if (!dir) return;
    for (size_t i = 0; i < dir->nn; ++i) free(dir->names[i]);
    free(dir->names);
    free(dir);
}
void AAsset_close(AAsset *a) {
    if (!a) return;
    if (a->buf && a->len) munmap(a->buf, a->len);
    free(a);
}
off64_t AAsset_getLength(AAsset *a) { return a ? (off64_t)a->len : -1; }
off64_t AAsset_getLength64(AAsset *a) { return a ? (off64_t)a->len : -1; }
ssize_t AAsset_read(AAsset *a, void *buf, size_t count) {
    if (!a) return -1;
    if (a->pos >= a->len) return 0;
    size_t n = a->len - a->pos;
    if (n > count) n = count;
    memcpy(buf, a->buf + a->pos, n);
    a->pos += n;
    return (ssize_t)n;
}
off64_t AAsset_seek64(AAsset *a, off64_t offset, int origin) {
    if (!a) return -1;
    off64_t np;
    if (origin == SEEK_SET) np = offset;
    else if (origin == SEEK_CUR) np = a->pos + offset;
    else if (origin == SEEK_END) np = a->len + offset;
    else return -1;
    if (np < 0 || np > (off64_t)a->len) return -1;
    a->pos = (size_t)np;
    return np;
}

AConfiguration *AConfiguration_new(void) { return (AConfiguration *)calloc(1, 8); }
void AConfiguration_delete(AConfiguration *c) { free(c); }
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *m) { (void)m; }
const char *AConfiguration_getLanguage(AConfiguration *c) {
    (void)c;
    const char *v = getenv("SUP_LANG");
    return v && *v ? v : "en";
}
const char *AConfiguration_getCountry(AConfiguration *c) {
    (void)c;
    const char *v = getenv("SUP_COUNTRY");
    return v && *v ? v : "US";
}

/* ==================== ввод: AInputQueue + X11 ========================
 *
 * Игра идёт классическим NativeActivity-путём: framework создаёт очередь,
 * зовёт callbacks->onInputQueueCreated(act, queue); glue-слой игры пишет
 * APP_CMD_INPUT_CHANGED в свой командный pipe, главный цикл его читает и
 * делает AInputQueue_attachLooper(queue, app->looper, LOOPER_ID_INPUT,
 * NULL, NULL). Дальше цикл сам зовёт AInputQueue_getEvent.
 *
 * Очередь здесь настоящая: кольцо событий + pipe-«будильник» (его поллит
 * ALooper_pollAll). Источник событий — X11: мышь отдаём как touch
 * (DOWN/MOVE/UP — это и «тап» по титульнику, и прицел), клавиатуру — как
 * KEY-события с Android keycode.
 */
#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AINPUT_SOURCE_KEYBOARD 0x00000101
#define AINPUT_SOURCE_TOUCHSCREEN 0x00001002
#define AINPUT_ACTION_DOWN 0
#define AINPUT_ACTION_UP 1
#define AINPUT_ACTION_MOVE 2
#define AINPUT_ACTION_CANCEL 3
#define AINPUT_MAX_PTR 10
#define AINPUT_QCAP 256

struct AInputEvent {
    int32_t type;
    int32_t source;
    int32_t deviceId;
    int32_t action;
    int32_t flags;
    int32_t keycode;
    int32_t metastate;
    int32_t repeatCount;
    int32_t buttonState;
    int32_t pointerCount;
    int32_t ids[AINPUT_MAX_PTR];
    float xs[AINPUT_MAX_PTR];
    float ys[AINPUT_MAX_PTR];
};

struct AInputQueue {
    pthread_mutex_t lock;
    AInputEvent *ev[AINPUT_QCAP];
    int head, tail, count;
    int fd_r, fd_w;
    ALooper *looper;
    int ident;
    ALooper_callbackFunc cb;
    void *cbdata;
};

static AInputQueue g_input_q;
static int g_input_q_ready;

/* определены ниже, в секции ALooper */
int ALooper_addFd(ALooper *l, int fd, int ident, int events,
                  ALooper_callbackFunc cb, void *data);
int ALooper_removeFd(ALooper *l, int fd);

static void q_drain(AInputQueue *q) {
    char b[64];
    while (read(q->fd_r, b, sizeof b) > 0) {}
}

AInputQueue *sup_input_queue_new(void) {
    AInputQueue *q = &g_input_q;
    if (g_input_q_ready) return q;
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        fprintf(stderr, "XIN> pipe2 не удался: %s\n", strerror(errno));
        return NULL;
    }
    q->fd_r = fds[0];
    q->fd_w = fds[1];
    g_input_q_ready = 1;
    fprintf(stderr, "XIN> очередь ввода создана (pipe %d/%d)\n", q->fd_r, q->fd_w);
    return q;
}

static int in_trace(void) {
    static int v = -1;
    if (v < 0) v = getenv("SUP_INPUT_TRACE") ? 1 : 0;
    return v;
}

static void q_push(AInputQueue *q, AInputEvent *e) {
    if (!q || !g_input_q_ready) return;
    if (in_trace() && e)
        fprintf(stderr, "XIN> push %s %s x=%.0f y=%.0f key=%d\n",
                e->type == AINPUT_EVENT_TYPE_KEY ? "key" : "touch",
                e->action == AINPUT_ACTION_DOWN ? "down" :
                e->action == AINPUT_ACTION_UP ? "up" : "move",
                e->pointerCount ? e->xs[0] : 0.0f,
                e->pointerCount ? e->ys[0] : 0.0f, e->keycode);
    pthread_mutex_lock(&q->lock);
    if (q->count == AINPUT_QCAP) {
        AInputEvent *old = q->ev[q->head];
        q->ev[q->head] = NULL;
        q->head = (q->head + 1) % AINPUT_QCAP;
        --q->count;
        free(old);
    }
    q->ev[q->tail] = e;
    q->tail = (q->tail + 1) % AINPUT_QCAP;
    ++q->count;
    pthread_mutex_unlock(&q->lock);
    ssize_t w = write(q->fd_w, "i", 1);
    (void)w;
}

static AInputEvent *ev_new(int type, int source, int dev) {
    AInputEvent *e = (AInputEvent *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->type = type;
    e->source = source;
    e->deviceId = dev;
    return e;
}

void AInputQueue_attachLooper(AInputQueue *q, ALooper *looper, int ident,
                              ALooper_callbackFunc cb, void *data) {
    if (!q) return;
    q->looper = looper;
    q->ident = ident;
    q->cb = cb;
    q->cbdata = data;
    ALooper_addFd(looper, q->fd_r, ident, 1 /* ALOOPER_EVENT_INPUT */, cb, data);
    fprintf(stderr, "XIN> attachLooper(fd=%d, ident=%d, cb=%p)\n",
            q->fd_r, ident, (void *)cb);
}

void AInputQueue_detachLooper(AInputQueue *q) {
    if (!q || !q->looper) return;
    ALooper_removeFd(q->looper, q->fd_r);
    q->looper = NULL;
}

int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **event) {
    if (event) *event = NULL;
    if (!q || !g_input_q_ready) return -1;
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        q_drain(q);
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    AInputEvent *e = q->ev[q->head];
    q->ev[q->head] = NULL;
    q->head = (q->head + 1) % AINPUT_QCAP;
    --q->count;
    if (q->count == 0) q_drain(q);
    pthread_mutex_unlock(&q->lock);
    if (event) *event = e;
    if (in_trace() && e)
        fprintf(stderr, "XIN> pop %s %s x=%.0f y=%.0f key=%d\n",
                e->type == AINPUT_EVENT_TYPE_KEY ? "key" : "touch",
                e->action == AINPUT_ACTION_DOWN ? "down" :
                e->action == AINPUT_ACTION_UP ? "up" : "move",
                e->pointerCount ? e->xs[0] : 0.0f,
                e->pointerCount ? e->ys[0] : 0.0f, e->keycode);
    return 0;
}

int32_t AInputQueue_hasEvents(AInputQueue *q) {
    if (!q || !g_input_q_ready) return 0;
    pthread_mutex_lock(&q->lock);
    int n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n ? 1 : 0;
}

void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *event, int handled) {
    (void)q;
    (void)handled;
    free(event);
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *event) {
    (void)q;
    (void)event;
    return 0;
}

int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e ? e->deviceId : 0; }
int32_t AInputEvent_getSource(const AInputEvent *e) { return e ? e->source : 0; }
int32_t AInputEvent_getType(const AInputEvent *e) { return e ? e->type : 0; }

int32_t AKeyEvent_getAction(const AInputEvent *e) { return e ? e->action : 0; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e ? e->keycode : 0; }
uint32_t AKeyEvent_getMetaState(const AInputEvent *e) { return e ? (uint32_t)e->metastate : 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e ? e->repeatCount : 0; }

int32_t AMotionEvent_getAction(const AInputEvent *e) { return e ? e->action : 0; }
float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t pi) {
    if (!e || pi >= (size_t)e->pointerCount) return 0.0f;
    if (axis == 0) return e->xs[pi];
    if (axis == 1) return e->ys[pi];
    return 0.0f;
}
int32_t AMotionEvent_getButtonState(const AInputEvent *e) { return e ? e->buttonState : 0; }
float AMotionEvent_getHistoricalX(const AInputEvent *e, size_t pi, size_t h) {
    (void)h;
    return (e && pi < (size_t)e->pointerCount) ? e->xs[pi] : 0.0f;
}
float AMotionEvent_getHistoricalY(const AInputEvent *e, size_t pi, size_t h) {
    (void)h;
    return (e && pi < (size_t)e->pointerCount) ? e->ys[pi] : 0.0f;
}
size_t AMotionEvent_getHistorySize(const AInputEvent *e) { (void)e; return 0; }
size_t AMotionEvent_getPointerCount(const AInputEvent *e) {
    return e ? (size_t)e->pointerCount : 0;
}
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t pi) {
    return (e && pi < (size_t)e->pointerCount) ? e->ids[pi] : 0;
}
float AMotionEvent_getRawX(const AInputEvent *e, size_t pi) {
    return (e && pi < (size_t)e->pointerCount) ? e->xs[pi] : 0.0f;
}
float AMotionEvent_getRawY(const AInputEvent *e, size_t pi) {
    return (e && pi < (size_t)e->pointerCount) ? e->ys[pi] : 0.0f;
}
float AMotionEvent_getX(const AInputEvent *e, size_t pi) {
    return (e && pi < (size_t)e->pointerCount) ? e->xs[pi] : 0.0f;
}
float AMotionEvent_getY(const AInputEvent *e, size_t pi) {
    return (e && pi < (size_t)e->pointerCount) ? e->ys[pi] : 0.0f;
}

/* ---- публичные «кормилки» для X11-потока ---- */
void sup_input_push_touch(float x, float y, int action) {
    AInputEvent *e = ev_new(AINPUT_EVENT_TYPE_MOTION, AINPUT_SOURCE_TOUCHSCREEN, 2);
    if (!e) return;
    e->action = action;
    e->buttonState = (action == AINPUT_ACTION_UP) ? 0 : 0x1;
    e->pointerCount = 1;
    e->ids[0] = 0;
    e->xs[0] = x;
    e->ys[0] = y;
    q_push(&g_input_q, e);
}

void sup_input_push_key(int32_t keycode, int action) {
    AInputEvent *e = ev_new(AINPUT_EVENT_TYPE_KEY, AINPUT_SOURCE_KEYBOARD, 1);
    if (!e) return;
    e->action = action;
    e->keycode = keycode;
    q_push(&g_input_q, e);
}

/* ---- X11-поток: читаем события окна и переводим в Android-формат ----
 * XEvent в x86_64 — 192 байта; берём с запасом и читаем поля по
 * известной раскладке XKeyEvent/XButtonEvent/XMotionEvent (до keycode/
 * same_screen у них общий префикс). */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window, root, subwindow;
    unsigned long time;
    int x, y, x_root, y_root;
    unsigned int state;
    unsigned int detail; /* keycode / button / is_hint */
    int same_screen;
} SupXEv;

static int g_x_down;
static unsigned long g_x_win;

static int keysym_to_android(unsigned long ks) {
    if (ks >= 'a' && ks <= 'z') return 29 + (int)(ks - 'a');
    if (ks >= 'A' && ks <= 'Z') return 29 + (int)(ks - 'A');
    if (ks >= '0' && ks <= '9') return 7 + (int)(ks - '0');
    if (ks >= 0xffbe && ks <= 0xffc9) return 131 + (int)(ks - 0xffbe); /* F1..F12 */
    switch (ks) {
        case 0xff08: return 67;  /* BackSpace -> DEL */
        case 0xff0d: return 66;  /* Return */
        case 0xff1b: return 111; /* Escape */
        case 0xff09: return 61;  /* Tab */
        case 0x20:   return 62;  /* space */
        case 0xff51: return 21;  /* Left */
        case 0xff52: return 19;  /* Up */
        case 0xff53: return 22;  /* Right */
        case 0xff54: return 20;  /* Down */
        case 0xffe1: return 59;  /* Shift_L */
        case 0xffe2: return 60;  /* Shift_R */
        case 0xffe3: return 113; /* Control_L */
        case 0xffe4: return 114; /* Control_R */
        case 0xffe9: return 57;  /* Alt_L */
        case 0xffea: return 58;  /* Alt_R */
        case 0xffff: return 112; /* Delete */
        default: return 0;
    }
}

static void *x_input_thread(void *arg) {
    (void)arg;
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = RTLD_DEFAULT;
    /* Своё X-соединение: чужой Display*, открытый в loader'е, нельзя
     * дёргать из второго потока — Xlib рвёт соединение (проверено A/B).
     * Отдельный клиент спокойно делает XSelectInput на чужом окне. */
    void *(*XOpenDisplay)(const char *) = NULL;
    int (*XCloseDisplay)(void *) = NULL;
    *(void **)&XOpenDisplay = dlsym(h, "XOpenDisplay");
    *(void **)&XCloseDisplay = dlsym(h, "XCloseDisplay");
    int (*XSelectInput)(void *, unsigned long, long) = NULL;
    int (*XNextEvent)(void *, void *) = NULL;
    unsigned long (*XLookupKeysym)(void *, int) = NULL;
    int (*XSetInputFocus)(void *, unsigned long, int, unsigned long) = NULL;
    int (*XFlush)(void *) = NULL;
    *(void **)&XSelectInput = dlsym(h, "XSelectInput");
    *(void **)&XNextEvent = dlsym(h, "XNextEvent");
    *(void **)&XLookupKeysym = dlsym(h, "XLookupKeysym");
    *(void **)&XSetInputFocus = dlsym(h, "XSetInputFocus");
    *(void **)&XFlush = dlsym(h, "XFlush");
    if (!XSelectInput || !XNextEvent || !XOpenDisplay) {
        fprintf(stderr, "XIN> X11-функции не найдены, ввод выключен\n");
        return NULL;
    }
    void *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XIN> XOpenDisplay(NULL) не удался, ввод выключен\n");
        return NULL;
    }
    fprintf(stderr, "XIN> своё X-соединение %p\n", dpy);
    /* KeyPress 1, KeyRelease 2, ButtonPress 4, ButtonRelease 8,
     * PointerMotion 64, Exposure 0x8000, StructureNotify 0x20000 */
    XSelectInput(dpy, g_x_win,
                 (1L << 0) | (1L << 1) | (1L << 2) | (1L << 3) |
                 (1L << 6) | (1L << 17));
    if (XSetInputFocus) XSetInputFocus(dpy, g_x_win, 2 /*RevertToParent*/, 0);
    if (XFlush) XFlush(dpy);
    fprintf(stderr, "XIN> поток ввода активен, окно %#lx\n", g_x_win);
    fflush(stderr);
    for (;;) {
        unsigned char buf[256];
        XNextEvent(dpy, buf);
        SupXEv *e = (SupXEv *)buf;
        switch (e->type) {
            case 2: /* KeyRelease */
            case 3: /* KeyPress */
            {
                if (!XLookupKeysym) break;
                unsigned long ks = XLookupKeysym(buf, 0);
                int kc = keysym_to_android(ks);
                if (kc) sup_input_push_key(kc, e->type == 3 ? AINPUT_ACTION_DOWN
                                                            : AINPUT_ACTION_UP);
                break;
            }
            case 4: /* ButtonPress */
                if (e->detail == 1) {
                    g_x_down = 1;
                    sup_input_push_touch((float)e->x, (float)e->y, AINPUT_ACTION_DOWN);
                } else if (e->detail == 3) { /* правая кнопка = BACK */
                    sup_input_push_key(4 /* AKEYCODE_BACK */, AINPUT_ACTION_DOWN);
                    sup_input_push_key(4, AINPUT_ACTION_UP);
                }
                break;
            case 5: /* ButtonRelease */
                if (e->detail == 1 && g_x_down) {
                    g_x_down = 0;
                    sup_input_push_touch((float)e->x, (float)e->y, AINPUT_ACTION_UP);
                }
                break;
            case 6: /* MotionNotify */
                if (g_x_down)
                    sup_input_push_touch((float)e->x, (float)e->y, AINPUT_ACTION_MOVE);
                break;
            case 8: /* MapNotify */
            case 9: /* UnmapNotify */
            default:
                break;
        }
    }
    return NULL;
}

void sup_input_start_x11(void *dpy, unsigned long win) {
    static int started;
    if (started) return;
    if (getenv("SUP_NO_PUMP")) {  /* A/B: очередь есть, X-потока нет */
        fprintf(stderr, "XIN> X11-поток выключен (SUP_NO_PUMP)\n");
        return;
    }
    g_x_win = win;
    pthread_t th;
    if (pthread_create(&th, NULL, x_input_thread, NULL) == 0) {
        pthread_detach(th);
        started = 1;
    } else {
        fprintf(stderr, "XIN> pthread_create не удался\n");
    }
}

typedef struct alooper_fd {
    int fd;
    int ident;
    int events;
    ALooper_callbackFunc cb;
    void *data;
} alooper_fd;

static alooper_fd g_afd[16];
static int g_nafd;
static pthread_mutex_t g_afd_lock = PTHREAD_MUTEX_INITIALIZER;

ALooper *ALooper_prepare(int opts) { (void)opts; return (ALooper *)1; }

int ALooper_addFd(ALooper *l, int fd, int ident, int events,
                  ALooper_callbackFunc cb, void *data) {
    (void)l;
    pthread_mutex_lock(&g_afd_lock);
    for (int i = 0; i < g_nafd; ++i) {
        if (g_afd[i].fd == fd) { /* повторная регистрация того же fd */
            g_afd[i].ident = ident;
            g_afd[i].events = events;
            g_afd[i].cb = cb;
            g_afd[i].data = data;
            pthread_mutex_unlock(&g_afd_lock);
            return 1;
        }
    }
    if (g_nafd < 16) {
        g_afd[g_nafd].fd = fd;
        g_afd[g_nafd].ident = ident;
        g_afd[g_nafd].events = events;
        g_afd[g_nafd].cb = cb;
        g_afd[g_nafd].data = data;
        ++g_nafd;
    }
    pthread_mutex_unlock(&g_afd_lock);
    if (getenv("SUP_INPUT_TRACE"))
        fprintf(stderr, "LUP> addFd fd=%d ident=%d cb=%p data=%p\n",
                fd, ident, (void *)cb, data);
    return 1;
}

int ALooper_removeFd(ALooper *l, int fd) {
    (void)l;
    pthread_mutex_lock(&g_afd_lock);
    for (int i = 0; i < g_nafd; ++i) {
        if (g_afd[i].fd == fd) {
            g_afd[i] = g_afd[g_nafd - 1];
            --g_nafd;
            break;
        }
    }
    pthread_mutex_unlock(&g_afd_lock);
    return 1;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        alooper_fd snap[16];
        int used = 0;
        pthread_mutex_lock(&g_afd_lock);
        for (int i = 0; i < g_nafd && used < 16; ++i) snap[used++] = g_afd[i];
        pthread_mutex_unlock(&g_afd_lock);

        /* Нет fd: проспать таймаут и вернуть ALOOPER_POLL_TIMEOUT (-3),
         * а не -1 (ALOOPER_POLL_WAKE) — иначе главный цикл движка
         * крутится вхолостую без рендера. */
        if (!used) {
            int t = (timeoutMillis < 0) ? 50 : timeoutMillis;
            if (t > 0) usleep((useconds_t)t * 1000);
            return -3;
        }

        int cap = (timeoutMillis < 0) ? 50 : timeoutMillis;
        if (timeoutMillis >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long el = (now.tv_sec - t0.tv_sec) * 1000L +
                      (now.tv_nsec - t0.tv_nsec) / 1000000L;
            cap = (int)(timeoutMillis - el);
            if (cap < 0) cap = 0;
        }

        struct pollfd pfds[16];
        for (int i = 0; i < used; ++i) {
            pfds[i].fd = snap[i].fd;
            pfds[i].events = (short)(snap[i].events & 3);
            pfds[i].revents = 0;
        }
        int r = poll(pfds, used, cap);
        if (r <= 0) return r == 0 ? -3 : -1;

        int cb_fired = 0;
        for (int i = 0; i < used; ++i) {
            if (!pfds[i].revents) continue;
            int rv = (pfds[i].revents & POLLIN) ? 1 : 0;
            if (snap[i].cb) { /* как настоящий pollAll: колбэк и ещё круг */
                snap[i].cb(snap[i].fd, rv, snap[i].data);
                cb_fired = 1;
                continue;
            }
            if (outFd) *outFd = snap[i].fd;
            if (outEvents) *outEvents = rv;
            if (outData) *outData = snap[i].data;
            if (getenv("SUP_INPUT_TRACE"))
                fprintf(stderr, "LUP> pollAll -> ident=%d fd=%d revents=%#x\n",
                        snap[i].ident, snap[i].fd, pfds[i].revents);
            return snap[i].ident;
        }
        if (!cb_fired) return -1;
        /* были только колбэки — pollAll по контракту повторяет опрос */
    }
}

void ANativeActivity_finish(ANativeActivity *a) {}
static int g_win_w = 0, g_win_h = 0;
static void win_size(void) {
    if (g_win_w) return;
    const char *w = getenv("SUP_WIDTH"), *h = getenv("SUP_HEIGHT");
    g_win_w = w && *w ? atoi(w) : 1280;
    g_win_h = h && *h ? atoi(h) : 720;
}
int32_t ANativeWindow_getHeight(ANativeWindow *w) {
    (void)w; win_size();
    fprintf(stderr, "ANW> getHeight -> %d\n", g_win_h);
    fflush(stderr);
    return g_win_h;
}
int32_t ANativeWindow_getWidth(ANativeWindow *w) {
    (void)w; win_size();
    fprintf(stderr, "ANW> getWidth -> %d\n", g_win_w);
    fflush(stderr);
    return g_win_w;
}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height,
                                         int32_t format) {
    fprintf(stderr, "ANW> setBuffersGeometry(w=%p, %d, %d, fmt=%d)\n",
            w, width, height, format);
    fflush(stderr);
    if (width > 0) g_win_w = width;
    if (height > 0) g_win_h = height;
    return 0;
}
int32_t ANativeWindow_lock(ANativeWindow *w, void *out, void *rect) {
    fprintf(stderr, "ANW> lock(w=%p, out=%p, rect=%p)\n", w, out, rect);
    fflush(stderr);
    if (out) memset(out, 0, 0x30);
    return 0;
}
void ANativeWindow_release(ANativeWindow *w) {
    fprintf(stderr, "ANW> release(w=%p)\n", w);
    fflush(stderr);
}
void *ANativeWindow_getBuffer(ANativeWindow *w) { (void)w; return NULL; }
int32_t ANativeWindow_queueBuffer(ANativeWindow *w, void *buf) { (void)w; (void)buf; return 0; }

size_t __strlen_chk(const char *s) { return strlen(s); }
char *__strchr_chk(const char *s, int c) { return (char *)strchr(s, c); }
char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) { return strncpy(dst, src, n); }
char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len) { return strncpy(dst, src, n); }
ssize_t __write_chk(int fd, const void *buf, size_t n, size_t buf_len) { return write(fd, buf, n); }

void __FD_CLR_chk(int fd, fd_set *set, size_t fdset_size) { (void)fdset_size; FD_CLR(fd, set); }
int __FD_ISSET_chk(int fd, const fd_set *set, size_t fdset_size) { (void)fdset_size; return FD_ISSET(fd, set); }
void __FD_SET_chk(int fd, fd_set *set, size_t fdset_size) { (void)fdset_size; FD_SET(fd, set); }

int *__errno(void) { return &errno; }

struct __pthread_cleanup_handler {
    struct __pthread_cleanup_handler *__next;
    void (*__routine)(void *);
    void *__arg;
};

static __thread struct __pthread_cleanup_handler *cleanup_handlers;

void __pthread_cleanup_push(struct __pthread_cleanup_handler *handler,
                            void (*routine)(void *), void *arg) {
    handler->__routine = routine;
    handler->__arg = arg;
    handler->__next = cleanup_handlers;
    cleanup_handlers = handler;
}

void __pthread_cleanup_pop(struct __pthread_cleanup_handler *handler, int execute) {
    if (cleanup_handlers == handler) cleanup_handlers = handler->__next;
    if (execute && handler->__routine) handler->__routine(handler->__arg);
}

typedef unsigned int EGLBoolean;
EGLBoolean eglWaitClient(void) { return 1; }

extern unsigned char _ctype_[512];

unsigned char _ctype_[512];

static void ctype_init(void) __attribute__((constructor));
static void ctype_init(void) {
    int c;
    for (c = 0; c < 256; ++c) {
        int mark = 0;
        if (c >= '0' && c <= '9') mark |= 0x04;
        if (c >= 'a' && c <= 'f') mark |= 0x40;
        if (c >= 'A' && c <= 'F') mark |= 0x40;
        if (c >= 'a' && c <= 'z') mark |= 0x02;
        if (c >= 'A' && c <= 'Z') mark |= 0x01;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r')
            mark |= 0x08;
        if (c == ' ' || c == '\t') mark |= 0x80;
        if (c > 0x20 && c < 0x7f) mark |= 0x10;
        if (c < 0x20 || c == 0x7f) mark |= 0x20;
        _ctype_[c + 1] = (unsigned char)mark;
    }
}

/* Bionic indirection for _ctype_.
 *
 * The compiled game reads it with TWO dereferences, verified by objdump at
 * .text+0xf94db0e:
 *     mov    0x8d0dc3(%rip),%rdx        ; rdx = GOT slot -> must be a POINTER var
 *     mov    (%rdx),%rdx                ; rdx = that pointer -> the table base
 *     movzbl 0x1(%rcx,%rdx,1),%ecx      ; table[c+1]  (== our _ctype_[c+1])
 *     and    $0x7,%cl
 *
 * Binding the GOT slot straight to &table makes the first deref read the class
 * bytes of row 0 (00 20 20 20 20 20 20 20), yielding 0x2020202020202000 which
 * is then used as an address -> SEGV. So the slot must hold the ADDRESS OF A
 * POINTER whose value is the table base. Base = &_ctype_[0] (not +1) so that
 * the indexed byte lands exactly on _ctype_[c+1]. */
static unsigned char *g_ctype_b_ptr = _ctype_;

unsigned char **sup_ctype_b_ptr_pub(void) { return &g_ctype_b_ptr; }

/* Реестр исполняемых диапазонов для валидации callback-указателей, которые
 * движок передаёт в JNI (например runNativeCallbackOnUiThread(long)).
 *
 * Живёт здесь, а не в loader.c, потому что libsupcompat.so собирается из
 * compat.c ndk.c jni_runtime.c gl_bridge.S gl_slots.c — loader.c в неё НЕ
 * входит. Если объявить extern-функцию в jni_runtime.c, а определить в
 * loader.c, то символ останется неопределённым, и dlopen(..., RTLD_NOW)
 * для всей .so провалится: «undefined symbol», библиотека не загрузится,
 * и все хуки (EGL/GL/_ctype_) перестанут резолвиться. Диапазоны сюда
 * заливает loader.c через dlsym("sup_exec_add"). */
#define SUP_EXEC_MAX 32
static uintptr_t sup_exec_lo[SUP_EXEC_MAX];
static uintptr_t sup_exec_hi[SUP_EXEC_MAX];
static size_t sup_exec_n;

void sup_exec_add(uintptr_t lo, uintptr_t hi) {
    if (!lo || lo >= hi || sup_exec_n >= SUP_EXEC_MAX) return;
    sup_exec_lo[sup_exec_n] = lo;
    sup_exec_hi[sup_exec_n] = hi;
    ++sup_exec_n;
}

int sup_ptr_in_exec(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!v) return 0;
    for (size_t i = 0; i < sup_exec_n; ++i)
        if (v >= sup_exec_lo[i] && v < sup_exec_hi[i]) return 1;
    return 0;
}

/* Bionic: extern struct __sFILE __sF[3]; with sizeof(struct __sFILE) == 0x98 (LP64),
 * so libc++/libminecraftpe compute stderr as __sF + 2*0x98 = __sF + 0x130 and use
 * that ADDRESS as the FILE*. A FILE*[3] here pointed the consumer 0x130 bytes past
 * a 24-byte array: every fatal diagnostic then died in glibc's _IO_vtable_check
 * with "glibc detected an invalid stdio handle". glibc's FILE is 0xE0 bytes, so
 * three independent copies cannot fit at 0x98 stride; copy real stderr into all
 * three slots (slot 2 written last) so each slot is a complete, valid handle. */
#define BIONIC_SFILE_SIZE 0x98u
#define BIONIC_STDERR_OFF (2u * BIONIC_SFILE_SIZE)

unsigned char __sF[BIONIC_STDERR_OFF + 0x100u] __attribute__((aligned(16)));

static void sF_init(void) __attribute__((constructor));
static void sF_init(void) {
    for (size_t i = 0; i < 3; ++i)
        memcpy(__sF + i * BIONIC_SFILE_SIZE, stderr, sizeof(FILE));
}