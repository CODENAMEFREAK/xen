/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright (C) IBM Corp. 2005, 2006
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 *          Jimi Xenidis <jimix@watson.ibm.com>
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/lib.h>
#include <asm/time.h>
#include <asm/current.h>
#include <asm/powerpc64/procarea.h>
#include <asm/powerpc64/processor.h>
#include <asm/powerpc64/ppc970-hid.h>

#undef SERIALIZE

extern volatile struct processor_area * volatile global_cpu_table[];

struct rma_settings {
    int order;
    int rmlr0;
    int rmlr12;
};

static struct rma_settings rma_orders[] = {
    { .order = 26, .rmlr0 = 0, .rmlr12 = 3, }, /*  64 MB */
    { .order = 27, .rmlr0 = 1, .rmlr12 = 3, }, /* 128 MB */
    { .order = 28, .rmlr0 = 1, .rmlr12 = 0, }, /* 256 MB */
    { .order = 30, .rmlr0 = 0, .rmlr12 = 2, }, /*   1 GB */
    { .order = 34, .rmlr0 = 0, .rmlr12 = 1, }, /*  16 GB */
    { .order = 38, .rmlr0 = 0, .rmlr12 = 0, }, /* 256 GB */
};

static struct rma_settings *cpu_find_rma(unsigned int order)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(rma_orders); i++) {
        if (rma_orders[i].order == order)
            return &rma_orders[i];
    }
    return NULL;
}

unsigned int cpu_default_rma_order_pages(void)
{
    return rma_orders[0].order - PAGE_SHIFT;
}

unsigned int cpu_large_page_orders(uint *sizes, uint max)
{
    uint lp_log_size = 4 + 20; /* (1 << 4) == 16M */
    if (max < 1)
        return 0;

    sizes[0] = lp_log_size - PAGE_SHIFT;

    return 1;
}    

void cpu_initialize(int cpuid)
{
    ulong r1, r2;
    __asm__ __volatile__ ("mr %0, 1" : "=r" (r1));
    __asm__ __volatile__ ("mr %0, 2" : "=r" (r2));

    /* This is SMP safe because the compiler must use r13 for it.  */
    parea = global_cpu_table[cpuid];
    ASSERT(parea != NULL);

    mthsprg0((ulong)parea); /* now ready for exceptions */

    /* Set decrementers for 1 second to keep them out of the way during
     * intialization. */
    /* XXX make tickless */
    mtdec(timebase_freq);
    mthdec(timebase_freq);

    union hid0 hid0;

    hid0.word = mfhid0();
    hid0.bits.nap = 1;
    hid0.bits.dpm = 1;
    hid0.bits.nhr = 1;
    hid0.bits.hdice = 1; /* enable HDEC */
    hid0.bits.eb_therm = 1;
    hid0.bits.en_attn = 1;
#ifdef SERIALIZE
    ulong s = 0;

    s |= 1UL << (63-0);     /* one_ppc */
    s |= 1UL << (63-2);     /* isync_sc */
    s |= 1UL << (63-16);     /* inorder */
    /* may not want these */
    s |= 1UL << (63-1);     /* do_single */
    s |= 1UL << (63-3);     /* ser-gp */
    hid0.word |= s;
#endif

    printk("CPU #%d: Hello World! SP = %lx TOC = %lx HID0 = %lx\n", 
           smp_processor_id(), r1, r2, hid0.word);

    mthid0(hid0.word);

    union hid1 hid1;

    hid1.word = mfhid1();
    hid1.bits.bht_pm = 7;
    hid1.bits.en_ls = 1;

    hid1.bits.en_cc = 1;
    hid1.bits.en_ic = 1;

    hid1.bits.pf_mode = 2;

    hid1.bits.en_if_cach = 1;
    hid1.bits.en_ic_rec = 1;
    hid1.bits.en_id_rec = 1;
    hid1.bits.en_er_rec = 1;

    hid1.bits.en_sp_itw = 1;
    mthid1(hid1.word);

    union hid5 hid5;

    hid5.word = mfhid5();
    hid5.bits.DCBZ_size = 0;
    hid5.bits.DCBZ32_ill = 0;
    mthid5(hid5.word);

    __asm__ __volatile__("isync; slbia; isync" : : : "memory");
}

void cpu_init_vcpu(struct vcpu *v)
{
    struct domain *d = v->domain;
    union hid4 hid4;
    struct rma_settings *rma_settings;

    hid4.word = mfhid4();

    hid4.bits.lpes0 = 0; /* exceptions set MSR_HV=1 */
    hid4.bits.lpes1 = 1; /* RMA applies */

    hid4.bits.rmor = page_to_maddr(d->arch.rma_page) >> 26;

    hid4.bits.lpid01 = d->domain_id & 3;
    hid4.bits.lpid25 = (d->domain_id >> 2) & 0xf;

    rma_settings = cpu_find_rma(d->arch.rma_order + PAGE_SHIFT);
    ASSERT(rma_settings != NULL);
    hid4.bits.rmlr0 = rma_settings->rmlr0;
    hid4.bits.rmlr12 = rma_settings->rmlr12;

    v->arch.cpu.hid4.word = hid4.word;
}

void save_cpu_sprs(struct vcpu *v)
{
    /* HID4 is initialized with a per-domain value at domain creation time, and
     * does not change after that. */
}

void load_cpu_sprs(struct vcpu *v)
{
    mthid4(v->arch.cpu.hid4.word);
}
