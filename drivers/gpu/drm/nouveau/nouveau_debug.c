
#include <linux/highmem.h>
#include <linux/printk.h>
#include <core/object.h>
#include <core/gpuobj.h>
#include <engine/fifo.h>
#include <subdev/vm.h>
#include <nouveau_drm.h>
#include <nouveau_chan.h>
#include <nouveau_bo.h>

static u32
pfn_for_vaddr(u32 pgtpfn, u64 vaddr)
{
	u32 pde = vaddr >> 26;
	u32 pte = (vaddr & 0x3ffffff) >> 12;
	u32 ptepfn;
	struct page *p = pfn_to_page(pgtpfn);
	u32 *pgt = kmap(p);
	u32 *pet;
	u32 ret;
	ptepfn = pgt[pde * 2 + 1] >> 4;
	kunmap(p);

	p = pfn_to_page(ptepfn);
	pet = kmap(p);
	ret = pet[pte * 2] >> 4;
	kunmap(p);

	return ret;
}

void
dump_pte(u32 ptepfn, u32 base, bool big)
{
	struct page *p = pfn_to_page(ptepfn);
	u32 *pte = kmap(p);
	int k;
	int max_entries = big ? 0x400 : 0x8000;

	for (k = 0; k < max_entries; k++) {
		u32 mappfn = pte[0] >> 4;
		if (pte[0] & 1)
			printk("  %08x -> %08x   [ %08x %08x ]\n", base * 0x4000000 + k * (big ? 0x20000 : 0x1000), mappfn << 12, pte[0], pte[1]);

		pte += 2;

		/* page boundary - each page contains 512 entries */
		if ((k % 0x200) == 0 && k > 0) {
			kunmap(p);
			p = pfn_to_page(++ptepfn);
			pte = kmap(p);
		}
	}

	kunmap(p);
}

void
dump_pgt(u32 pgtpfn)
{
	struct page *p = pfn_to_page(pgtpfn);
	u32 *pgt = kmap(p);
	int j;

	for (j = 0; j < 0x2000; j++) {
		int flags;
		int ptepfn;

		if (!pgt[0] && !pgt[1])
			continue;
		flags = pgt[0] & 0xf;
		ptepfn = pgt[0] >> 4;
		printk("PDE+%03x: %08x %08x\n", j, pgt[0], pgt[1]);

		if ((flags & 0x3) != 0) {
			printk("BIG PAGES:\n");
			dump_pte(ptepfn, j, true);
		}

		flags = pgt[1] & 0xf;
		ptepfn = pgt[1] >> 4;
		if ((flags & 0x3) != 0) {
			printk("SMALL PAGES:\n");
			dump_pte(ptepfn, j, false);
		}
		pgt += 2;

		/* page boundary - each page contains 512 entries */
		if ((j % 0x200) == 0 && j > 0) {
			kunmap(p);
			p = pfn_to_page(++pgtpfn);
			pgt = kmap(p);
		}
	}

	kunmap(p);
}

void
dump_ramuser(struct nouveau_channel *ch)
{
	struct nouveau_fifo_chan *chan = (struct nouveau_fifo_chan *) ch->object;
	/* Read the values from the snoop area to get up-to-date information */
	u32 *ramuser = chan->user;
	u32 get, get_hi, put, put_hi;
	/* Read the low fields first as specified in the doc */
	get = ramuser[17];
	rmb();
	get_hi = ramuser[24];
	rmb();
	put = ramuser[16];
	rmb();
	put_hi = ramuser[19];
	rmb();
	printk("    PUT: %08x%08x\n", put_hi, put);
	printk("    GET: %08x%08x\n", get_hi, get);
	printk("    GP_PUT: %08x\n", ramuser[35]);
	printk("    GP_GET: %08x\n", ramuser[34]);
}

void dump_pb(u32 pbpfn, u32 start, u32 len)
{
	struct page *p = pfn_to_page(pbpfn);
	u32 *pb = kmap(p) + start;
	u32 i;

	for (i = start; i < start + len; i++)
		printk("%08x:  %08x\n", (pbpfn << PAGE_SHIFT) + i * 4, pb[i]);

	kunmap(p);
}

void dump_gpentries(struct nouveau_channel *ch, u32 gppfn, u32 pgtpfn)
{
	struct nouveau_fifo_chan *chan = (struct nouveau_fifo_chan *) ch->object;
	struct page *p = pfn_to_page(gppfn);
	u32 *gp = kmap(p);
	u32 *ramuser = chan->user;
	u32 gp0, gp1;
	u64 addr;
	u32 len;
	u32 gp_get, gp_put, i;

	gp_get = ramuser[34];
	gp_put = ramuser[35];

	for (i = gp_get; i <= gp_put; i++) {
		gp0 = gp[i * 2];
		gp1 = gp[i * 2 + 1];
		addr = ((u64)gp0) | (((u64)(gp1 & 0xff)) << 32);
		len = ((gp1 & 0x7ffffc00) >> 10);
		printk("    addr: %llx len: %x\n", addr, len);
		if (addr != 0)
			dump_pb(pfn_for_vaddr(pgtpfn, addr), addr & (~PAGE_MASK), len);
	}

	kunmap(p);
}

void dump_ramin(struct nouveau_channel *ch, u32 raminpfn)
{
	struct page *p = pfn_to_page(raminpfn);
	u32 *ramin = kmap(p);
	u32 pgtpfn;
	u64 gpbase;

	pgtpfn = (ramin[128] & (~0x3)) >> PAGE_SHIFT;

	gpbase = (((u64)(ramin[19] & 0xff)) << 32) | (ramin[18] & 0xfffffff8);

	printk("  GP_BASE: %llx\n", gpbase);
	printk("   phys: %x\n", pfn_for_vaddr(pgtpfn, gpbase));
	if (0) {
		printk("  GP ENTRIES:\n");
		dump_gpentries(ch, pfn_for_vaddr(pgtpfn, gpbase), pgtpfn);
	}
	printk("  USERD: %08x%08x\n", ramin[3], ramin[2]);
	dump_ramuser(ch);
	printk("  PGT: %02x%08x %02x%08x\n", ramin[129], ramin[128], ramin[131], ramin[130]);
	dump_pgt(pgtpfn);
	kunmap(p);
}

void dump_channel(struct nouveau_channel *chan)
{
	struct nouveau_device *device = nv_device(&chan->drm->device);
	struct nouveau_fifo_chan *fchan = (struct nouveau_fifo_chan *) chan->object;

	u32 raminpfn = nv_rd32(device, 0x800000 + (fchan->chid * 8)) & 0xfffff;
	printk("Channel 0x%x, RAMIN: %x\n", fchan->chid, raminpfn << PAGE_SHIFT);

	dump_ramin(chan, raminpfn);
}

/*
void
dump_runlist(struct nouveau_object *priv, u32 nchan, struct nouveau_gpuobj *cur)
{
	int i;

	for (i = 0; i < nchan; i++) {
		u32 val = nv_ro32(cur, i * 8);
		u32 chst1 = nv_rd32(priv, 0x800000 + (val * 8));
		u32 chst2 = nv_rd32(priv, 0x800004 + (val * 8));
		u32 raminpfn = chst1 & 0xfffff;
		if ((chst1 & 0x80000000) && val) {
			printk("  channel %x %08x %08x\n", val, chst1, chst2);
			printk("    RAMIN: %08x\n", raminpfn << PAGE_SHIFT);
			dump_ramin(raminpfn);
		}
	}
}
*/
