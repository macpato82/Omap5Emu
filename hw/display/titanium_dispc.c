/*
 * TI AM5728 DSS / DISPC display controller — minimal framebuffer scan-out
 * for the Elesar Titanium machine (RISC OS 5).
 *
 * Models just enough of DISPC to (a) satisfy the HAL/kernel's register polls
 * and (b) scan out the GFX pipeline framebuffer that RISC OS sets up in DRAM,
 * so the screen becomes visible. Registers of interest (base 0x58001000):
 *   0x00 REVISION         (RO)
 *   0x14 SYSSTATUS        (RO, bit0 RESETDONE)
 *   0x40 CONTROL1         (bit0 LCDENABLE, bit1 DIGITALENABLE)
 *   0x80 GFX_BA_0         (framebuffer base address in DRAM)
 *   0x8C GFX_SIZE         ((w-1) | (h-1)<<16)
 *   0xA0 GFX_ATTRIBUTES   (bit0 ENABLE, bits[5:1] FORMAT)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "hw/display/framebuffer.h"
#include "ui/pixel_ops.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "qom/object.h"

#define TYPE_TITANIUM_DISPC "titanium-dispc"
OBJECT_DECLARE_SIMPLE_TYPE(TitaniumDISPCState, TITANIUM_DISPC)

#define R_REVISION       (0x00 / 4)
#define R_SYSSTATUS      (0x14 / 4)
#define R_IRQSTATUS      (0x18 / 4)
#define R_IRQENABLE      (0x1C / 4)
#define R_CONTROL1       (0x40 / 4)
#define R_SIZE_LCD       (0x7C / 4)
#define R_GFX_BA_0       (0x80 / 4)
#define R_GFX_SIZE       (0x8C / 4)
#define R_GFX_ATTRIBUTES (0xA0 / 4)

#define DISPC_IRQ_FRAMEDONE (1u << 0)
#define DISPC_IRQ_VSYNC     (1u << 1)
#define DISPC_VSYNC_HZ      60

#define CONTROL1_LCDENABLE     (1u << 0)
#define CONTROL1_DIGITALENABLE (1u << 1)
#define GFX_ATTR_ENABLE        (1u << 0)
#define GFX_ATTR_FORMAT_SHIFT  1
#define GFX_ATTR_FORMAT_MASK   0x1f

struct TitaniumDISPCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;

    uint32_t regs[0x1000 / 4];
    uint32_t width, height;
    bool fbsection_valid;
    int invalidate;

    qemu_irq irq;
    QEMUTimer *vsync;
};

/* The video driver enables DISPC_IRQENABLE.VSYNC and waits for the interrupt
 * before it considers the display up, so we must raise a periodic VSYNC. */
static void dispc_update_irq(TitaniumDISPCState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_IRQSTATUS] & s->regs[R_IRQENABLE]) != 0);
}

/* Debug aid: dump the GFX framebuffer to a PPM (TITANIUM_FB_DUMP=path), so the
 * desktop can be inspected on a headless host. Throttled to ~1 Hz. */
static void dispc_dump_ppm(TitaniumDISPCState *s)
{
    const char *path = getenv("TITANIUM_FB_DUMP");
    uint32_t ba, gsz, w, h, fmt, x, y;
    int bpp;
    uint8_t *fb;
    FILE *f;

    if (!path) {
        return;
    }
    ba = s->regs[R_GFX_BA_0];
    gsz = s->regs[R_GFX_SIZE];
    w = (gsz & 0x7ff) + 1;
    h = ((gsz >> 16) & 0x7ff) + 1;
    if (w <= 1 || h <= 1) {                 /* GFX_SIZE unset: use LCD panel size */
        uint32_t lcd = s->regs[0x07c / 4];
        w = (lcd & 0x7ff) + 1;
        h = ((lcd >> 16) & 0x7ff) + 1;
    }
    if (ba == 0 || w <= 1 || h <= 1 || w > 4096 || h > 4096) {
        return;
    }
    fmt = (s->regs[R_GFX_ATTRIBUTES] >> GFX_ATTR_FORMAT_SHIFT) & GFX_ATTR_FORMAT_MASK;
    /* OMAP DISPC GFX formats: 0x3=CLUT8, 0x6=RGB16, 0x8=RGB24, 0x9/0xC/0xD=32bpp */
    bpp = (fmt == 0x3) ? 1 : (fmt == 0x6) ? 2 : 4;
    if (getenv("TITANIUM_FB_BPP")) {
        bpp = atoi(getenv("TITANIUM_FB_BPP"));
    }

    fb = g_malloc((size_t)w * h * bpp);
    cpu_physical_memory_read(ba, fb, (size_t)w * h * bpp);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint8_t *p = fb + ((size_t)y * w + x) * bpp;
                uint8_t r, gc, b;
                if (bpp == 1) {
                    r = gc = b = *p;        /* CLUT8: grayscale by index */
                } else if (bpp == 2) {
                    uint16_t v = lduw_le_p(p);
                    r = ((v >> 11) & 0x1f) << 3;
                    gc = ((v >> 5) & 0x3f) << 2;
                    b = (v & 0x1f) << 3;
                } else {
                    uint32_t v = ldl_le_p(p);
                    r = v & 0xff; gc = (v >> 8) & 0xff; b = (v >> 16) & 0xff;
                }
                fputc(r, f); fputc(gc, f); fputc(b, f);
            }
        }
        fclose(f);
        fprintf(stderr, "[dispc] dumped %ux%u fmt=0x%x %dbpp fb @%08x -> %s\n",
                w, h, fmt, bpp * 8, ba, path);
    }
    g_free(fb);
}

static void dispc_vsync_tick(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    static int n;

    s->regs[R_IRQSTATUS] |= DISPC_IRQ_VSYNC;
    dispc_update_irq(s);
    if (++n % 60 == 0) {
        dispc_dump_ppm(s);
    }
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / DISPC_VSYNC_HZ);
}

/* Source framebuffer line -> surface (xRGB8888). RISC OS uses BGRx order. */
static void draw_line32(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint32_t v = ldl_le_p(s);
        uint8_t r = v & 0xff, g = (v >> 8) & 0xff, b = (v >> 16) & 0xff;
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 4;
        d += 4;
    }
}

static void draw_line16(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint16_t v = lduw_le_p(s);
        uint8_t r = ((v >> 11) & 0x1f) << 3;
        uint8_t g = ((v >> 5) & 0x3f) << 2;
        uint8_t b = (v & 0x1f) << 3;
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 2;
        d += 4;
    }
}

/* CLUT8 (8bpp) -> grayscale by index. RISC OS's palette here is ~a gamma ramp,
 * so index-as-luminance reproduces the (largely monochrome) desktop well. */
static void draw_line8(void *opaque, uint8_t *d, const uint8_t *s,
                       int width, int deststep)
{
    while (width--) {
        uint8_t v = *s;
        *(uint32_t *)d = rgb_to_pixel32(v, v, v);
        s += 1;
        d += 4;
    }
}

static bool dispc_enabled(TitaniumDISPCState *s)
{
    /* Don't require GFX_ATTR_ENABLE: RISC OS toggles it during updates. */
    return (s->regs[R_CONTROL1] &
            (CONTROL1_LCDENABLE | CONTROL1_DIGITALENABLE)) &&
           s->regs[R_GFX_BA_0] != 0;
}

static void dispc_update_geometry(TitaniumDISPCState *s)
{
    uint32_t sz = s->regs[R_GFX_SIZE];
    uint32_t w = (sz & 0x7ff) + 1;
    uint32_t h = ((sz >> 16) & 0x7ff) + 1;

    if (w <= 1 || h <= 1) {     /* GFX_SIZE unset: use the LCD panel size */
        uint32_t lcd = s->regs[R_SIZE_LCD];
        w = (lcd & 0x7ff) + 1;
        h = ((lcd >> 16) & 0x7ff) + 1;
    }

    if (w != s->width || h != s->height) {
        s->width = w;
        s->height = h;
        s->fbsection_valid = false;
        if (w > 1 && h > 1) {
            qemu_console_resize(s->con, w, h);
        }
    }
}

static void dispc_gfx_update(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    DisplaySurface *surface;
    uint32_t fmt = (s->regs[R_GFX_ATTRIBUTES] >> GFX_ATTR_FORMAT_SHIFT) &
                   GFX_ATTR_FORMAT_MASK;
    /* fmt: 0x6 = RGB16; 0x8/0x9/0xC/0xD = 32bpp; else (0..3) = CLUT8/8bpp */
    int bpp = (fmt == 0x6) ? 2 : (fmt >= 0x8) ? 4 : 1;
    drawfn fn = (bpp == 1) ? draw_line8 : (bpp == 2) ? draw_line16 : draw_line32;
    int src_width, dstride, y;
    int en = dispc_enabled(s);
    uint8_t *src, *dst;

    if (!en) {
        return;
    }
    dispc_update_geometry(s);
    if (s->width <= 1 || s->height <= 1) {
        return;
    }

    /* Fetch the surface AFTER any resize so we never draw to a stale surface. */
    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    /*
     * Read the framebuffer straight from guest DRAM and convert it line by line.
     * We deliberately avoid framebuffer_update_display(): it sources pixels via a
     * DIRTY_MEMORY_VGA snapshot, which we never enable on plain DRAM, so it would
     * read zeros and leave the screen black. A direct read of a 640x480 frame is
     * cheap and always shows the current contents.
     */
    /* Clamp to the surface so a lagging resize can never overflow it. */
    {
        int sw = surface_width(surface), sh = surface_height(surface);
        if (s->width > sw || s->height > sh) {
            return;
        }
    }

    src_width = s->width * bpp;
    dstride = surface_stride(surface);
    src = g_malloc((size_t)src_width * s->height);
    cpu_physical_memory_read(s->regs[R_GFX_BA_0], src,
                             (size_t)src_width * s->height);
    dst = surface_data(surface);
    for (y = 0; y < s->height; y++) {
        fn(s, dst + (size_t)y * dstride, src + (size_t)y * src_width,
           s->width, 0);
    }
    g_free(src);

    /*
     * Composite the hardware mouse pointer. RISC OS (the GC320Video driver)
     * advertises GVDisplayFeature_HardwarePointer, so it does NOT draw the
     * pointer into the GFX framebuffer: it programs a 32x32 ARGB sprite on a
     * DISPC VID overlay and moves it by rewriting the overlay POSITION every
     * frame. We only scanned out the GFX layer, so the pointer was invisible
     * even though the mouse works. Overlay registers (found by tracing the
     * live driver): 0x14C base addr, 0x154 POSITION (X | Y<<16), 0x158 SIZE
     * ((w-1) | (h-1)<<16), 0x15C ATTRIBUTES (bit0 = enable, format 0xC ARGB32).
     */
    {
        uint32_t attr  = s->regs[0x15c / 4];
        uint32_t cbase = s->regs[0x14c / 4];
        if ((attr & 1) && cbase != 0) {
            uint32_t pos = s->regs[0x154 / 4];
            uint32_t csz = s->regs[0x158 / 4];
            int cw = (int)(csz & 0x7ff) + 1;
            int ch = (int)((csz >> 16) & 0x7ff) + 1;
            int px = (int)(pos & 0xffff);
            int py = (int)((pos >> 16) & 0xffff);
            if (cw > 0 && cw <= 256 && ch > 0 && ch <= 256) {
                uint32_t *cur = g_malloc((size_t)cw * ch * 4);
                cpu_physical_memory_read(cbase, cur, (size_t)cw * ch * 4);
                for (int cy = 0; cy < ch; cy++) {
                    int sy = py + cy;
                    if (sy < 0 || sy >= s->height) {
                        continue;
                    }
                    uint32_t *drow = (uint32_t *)(dst + (size_t)sy * dstride);
                    for (int cx = 0; cx < cw; cx++) {
                        int sx = px + cx;
                        if (sx < 0 || sx >= s->width) {
                            continue;
                        }
                        uint32_t p = cur[cy * cw + cx];   /* ARGB8888 */
                        uint32_t a = p >> 24;
                        uint32_t r = (p >> 16) & 0xff;
                        uint32_t g = (p >> 8) & 0xff;
                        uint32_t b = p & 0xff;
                        if (a == 0) {
                            continue;
                        }
                        if (a != 0xff) {
                            uint32_t o = drow[sx];
                            uint32_t orr = (o >> 16) & 0xff;
                            uint32_t og = (o >> 8) & 0xff;
                            uint32_t ob = o & 0xff;
                            r = (r * a + orr * (255 - a)) / 255;
                            g = (g * a + og * (255 - a)) / 255;
                            b = (b * a + ob * (255 - a)) / 255;
                        }
                        drow[sx] = rgb_to_pixel32(r, g, b);
                    }
                }
                g_free(cur);
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, s->width, s->height);

    /* Debug: dump the rendered display SURFACE (what the user sees) to a PPM. */
    if (getenv("TITANIUM_SURF_DUMP")) {
        static int n;
        if (n++ % 120 == 119) {
            FILE *f = fopen(getenv("TITANIUM_SURF_DUMP"), "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
                for (y = 0; y < s->height; y++) {
                    uint8_t *row = dst + (size_t)y * dstride;
                    for (int x = 0; x < s->width; x++) {
                        uint32_t v = ((uint32_t *)row)[x];
                        fputc((v >> 16) & 0xff, f);
                        fputc((v >> 8) & 0xff, f);
                        fputc(v & 0xff, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "[dispc] surface dumped %dx%d\n", s->width, s->height);
            }
        }
    }
}

static void dispc_invalidate(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    s->invalidate = 1;
    s->fbsection_valid = false;
}

static uint64_t dispc_read(void *opaque, hwaddr addr, unsigned size)
{
    TitaniumDISPCState *s = opaque;
    uint64_t v;

    switch (addr >> 2) {
    case R_REVISION:  v = 0x00000061; break;   /* DISPC v6.1-ish, nonzero */
    case R_SYSSTATUS: v = 1; break;            /* RESETDONE */
    case R_IRQSTATUS: v = s->regs[R_IRQSTATUS]; break;
    case R_CONTROL1:
        /* GOLCD (bit5) / GODIGITAL (bit6) are set by software to latch the
         * shadow registers and self-clear at the next vsync. Report them clear
         * so the driver's "set GO, poll until clear" loop completes. */
        v = s->regs[R_CONTROL1] & ~0x60u;
        break;
    default:          v = s->regs[(addr >> 2) & 0x3ff]; break;
    }
    if (getenv("TITANIUM_DISPC_TRACE")) {
        fprintf(stderr, "[dispc] rd %03x -> %08x\n",
                (unsigned)(addr & 0xfff), (unsigned)v);
    }
    return v;
}

static void dispc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TitaniumDISPCState *s = opaque;

    if (getenv("TITANIUM_DISPC_TRACE")) {
        fprintf(stderr, "[dispc] wr %03x <- %08x\n",
                (unsigned)(addr & 0xfff), (unsigned)val);
    }
    /* DISPC_IRQSTATUS is write-1-to-clear; everything else stores verbatim. */
    if ((addr >> 2) == R_IRQSTATUS) {
        s->regs[R_IRQSTATUS] &= ~(uint32_t)val;
        dispc_update_irq(s);
        return;
    }

    s->regs[(addr >> 2) & 0x3ff] = (uint32_t)val;

    if ((addr >> 2) == R_IRQENABLE) {
        dispc_update_irq(s);
        return;
    }

    /* Any change to enable/geometry/base may change the picture */
    switch (addr >> 2) {
    case R_CONTROL1:
    case R_GFX_BA_0:
    case R_GFX_SIZE:
    case R_GFX_ATTRIBUTES:
        s->fbsection_valid = false;
        s->invalidate = 1;
        if (s->con) {
            dispc_update_geometry(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dispc_ops = {
    .read = dispc_read,
    .write = dispc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const GraphicHwOps dispc_gfx_ops = {
    .invalidate = dispc_invalidate,
    .gfx_update = dispc_gfx_update,
};

static void dispc_realize(DeviceState *dev, Error **errp)
{
    TitaniumDISPCState *s = TITANIUM_DISPC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dispc_ops, s,
                          "titanium-dispc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->con = graphic_console_init(dev, 0, &dispc_gfx_ops, s);
    /* Size the console once, here, so the display backend binds to a stable
     * surface. Resizing from inside gfx_update (which runs during the backend's
     * own refresh) is re-entrant and leaves SDL rendering a stale surface. */
    s->width = 640;
    s->height = 480;
    qemu_console_resize(s->con, s->width, s->height);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispc_vsync_tick, s);
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / DISPC_VSYNC_HZ);
}

static void dispc_reset_hold(Object *obj, ResetType type)
{
    TitaniumDISPCState *s = TITANIUM_DISPC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->width = s->height = 0;
    s->fbsection_valid = false;
    s->invalidate = 1;
}

static void dispc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dispc_realize;
    rc->phases.hold = dispc_reset_hold;
}

static const TypeInfo titanium_dispc_info = {
    .name          = TYPE_TITANIUM_DISPC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TitaniumDISPCState),
    .class_init    = dispc_class_init,
};

static void titanium_dispc_register_types(void)
{
    type_register_static(&titanium_dispc_info);
}

type_init(titanium_dispc_register_types)
