// (C) ncc_accel platform driver — EOS Korak 3+5 (skelet + mmap)
//
// Dve nezavisne instance (ncc0 @ 0x5000_0000 / 0x5002_0000, ncc1 @ 0x5100_0000 /
// 0x5102_0000), svaka sa dva AXI resursa iz device tree-a:
//   reg[0] = S00 (AXI-Lite, kontrola, 4K)
//   reg[1] = S01 (AXI-Full, memorije: slika/šablon/rezultat, 128K)
// Vidi EOS Vault/CLAUDE.md, "Hardverski ugovor" za tačan raspored registara/memorije.
//
// compatible string "xlnx,ncc-accel-1.0" POTVRĐEN na pravom hardveru (insmod test
// 2026-09-10: probe() uspešan za obe instance, tačne adrese).
//
// IMG_ADDR (0x10) / TMP_ADDR (0x14) NEMAJU EFEKAT NA HARDVER — utvrđeno, ne
// pretpostavljeno. Direktna provera PSDS RTL izvora (VHDL):
//   - ncc_accel_slave_lite_v1_0_S00_AXI.vhd: oba offseta su obični loopback
//     registri (upis pa čitanje vraća upisanu vrednost), ali entitet nema
//     img_addr/tmp_addr port ka ncc_core — vrednost nikuda ne ide.
//   - Bare-metal ncc_hw.h: "0x10 IMG_ADDR i 0x14 TMP_ADDR su REZERVISANI --
//     ne koristiti."
//   - Jezgro uvek čita sa fiksnih offseta u S01 (slika +0x00000, šablon +0x08000,
//     rezultat +0x10000) — CPU ne bira adrese.
//
// `img_dirty` (SAT-rebuild-skip optimizacija) NE POSTOJI u hardveru. Živi samo u
// SystemC ESL modelu (PEUSN faza, src/ncc.cpp), nikad nije ušla u sintetizovani
// RTL. SAT tabela u ncc_core.vhd jeste realan hardver, ali se gradi BEZUSLOVNO na
// svakom start pulsu. NE koristiti chess-vp/ncc_blok_detaljno.md kao izvor za
// registre — to je PEUSN SystemC virtuelni prototip, ne stvarni Vivado IP.

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/bits.h>

#include "ncc_accel_uapi.h"

#define DRV_NAME	"ncc_accel"
#define NCC_MAX_DEV	2

#define NCC0_S00_BASE	0x50000000UL
#define NCC1_S00_BASE	0x51000000UL

/* Registri S00 (AXI-Lite; prozor je 4K ali je validno samo 0x00-0x3F, jer je
 * xlnx,s00-axi-addr-width = 6 bitova). 0x10/0x14 (IMG_ADDR/TMP_ADDR) su
 * REZERVISANI — potvrđeno u RTL-u da su loopback registri bez efekta na
 * hardver (vidi CLAUDE.md, "Hardverski ugovor"). NE upisivati u njih. */
#define NCC_REG_IMG_W		0x00
#define NCC_REG_IMG_H		0x04
#define NCC_REG_TMP_W		0x08
#define NCC_REG_TMP_H		0x0C
#define NCC_REG_CTRL		0x30	/* upis 1 = start; briše done_sticky */
#define NCC_REG_STATUS		0x34

#define NCC_STATUS_DONE		BIT(0)	/* done_sticky, read-only */
#define NCC_STATUS_BUSY		BIT(1)

#define NCC_CTRL_START		0x1

/* NCC_BRAM_*_OFF dolaze iz ncc_accel_uapi.h (deljeno sa userspace testom). */
#define NCC_BRAM_IMG_MAX	(NCC_BRAM_TMP_OFF - NCC_BRAM_IMG_OFF)	/* 32K */
#define NCC_BRAM_TMP_MAX	(NCC_BRAM_RES_OFF - NCC_BRAM_TMP_OFF)	/* 32K */
#define NCC_BRAM_RES_MAX	(0x20000 - NCC_BRAM_RES_OFF)		/* 64K */

struct ncc_dev {
	struct cdev cdev;
	struct device *device;
	void __iomem *ctrl_base;	/* S00, ioremap-ovan, koristi ga ioctl */
	struct resource *mem_res;	/* S01, fizički resurs za mmap, NIJE ioremap-ovan ovde */
	int minor;

	struct mutex lock;		/* serijalizuje SET_DIMS/START/WAIT_DONE */
	u32 img_w, img_h, tmp_w, tmp_h;
	bool configured;		/* SET_DIMS pozvan bar jednom */
	bool started;			/* START pozvan, WAIT_DONE još nije uspeo */
};

static struct class *ncc_class;
static dev_t ncc_devt_base;
static struct ncc_dev *ncc_devices[NCC_MAX_DEV];

static int ncc_open(struct inode *inode, struct file *filp)
{
	struct ncc_dev *ndev = container_of(inode->i_cdev, struct ncc_dev, cdev);

	filp->private_data = ndev;
	return 0;
}

static int ncc_release(struct inode *inode, struct file *filp)
{
	return 0;
}

// mmap /dev/nccN mapira ceo S01 region (128K: slika+šablon+rezultat na fiksnim
// offsetima) direktno u userspace. Nema copy_to/from_user — aplikacija piše/čita
// mapiranu memoriju kao obican niz, analogno Xil_Out32/In32 iz bare-metal-a.
// pgprot_noncached: ovo je MMIO iza AXI-Full, ne RAM — keširanje bi pokvarilo
// vidljivost upisa/hardvera (isti razlog zašto bare-metal ne kešira ovaj region).
static int ncc_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct ncc_dev *ndev = filp->private_data;
	unsigned long req_size = vma->vm_end - vma->vm_start;
	unsigned long phys_base = ndev->mem_res->start;
	unsigned long region_size = resource_size(ndev->mem_res);

	if (vma->vm_pgoff != 0) {
		dev_err(ndev->device, "mmap: nenulti offset nije podržan (region se uvek mapira od 0x0)\n");
		return -EINVAL;
	}

	if (req_size > region_size) {
		dev_err(ndev->device, "mmap: traženo %lu B > region %lu B\n",
			req_size, region_size);
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (io_remap_pfn_range(vma, vma->vm_start, phys_base >> PAGE_SHIFT,
				req_size, vma->vm_page_prot)) {
		dev_err(ndev->device, "mmap: io_remap_pfn_range neuspešan\n");
		return -EAGAIN;
	}

	return 0;
}

static inline u32 ncc_rd(struct ncc_dev *ndev, u32 off)
{
	return ioread32(ndev->ctrl_base + off);
}

static inline void ncc_wr(struct ncc_dev *ndev, u32 off, u32 val)
{
	iowrite32(val, ndev->ctrl_base + off);
}

/*
 * Anketiranje umesto prekida — device tree nema `interrupts` property ni na
 * jednoj instanci. Referentni slučaj (90x90 slika, 30x30 šablon) je ~10,36M
 * ciklusa; na PL taktu od 100 MHz to je ~104 ms, pa se spava između provera.
 *
 * Uslov je konjunkcija: done_sticky postavljen I busy spušten. Ispravno je i ako
 * hardver nakratko drži oba bita, i ako done_sticky kasni za busy-jem. AKO SVAKI
 * WAIT_DONE ISTIČE U TIMEOUT, prvo osumnjičiti OVAJ uslov pre hardvera — moguće
 * je da bit1 ne postoji kako je dokumentovano; tada skinuti drugi deo uslova.
 */
static int ncc_poll_done(struct ncc_dev *ndev, u32 timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;) {
		u32 st = ncc_rd(ndev, NCC_REG_STATUS);

		if ((st & NCC_STATUS_DONE) && !(st & NCC_STATUS_BUSY))
			return 0;

		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;

		if (fatal_signal_pending(current))
			return -EINTR;

		usleep_range(500, 1500);
	}
}

/*
 * Validacija protiv FIKSNOG rasporeda BRAM-a. Pošto CPU ne bira adrese, jedino
 * ograničenje je da slika, šablon i mapa rezultata stanu u svoje regione.
 * Piksel = 32-bitna reč (CLAUDE.md, "Memorije S01"), NE bajt.
 * NAPOMENA: region SLIKE (32KB = 8192 reči, ~90x90) je obično vezujući limit,
 * ne region rezultata — za mali šablon (npr. 1x1) region rezultata bi dozvolio
 * i do ~128x128 (16384 reči na 64KB/4B), ali slika sama ne staje preko ~90x90.
 */
static int ncc_validate_dims(const struct ncc_dims *d)
{
	u64 img_sz, tmp_sz, res_sz;
	u32 res_w, res_h;

	if (!d->img_w || !d->img_h || !d->tmp_w || !d->tmp_h)
		return -EINVAL;

	if (d->tmp_w > d->img_w || d->tmp_h > d->img_h)
		return -EINVAL;

	/* CLAUDE.md, "Memorije S01": jedan piksel = jedna 32-bitna reč (4B), NE 1B. */
	img_sz = (u64)d->img_w * d->img_h * sizeof(u32);
	tmp_sz = (u64)d->tmp_w * d->tmp_h * sizeof(u32);
	if (img_sz > NCC_BRAM_IMG_MAX || tmp_sz > NCC_BRAM_TMP_MAX)
		return -ERANGE;

	res_w = d->img_w - d->tmp_w + 1;
	res_h = d->img_h - d->tmp_h + 1;
	res_sz = (u64)res_w * res_h * sizeof(u32);	/* Q1.31 po poziciji */
	if (res_sz > NCC_BRAM_RES_MAX)
		return -ERANGE;

	return 0;
}

static long ncc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ncc_dev *ndev = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;

	if (_IOC_TYPE(cmd) != NCC_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&ndev->lock);

	switch (cmd) {
	case NCC_SET_DIMS: {
		struct ncc_dims d;

		if (copy_from_user(&d, uarg, sizeof(d))) {
			ret = -EFAULT;
			break;
		}
		if (d.reserved[0] || d.reserved[1]) {
			ret = -EINVAL;	/* odbij dok polja nemaju značenje */
			break;
		}

		ret = ncc_validate_dims(&d);
		if (ret)
			break;

		/* Samo četiri registra. IMG_ADDR/TMP_ADDR se NE diraju. */
		ncc_wr(ndev, NCC_REG_IMG_W, d.img_w);
		ncc_wr(ndev, NCC_REG_IMG_H, d.img_h);
		ncc_wr(ndev, NCC_REG_TMP_W, d.tmp_w);
		ncc_wr(ndev, NCC_REG_TMP_H, d.tmp_h);

		ndev->img_w = d.img_w;
		ndev->img_h = d.img_h;
		ndev->tmp_w = d.tmp_w;
		ndev->tmp_h = d.tmp_h;
		ndev->configured = true;
		ndev->started = false;
		break;
	}

	case NCC_START:
		if (!ndev->configured) {
			ret = -EINVAL;
			break;
		}
		/* Upis u CTRL briše done_sticky — posle ovoga je STATUS jednoznačan. */
		ncc_wr(ndev, NCC_REG_CTRL, NCC_CTRL_START);
		ndev->started = true;
		break;

	case NCC_WAIT_DONE: {
		u32 timeout_ms;

		if (get_user(timeout_ms, (u32 __user *)uarg)) {
			ret = -EFAULT;
			break;
		}
		if (!ndev->started) {
			ret = -EINVAL;	/* bez START-a bi done_sticky bio zaostao */
			break;
		}
		if (!timeout_ms || timeout_ms > 10000)
			timeout_ms = 1000;	/* ~10x referentnih 104 ms */

		ret = ncc_poll_done(ndev, timeout_ms);
		if (!ret)
			ndev->started = false;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&ndev->lock);
	return ret;
}

static const struct file_operations ncc_fops = {
	.owner = THIS_MODULE,
	.open = ncc_open,
	.release = ncc_release,
	.mmap = ncc_mmap,
	.unlocked_ioctl = ncc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int ncc_accel_probe(struct platform_device *pdev)
{
	struct ncc_dev *ndev;
	struct resource *ctrl_res, *mem_res;
	int minor, ret;

	ctrl_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!ctrl_res || !mem_res) {
		dev_err(&pdev->dev, "probe: očekivana 2 reg resursa (S00, S01), dobijeno manje\n");
		return -EINVAL;
	}

	/* Instanca se određuje po fizičkoj S00 adresi — deterministički, ne po
	 * redosledu probe() poziva (koji device tree ne garantuje). */
	if (ctrl_res->start == NCC0_S00_BASE)
		minor = 0;
	else if (ctrl_res->start == NCC1_S00_BASE)
		minor = 1;
	else {
		dev_err(&pdev->dev, "probe: nepoznata S00 baza 0x%pa (ni ncc0 ni ncc1)\n",
			&ctrl_res->start);
		return -EINVAL;
	}

	ndev = devm_kzalloc(&pdev->dev, sizeof(*ndev), GFP_KERNEL);
	if (!ndev)
		return -ENOMEM;

	ndev->ctrl_base = devm_ioremap_resource(&pdev->dev, ctrl_res);
	if (IS_ERR(ndev->ctrl_base))
		return PTR_ERR(ndev->ctrl_base);

	/* S01 se NAMERNO ne ioremap-uje ovde (nema kernel-side pristupa preko
	 * ovog pokazivača u ovoj verziji) — samo čuvamo resource za mmap, koji radi
	 * direktno sa fizičkom adresom preko io_remap_pfn_range. */
	ndev->mem_res = mem_res;
	ndev->minor = minor;
	mutex_init(&ndev->lock);

	cdev_init(&ndev->cdev, &ncc_fops);
	ndev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&ndev->cdev, MKDEV(MAJOR(ncc_devt_base), minor), 1);
	if (ret) {
		dev_err(&pdev->dev, "probe: cdev_add neuspešan (%d)\n", ret);
		return ret;
	}

	ndev->device = device_create(ncc_class, &pdev->dev,
				      MKDEV(MAJOR(ncc_devt_base), minor),
				      NULL, "ncc%d", minor);
	if (IS_ERR(ndev->device)) {
		cdev_del(&ndev->cdev);
		return PTR_ERR(ndev->device);
	}

	ncc_devices[minor] = ndev;
	platform_set_drvdata(pdev, ndev);

	dev_info(&pdev->dev, "ncc%d: S00 @ %pa (virt %p), S01 @ %pa (%lu B), /dev/ncc%d spreman\n",
		 minor, &ctrl_res->start, ndev->ctrl_base,
		 &mem_res->start, (unsigned long)resource_size(mem_res), minor);

	return 0;
}

/* Kernel 6.12.40 (linux-xlnx, PetaLinux 2025.2): .remove mora da vraća void
 * (include/linux/platform_device.h:245), ne int kao u starijim kernelima. */
static void ncc_accel_remove(struct platform_device *pdev)
{
	struct ncc_dev *ndev = platform_get_drvdata(pdev);

	device_destroy(ncc_class, MKDEV(MAJOR(ncc_devt_base), ndev->minor));
	cdev_del(&ndev->cdev);
	ncc_devices[ndev->minor] = NULL;
}

static const struct of_device_id ncc_accel_of_match[] = {
	/* TODO: potvrditi tačan string protiv generisanog pl.dtsi pre bring-up testa */
	{ .compatible = "xlnx,ncc-accel-1.0" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ncc_accel_of_match);

static struct platform_driver ncc_accel_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = ncc_accel_of_match,
	},
	.probe = ncc_accel_probe,
	.remove = ncc_accel_remove,
};

static int __init ncc_accel_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&ncc_devt_base, 0, NCC_MAX_DEV, DRV_NAME);
	if (ret)
		return ret;

	/* Kernel 6.12.40: class_create() više ne uzima owner argument
	 * (include/linux/device/class.h:228), samo ime. */
	ncc_class = class_create(DRV_NAME);
	if (IS_ERR(ncc_class)) {
		unregister_chrdev_region(ncc_devt_base, NCC_MAX_DEV);
		return PTR_ERR(ncc_class);
	}

	ret = platform_driver_register(&ncc_accel_driver);
	if (ret) {
		class_destroy(ncc_class);
		unregister_chrdev_region(ncc_devt_base, NCC_MAX_DEV);
		return ret;
	}

	return 0;
}

static void __exit ncc_accel_exit(void)
{
	platform_driver_unregister(&ncc_accel_driver);
	class_destroy(ncc_class);
	unregister_chrdev_region(ncc_devt_base, NCC_MAX_DEV);
}

module_init(ncc_accel_init);
module_exit(ncc_accel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan (C)");
MODULE_DESCRIPTION("EOS: platform drajver za ncc_accel (skelet + mmap S01 regiona)");
