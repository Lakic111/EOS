// Drajver za NCC akcelerator (dve instance: /dev/ncc0 i /dev/ncc1).
//
// Komande (write):
//   echo "dims 90 90 25 15" > /dev/ncc0    sirina/visina slike i sablona
//   echo "start" > /dev/ncc0               pokretanje obrade
// Status (read):
//   cat /dev/ncc0                          ispisuje "done=1 busy=0"
// Podaci (mmap): ceo S01 prostor od 128 KB
//   slika +0x00000, sablon +0x08000, rezultat +0x10000, jedan piksel = jedna 32-bitna rec

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/mm.h>

#include <linux/io.h>              // iowrite32, ioread32
#include <linux/slab.h>            // kmalloc, kfree
#include <linux/platform_device.h> // platform driver
#include <linux/of.h>              // of_match_table
#include <linux/ioport.h>          // request_mem_region

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("y25-g10");
MODULE_DESCRIPTION("Drajver za NCC akcelerator");

#define DRIVER_NAME  "ncc_driver"
#define BROJ_NCC     2
#define BUFF_SIZE    64

// registri na S00
#define REG_IMG_W    0x00
#define REG_IMG_H    0x04
#define REG_TMP_W    0x08
#define REG_TMP_H    0x0C
#define REG_CTRL     0x30   // upis 1 = start
#define REG_STATUS   0x34   // bit 0 = done, bit 1 = busy

// fizicke adrese S00 po kojima se razlikuju dve instance
#define NCC0_S00     0x50000000
#define NCC1_S00     0x51000000

struct ncc_info {
	unsigned long mem_start;    // S00, registri
	unsigned long mem_end;
	void __iomem *base_addr;
	unsigned long bram_start;   // S01, memorije (samo za mmap)
	unsigned long bram_end;
};

static dev_t my_dev_id;
static struct class *my_class;
static struct cdev *my_cdev;
static struct ncc_info *ncc[BROJ_NCC];
static struct semaphore sem[BROJ_NCC];

static int ncc_probe(struct platform_device *pdev);
static void ncc_remove(struct platform_device *pdev);
static int ncc_open(struct inode *pinode, struct file *pfile);
static int ncc_close(struct inode *pinode, struct file *pfile);
static ssize_t ncc_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
static ssize_t ncc_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);
static int ncc_mmap(struct file *pfile, struct vm_area_struct *vma);
static int __init ncc_init(void);
static void __exit ncc_exit(void);

static struct file_operations my_fops = {
	.owner = THIS_MODULE,
	.open = ncc_open,
	.release = ncc_close,
	.read = ncc_read,
	.write = ncc_write,
	.mmap = ncc_mmap,
};

static struct of_device_id ncc_of_match[] = {
	{ .compatible = "xlnx,ncc-accel-1.0", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, ncc_of_match);

static struct platform_driver ncc_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = ncc_of_match,
	},
	.probe = ncc_probe,
	.remove = ncc_remove,
};

// Redni broj instance (0 ili 1) po fizickoj adresi S00, -1 ako je nepoznata.
static int broj_instance(unsigned long s00)
{
	if (s00 == NCC0_S00)
		return 0;
	if (s00 == NCC1_S00)
		return 1;
	return -1;
}

//***************************************************************************
// PROBE I REMOVE -- poziva se jednom za svaku instancu iz device tree-a
static int ncc_probe(struct platform_device *pdev)
{
	struct resource *r_mem, *r_bram;
	struct ncc_info *np;
	int n;
	int rc = 0;

	r_mem  = platform_get_resource(pdev, IORESOURCE_MEM, 0);   // S00
	r_bram = platform_get_resource(pdev, IORESOURCE_MEM, 1);   // S01
	if (!r_mem || !r_bram) {
		printk(KERN_ALERT "ncc_probe: nedostaje reg resurs\n");
		return -ENODEV;
	}

	n = broj_instance(r_mem->start);
	if (n < 0) {
		printk(KERN_ALERT "ncc_probe: nepoznata adresa %lx\n", (unsigned long)r_mem->start);
		return -ENODEV;
	}

	np = kmalloc(sizeof(struct ncc_info), GFP_KERNEL);
	if (!np) {
		printk(KERN_ALERT "ncc_probe: kmalloc nije uspeo\n");
		return -ENOMEM;
	}
	np->mem_start  = r_mem->start;
	np->mem_end    = r_mem->end;
	np->bram_start = r_bram->start;
	np->bram_end   = r_bram->end;

	if (!request_mem_region(np->mem_start, np->mem_end - np->mem_start + 1, DRIVER_NAME)) {
		printk(KERN_ALERT "ncc_probe: S00 je zauzet\n");
		rc = -EBUSY;
		goto error1;
	}
	if (!request_mem_region(np->bram_start, np->bram_end - np->bram_start + 1, DRIVER_NAME)) {
		printk(KERN_ALERT "ncc_probe: S01 je zauzet\n");
		rc = -EBUSY;
		goto error2;
	}

	np->base_addr = ioremap(np->mem_start, np->mem_end - np->mem_start + 1);
	if (!np->base_addr) {
		printk(KERN_ALERT "ncc_probe: ioremap nije uspeo\n");
		rc = -EIO;
		goto error3;
	}

	ncc[n] = np;
	printk(KERN_INFO "ncc_probe: ncc%d, S00 %lx, S01 %lx\n", n, np->mem_start, np->bram_start);
	return 0;

error3:
	release_mem_region(np->bram_start, np->bram_end - np->bram_start + 1);
error2:
	release_mem_region(np->mem_start, np->mem_end - np->mem_start + 1);
error1:
	kfree(np);
	return rc;
}

// U kernelu 6.12 (PetaLinux 2025.2) remove vraca void, a ne int kao u vezbama.
static void ncc_remove(struct platform_device *pdev)
{
	struct resource *r_mem;
	int n;

	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	n = broj_instance(r_mem->start);
	if (n < 0 || !ncc[n])
		return;

	iounmap(ncc[n]->base_addr);
	release_mem_region(ncc[n]->bram_start, ncc[n]->bram_end - ncc[n]->bram_start + 1);
	release_mem_region(ncc[n]->mem_start, ncc[n]->mem_end - ncc[n]->mem_start + 1);
	kfree(ncc[n]);
	ncc[n] = NULL;
	printk(KERN_INFO "ncc_remove: ncc%d uklonjen\n", n);
}

//***************************************************************************
// FILE OPERATIONS
static int ncc_open(struct inode *pinode, struct file *pfile)
{
	return 0;
}

static int ncc_close(struct inode *pinode, struct file *pfile)
{
	return 0;
}

// Ispisuje stanje jezgra. Drugo citanje istog otvorenog fajla vraca 0 (kraj),
// isto kao endRead u vezbi 9, pa radi i "cat /dev/ncc0".
static ssize_t ncc_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset)
{
	int minor = MINOR(pfile->f_inode->i_rdev);
	char buff[BUFF_SIZE];
	u32 status;
	int len;

	if (*offset > 0)
		return 0;
	if (!ncc[minor])
		return -ENODEV;

	status = ioread32(ncc[minor]->base_addr + REG_STATUS);
	len = sprintf(buff, "done=%d busy=%d\n", status & 1, (status >> 1) & 1);
	if (length < len)
		return -EINVAL;
	if (copy_to_user(buffer, buff, len))
		return -EFAULT;

	*offset = len;
	return len;
}

// Komande "dims iw ih tw th" i "start". Dok jezgro radi obe se odbijaju
// (-EBUSY): jezgro bi start tada samo ignorisalo, a nove dimenzije bi
// promenile posao koji je u toku.
static ssize_t ncc_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset)
{
	int minor = MINOR(pfile->f_inode->i_rdev);
	char buff[BUFF_SIZE];
	int iw, ih, tw, th;
	int ret = length;

	if (!ncc[minor])
		return -ENODEV;
	if (length >= BUFF_SIZE)
		return -EINVAL;
	if (copy_from_user(buff, buffer, length))
		return -EFAULT;
	buff[length] = '\0';

	if (down_interruptible(&sem[minor]))
		return -ERESTARTSYS;

	if (ioread32(ncc[minor]->base_addr + REG_STATUS) & 2) {
		printk(KERN_WARNING "ncc%d: jezgro je zauzeto\n", minor);
		ret = -EBUSY;
	}
	else if (sscanf(buff, "dims %d %d %d %d", &iw, &ih, &tw, &th) == 4) {
		// granice koje jezgro podrzava (ncc_core.vhd)
		if (iw < 1 || iw > 90 || ih < 1 || ih > 90 ||
		    tw < 1 || tw > 30 || th < 1 || th > 30 || tw > iw || th > ih) {
			printk(KERN_WARNING "ncc%d: dimenzije van opsega\n", minor);
			ret = -EINVAL;
		}
		else {
			iowrite32(iw, ncc[minor]->base_addr + REG_IMG_W);
			iowrite32(ih, ncc[minor]->base_addr + REG_IMG_H);
			iowrite32(tw, ncc[minor]->base_addr + REG_TMP_W);
			iowrite32(th, ncc[minor]->base_addr + REG_TMP_H);
		}
	}
	else if (strncmp(buff, "start", 5) == 0) {
		iowrite32(1, ncc[minor]->base_addr + REG_CTRL);   // brise i done
	}
	else {
		printk(KERN_WARNING "ncc%d: pogresna komanda\n", minor);
		ret = -EINVAL;
	}

	up(&sem[minor]);
	return ret;
}

// Mapira memorije akceleratora (S01) u adresni prostor aplikacije.
// Za razliku od vezbe 12 ovo nije DMA bafer u RAM-u nego memorija uredjaja,
// pa se koristi remap_pfn_range, bez kesa (pgprot_noncached).
static int ncc_mmap(struct file *pfile, struct vm_area_struct *vma)
{
	int minor = MINOR(pfile->f_inode->i_rdev);
	unsigned long length = vma->vm_end - vma->vm_start;

	if (!ncc[minor])
		return -ENODEV;
	if (length > ncc[minor]->bram_end - ncc[minor]->bram_start + 1) {
		printk(KERN_ERR "ncc%d: mmap veci od 128 KB\n", minor);
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, ncc[minor]->bram_start >> PAGE_SHIFT,
			       length, vma->vm_page_prot);
}

//***************************************************************************
// INIT I EXIT
static int __init ncc_init(void)
{
	int ret = 0;
	int i;

	for (i = 0; i < BROJ_NCC; i++) {
		ncc[i] = NULL;
		sema_init(&sem[i], 1);
	}

	ret = alloc_chrdev_region(&my_dev_id, 0, BROJ_NCC, DRIVER_NAME);
	if (ret) {
		printk(KERN_ERR "ncc_init: alloc_chrdev_region nije uspeo\n");
		return ret;
	}

	// U kernelu 6.12 class_create prima samo ime (bez THIS_MODULE).
	my_class = class_create("ncc_class");
	if (IS_ERR(my_class)) {
		printk(KERN_ERR "ncc_init: class_create nije uspeo\n");
		goto fail_0;
	}

	// dva cvora, /dev/ncc0 i /dev/ncc1 (vise minor brojeva, kao vezba 6)
	if (IS_ERR(device_create(my_class, NULL, MKDEV(MAJOR(my_dev_id), 0), NULL, "ncc0")))
		goto fail_1;
	if (IS_ERR(device_create(my_class, NULL, MKDEV(MAJOR(my_dev_id), 1), NULL, "ncc1")))
		goto fail_2;

	my_cdev = cdev_alloc();
	my_cdev->ops = &my_fops;
	my_cdev->owner = THIS_MODULE;
	ret = cdev_add(my_cdev, my_dev_id, BROJ_NCC);
	if (ret) {
		printk(KERN_ERR "ncc_init: cdev_add nije uspeo\n");
		goto fail_3;
	}

	return platform_driver_register(&ncc_driver);

fail_3:
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id), 1));
fail_2:
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id), 0));
fail_1:
	class_destroy(my_class);
fail_0:
	unregister_chrdev_region(my_dev_id, BROJ_NCC);
	return -1;
}

static void __exit ncc_exit(void)
{
	platform_driver_unregister(&ncc_driver);
	cdev_del(my_cdev);
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id), 1));
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id), 0));
	class_destroy(my_class);
	unregister_chrdev_region(my_dev_id, BROJ_NCC);
}

module_init(ncc_init);
module_exit(ncc_exit);
