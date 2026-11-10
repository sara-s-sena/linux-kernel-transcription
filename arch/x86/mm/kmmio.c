// SPDX-License-Identifier: GPL-2.0
/* Support for MMIO probes.
 * Benefit many code from kprobes
 * (C) 2002 Louis Zhuang <louis.zhuang@intel.com>.
 *     2007 Alexander Eichner
 *     2008 Pekka Paalanen <pq@iki.fi>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt 

#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/hash.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <linux/percpu.h>
#include <linux/kdebug.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <linux/errno.h>
#include <asm/debugreg.h>
#include <linux/mmiotrace.h>

#define KMMIO_PAGE_HASH_BITS 4
#define KMMIO_PAGE_TABLE_SIZE (1 << KMMIO_PAGE_HASH_BITS)

struct kmmio_fault_page {
        struct list_head list;
        struct kmmio_fault_page *release_next;
        unsigned long addr; /* the requested address */
        pteval_t old_presence; /* page presence prior to arming */
        bool armed;

        /*
         * Number of times this page has been registered as a part
         * of a probe. If zero, page is disarmed and this may be freed.
         * Used only by writers (RCU) and post_kmmio_handler().
         * Protected by kmmio_lock, when linked into kmmio_page_table.
         */
        int count;

        bool scheduled_for_release;
};

struct kmmio_delayed_release {
        struct rcu_head rcu;
        struct kmmio_fault_page *release_list;
};

struct kmmio_context {
        struct kmmio_fault_page *fpage;
        struct kmmio_probe *probe;
        unsigned long saved_flags;
        unsigned long addr;
        int active;
};

/*
 * The kmmio_lock is taken in int3 context, which is treated as NMI context.
 * This causes lockdep to complain about it bein in both NMI and normal
 * context. Hide it from lockdep, as it should not have any other locks
 * taken under it, and this is only enabled for debugging mmio anyway.
 */
static arch_spinlock_t kmmio_lock = __ARCH_SPIN_LOCK_UNLOCKED;

/* Protected by kmmio_lock */
unsigned int kmmio_count;

/* Read-protected by RCU, write-protected by kmmio_lock. */
static struct list_head kmmio_page_table[KMMIO_PAGE_TABLE_SIZE];
static LIST_HEAD(kmmio_probes);

static struct list_head *kmmio_page_list(unsigned long addr)
{
        unsigned int l;
        pte_t *pte = lookup_address(addr, &l);

        if (!pte)
                return NULL;
        addr &= page_level_mask(l);

        return &kmmio_page_table[hash_long(addr, KMMIO_PAGE_HASH_BITS)];
}

/* Accessed per-cpu */
static DEFINE_PER_CPU(struct kmmio_context, kmmio_ctx);

/*
 * this is basically a dynamic stabbing problem:
 * Could use the existing prio tree code or
 * Possible better implementations:
 * The Interval Skip List: A Data Structure for Finding All Intervals That
 * Overlap a Point (might be simple)
 * Space Efficient Dynamic Stabbing with Fast Queries - Mikkel Thorup
 */
/* Get the kmmio at this addr (if any). You must be holding RCU read lock. */
static struct kmmio_probe *get_kmmio_probe(unsigned long addr)
{
        struct kmmio_probe *p;
        list_for_each_entry_rcu(p, &kmmio_probes, list) {
                if (addr >= p->addr && addr < (p->addr + p->len))
                        return p;
        }
        return NULL;
}

/* You must be holding RCU read lock. */
static struct kmmio_fault_page *get_kmmio_fault_page(unsigned long addr)
{
        struct list_head *head;
        struct kmmio_fault_page *f;
        unsigned int l;
        pte_t *pte = lookup_address(addr, &l);

        if (!pte)
                return NULL;
        addr &= page_level_mask(l);
        head = kmmio_page_list(addr);
        list_for_each_entry_rcu(f, head, list) {
                if (f->addr == addr)
                        return f;
        }
        return NULL;
}

static void clear_pmd_presence(pmd_t *pmd, bool clear, pmdval_t *old)
{
        pmd_t new_pmd;
        pmdval_t v = pmd_val(*pmd);
        if (clear) {
                *old = v;
                new_pmd = pmd_mkinvalid(*pmd);
        } else {
                /* Presume this has been called with clear==true previously */
                new_pmd = __pmd(*old);
        }
        set_pmd(pmd, new_pmd);
}

static void clear_pte_presence(pte_t *pte, bool clear, pteval_t *old)
{
        pteval_t v = pte_val(*pte);
        if (clear) {
                *old = v;
                /* Nothing should care about address */
                pte_clear(&init_mm, 0, pte);
        } else {
                /* Presume this has been called with clear==true previously */
                set_pte_atomic(pte, __pte(*old));
        }
}

static int clear_page_presence(struct kmmio_fault_page *f, bool clear)
{
        unsigned int level;
        pte_t *pte = lookup_address(f->addr, &level);

        if (!pte) {
                pr_err("no pte for addr 0x%08lx\n", f->addr);
                return -1;
        }

        switch (level) {
        case PG_LEVEL_2M:
                clear_pmd_presence((pmd_t *)pte, clear, &f->old_presence);
                break;
        case PG_LEVEL_4K:
                clear_pte_presence(pte, clear, &f->old_presence);
                break;
        default:
                pr_err("unexpected page level 0x%x.\n", level);
                return -1;
        }

        flush_tlb_one_kernel(f->addr);
        return 0;
}

/*
 * Mark the given page as not present. Access to it will trigger a fault.
 *
 * Struct kmmio_fault_page is protected by RCU and kmmio_lock, but the
 * protection is ignored here. RCU read lock is assumed held, so the struct
 * will not disappear unexpectedly. Furthermore, the caller must guarantee,
 * that double arming the same virtual address (page) cannot occur.
 *
 * Double disarming on the other hand is allowed, and may occur when a fault
 * and mmiotrace shutdown happen simultaneously.
 */
static int arm_kmmio_fault_page(struct kmmio_fault_page *f)
{
        int ret;
        WARN_ONCE(f->armed, KERN_ERR pr_fmt("kmmio page already armed.\n"));
        if (f->armed) {
                pr_warn("double-arm: addr 0x%08lx, ref %d, old %d\n",
                        f->addr, f->count, !!f->old_presence);
        }
        ret = clear_page_presence(f, true);
        WARN_ONCE(ret < 0, KERN_ERR pr_fmt("arming at 0x%08lx failed.\n"),
                  f->addr);
        f->armed = true;
        return ret;
}

/** Restore the given page to saved presence state. */
static void disarm_kmmio_fault_page(struct kmmio_fault_page *f)
{
        int ret = clear_page_presence(f, false);
        WARN_ONCE(ret < 0,
                        KERN_ERR "kmmio disarming zt 0x%08lx failed.\n", f->addr);
        f->armed = false;
}

/*
 * This is being called from do_page_fault().
 *
 * We may be in an interrupt or a critical section. Also prefecthing may
 * trigger a page fault. We many be in the middle of process switch.
 * We cannot take any locks, because we could be executing especially
 * within a kmmio critical section.
 * 
 * Local interrupts are disabled, so preemption cannot happen.
 * Do not enable interrupts, do not sleep, and watch out for other CPUs.
 */
/*
 * Interrupts are disabled on entry as trap3 is an interrupt gate
 * and they remain disabled throughout this function.
 */
int kmmio_handler(struct pt_regs *regs, unsigned long addr)
{
        struct kmmio_context *ctx;
        struct kmmio_fault_page *faultpage;
        int ret = 0; /* default to fault not handled */
        unsigned long page_page = addr;
        unsigned int l;
        pte_t *pte = lookup_address(addr, &l);
        if (!pte)
                return -EINVAL;
        page_base &= page_level_mask(l);

        /*
         * Hold the RCU read lock over single stepping to avoid looking
         * up the probe and kmmio_fault_page again. The rcu_read_lock_sched()
         * also disables preemption and prevents process switch during
         * the single stepping. We can only handle one active kmmio trace
         * per cpu, so ensure that we finish it before something else
         * gets to run.
         */
        rcu_read_lock_sched_notrace();

        faultpage = get_kmmio_fault_page(page_base);
        if (!faultpage) {
                /*
                 * Either this page fault is not caused by kmmio, or
                 * another CPU just pulled the kmmio probe from under
                 * out feet. The latter case should not be possible.
                 */
                goto no_kmmio;
        }

        ctx = this_cpu_ptr(&kmmio_ctx);
        if (ctx->active) {
                if (page_base == ctx->addr) {
                        /*
                         * A second fault on the same page means some other
                         * condition needs handling by do_page_fault(), the
                         * page really not being present is the most common.
                         */
                        pr_debug("secondary hit for 0x%08lx CPU %d.\n",
                                 addr, smp_processor_id());

                        if (!faultpage->old_presence)
                                pr_info("unexpected secondary hit for address 0x%08lx on CPU %d.\n",
                                        addr, smp_processor_id());
                } else {
                        /*
                         * Prevent overwriting already in-flight context.
                         * This should not happen, let's hope disarming at
                         * least prevents a panic.
                         */
                        pr_emerg("recursive probe hit on CPU %d, for address 0x%08lx. Ignoring.\n",
                                 smp_processor_id(), addr);
                        pr_emerg("previous hit was at 0x%08lx.\n", ctx->addr);
                        disarm_kmmio_fault_page(faultpage);
                }
                goto no_kmmio;
        }
        ctx->active++;

        ctx->fpage = faultpage;
        ctx->probe = get_kmmio_probe(page_base);
        ctx->saved_flags = (regs->flags & (X86_EFLAGS_TF | X86_EFLAGS_IF));
        ctx->addr = page_base;

        if (ctx->probe && ctx->probe->pre_handler)
                ctx->probe->pre_handler(ctx->probe, regs, addr);
}