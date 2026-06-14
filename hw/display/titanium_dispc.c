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
#include "qom/object.h"

#define TYPE_TITANIUM_DISPC "titanium-dispc"
OBJECT_DECLARE_SIMPLE_TYPE(TitaniumDISPCState, TITANIUM_DISPC)

#define R_REVISION       (0x00 / 4)
#define R_SYSSTATUS      (0x14 / 4)
#define R_IRQSTATUS      (0x18 / 4)
#define R_IRQENABLE      (0x1C / 4)
#define R_CONTROL1       (0x40 / 4)
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

static void dispc_vsync_tick(void *opaque)
{
    TitaniumDISPCState *s = opaque;

    s->regs[R_IRQSTATUS] |= DISPC_IRQ_VSYNC;
    dispc_update_irq(s);
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

static bool dispc_enabled(TitaniumDISPCState *s)
{
    return (s->regs[R_CONTROL1] &
            (CONTROL1_LCDENABLE | CONTROL1_DIGITALENABLE)) &&
           (s->regs[R_GFX_ATTRIBUTES] & GFX_ATTR_ENABLE) &&
           s->regs[R_GFX_BA_0] != 0;
}

static void dispc_update_geometry(TitaniumDISPCState *s)
{
    uint32_t sz = s->regs[R_GFX_SIZE];
    uint32_t w = (sz & 0x7ff) + 1;
    uint32_t h = ((sz >> 16) & 0x7ff) + 1;

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
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t fmt = (s->regs[R_GFX_ATTRIBUTES] >> GFX_ATTR_FORMAT_SHIFT) &
                   GFX_ATTR_FORMAT_MASK;
    int bpp = (fmt == 0x6) ? 2 : 4;   /* 0x6 = RGB16 (565); else 32bpp */
    drawfn fn = (bpp == 2) ? draw_line16 : draw_line32;
    int first = 0, last = 0;
    int src_width;

    if (!dispc_enabled(s)) {
        return;
    }
    dispc_update_geometry(s);
    if (s->width <= 1 || s->height <= 1) {
        return;
    }

    src_width = s->width * bpp;
    if (!s->fbsection_valid) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                          s->regs[R_GFX_BA_0],
                                          s->height, src_width);
        s->fbsection_valid = true;
        s->invalidate = 1;
    }

    framebuffer_update_display(surface, &s->fbsection, s->width, s->height,
                               src_width, s->width * 4, 0, s->invalidate,
                               fn, s, &first, &last);
    s->invalidate = 0;
    if (last >= first) {
        dpy_gfx_update(s->con, 0, first, s->width, last - first + 1);
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
