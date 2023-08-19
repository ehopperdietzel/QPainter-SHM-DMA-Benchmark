#include <fcntl.h>
#include <wayland-client.h>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>

#include <unistd.h>
#include <sys/mman.h>
#include <gbm.h>
#include <xf86drm.h>
#include <sys/ioctl.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1.h"
#include "wl_drm.h"

#include "shm.h"

static wl_display *display = NULL;

// Globals
static wl_shm *shm = NULL;
static wl_compositor *compositor = NULL;
static xdg_wm_base *wm_base = NULL;
static zwp_linux_dmabuf_v1 *linux_dmabuf = NULL;
static wl_drm *drm = NULL;

// DMA stuff
static struct DMA
{
    int drm;
    gbm_device *gbm;
    bool argb32Supported = false;
    bool linearModSupported = false;
    bool drmAuthenticated = false;
} dma;

// Buffers
static int width, height;
static int bufferScale = 1;

struct Buffer
{
    int i;
    int fd;
    int width;
    int height;
    uint stride;
    int mapSize;
    uchar *pixels;
    wl_buffer *buffer;
    bool realeased = true;
    bool commited = true;
    bool callbacked = true;

    bool rendered = false;
};

struct DMABuffer
{
    Buffer buffer;
    dma_buf_sync sync;
    gbm_bo *bo = NULL;
    uchar *map = NULL;
    void **gbmMap = NULL;
};

#define BUFFS 3

static Buffer *shmBuffers[BUFFS];
static Buffer *dmaBuffers[BUFFS];

// Toplevel
struct Toplevel
{
    wl_surface *surface = NULL;
    xdg_surface *xdgSurface = NULL;
    xdg_toplevel *xdgToplevel = NULL;
    bool pendingCallback = false;
    Buffer **buffers;
    bool configured = false;
    int i = 0;
    int s = 0;
};

static Toplevel *toplevel = NULL;

// Measure
static bool testingDMA = false;
static bool benchFinished = false;
int renderedFrames = 0;
struct timespec renderStart, renderEnd;

static void wl_buffer_handle_release(void *, wl_buffer *buff);

struct wl_buffer_listener buffer_listener =
{
    .release = &wl_buffer_handle_release
};

static Buffer *create_shm_buffer(int w, int h)
{
    Buffer *buffer = new Buffer();

    buffer->width = w;
    buffer->height = h;
    buffer->stride = w * 4;

    buffer->mapSize = buffer->stride * h;

    buffer->fd = create_shm_file(buffer->stride * buffer->height);

    if (buffer->fd < 0)
    {
        qFatal() << "Failed to create SHM buffer";
        exit(EXIT_FAILURE);
    }

    buffer->pixels = (uint8_t*)mmap(NULL, buffer->mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);

    if (buffer->pixels == MAP_FAILED)
    {
        qFatal() << "Failed to mmap SHM buffer";
        close(buffer->fd);
        exit(EXIT_FAILURE);
    }

    wl_shm_pool *pool = wl_shm_create_pool(shm, buffer->fd, buffer->mapSize);
    buffer->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, buffer->stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_set_user_data(buffer->buffer, buffer);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
    wl_shm_pool_destroy(pool);

    return buffer;
}

static int get_bo_fd(gbm_bo *bo)
{
    struct drm_prime_handle prime_handle;
    memset(&prime_handle, 0, sizeof(prime_handle));
    prime_handle.handle = gbm_bo_get_handle(bo).u32;
    prime_handle.flags = DRM_CLOEXEC | DRM_RDWR;
    prime_handle.fd = -1;

    if (ioctl(dma.drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) != 0)
        goto fail;

    if (prime_handle.fd < 0)
        goto fail;

    // Set read and write permissions on the file descriptor
    if (fcntl(prime_handle.fd, F_SETFL, fcntl(prime_handle.fd, F_GETFL) | O_RDWR) == -1)
    {
        close(prime_handle.fd);
        goto fail;
    }

    return prime_handle.fd;

fail:

    prime_handle.fd = gbm_bo_get_fd(bo);

    if (prime_handle.fd >= 0)
        return prime_handle.fd;

    return -1;
}

static Buffer *create_dma_buffer(int w, int h)
{
    DMABuffer *buffer = new DMABuffer();

    buffer->buffer.width = w;
    buffer->buffer.height = h;

    buffer->bo = gbm_bo_create(dma.gbm, w, h, WL_DRM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);

    if (!buffer->bo)
    {
        qFatal() << "Failed to create GBM bo";
        exit(1);
    }

    // Get FD
    buffer->buffer.fd = get_bo_fd(buffer->bo);

    if (buffer->buffer.fd == -1)
    {
        qFatal() << "Failed to get GBM bo fd";
        exit(1);
    }

    buffer->buffer.stride = gbm_bo_get_stride(buffer->bo);

    buffer->buffer.mapSize = height * buffer->buffer.stride;

    // Map
    buffer->map = (uchar*)mmap(NULL, buffer->buffer.mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->buffer.fd, 0);

    if (buffer->map == MAP_FAILED)
    {
        buffer->map = (uchar*)mmap(NULL, buffer->buffer.mapSize, PROT_WRITE, MAP_SHARED,  buffer->buffer.fd, 0);

        if (buffer->map == MAP_FAILED)
        {
            buffer->map = (uchar*)gbm_bo_map(buffer->bo, 0, 0, width, height, GBM_BO_TRANSFER_READ, &buffer->buffer.stride, buffer->gbmMap);
        }
    }

    if (buffer->map == NULL || buffer->map == MAP_FAILED)
    {
        qFatal() << "Failed to map GBM bo";
        exit(1);
    }

    buffer->buffer.pixels = &buffer->map[gbm_bo_get_offset(buffer->bo, 0)];

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(linux_dmabuf);
    zwp_linux_buffer_params_v1_add(params,
                                   buffer->buffer.fd,
                                   0,
                                   0,
                                   buffer->buffer.stride,
                                   DRM_FORMAT_MOD_LINEAR >> 32,
                                   DRM_FORMAT_MOD_LINEAR & 0xffffffff);

    buffer->buffer.buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h, DRM_FORMAT_ARGB8888, 0);
    wl_buffer_set_user_data(buffer->buffer.buffer, buffer);

    wl_buffer_add_listener(buffer->buffer.buffer, &buffer_listener, buffer);

    wl_display_roundtrip(display);

    return (Buffer*)buffer;
}

static void wl_drm_handle_authenticated(void *, wl_drm *)
{
    dma.drmAuthenticated = true;
    dma.gbm = gbm_create_device(dma.drm);

    if (!dma.gbm)
    {
        qFatal() << "Failed to create gbm device";
        exit(1);
    }

    drmVersionPtr version = drmGetVersion(dma.drm);

    if (version)
    {
        qDebug("Driver: %s %s (%d.%d.%d) - %s",
               version->name,
               version->desc,
               version->version_major,
               version->version_minor,
               version->version_patchlevel,
               version->date);

        drmFreeVersion(version);
    }
}

static void wl_drm_handle_device(void *, wl_drm *, const char *device)
{
    dma.drm = open(device, O_RDWR);

    if (dma.drm < 0)
    {
        qFatal() << "Failed to open DRM device" << device;
        exit(1);
    }

    wl_drm_handle_authenticated(NULL, NULL);
    return;

    drm_magic_t magic;

    int ret = drmGetMagic(dma.drm, &magic);

    if (ret == 0)
    {
        wl_drm_handle_authenticated(NULL, NULL);
        return;
    }
    else if (ret < 0)
    {
        qFatal() << "Failed to get DRM magic";
        exit(1);
    }

    wl_drm_authenticate(drm, magic);
}

static const wl_drm_listener drm_listener
{
    .device = &wl_drm_handle_device,
    .format = [](void*, wl_drm*, uint32_t){},
    .authenticated = &wl_drm_handle_authenticated,
    .capabilities = [](void*, wl_drm*, uint32_t){},
};

static void wm_base_handle_ping(void *, xdg_wm_base *wm, uint32_t serial)
{
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener =
{
    .ping = &wm_base_handle_ping
};

static void handle_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;

    if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 3);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        wm_base = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        linux_dmabuf = (zwp_linux_dmabuf_v1*)wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
    else if (strcmp(interface, wl_drm_interface.name) == 0)
    {
        drm = (wl_drm*)wl_registry_bind(registry, name, &wl_drm_interface, 1);
        wl_drm_add_listener(drm, &drm_listener, NULL);
    }
}

static const wl_registry_listener registry_listener =
{
    .global = &handle_global,
    .global_remove = NULL
};

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    toplevel->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener =
{
    .configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h, struct wl_array *states)
{
    (void)data;
    (void)xdg_toplevel;
    (void)states;
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    (void)data;
    (void)xdg_toplevel;
    exit(1);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener =
{
    .configure = &xdg_toplevel_handle_configure,
    .close = &xdg_toplevel_handle_close
};

static void createToplevel()
{
    toplevel = new Toplevel();
    toplevel->surface = wl_compositor_create_surface(compositor);

    toplevel->xdgSurface = xdg_wm_base_get_xdg_surface(wm_base, toplevel->surface);
    xdg_surface_add_listener(toplevel->xdgSurface, &xdg_surface_listener, toplevel);

    toplevel->xdgToplevel = xdg_surface_get_toplevel(toplevel->xdgSurface);
    xdg_toplevel_add_listener(toplevel->xdgToplevel, &xdg_toplevel_listener, toplevel);

    wl_surface_set_buffer_scale(toplevel->surface, bufferScale);
    wl_surface_attach(toplevel->surface, NULL, 0, 0);
    wl_surface_commit(toplevel->surface);

    wl_display_roundtrip(display);

    while (!toplevel->configured)
        wl_display_roundtrip(display);

    toplevel->buffers = shmBuffers;

}

static void dmaWriteBegin(DMABuffer *buffer)
{
    buffer->sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    ioctl(buffer->buffer.fd, DMA_BUF_IOCTL_SYNC, &buffer->sync);
}

static void dmaWriteEnd(DMABuffer *buffer)
{
    buffer->sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(buffer->buffer.fd, DMA_BUF_IOCTL_SYNC, &buffer->sync);
}

// Client only tests

static void drawTest1(bool isDMA, Buffer *buffer, int slices)
{
    struct timespec start_time, end_time;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // BEGIN

    QImage img = QImage(buffer->pixels, buffer->width, buffer->height, QImage::Format_ARGB32);
    QPainter painter(&img);

    int loops = 10;

    QSize squareSize(img.width() / slices, img.height() / slices);

    painter.setPen(Qt::NoPen);

    if (isDMA)
        dmaWriteBegin((DMABuffer*)buffer);

    for (int i = 0; i < loops; i++)
    {
        for (int x = 0; x < slices; x++)
        {
            for (int y = 0; y < slices; y++)
            {
                painter.setBrush(QColor(x, y, x + y));
                painter.drawRect(x * squareSize.width(),
                                 y * squareSize.height(),
                                 squareSize.width(),
                                 squareSize.height());
            }
        }
    }
    painter.end();

    if (isDMA)
        dmaWriteEnd((DMABuffer*)buffer);

    // END

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL + (end_time.tv_nsec - start_time.tv_nsec);

    qDebug() << "drawTest1:" << slices * slices << "drawRect() opaque calls of " << squareSize << (isDMA ? "DMA" : "SHM") << ":" << elapsed_ns / loops << "nanoseconds";
}

static void drawTest2(bool isDMA, Buffer *buffer, int slices)
{
    struct timespec start_time, end_time;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // BEGIN

    QImage img = QImage(buffer->pixels, buffer->width, buffer->height, QImage::Format_ARGB32);
    QPainter painter(&img);

    int loops = 10;

    QSize squareSize(img.width() / slices, img.height() / slices);

    painter.setPen(Qt::NoPen);

    if (isDMA)
        dmaWriteBegin((DMABuffer*)buffer);

    for (int i = 0; i < loops; i++)
    {
        for (int x = 0; x < slices; x++)
        {
            for (int y = 0; y < slices; y++)
            {
                painter.setBrush(QColor(x, y, x + y, 50));
                painter.drawRect(x * squareSize.width(),
                                 y * squareSize.height(),
                                 squareSize.width(),
                                 squareSize.height());
            }
        }
    }
    painter.end();

    if (isDMA)
        dmaWriteEnd((DMABuffer*)buffer);

    // END

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL + (end_time.tv_nsec - start_time.tv_nsec);

    qDebug() << "drawTest2:" << slices * slices << "drawRect() translucent calls of " << squareSize << (isDMA ? "DMA" : "SHM") << ":" << elapsed_ns / loops << "nanoseconds";
}

static void drawTest3(bool isDMA, Buffer *buffer)
{
    struct timespec start_time, end_time;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // BEGIN

    QImage img = QImage(buffer->pixels, buffer->width, buffer->height, QImage::Format_ARGB32);
    QPainter painter(&img);

    int loops = 10;

    painter.setBrush(Qt::black);

    int col;

    if (isDMA)
        dmaWriteBegin((DMABuffer*)buffer);

    for (int i = 0; i < loops; i++)
    {
        for (int x = 0; x < img.width(); x++)
        {
            col = x % 255;
            painter.setPen(QColor(col, col, col));
            painter.drawLine(x, 0, 0, x);
        }
    }
    painter.end();

    if (isDMA)
        dmaWriteEnd((DMABuffer*)buffer);

    // END

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL + (end_time.tv_nsec - start_time.tv_nsec);

    qDebug() << "drawTest3:" << img.width() << "diagonal drawLine() opaque calls"  << (isDMA ? "DMA" : "SHM") << ":" << elapsed_ns / loops << "nanoseconds";
}

static void drawTest4(bool isDMA, Buffer *buffer)
{
    struct timespec start_time, end_time;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // BEGIN

    QImage img = QImage(buffer->pixels, buffer->width, buffer->height, QImage::Format_ARGB32);
    QPainter painter(&img);

    int loops = 10;

    painter.setBrush(QColor(50, 50, 50, 50));

    int col;

    if (isDMA)
        dmaWriteBegin((DMABuffer*)buffer);

    for (int i = 0; i < loops; i++)
    {
        for (int x = 0; x < img.width(); x++)
        {
            col = x % 255;
            painter.setPen(QColor(col, col, col, 50));
            painter.drawLine(x, 0, 0, x);
        }
    }
    painter.end();

    if (isDMA)
        dmaWriteEnd((DMABuffer*)buffer);

    // END

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL + (end_time.tv_nsec - start_time.tv_nsec);

    qDebug() << "drawTest4:" << img.width() << "diagonal drawLine() translucent calls"  << (isDMA ? "DMA" : "SHM") << ":" << elapsed_ns / loops << "nanoseconds";
}

// Client + compositor tests

static int next(int i, int max)
{
    if (i == max -1)
        return 0;

    return i+1;
}

static int prev(int i, int max)
{
    if (i == 0)
        return max - 1;

    return i-1;
}

static void renderTestDraw();

static void wl_callback_handle_done(void *, struct wl_callback *callback, uint32_t);

struct wl_callback_listener wl_callback_listener =
{
    .done = &wl_callback_handle_done
};

static unsigned long long nanos = 0;
int writes = 0;

static void wl_callback_handle_done(void *, struct wl_callback *callback, uint32_t)
{
    Buffer *buffer = (Buffer*)wl_callback_get_user_data(callback);
    wl_callback_destroy(callback);
    renderedFrames++;
    buffer->callbacked = true;
    toplevel->pendingCallback = false;

    clock_gettime(CLOCK_MONOTONIC, &renderEnd);
    long long elapsed_ns = (renderEnd.tv_sec - renderStart.tv_sec) * 1000000000LL + (renderEnd.tv_nsec - renderEnd.tv_nsec);

    if (elapsed_ns >= 1000000000LL * 10LL)
    {
        float secs = elapsed_ns / 1000000000LL;

        qDebug() << "- WRITES" << writes;
        qDebug() << "- SECS:" << secs;
        qDebug() << "- FRAMES:" << renderedFrames;
        qDebug() << "- FPS:" << float(renderedFrames) / secs;

        benchFinished = true;
        return;
    }

    if (!testingDMA && !toplevel->buffers[0]->commited)
    {
        Buffer *buffer = toplevel->buffers[0];
        toplevel->pendingCallback = true;
        wl_callback *callback = wl_surface_frame(toplevel->surface);
        wl_callback_add_listener(callback, &wl_callback_listener, buffer);
        wl_surface_attach(toplevel->surface, buffer->buffer, 0, 0);
        wl_surface_damage(toplevel->surface, 0, 0, buffer->width, buffer->height);
        wl_surface_commit(toplevel->surface);
        buffer->realeased = false;
        buffer->commited = true;
        buffer->callbacked = false;
        return;
    }

    renderTestDraw();
}

static void render(Buffer *buffer)
{
    struct timespec start_time, end_time;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (testingDMA)
        dmaWriteBegin((DMABuffer*)buffer);

    QImage img = QImage(buffer->pixels, buffer->width, buffer->height, QImage::Format_ARGB32);
    QPainter painter(&img);

    int slices = 100;

    QSize squareSize(img.width() / slices, img.height() / slices);

    painter.setPen(Qt::NoPen);

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setBrush(Qt::transparent);

    painter.drawRect(0, 0, img.width(), img.height());
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int x = 0; x < slices; x++)
    {
        for (int y = 0; y < slices; y++)
        {
            painter.setBrush(QColor(rand() % 255, rand() % 255, rand() % 255, 200));
            painter.drawRect(x * squareSize.width(),
                             y * squareSize.height(),
                             squareSize.width(),
                             squareSize.height());
        }
    }

    painter.end();

    if (testingDMA)
        dmaWriteEnd((DMABuffer*)buffer);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL + (end_time.tv_nsec - start_time.tv_nsec);

    nanos += elapsed_ns;
    writes++;
}

static void renderTestDraw()
{
    if (testingDMA)
    {
        render(toplevel->buffers[toplevel->i]);
        Buffer *buffer = toplevel->buffers[prev(toplevel->i, BUFFS)];
        toplevel->pendingCallback = true;
        wl_callback *callback = wl_surface_frame(toplevel->surface);
        wl_callback_add_listener(callback, &wl_callback_listener, buffer);
        wl_surface_attach(toplevel->surface, buffer->buffer, 0, 0);
        wl_surface_damage(toplevel->surface, 0, 0, buffer->width, buffer->height);
        wl_surface_commit(toplevel->surface);
        buffer->realeased = false;
        buffer->commited = true;
        buffer->callbacked = false;
        toplevel->i = next(toplevel->i, BUFFS);
        return;
    }

    Buffer *buffer = toplevel->buffers[0];

    if (buffer->realeased && buffer->commited)
    {
        render(buffer);
        buffer->commited = false;

        if (toplevel->pendingCallback)
            return;

        toplevel->pendingCallback = true;
        wl_callback *callback = wl_surface_frame(toplevel->surface);
        wl_callback_add_listener(callback, &wl_callback_listener, buffer);
        wl_surface_attach(toplevel->surface, buffer->buffer, 0, 0);
        wl_surface_damage(toplevel->surface, 0, 0, buffer->width, buffer->height);
        wl_surface_commit(toplevel->surface);
        buffer->realeased = false;
        buffer->commited = true;
        buffer->callbacked = false;
    }

/*
    // Check if there is a non commited buffer
    for (int i = 0; i < 1; i++)
    {
        if (!toplevel->pendingCallback && !toplevel->buffers[i]->commited)
        {
            qDebug() << "SEND ALREADY RENDERED BUFF" << i;

            toplevel->pendingCallback = true;
            wl_callback *callback = wl_surface_frame(toplevel->surface);
            wl_callback_add_listener(callback, &wl_callback_listener,  toplevel->buffers[i]);
            wl_surface_attach(toplevel->surface, toplevel->buffers[i]->buffer, 0, 0);
            wl_surface_damage(toplevel->surface, 0, 0, toplevel->buffers[i]->width, toplevel->buffers[i]->height);
            wl_surface_commit(toplevel->surface);
            toplevel->buffers[i]->realeased = false;
            toplevel->buffers[i]->commited = true;
            toplevel->buffers[i]->callbacked = false;
            break;
        }
    }

    Buffer *buffer = nullptr;
    int index = -1;

    // Find a free buffer
    for (int i = 0; i < 1; i++)
    {
        if (toplevel->buffers[i]->commited && toplevel->buffers[i]->callbacked && toplevel->buffers[i]->realeased)
        {
            index = i;
            buffer = toplevel->buffers[i];
            break;
        }
    }

    // If no buffer free buffer, wait for frame callback or buffer release
    if (!buffer)
        return;

    qDebug() << "RENDER" << index;



    buffer->commited = false;

    if (toplevel->pendingCallback)
    {
        renderTestDraw();
        return;
    }

    qDebug() << "SEND BUFF" << index;

    toplevel->pendingCallback = true;
    wl_callback *callback = wl_surface_frame(toplevel->surface);
    wl_callback_add_listener(callback, &wl_callback_listener, buffer);
    wl_surface_attach(toplevel->surface, buffer->buffer, 0, 0);
    wl_surface_damage(toplevel->surface, 0, 0, buffer->width, buffer->height);
    wl_surface_commit(toplevel->surface);
    buffer->realeased = false;
    buffer->commited = true;
    buffer->callbacked = false;

    // If DMA, try to render next frame immediatly
    if (testingDMA)
        renderTestDraw();
*/
}

static void wl_buffer_handle_release(void *, wl_buffer *buff)
{
    Buffer *buffer = (Buffer*)wl_buffer_get_user_data(buff);
    buffer->realeased = true;

    if (!testingDMA)
        renderTestDraw();
}

static void renderTestSHMBegin()
{
    qDebug() << "SHM Rendering Test:";
    writes = 0;
    testingDMA = false;
    renderedFrames = 0;
    nanos = 0;
    toplevel->buffers = shmBuffers;
    clock_gettime(CLOCK_MONOTONIC, &renderStart);
    renderTestDraw();
}

static void renderTestDMABegin()
{
    qDebug() << "DMA Rendering Test:";
    writes = 0;
    testingDMA = true;
    renderedFrames = 0;
    nanos = 0;
    toplevel->buffers = dmaBuffers;
    clock_gettime(CLOCK_MONOTONIC, &renderStart);
    Buffer *buffer = toplevel->buffers[0];
    render(buffer);
    toplevel->i = 1;
    renderTestDraw();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc < 4)
    {
        qFatal() << "Run example: ./benchmark compositorName bufferWidth bufferHeight bufferScale";
        exit(0);
    }

    qDebug() << "Compositor:" << argv[1];

    width = atoi(argv[2]);
    height = atoi(argv[3]);
    bufferScale = atoi(argv[4]);

    display = wl_display_connect(NULL);

    if (!display)
    {
        qFatal() << "Failed to connect to Wayland server";
        return 0;
    }

    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (shm == NULL || compositor == NULL || wm_base == NULL)
    {
        qFatal() << "Missing Wayland Server globals";
        exit(EXIT_FAILURE);
    }

    // Create buffers
    for (int i = 0; i < BUFFS; i++)
    {
        shmBuffers[i] = create_shm_buffer(width, height);
        shmBuffers[i]->i =  i;
        dmaBuffers[i] = create_dma_buffer(width, height);
        dmaBuffers[i]->i =  i;
    }

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    qDebug("Buffer size: %dx%d", width, height);

    drawTest1(false, shmBuffers[0],100);
    drawTest1(true, dmaBuffers[0], 100);

    drawTest2(false, shmBuffers[0], 100);
    drawTest2(true, dmaBuffers[0], 100);

    drawTest1(false, shmBuffers[0], 10);
    drawTest1(true, dmaBuffers[0], 10);
    drawTest2(false, shmBuffers[0], 10);
    drawTest2(true, dmaBuffers[0], 10);

    drawTest1(false, shmBuffers[0], 1);
    drawTest1(true, dmaBuffers[0], 1);
    drawTest2(false, shmBuffers[0], 1);
    drawTest2(true, dmaBuffers[0], 1);

    drawTest3(false, shmBuffers[0]);
    drawTest3(true, dmaBuffers[0]);

    drawTest4(false, shmBuffers[0]);
    drawTest4(true, dmaBuffers[0]);

    createToplevel();

    usleep(1000000);

    renderTestSHMBegin();

    while (wl_display_dispatch(display) != -1)
    {
        if (benchFinished)
        {
            benchFinished = false;
            break;
        }
    }

    usleep(1000000);

    renderTestDMABegin();

    while (wl_display_dispatch(display) != -1)
    {
        if (benchFinished)
        {
            benchFinished = false;
            break;
        }
    }
}
