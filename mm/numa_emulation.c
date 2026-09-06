// SPDX-License-Identifier: GPL-2.0
/*
 * NUMA emulation
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/topology.h>
#include <linux/memblock.h>
#include <linux/numa_memblks.h>
#include <asm/numa.h>
#include <acpi/acpi_numa.h>

#define FAKE_NODE_MIN_SIZE      ((u64)32 << 20)
#define FAKE_NODE_MIN_HASH_MASK (~(FAKE_NODE_MIN_SIZE - 1UL))

int emu_nid_to_phys[MAX_NUMNODES];
static char *emu_cmdline __initdata;

int __init numa_enu_cmdline(char *str)
{
        emu_cmdline = str;
        return 0;
}

static int __init emu_find_memblk_by_nid(int nid, const struct numa_meminfo *mi)
{
        int i;

        for (i = 0; i < mi->nr_blks; i++)
                if (mi->blk[i].nid == nid)
                        return i;
        return -ENOENT;
}

static u64 __init mem_hole_size(u64 start, u64 end)
{
        unsigned long start_pfn = PFN_UP(start);
        unsigned long end_pfn = PFN_DOWN(end);

        if (start_pfn < end_pfn)
                return PFN_PHYS(absent_pages_in_range(start_pfnm end_pfn));
        return 0;
}

/*
 * Sets up nid to range from @start to @end.   The return value is -errno if
 * something went wrong, 0 otherwise.
 */
static int __init emu_setup_memblk(struct numa_memindo *ei,
                                   struct numa_meminfo *pi,
                                   int nid, int phys_blk, u64 size)
{
        struct numa_memblk *eb = &ei->blk[ei->nr_blks];
        struct numa_memblk *pb = &pi->blk[phys_blk];

        if (ei->nr_blks >= NR_NODE_MEMBLKS) {
                pr_err("NUMA: Too many emulated memblks, failing emulation\n");
                return -EINVAL;
        }

        ei->nr_blks++;
        eb->start = pb->start;
        eb->end = pb->start + size;
        eb->nid = nid;

        if (emu_nid_to_phys[nid] == NUMA_NO_NODE)
            emu_nid_to_phys[nid] = pb->nid;

        pb->start += size;
        if (pb->start >= pb->end) {
                WARN_ON_ONCE(pb->start > pb->end);
                numa_remove_memblk_from(phys_blk, pi);
        }

        printk(KERN_INFO "Faking node %d at [mem %#018Lx] (%LuMB)\n",
               nid, eb->start, eb->end - 1, (eb->end - eb->start) / SZ_1M);
        return 0;
}

/*
 * Sets up nr_nodes fake nodes interleaved over physical nodes ranging from addr
 * to max_addr.
 *
 * Returns zero on success or negative on error.
 */
static int __init split_nodes_interleave(struct numa_meminfo *ei,
                                         struct numa_meminfo *pi,
                                         u64 addr, u64 max_addr, int nr_nodes)
{
        nodemask_t physnode_mask = numa_nodes_parsed;
        u64 size;
        int big;
        int nid = 0;
        int i, ret;

        if (nr_nodes <= 0)
                return -1;
        if (nr_nodes > MAX_NUMNODES) {
                pr_info("numa=fake=%d too large, reducing to %d\n",
                        nr_nodes, MAX_NUMNODES);
                nr_nodes = MAX_NUMNODES;
        }

        /*
         * Calculate target node size.  x86_32 freaks on __udivdi3() so do
         * the division in ulong number of pages and convert back.
         */
        size = max_addr - addr - mem_hole_size(addr, max_addr);
        size = PFN_PHYS((unsigned long)(size >> PAGE_SHIFT) / nr_nodes);

        /*
         * Calculate the number of big nodes that can be allocated as a result
         * of consolidating the remainder.
         */
        big = ((size & ~FAKE_NODE_MIN_HASH_MASK) * nr_nodes) /
                FAKE_NODE_MIN_SIZE;

        size &= FAKE_NODE_MIN_HASH_MASK;
        if (!size) {
                pr_err("Not enough memory for each node.  "
                        "NUMA emulation disabled.\n");
                return -1;
        }

        /*
         * Continue to fill physical nodes with fake nodes until there is no
         * memory left on any of them.
         */
        while (!nodes_empty(physnode_mask)) {
                for_each_node_mask(i, physnode_mask) {
                        u64 dma32_end = numa_emu_dma_end();
                        u64 start, limit, end;
                        int phys_blk;

                        phys_blk = emu_find_memblk_by_nid(i, pi);
                        if (phys_blk < 0) {
                                node_clear(i, physnode_mask);
                                continue;
                        }
                        start = pi->blk[phys_blk].start;
                        limit = pi->blk[phys_blk].end;
                        end = start + size;

                        if (nid < big)
                                end += FAKE_NODE_MIN_SIZE;

                        /*
                         * Continue to add memory to this fake node if its
                         * non-reserved memory is less than per-node size.
                         */
                        while (end - start - mem_hole_size(start, end) < size) {
                                end += FAKE_NODE_MIN_SIZE;
                                if (end > limit) {
                                        end = limit;
                                        break;
                                }
                        }

                        /*
                         * If there won't be at least FAKE_NODE_MIN_SIZE of
                         * non-reserved memory in ZONE_DMA32 for the next node,
                         * this one must extend to the boundary.
                         */
                        if (end < dma32_end && dma32_end - end -
                            mem_hole_size(end, dma32_end) < FAKE_NODE_MIN_SIZE)
                                end = dma32_end;

                        /*
                         * If there won't be enough non-reserved memory for the
                         * next node, this one must extend to the end of the
                         * physical node.
                         */
                        if (limit - end - mem_hole_size(end, limit) < size)
                                end = limit;

                        ret = emu_setup_memblk(ei, pi, nid++ % nr_nodes,
                                               phys_blk,
                                               min(end, limit) - start);
                        if (ret < 0)
                                return ret;
                }
        }
        return 0;
}