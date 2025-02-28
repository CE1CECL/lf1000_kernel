/*
 * Character-device access to raw MTD devices.
 *
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/backing-dev.h>
#include <linux/compat.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/compatmac.h>

#include <asm/uaccess.h>

#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM
#include <linux/io.h>
#include <linux/mtd/nand.h>
#include <linux/delay.h>
#endif
/*
 * Data structure to hold the pointer to the mtd device as well
 * as mode information ofr various use cases.
 */
struct mtd_file_info {
	struct mtd_info *mtd;
	enum mtd_file_modes mode;
#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM
	int  otp_flag;
#endif	
};

static loff_t mtd_lseek (struct file *file, loff_t offset, int orig)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

	switch (orig) {
	case SEEK_SET:
		break;
	case SEEK_CUR:
		offset += file->f_pos;
		break;
	case SEEK_END:
		offset += mtd->size;
		break;
	default:
		return -EINVAL;
	}

	if (offset >= 0 && offset <= mtd->size)
		return file->f_pos = offset;

	return -EINVAL;
}



static int mtd_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	int devnum = minor >> 1;
	int ret = 0;
	struct mtd_info *mtd;
	struct mtd_file_info *mfi;

	DEBUG(MTD_DEBUG_LEVEL0, "MTD_open\n");

	if (devnum >= MAX_MTD_DEVICES)
		return -ENODEV;

	/* You can't open the RO devices RW */
	if ((file->f_mode & FMODE_WRITE) && (minor & 1))
		return -EACCES;

	lock_kernel();
	mtd = get_mtd_device(NULL, devnum);

	if (IS_ERR(mtd)) {
		ret = PTR_ERR(mtd);
		goto out;
	}

	if (mtd->type == MTD_ABSENT) {
		put_mtd_device(mtd);
		ret = -ENODEV;
		goto out;
	}

	if (mtd->backing_dev_info)
		file->f_mapping->backing_dev_info = mtd->backing_dev_info;

	/* You can't open it RW if it's not a writeable device */
	if ((file->f_mode & FMODE_WRITE) && !(mtd->flags & MTD_WRITEABLE)) {
		put_mtd_device(mtd);
		ret = -EACCES;
		goto out;
	}

	mfi = kzalloc(sizeof(*mfi), GFP_KERNEL);
	if (!mfi) {
		put_mtd_device(mtd);
		ret = -ENOMEM;
		goto out;
	}
	mfi->mtd = mtd;
#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM		
	mfi->otp_flag = 0;
#endif	
	file->private_data = mfi;

out:
	unlock_kernel();
	return ret;
} /* mtd_open */

/*====================================================================*/

static int mtd_close(struct inode *inode, struct file *file)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

	DEBUG(MTD_DEBUG_LEVEL0, "MTD_close\n");

#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM		
	mfi->otp_flag = 0;
#endif	

	/* Only sync if opened RW */
	if ((file->f_mode & FMODE_WRITE) && mtd->sync)
		mtd->sync(mtd);

	put_mtd_device(mtd);
	file->private_data = NULL;
	kfree(mfi);

	return 0;
} /* mtd_close */

/* FIXME: This _really_ needs to die. In 2.5, we should lock the
   userspace buffer down and use it directly with readv/writev.
*/
#define MAX_KMALLOC_SIZE 0x20000

static ssize_t mtd_read(struct file *file, char __user *buf, size_t count,loff_t *ppos)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	size_t retlen=0;
	size_t total_retlen=0;
	int ret=0;
	int len;
	char *kbuf;

	DEBUG(MTD_DEBUG_LEVEL0,"MTD_read\n");

	if (*ppos + count > mtd->size)
		count = mtd->size - *ppos;

	if (!count)
		return 0;

	/* FIXME: Use kiovec in 2.5 to lock down the user's buffers
	   and pass them directly to the MTD functions */

	if (count > MAX_KMALLOC_SIZE)
		kbuf=kmalloc(MAX_KMALLOC_SIZE, GFP_KERNEL);
	else
		kbuf=kmalloc(count, GFP_KERNEL);

	if (!kbuf)
		return -ENOMEM;

	while (count) {

		if (count > MAX_KMALLOC_SIZE)
			len = MAX_KMALLOC_SIZE;
		else
			len = count;

		switch (mfi->mode) {
		case MTD_MODE_OTP_FACTORY:
			ret = mtd->read_fact_prot_reg(mtd, *ppos, len, &retlen, kbuf);
			break;
		case MTD_MODE_OTP_USER:
			ret = mtd->read_user_prot_reg(mtd, *ppos, len, &retlen, kbuf);
			break;
		case MTD_MODE_RAW:
		{
			struct mtd_oob_ops ops;

			ops.mode = MTD_OOB_RAW;
			ops.datbuf = kbuf;
			ops.oobbuf = NULL;
			ops.len = len;

			ret = mtd->read_oob(mtd, *ppos, &ops);
			retlen = ops.retlen;
			break;
		}
		default:
			ret = mtd->read(mtd, *ppos, len, &retlen, kbuf);
		}
		/* Nand returns -EBADMSG on ecc errors, but it returns
		 * the data. For our userspace tools it is important
		 * to dump areas with ecc errors !
		 * For kernel internal usage it also might return -EUCLEAN
		 * to signal the caller that a bitflip has occured and has
		 * been corrected by the ECC algorithm.
		 * Userspace software which accesses NAND this way
		 * must be aware of the fact that it deals with NAND
		 */
		if (!ret || (ret == -EUCLEAN) || (ret == -EBADMSG)) {
			*ppos += retlen;
			if (copy_to_user(buf, kbuf, retlen)) {
				kfree(kbuf);
				return -EFAULT;
			}
			else
				total_retlen += retlen;

			count -= retlen;
			buf += retlen;
			if (retlen == 0)
				count = 0;
		}
		else {
			kfree(kbuf);
			return ret;
		}

	}

	kfree(kbuf);
	return total_retlen;
} /* mtd_read */

#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM

#define OTP_SANDISK_PAGESIZE   512
#define OTP_SANDISK_PAGESIZE_SHIFT   9

/* return value negative: something wrong, positive, number of bytes written */
static ssize_t mtd_write_sandisk_otp(struct file *file, const char __user *buf, size_t count,loff_t *ppos)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	char *kbuf;

	uint8_t b1;
	struct nand_chip *chip = mtd->priv;
	uint32_t addr;
	int i;
		
	if(count != OTP_SANDISK_PAGESIZE) {
		printk(KERN_ERR "Only support write to OTP %dbyte per write !\n", OTP_SANDISK_PAGESIZE);
		return -EINVAL;
	}

	kbuf=kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	
	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}
	// printk(KERN_INFO "Program offset at %lld \n", *ppos);
	
	chip->select_chip(mtd, 0);
	udelay(2);
	chip->cmd_ctrl(mtd, NAND_CMD_SEQIN, NAND_CLE);
	
	addr = *ppos;
	
	if((addr & 0x01FF) != 0) {
		printk(KERN_ERR "Error, address is not page aligned, addr = 0x%x\n", addr);
	}
	
#if 0	
	chip->cmd_ctrl(mtd, addr, NAND_ALE);
	chip->cmd_ctrl(mtd, (addr>> 8), NAND_ALE);
	chip->cmd_ctrl(mtd, (addr>> 16), NAND_ALE);
	chip->cmd_ctrl(mtd, (addr>> 24), NAND_ALE);
#else
	addr = addr >> OTP_SANDISK_PAGESIZE_SHIFT;
	chip->cmd_ctrl(mtd, 0, NAND_ALE);
	chip->cmd_ctrl(mtd, (addr>>0), NAND_ALE);	
	chip->cmd_ctrl(mtd, (addr>>8), NAND_ALE);
	chip->cmd_ctrl(mtd, (addr>>16), NAND_ALE);
#endif	
	
	for( i=0; i<OTP_SANDISK_PAGESIZE; i++ )	
		writeb(kbuf[i], chip->IO_ADDR_W);
	
	chip->cmd_ctrl(mtd, NAND_CMD_PAGEPROG, NAND_CLE);

	#if 1
	chip->cmd_ctrl(mtd, NAND_CMD_STATUS, NAND_CLE);
	for(i=0; i<80; i++) {
		b1 = readb(chip->IO_ADDR_R);
		
		/* page successfully written */
		if(b1 & 0x40) {
			*ppos += OTP_SANDISK_PAGESIZE;				
			kfree(kbuf);
			
			/* is it the end of the chip*/
			if(*ppos == mtd->size) {
				mfi->otp_flag = 0;
				printk(KERN_INFO "Driver: Program OTP at the end i=%d !! \n", i);				
			}
			return OTP_SANDISK_PAGESIZE;
		}
		udelay(100);
	}
	#else
	for(i=0; i<80; i++) {
		if(chip->dev_ready(mtd)) {
			*ppos += OTP_SANDISK_PAGESIZE;				
			kfree(kbuf);

			/* is it the end of the chip*/
			if(*ppos == mtd->size) {
				mfi->otp_flag = 0;
			}
			return OTP_SANDISK_PAGESIZE;
		}
		udelay(100);
	}
	#endif

	printk(KERN_INFO "Program offset at %lld time out !! \n", *ppos);
	
	mfi->otp_flag = 0;
	kfree(kbuf);
	return -EFAULT;
}
#endif

static ssize_t mtd_write(struct file *file, const char __user *buf, size_t count,loff_t *ppos)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	char *kbuf;
	size_t retlen;
	size_t total_retlen=0;
	int ret=0;
	int len;

	DEBUG(MTD_DEBUG_LEVEL0,"MTD_write\n");

#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM
	if((strcmp(mtd->name, "Cartridge") == 0)  && (mfi->otp_flag == 1)){
		return mtd_write_sandisk_otp(file, buf, count, ppos);
	}
#endif
	
	if (*ppos == mtd->size)
		return -ENOSPC;

	if (*ppos + count > mtd->size)
		count = mtd->size - *ppos;

	if (!count)
		return 0;

	if (count > MAX_KMALLOC_SIZE)
		kbuf=kmalloc(MAX_KMALLOC_SIZE, GFP_KERNEL);
	else
		kbuf=kmalloc(count, GFP_KERNEL);

	if (!kbuf)
		return -ENOMEM;

	while (count) {

		if (count > MAX_KMALLOC_SIZE)
			len = MAX_KMALLOC_SIZE;
		else
			len = count;

		if (copy_from_user(kbuf, buf, len)) {
			kfree(kbuf);
			return -EFAULT;
		}

		switch (mfi->mode) {
		case MTD_MODE_OTP_FACTORY:
			ret = -EROFS;
			break;
		case MTD_MODE_OTP_USER:
			if (!mtd->write_user_prot_reg) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = mtd->write_user_prot_reg(mtd, *ppos, len, &retlen, kbuf);
			break;

		case MTD_MODE_RAW:
		{
			struct mtd_oob_ops ops;

			ops.mode = MTD_OOB_RAW;
			ops.datbuf = kbuf;
			ops.oobbuf = NULL;
			ops.len = len;

			ret = mtd->write_oob(mtd, *ppos, &ops);
			retlen = ops.retlen;
			break;
		}

		default:
			ret = (*(mtd->write))(mtd, *ppos, len, &retlen, kbuf);
		}
		if (!ret) {
			*ppos += retlen;
			total_retlen += retlen;
			count -= retlen;
			buf += retlen;
		}
		else {
			kfree(kbuf);
			return ret;
		}
	}

	kfree(kbuf);
	return total_retlen;
} /* mtd_write */

/*======================================================================

    IOCTL calls for getting device parameters.

======================================================================*/
static void mtdchar_erase_callback (struct erase_info *instr)
{
	wake_up((wait_queue_head_t *)instr->priv);
}

#ifdef CONFIG_HAVE_MTD_OTP
static int otp_select_filemode(struct mtd_file_info *mfi, int mode)
{
	struct mtd_info *mtd = mfi->mtd;
	int ret = 0;

	switch (mode) {
	case MTD_OTP_FACTORY:
		if (!mtd->read_fact_prot_reg)
			ret = -EOPNOTSUPP;
		else
			mfi->mode = MTD_MODE_OTP_FACTORY;
		break;
	case MTD_OTP_USER:
		if (!mtd->read_fact_prot_reg)
			ret = -EOPNOTSUPP;
		else
			mfi->mode = MTD_MODE_OTP_USER;
		break;
	default:
		ret = -EINVAL;
	case MTD_OTP_OFF:
		break;
	}
	return ret;
}
#else
# define otp_select_filemode(f,m)	-EOPNOTSUPP
#endif

static int mtd_do_writeoob(struct file *file, struct mtd_info *mtd,
	uint64_t start, uint32_t length, void __user *ptr,
	uint32_t __user *retp)
{
	struct mtd_oob_ops ops;
	uint32_t retlen;
	int ret = 0;

	if (!(file->f_mode & FMODE_WRITE))
		return -EPERM;

	if (length > 4096)
		return -EINVAL;

	if (!mtd->write_oob)
		ret = -EOPNOTSUPP;
	else
		ret = access_ok(VERIFY_READ, ptr, length) ? 0 : EFAULT;

	if (ret)
		return ret;

	ops.ooblen = length;
	ops.ooboffs = start & (mtd->oobsize - 1);
	ops.datbuf = NULL;
	ops.mode = MTD_OOB_PLACE;

	if (ops.ooboffs && ops.ooblen > (mtd->oobsize - ops.ooboffs))
		return -EINVAL;

	ops.oobbuf = kmalloc(length, GFP_KERNEL);
	if (!ops.oobbuf)
		return -ENOMEM;

	if (copy_from_user(ops.oobbuf, ptr, length)) {
		kfree(ops.oobbuf);
		return -EFAULT;
	}

	start &= ~((uint64_t)mtd->oobsize - 1);
	ret = mtd->write_oob(mtd, start, &ops);

	if (ops.oobretlen > 0xFFFFFFFFU)
		ret = -EOVERFLOW;
	retlen = ops.oobretlen;
	if (copy_to_user(retp, &retlen, sizeof(length)))
		ret = -EFAULT;

	kfree(ops.oobbuf);
	return ret;
}

static int mtd_do_readoob(struct mtd_info *mtd, uint64_t start,
	uint32_t length, void __user *ptr, uint32_t __user *retp)
{
	struct mtd_oob_ops ops;
	int ret = 0;

	if (length > 4096)
		return -EINVAL;

	if (!mtd->read_oob)
		ret = -EOPNOTSUPP;
	else
		ret = access_ok(VERIFY_WRITE, ptr,
				length) ? 0 : -EFAULT;
	if (ret)
		return ret;

	ops.ooblen = length;
	ops.ooboffs = start & (mtd->oobsize - 1);
	ops.datbuf = NULL;
	ops.mode = MTD_OOB_PLACE;

	if (ops.ooboffs && ops.ooblen > (mtd->oobsize - ops.ooboffs))
		return -EINVAL;

	ops.oobbuf = kmalloc(length, GFP_KERNEL);
	if (!ops.oobbuf)
		return -ENOMEM;

	start &= ~((uint64_t)mtd->oobsize - 1);
	ret = mtd->read_oob(mtd, start, &ops);

	if (put_user(ops.oobretlen, retp))
		ret = -EFAULT;
	else if (ops.oobretlen && copy_to_user(ptr, ops.oobbuf,
					    ops.oobretlen))
		ret = -EFAULT;

	kfree(ops.oobbuf);
	return ret;
}

static int mtd_ioctl(struct inode *inode, struct file *file,
		     u_int cmd, u_long arg)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	void __user *argp = (void __user *)arg;
	int ret = 0;
	u_long size;
	struct mtd_info_user info;

	DEBUG(MTD_DEBUG_LEVEL0, "MTD_ioctl\n");

	size = (cmd & IOCSIZE_MASK) >> IOCSIZE_SHIFT;
	if (cmd & IOC_IN) {
		if (!access_ok(VERIFY_READ, argp, size))
			return -EFAULT;
	}
	if (cmd & IOC_OUT) {
		if (!access_ok(VERIFY_WRITE, argp, size))
			return -EFAULT;
	}

	switch (cmd) {
	case MEMGETREGIONCOUNT:
		if (copy_to_user(argp, &(mtd->numeraseregions), sizeof(int)))
			return -EFAULT;
		break;

	case MEMGETREGIONINFO:
	{
		uint32_t ur_idx;
		struct mtd_erase_region_info *kr;
		struct region_info_user *ur = (struct region_info_user *) argp;

		if (get_user(ur_idx, &(ur->regionindex)))
			return -EFAULT;

		kr = &(mtd->eraseregions[ur_idx]);

		if (put_user(kr->offset, &(ur->offset))
		    || put_user(kr->erasesize, &(ur->erasesize))
		    || put_user(kr->numblocks, &(ur->numblocks)))
			return -EFAULT;

		break;
	}

	case MEMGETINFO:
		info.type	= mtd->type;
		info.flags	= mtd->flags;
		info.size	= mtd->size;
		info.erasesize	= mtd->erasesize;
		info.writesize	= mtd->writesize;
		info.oobsize	= mtd->oobsize;
		/* The below fields are obsolete */
		info.ecctype	= -1;
		info.eccsize	= 0;
		if (copy_to_user(argp, &info, sizeof(struct mtd_info_user)))
			return -EFAULT;
		break;

	case MEMERASE:
	case MEMERASE64:
	{
		struct erase_info *erase;

		if(!(file->f_mode & FMODE_WRITE))
			return -EPERM;

		erase=kzalloc(sizeof(struct erase_info),GFP_KERNEL);
		if (!erase)
			ret = -ENOMEM;
		else {
			wait_queue_head_t waitq;
			DECLARE_WAITQUEUE(wait, current);

			init_waitqueue_head(&waitq);

			if (cmd == MEMERASE64) {
				struct erase_info_user64 einfo64;

				if (copy_from_user(&einfo64, argp,
					    sizeof(struct erase_info_user64))) {
					kfree(erase);
					return -EFAULT;
				}
				erase->addr = einfo64.start;
				erase->len = einfo64.length;
			} else {
				struct erase_info_user einfo32;

				if (copy_from_user(&einfo32, argp,
					    sizeof(struct erase_info_user))) {
					kfree(erase);
					return -EFAULT;
				}
				erase->addr = einfo32.start;
				erase->len = einfo32.length;
			}
			erase->mtd = mtd;
			erase->callback = mtdchar_erase_callback;
			erase->priv = (unsigned long)&waitq;

			/*
			  FIXME: Allow INTERRUPTIBLE. Which means
			  not having the wait_queue head on the stack.

			  If the wq_head is on the stack, and we
			  leave because we got interrupted, then the
			  wq_head is no longer there when the
			  callback routine tries to wake us up.
			*/
			ret = mtd->erase(mtd, erase);
			if (!ret) {
				set_current_state(TASK_UNINTERRUPTIBLE);
				add_wait_queue(&waitq, &wait);
				if (erase->state != MTD_ERASE_DONE &&
				    erase->state != MTD_ERASE_FAILED)
					schedule();
				remove_wait_queue(&waitq, &wait);
				set_current_state(TASK_RUNNING);

				ret = (erase->state == MTD_ERASE_FAILED)?-EIO:0;
			}
			kfree(erase);
		}
		break;
	}

	case MEMWRITEOOB:
	{
		struct mtd_oob_buf buf;
		struct mtd_oob_buf __user *buf_user = argp;

		/* NOTE: writes return length to buf_user->length */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_writeoob(file, mtd, buf.start, buf.length,
				buf.ptr, &buf_user->length);
		break;
	}

	case MEMREADOOB:
	{
		struct mtd_oob_buf buf;
		struct mtd_oob_buf __user *buf_user = argp;

		/* NOTE: writes return length to buf_user->start */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_readoob(mtd, buf.start, buf.length,
				buf.ptr, &buf_user->start);
		break;
	}

	case MEMWRITEOOB64:
	{
		struct mtd_oob_buf64 buf;
		struct mtd_oob_buf64 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_writeoob(file, mtd, buf.start, buf.length,
				(void __user *)(uintptr_t)buf.usr_ptr,
				&buf_user->length);
		break;
	}

	case MEMREADOOB64:
	{
		struct mtd_oob_buf64 buf;
		struct mtd_oob_buf64 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_readoob(mtd, buf.start, buf.length,
				(void __user *)(uintptr_t)buf.usr_ptr,
				&buf_user->length);
		break;
	}

	case MEMLOCK:
	{
		struct erase_info_user einfo;

		if (copy_from_user(&einfo, argp, sizeof(einfo)))
			return -EFAULT;

		if (!mtd->lock)
			ret = -EOPNOTSUPP;
		else
			ret = mtd->lock(mtd, einfo.start, einfo.length);
		break;
	}

	case MEMUNLOCK:
	{
		struct erase_info_user einfo;

		if (copy_from_user(&einfo, argp, sizeof(einfo)))
			return -EFAULT;

		if (!mtd->unlock)
			ret = -EOPNOTSUPP;
		else
			ret = mtd->unlock(mtd, einfo.start, einfo.length);
		break;
	}

	/* Legacy interface */
	case MEMGETOOBSEL:
	{
		struct nand_oobinfo oi;

		if (!mtd->ecclayout)
			return -EOPNOTSUPP;
		if (mtd->ecclayout->eccbytes > ARRAY_SIZE(oi.eccpos))
			return -EINVAL;

		oi.useecc = MTD_NANDECC_AUTOPLACE;
		memcpy(&oi.eccpos, mtd->ecclayout->eccpos, sizeof(oi.eccpos));
		memcpy(&oi.oobfree, mtd->ecclayout->oobfree,
		       sizeof(oi.oobfree));
		oi.eccbytes = mtd->ecclayout->eccbytes;

		if (copy_to_user(argp, &oi, sizeof(struct nand_oobinfo)))
			return -EFAULT;
		break;
	}

	case MEMGETBADBLOCK:
	{
		loff_t offs;

		if (copy_from_user(&offs, argp, sizeof(loff_t)))
			return -EFAULT;
		if (!mtd->block_isbad)
			ret = -EOPNOTSUPP;
		else
			return mtd->block_isbad(mtd, offs);
		break;
	}

	case MEMSETBADBLOCK:
	{
		loff_t offs;

		if (copy_from_user(&offs, argp, sizeof(loff_t)))
			return -EFAULT;
		if (!mtd->block_markbad)
			ret = -EOPNOTSUPP;
		else
			return mtd->block_markbad(mtd, offs);
		break;
	}

#ifdef CONFIG_HAVE_MTD_OTP
	case OTPSELECT:
	{
		int mode;
		if (copy_from_user(&mode, argp, sizeof(int)))
			return -EFAULT;

		mfi->mode = MTD_MODE_NORMAL;

		ret = otp_select_filemode(mfi, mode);

		file->f_pos = 0;
		break;
	}

	case OTPGETREGIONCOUNT:
	case OTPGETREGIONINFO:
	{
		struct otp_info *buf = kmalloc(4096, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		ret = -EOPNOTSUPP;
		switch (mfi->mode) {
		case MTD_MODE_OTP_FACTORY:
			if (mtd->get_fact_prot_info)
				ret = mtd->get_fact_prot_info(mtd, buf, 4096);
			break;
		case MTD_MODE_OTP_USER:
			if (mtd->get_user_prot_info)
				ret = mtd->get_user_prot_info(mtd, buf, 4096);
			break;
		default:
			break;
		}
		if (ret >= 0) {
			if (cmd == OTPGETREGIONCOUNT) {
				int nbr = ret / sizeof(struct otp_info);
				ret = copy_to_user(argp, &nbr, sizeof(int));
			} else
				ret = copy_to_user(argp, buf, ret);
			if (ret)
				ret = -EFAULT;
		}
		kfree(buf);
		break;
	}

	case OTPLOCK:
	{
		struct otp_info oinfo;

		if (mfi->mode != MTD_MODE_OTP_USER)
			return -EINVAL;
		if (copy_from_user(&oinfo, argp, sizeof(oinfo)))
			return -EFAULT;
		if (!mtd->lock_user_prot_reg)
			return -EOPNOTSUPP;
		ret = mtd->lock_user_prot_reg(mtd, oinfo.start, oinfo.length);
		break;
	}
#endif

#ifdef CONFIG_MTD_OTP_SANDISK_PROGRAM
	case SDOTPPREP:
	{
		uint8_t b1, b2;
		struct nand_chip *chip;
	
		chip = mtd->priv;
		if(!chip) {
			printk(KERN_ERR "SDOTPPREP, mtd->priv null !!\n");
			return -EOPNOTSUPP;
		}
		
		// printk(KERN_INFO "page_shift=0x%x, pagemask = 0x%x, \n", chip->page_shift, chip->pagemask);

		
		chip->select_chip(mtd, 0);         /* 0: select chip, -1: deselect chip */

		/* first verify that the mtd is the right device type */
		chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
		udelay(10);
		chip->cmdfunc(mtd, NAND_CMD_READID, 0x00, -1);
		b1 = chip->read_byte(mtd);
		b2 = chip->read_byte(mtd);
		if ((0x45 != b1) ||  (0x76 != b2)) {
			printk(KERN_ERR "Not SanDisk OTP chip, b1 = 0x%u b2=0x%u\n", b1, b2);
			return -EOPNOTSUPP;
		}
		
		/* Reset */
		chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);

		// wait for 10 microseconds		
		udelay(10);

		/* Read ID 3x */
		#if 0
		chip->cmdfunc(mtd, NAND_CMD_READID, 0x00, -1);
		chip->cmdfunc(mtd, NAND_CMD_READID, 0x00, -1);
		chip->cmdfunc(mtd, NAND_CMD_READID, 0x00, -1);
		#else
		chip->cmd_ctrl(mtd, NAND_CMD_READID, NAND_CLE);
		chip->cmd_ctrl(mtd, NAND_CMD_READID, NAND_CLE);
		chip->cmd_ctrl(mtd, NAND_CMD_READID, NAND_CLE);		
		#endif
		
		mfi->otp_flag = 1;
		
		break;
	}
#endif
	
	case ECCGETLAYOUT:
	{
		if (!mtd->ecclayout)
			return -EOPNOTSUPP;

		if (copy_to_user(argp, mtd->ecclayout,
				 sizeof(struct nand_ecclayout)))
			return -EFAULT;
		break;
	}

	case ECCGETSTATS:
	{
		if (copy_to_user(argp, &mtd->ecc_stats,
				 sizeof(struct mtd_ecc_stats)))
			return -EFAULT;
		break;
	}

	case MTDFILEMODE:
	{
		mfi->mode = 0;

		switch(arg) {
		case MTD_MODE_OTP_FACTORY:
		case MTD_MODE_OTP_USER:
			ret = otp_select_filemode(mfi, arg);
			break;

		case MTD_MODE_RAW:
			if (!mtd->read_oob || !mtd->write_oob)
				return -EOPNOTSUPP;
			mfi->mode = arg;

		case MTD_MODE_NORMAL:
			break;
		default:
			ret = -EINVAL;
		}
		file->f_pos = 0;
		break;
	}
	
	case BBTERASE:
	{
		void bbt_erase (struct mtd_info *mtd);
		/*** This code lifted from mtdpart.c ***/
		/* Our partition node structure */
		struct mtd_part {
			struct mtd_info mtd;
			struct mtd_info *master;
			u_int32_t offset;
			int index;
			struct list_head list;
			int registered;
		};

#define PART(x)  ((struct mtd_part *)(x))
		struct mtd_part *part = PART(mtd);
		struct mtd_info *master = part->master;
		/*** End of lifting from mtdpart.c ***/
		bbt_erase (master);
		break;
	}

	case BBTSCAN:
	{
		void bbt_scan (struct mtd_info *mtd);
		/*** This code lifted from mtdpart.c ***/
		/* Our partition node structure */
		struct mtd_part {
			struct mtd_info mtd;
			struct mtd_info *master;
			u_int32_t offset;
			int index;
			struct list_head list;
			int registered;
		};

		struct mtd_part *part = PART(mtd);
		struct mtd_info *master = part->master;
		/*** End of lifting from mtdpart.c ***/
		bbt_scan (master);
		break;
	}


	default:
		ret = -ENOTTY;
	}

	return ret;
} /* memory_ioctl */

#ifdef CONFIG_COMPAT

struct mtd_oob_buf32 {
	u_int32_t start;
	u_int32_t length;
	compat_caddr_t ptr;	/* unsigned char* */
};

#define MEMWRITEOOB32		_IOWR('M', 3, struct mtd_oob_buf32)
#define MEMREADOOB32		_IOWR('M', 4, struct mtd_oob_buf32)

static long mtd_compat_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	void __user *argp = compat_ptr(arg);
	int ret = 0;

	lock_kernel();

	switch (cmd) {
	case MEMWRITEOOB32:
	{
		struct mtd_oob_buf32 buf;
		struct mtd_oob_buf32 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_writeoob(file, mtd, buf.start,
				buf.length, compat_ptr(buf.ptr),
				&buf_user->length);
		break;
	}

	case MEMREADOOB32:
	{
		struct mtd_oob_buf32 buf;
		struct mtd_oob_buf32 __user *buf_user = argp;

		/* NOTE: writes return length to buf->start */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtd_do_readoob(mtd, buf.start,
				buf.length, compat_ptr(buf.ptr),
				&buf_user->start);
		break;
	}
	default:
		ret = mtd_ioctl(inode, file, cmd, (unsigned long)argp);
	}

	unlock_kernel();

	return ret;
}

#endif /* CONFIG_COMPAT */

/*
 * try to determine where a shared mapping can be made
 * - only supported for NOMMU at the moment (MMU can't doesn't copy private
 *   mappings)
 */
#ifndef CONFIG_MMU
static unsigned long mtd_get_unmapped_area(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

	if (mtd->get_unmapped_area) {
		unsigned long offset;

		if (addr != 0)
			return (unsigned long) -EINVAL;

		if (len > mtd->size || pgoff >= (mtd->size >> PAGE_SHIFT))
			return (unsigned long) -EINVAL;

		offset = pgoff << PAGE_SHIFT;
		if (offset > mtd->size - len)
			return (unsigned long) -EINVAL;

		return mtd->get_unmapped_area(mtd, len, offset, flags);
	}

	/* can't map directly */
	return (unsigned long) -ENOSYS;
}
#endif

/*
 * set up a mapping for shared memory segments
 */
static int mtd_mmap(struct file *file, struct vm_area_struct *vma)
{
#ifdef CONFIG_MMU
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

	if (mtd->type == MTD_RAM || mtd->type == MTD_ROM)
		return 0;
	return -ENOSYS;
#else
	return vma->vm_flags & VM_SHARED ? 0 : -ENOSYS;
#endif
}

static const struct file_operations mtd_fops = {
	.owner		= THIS_MODULE,
	.llseek		= mtd_lseek,
	.read		= mtd_read,
	.write		= mtd_write,
	.ioctl		= mtd_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= mtd_compat_ioctl,
#endif
	.open		= mtd_open,
	.release	= mtd_close,
	.mmap		= mtd_mmap,
#ifndef CONFIG_MMU
	.get_unmapped_area = mtd_get_unmapped_area,
#endif
};

static int __init init_mtdchar(void)
{
	int status;

	status = register_chrdev(MTD_CHAR_MAJOR, "mtd", &mtd_fops);
	if (status < 0) {
		printk(KERN_NOTICE "Can't allocate major number %d for Memory Technology Devices.\n",
		       MTD_CHAR_MAJOR);
	}

	return status;
}

static void __exit cleanup_mtdchar(void)
{
	unregister_chrdev(MTD_CHAR_MAJOR, "mtd");
}

module_init(init_mtdchar);
module_exit(cleanup_mtdchar);

MODULE_ALIAS_CHARDEV_MAJOR(MTD_CHAR_MAJOR);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("Direct character-device access to MTD devices");
MODULE_ALIAS_CHARDEV_MAJOR(MTD_CHAR_MAJOR);
