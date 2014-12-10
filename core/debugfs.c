/*
 * Debugfs support for hosts and cards
 *
 * Copyright (C) 2008 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/moduleparam.h>
#include <linux/export.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/fault-inject.h>
#include <linux/printk.h>

#include <linux/scatterlist.h>
#include <linux/mmc/core.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <asm/uaccess.h>


#include "core.h"
#include "mmc_ops.h"

#define RESULT_OK		0
#define RESULT_FAIL		1
#define RESULT_UNSUP_HOST	2
#define RESULT_UNSUP_CARD	3

#define BUFFER_ORDER		2
#define BUFFER_SIZE		(PAGE_SIZE << BUFFER_ORDER)

#define MMC_SET_HYNIX_SPICIFIC_CMD     60   /* ac   [31:16] RCA        R1  */	
#define MMC_SET_HYNIX_ARG_FIRST     0x534D4900
#define MMC_SET_HYNIX_ARG_SECOND    0x48525054


#ifdef CONFIG_FAIL_MMC_REQUEST

static DECLARE_FAULT_ATTR(fail_default_attr);
static char *fail_request;
module_param(fail_request, charp, 0);

#endif /* CONFIG_FAIL_MMC_REQUEST */

/* The debugfs functions are optimized away when CONFIG_DEBUG_FS isn't set. */
static int mmc_ios_show(struct seq_file *s, void *data)
{
	static const char *vdd_str[] = {
		[8]	= "2.0",
		[9]	= "2.1",
		[10]	= "2.2",
		[11]	= "2.3",
		[12]	= "2.4",
		[13]	= "2.5",
		[14]	= "2.6",
		[15]	= "2.7",
		[16]	= "2.8",
		[17]	= "2.9",
		[18]	= "3.0",
		[19]	= "3.1",
		[20]	= "3.2",
		[21]	= "3.3",
		[22]	= "3.4",
		[23]	= "3.5",
		[24]	= "3.6",
	};
	struct mmc_host	*host = s->private;
	struct mmc_ios	*ios = &host->ios;
	const char *str;

	seq_printf(s, "clock:\t\t%u Hz\n", ios->clock);
	if (host->actual_clock)
		seq_printf(s, "actual clock:\t%u Hz\n", host->actual_clock);
	seq_printf(s, "vdd:\t\t%u ", ios->vdd);
	if ((1 << ios->vdd) & MMC_VDD_165_195)
		seq_printf(s, "(1.65 - 1.95 V)\n");
	else if (ios->vdd < (ARRAY_SIZE(vdd_str) - 1)
			&& vdd_str[ios->vdd] && vdd_str[ios->vdd + 1])
		seq_printf(s, "(%s ~ %s V)\n", vdd_str[ios->vdd],
				vdd_str[ios->vdd + 1]);
	else
		seq_printf(s, "(invalid)\n");

	switch (ios->bus_mode) {
	case MMC_BUSMODE_OPENDRAIN:
		str = "open drain";
		break;
	case MMC_BUSMODE_PUSHPULL:
		str = "push-pull";
		break;
	default:
		str = "invalid";
		break;
	}
	seq_printf(s, "bus mode:\t%u (%s)\n", ios->bus_mode, str);

	switch (ios->chip_select) {
	case MMC_CS_DONTCARE:
		str = "don't care";
		break;
	case MMC_CS_HIGH:
		str = "active high";
		break;
	case MMC_CS_LOW:
		str = "active low";
		break;
	default:
		str = "invalid";
		break;
	}
	seq_printf(s, "chip select:\t%u (%s)\n", ios->chip_select, str);

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		str = "off";
		break;
	case MMC_POWER_UP:
		str = "up";
		break;
	case MMC_POWER_ON:
		str = "on";
		break;
	default:
		str = "invalid";
		break;
	}
	seq_printf(s, "power mode:\t%u (%s)\n", ios->power_mode, str);
	seq_printf(s, "bus width:\t%u (%u bits)\n",
			ios->bus_width, 1 << ios->bus_width);

	switch (ios->timing) {
	case MMC_TIMING_LEGACY:
		str = "legacy";
		break;
	case MMC_TIMING_MMC_HS:
		str = "mmc high-speed";
		break;
	case MMC_TIMING_SD_HS:
		str = "sd high-speed";
		break;
	case MMC_TIMING_UHS_SDR50:
		str = "sd uhs SDR50";
		break;
	case MMC_TIMING_UHS_SDR104:
		str = "sd uhs SDR104";
		break;
	case MMC_TIMING_UHS_DDR50:
		str = "sd uhs DDR50";
		break;
	case MMC_TIMING_MMC_HS200:
		str = "mmc high-speed SDR200";
		break;
	default:
		str = "invalid";
		break;
	}
	seq_printf(s, "timing spec:\t%u (%s)\n", ios->timing, str);

	return 0;
}

static int mmc_ios_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_ios_show, inode->i_private);
}

static const struct file_operations mmc_ios_fops = {
	.open		= mmc_ios_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int mmc_clock_opt_get(void *data, u64 *val)
{
	struct mmc_host *host = data;

	*val = host->ios.clock;

	return 0;
}

static int mmc_clock_opt_set(void *data, u64 val)
{
	struct mmc_host *host = data;

	/* We need this check due to input value is u64 */
	if (val > host->f_max)
		return -EINVAL;

	mmc_rpm_hold(host, &host->class_dev);
	mmc_claim_host(host);
	mmc_set_clock(host, (unsigned int) val);
	mmc_release_host(host);
	mmc_rpm_release(host, &host->class_dev);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(mmc_clock_fops, mmc_clock_opt_get, mmc_clock_opt_set,
	"%llu\n");

static int mmc_max_clock_get(void *data, u64 *val)
{
	struct mmc_host *host = data;

	if (!host)
		return -EINVAL;

	*val = host->f_max;

	return 0;
}

static int mmc_max_clock_set(void *data, u64 val)
{
	struct mmc_host *host = data;
	int err = -EINVAL;
	unsigned long freq = val;
	unsigned int old_freq;

	if (!host || (val < host->f_min))
		goto out;

	mmc_rpm_hold(host, &host->class_dev);
	mmc_claim_host(host);
	if (host->bus_ops && host->bus_ops->change_bus_speed) {
		old_freq = host->f_max;
		host->f_max = freq;

		err = host->bus_ops->change_bus_speed(host, &freq);

		if (err)
			host->f_max = old_freq;
	}
	mmc_release_host(host);
	mmc_rpm_release(host, &host->class_dev);
out:
	return err;
}

DEFINE_SIMPLE_ATTRIBUTE(mmc_max_clock_fops, mmc_max_clock_get,
		mmc_max_clock_set, "%llu\n");

void mmc_add_host_debugfs(struct mmc_host *host)
{
	struct dentry *root;

	root = debugfs_create_dir(mmc_hostname(host), NULL);
	if (IS_ERR(root))
		/* Don't complain -- debugfs just isn't enabled */
		return;
	if (!root)
		/* Complain -- debugfs is enabled, but it failed to
		 * create the directory. */
		goto err_root;

	host->debugfs_root = root;

	if (!debugfs_create_file("ios", S_IRUSR, root, host, &mmc_ios_fops))
		goto err_node;

	if (!debugfs_create_file("clock", S_IRUSR | S_IWUSR, root, host,
			&mmc_clock_fops))
		goto err_node;

	if (!debugfs_create_file("max_clock", S_IRUSR | S_IWUSR, root, host,
		&mmc_max_clock_fops))
		goto err_node;

#ifdef CONFIG_MMC_CLKGATE
	if (!debugfs_create_u32("clk_delay", (S_IRUSR | S_IWUSR),
				root, &host->clk_delay))
		goto err_node;
#endif
#ifdef CONFIG_FAIL_MMC_REQUEST
	if (fail_request)
		setup_fault_attr(&fail_default_attr, fail_request);
	host->fail_mmc_request = fail_default_attr;
	if (IS_ERR(fault_create_debugfs_attr("fail_mmc_request",
					     root,
					     &host->fail_mmc_request)))
		goto err_node;
#endif
	return;

err_node:
	debugfs_remove_recursive(root);
	host->debugfs_root = NULL;
err_root:
	dev_err(&host->class_dev, "failed to initialize debugfs\n");
}

void mmc_remove_host_debugfs(struct mmc_host *host)
{
	debugfs_remove_recursive(host->debugfs_root);
}

static int mmc_dbg_card_status_get(void *data, u64 *val)
{
	struct mmc_card	*card = data;
	u32		status;
	int		ret;

	mmc_rpm_hold(card->host, &card->dev);
	mmc_claim_host(card->host);

	ret = mmc_send_status(data, &status);
	if (!ret)
		*val = status;

	mmc_release_host(card->host);
	mmc_rpm_release(card->host, &card->dev);

	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(mmc_dbg_card_status_fops, mmc_dbg_card_status_get,
		NULL, "%08llx\n");




#ifdef CONFIG_MACH_LGE
/* LGE_CHANGE
* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
* 2012-07-09, J1-FS@lge.com
*/
static int mmc_ext_csd_read(struct seq_file *s, void *data)
#else
#define EXT_CSD_STR_LEN 1025

static int mmc_ext_csd_open(struct inode *inode, struct file *filp)
#endif
{
#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	struct mmc_card *card = s->private;
#else
	struct mmc_card *card = inode->i_private;
	char *buf;
	ssize_t n = 0;
#endif
	u8 *ext_csd;
	u32 *ext_csd2 ;
#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	u8 ext_csd_rev;
	int err, err1 = 0, err2 = 0;
	const char *str;
	char *buf_for_health_report;
	char *buf_for_firmwware_version;
	ssize_t output = 0;
	int cnt;
#else
	int err, i;

	buf = kmalloc(EXT_CSD_STR_LEN + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
#endif
	size_t  size = 512 ;

	if (card->cid.manfid == CID_MANFID_HYNIX)
		size = 1024 ;
	
	ext_csd = kmalloc(size, GFP_KERNEL);
	
	if (!ext_csd) {
		err = -ENOMEM;
		goto out_free;
	}

	memset(ext_csd, 0, size ) ;



	mmc_rpm_hold(card->host, &card->dev);
	mmc_claim_host(card->host);
	err = mmc_send_ext_csd(card, ext_csd);

	if (card->cid.manfid == CID_MANFID_HYNIX)
	{
		ext_csd2 = (u32*)&ext_csd[512] ;
		err1 = mmc_send_maker_cmd(card, MMC_SET_HYNIX_SPICIFIC_CMD, MMC_SET_HYNIX_ARG_FIRST);
		err2 = mmc_send_maker_cmd(card, MMC_SET_HYNIX_SPICIFIC_CMD, MMC_SET_HYNIX_ARG_SECOND);
		err = mmc_send_ext_csd(card, (u8*)ext_csd2);
	}
	
	mmc_release_host(card->host);
	mmc_rpm_release(card->host, &card->dev);
	if (err)
		goto out_free;

	if (err1)
	{
		seq_printf(s, "err1 =  0x%x\n", err1);
		goto out_free;
	}
	if (err2)
	{
		seq_printf(s,"err2 =  0x%x\n", err2);
		goto out_free;
	}

	
#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	ext_csd_rev = ext_csd[192];
#else
	for (i = 511; i >= 0; i--)
		n += sprintf(buf + n, "%02x", ext_csd[i]);
	n += sprintf(buf + n, "\n");
	BUG_ON(n != EXT_CSD_STR_LEN);

	filp->private_data = buf;
	kfree(ext_csd);
	return 0;
#endif

#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/

	switch (ext_csd_rev) {
	case 7:
	       str = "5.0";
	       break;
       case 6:
               str = "4.5";
               break;
       case 5:
               str = "4.41";
               break;
       case 3:
               str = "4.3";
               break;
       case 2:
               str = "4.2";
               break;
       case 1:
               str = "4.1";
               break;
       case 0:
               str = "4.0";
               break;
       default:
               goto out_free;
       }
       seq_printf(s, "Extended CSD rev 1.%d (MMC %s)\n", ext_csd_rev, str);
	   seq_printf(s, "CID.MANFID = 0x%02x \n",card->cid.manfid );

		if (card->cid.manfid == CID_MANFID_HYNIX)
		{
			ext_csd2 = (u32*)&ext_csd[512] ;
			seq_printf(s, "[0] samrt report: %08x\n", ext_csd2[0]);
			seq_printf(s, "[1] samrt report: %08x\n", ext_csd2[1]);
			seq_printf(s, "[2] samrt report: %08x\n", ext_csd2[2]);
			seq_printf(s, "[3] samrt report: %08x\n", ext_csd2[3]);
			seq_printf(s, "[4] samrt report: %08x\n", ext_csd2[4]);
			seq_printf(s, "[5] samrt report: %08x\n", ext_csd2[5]);
			seq_printf(s, "[6] samrt report: %08x\n", ext_csd2[6]);
			seq_printf(s, "[7] samrt report: %08x\n", ext_csd2[7]);
			seq_printf(s, "[8] samrt report: %08x\n", ext_csd2[8]);
			seq_printf(s, "[9] samrt report: %08x\n", ext_csd2[9]);
			seq_printf(s, "[10] samrt report: %08x\n", ext_csd2[10]);
			seq_printf(s, "[11] samrt report: %08x\n", ext_csd2[11]);
			seq_printf(s, "[12] samrt report: %08x\n", ext_csd2[12]);
			seq_printf(s, "[13] samrt report: %08x\n", ext_csd2[13]);
			seq_printf(s, "[14] samrt report: %08x\n", ext_csd2[14]);
			seq_printf(s, "[15] samrt report: %08x\n", ext_csd2[15]);
			seq_printf(s, "[16] samrt report: %08x\n", ext_csd2[16]);
			seq_printf(s, "[17] samrt report: %08x\n", ext_csd2[17]);
			seq_printf(s, "[18] samrt report: %08x\n", ext_csd2[18]);
			seq_printf(s, "[19] samrt report: %08x\n", ext_csd2[19]);
			seq_printf(s, "[20] samrt report: %08x\n", ext_csd2[20]);
			seq_printf(s, "[21] samrt report: %08x\n", ext_csd2[21]);
		
		}
	   

       if (ext_csd_rev < 3)
               goto out_free; /* No ext_csd */

       /* Parse the Extended CSD registers.
        * Reserved bit should be read as "0" in case of spec older
        * than A441.
        */
	if (ext_csd_rev >= 7) {
		seq_printf(s, "[505] Extended Security Commands Error, ext_security_err: 0x%02x\n", ext_csd[505]);
	}
       seq_printf(s, "[504] Supported Command Sets, s_cmd_set: 0x%02x\n", ext_csd[504]);
       seq_printf(s, "[503] HPI features, hpi_features: 0x%02x\n", ext_csd[503]);
       seq_printf(s, "[502] Background operations support, bkops_support: 0x%02x\n", ext_csd[502]);

       if (ext_csd_rev >= 6) {
		seq_printf(s, "[501] Max packed read commands, max_packed_reads: 0x%02x\n", ext_csd[501]);
		seq_printf(s, "[500] Max packed write commands, max_packed_writes: 0x%02x\n", ext_csd[500]);
		seq_printf(s, "[499] Data Tag Support, data_tag_support: 0x%02x\n", ext_csd[499]);
		seq_printf(s, "[498] Tag Unit Size, tag_unit_size: 0x%02x\n", ext_csd[498]);
		seq_printf(s, "[497] Tag Resources Size, tag_res_size: 0x%02x\n", ext_csd[497]);
		seq_printf(s, "[496] Context management capabilities, context_capabilities: 0x%02x\n", ext_csd[496]);
		seq_printf(s, "[495] Large Unit size, large_unit_size_m1: 0x%02x\n", ext_csd[495]);
		seq_printf(s, "[494] Extended partitions attribute support, ext_support: 0x%02x\n", ext_csd[494]);
		if (ext_csd_rev >= 7) {
			buf_for_health_report = kmalloc(66, GFP_KERNEL);
			if (!buf_for_health_report)
			return -ENOMEM;
			buf_for_firmwware_version = kmalloc(18, GFP_KERNEL);
			if (!buf_for_firmwware_version)
			return -ENOMEM;
			seq_printf(s, "[493] Supported modes, supported_modes: 0x%02x\n", ext_csd[493]);
			seq_printf(s, "[492] Ffu features, ffu_features: 0x%02x\n", ext_csd[492]);
			seq_printf(s, "[491] Operation codes timeout, operation_code_timeout: 0x%02x\n", ext_csd[491]);
			seq_printf(s, "[490:487] Ffu features, ffu_features: 0x%08x\n", (ext_csd[487] << 0) | (ext_csd[488] << 8) | (ext_csd[489] << 16) | (ext_csd[490] << 24));
			seq_printf(s, "[305:302] Number of FW sectors correctly programmed, number_of_fw_sectors_correctly_programmed: 0x%08x\n", (ext_csd[302] << 0) | (ext_csd[303] << 8) | (ext_csd[304] << 16) | (ext_csd[305] << 24));
			output = 0;
			for (cnt = 301 ; cnt >= 270 ; cnt--)
				output += sprintf(buf_for_health_report + output, "%02x", ext_csd[cnt]);
			output += sprintf(buf_for_health_report + output, "\n");
			seq_printf(s, "[301:270] Vendor proprietary health report, vendor_proprietary_health_report(raw data): %s", buf_for_health_report); //mina.park;
			kfree(buf_for_health_report);
			seq_printf(s, "[269] Device life time estimation type B, device_life_time_est_typ_b: 0x%02x\n", ext_csd[269]);
			seq_printf(s, "[268] Device life time estimation type A, device_life_time_est_typ_a: 0x%02x\n", ext_csd[268]);
			seq_printf(s, "[267] Pre EOL information, pre_eol_info: 0x%02x\n", ext_csd[267]);
			seq_printf(s, "[266] Optimal read size, optimal_read_size: 0x%02x\n", ext_csd[266]);
			seq_printf(s, "[265] Optimal write size, optimal_write_size: 0x%02x\n", ext_csd[265]);
			seq_printf(s, "[264] Optimal trim unit size, optimal_trim_unit_size: 0x%02x\n", ext_csd[264]);
			seq_printf(s, "[263:262] Device version, device_version: 0x%02x\n", (ext_csd[262] << 0) | (ext_csd[263] << 8));
			output=0;
			for (cnt = 261 ; cnt >= 254 ; cnt--)
				output += sprintf(buf_for_firmwware_version + output, "%02x", ext_csd[cnt]);
			output += sprintf(buf_for_firmwware_version + output, "\n");
			seq_printf(s, "[261:254] Firmware version, firmwware_version(raw data): %s", buf_for_firmwware_version); //mina.park;
			kfree(buf_for_firmwware_version);
			seq_printf(s, "[253] Power class for 200MHz, DDR at VCC=3.6V, pwr_cl_ddr_200_360: 0x%02x\n", ext_csd[253]);
		}
		seq_printf(s, "[252:249] Cache size, cache_size %d KiB\n", (ext_csd[249] << 0) |
                          (ext_csd[250] << 8) | (ext_csd[251] << 16) |
                          (ext_csd[252] << 24));
		seq_printf(s, "[248] Generic CMD6 timeout, generic_cmd6_time: 0x%02x\n", ext_csd[248]);
		seq_printf(s, "[247] Power off notification timeout, power_off_long_time: 0x%02x\n", ext_csd[247]);
		seq_printf(s, "[246] Background operations status, bkops_status: 0x%02x\n", ext_csd[246]);
		seq_printf(s, "[245:242] Number of correctly programmed sectors, correctly_prg_sectors_num %d KiB\n", (ext_csd[242] << 0) | (ext_csd[243] << 8) | (ext_csd[244] << 16) | (ext_csd[245] << 24));
       }

       /* A441: Reserved [501:247]
           A43: reserved [246:229] */
       if (ext_csd_rev >= 5) {
               seq_printf(s, "[241] 1st initialization time after partitioning, ini_timeout_ap: 0x%02x\n", ext_csd[241]);
               /* A441: reserved [240] */
               seq_printf(s, "[239] Power class for 52MHz, DDR at 3.6V, pwr_cl_ddr_52_360: 0x%02x\n", ext_csd[239]);
               seq_printf(s, "[238] POwer class for 52MHz, DDR at 1.95V, pwr_cl_ddr_52_195: 0x%02x\n", ext_csd[238]);

               /* A441: reserved [237-236] */

               if (ext_csd_rev >= 6) {
			seq_printf(s, "[237] Power class for 200MHz, SDR at 3.6V, pwr_cl_200_360: 0x%02x\n", ext_csd[237]);
			seq_printf(s, "[236] Power class for 200MHz, SDR at 1.95V, pwr_cl_200_195: 0x%02x\n", ext_csd[236]);
               }

               seq_printf(s, "[235] Minimun Write Performance for 8bit at 52MHz in DDR mode, min_perf_ddr_w_8_52: 0x%02x\n", ext_csd[235]);
               seq_printf(s, "[234] Minimun Read Performance for 8bit at 52MHz in DDR modemin_perf_ddr_r_8_52: 0x%02x\n", ext_csd[234]);
               /* A441: reserved [233] */
               seq_printf(s, "[232] TRIM Multiplier, trim_mult: 0x%02x\n", ext_csd[232]);
               seq_printf(s, "[231] Secure Feature support, sec_feature_support: 0x%02x\n", ext_csd[231]);
       }
	if (ext_csd_rev == 5 || ext_csd_rev == 7) { /* Obsolete in 4.5 */  /*---->revived in 5.0*/
               seq_printf(s, "[230] Secure Erase Multiplier, sec_erase_mult: 0x%02x\n", ext_csd[230]);
               seq_printf(s, "[229] Secure TRIM Multiplier, sec_trim_mult:  0x%02x\n", ext_csd[229]);
       }
       seq_printf(s, "[228] Boot information, boot_info: 0x%02x\n", ext_csd[228]);
       /* A441/A43: reserved [227] */
       seq_printf(s, "[226] Boot partition size, boot_size_mult : 0x%02x\n", ext_csd[226]);
       seq_printf(s, "[225] Access size, acc_size: 0x%02x\n", ext_csd[225]);
       seq_printf(s, "[224] High-capacity erase unit size, hc_erase_grp_size: 0x%02x\n", ext_csd[224]);
       seq_printf(s, "[223] High-capacity erase timeout, erase_timeout_mult: 0x%02x\n", ext_csd[223]);
       seq_printf(s, "[222] Reliable write sector count, rel_wr_sec_c: 0x%02x\n", ext_csd[222]);
       seq_printf(s, "[221] High-capacity write protect group size, hc_wp_grp_size: 0x%02x\n", ext_csd[221]);
       seq_printf(s, "[220] Sleep current(VCC), s_c_vcc: 0x%02x\n", ext_csd[220]);
       seq_printf(s, "[219] Sleep current(VCCQ), s_c_vccq: 0x%02x\n", ext_csd[219]);
	if (ext_csd_rev == 7) {
		seq_printf(s, "[218] Production state awareness timeout, production_state_awareness_timeout: 0x%02x\n", ext_csd[218]);
	}
       /* A441/A43: reserved [218] */
       seq_printf(s, "[217] Sleep/awake timeout, s_a_timeout: 0x%02x\n", ext_csd[217]);
		if (ext_csd_rev == 7) {
		seq_printf(s, "[216] Sleep notification timeout, sleep_notification_time: 0x%02x\n", ext_csd[216]);
	}
       /* A441/A43: reserved [216] */
       seq_printf(s, "[215:212] Sector Count, sec_count: 0x%08x\n", (ext_csd[215] << 24) |(ext_csd[214] << 16) | (ext_csd[213] << 8)  | ext_csd[212]);
       /* A441/A43: reserved [211] */
       seq_printf(s, "[210] Minimum Write Performance for 8bit at 52MHz, min_perf_w_8_52: 0x%02x\n", ext_csd[210]);
       seq_printf(s, "[209] Minimum Read Performance for 8bit at 52MHz, min_perf_r_8_52: 0x%02x\n", ext_csd[209]);
       seq_printf(s, "[208] Minimum Write Performance for 8bit at 26MHz, for 4bit at 52MHz, min_perf_w_8_26_4_52: 0x%02x\n", ext_csd[208]);
       seq_printf(s, "[207] Minimum Read Performance for 8bit at 26MHz, for 4bit at 52MHz, min_perf_r_8_26_4_52: 0x%02x\n", ext_csd[207]);
       seq_printf(s, "[206] Minimum Write Performance for 4bit at 26MHz, min_perf_w_4_26: 0x%02x\n", ext_csd[206]);
       seq_printf(s, "[205] Minimum Read Performance for 4bit at 26MHz, min_perf_r_4_26: 0x%02x\n", ext_csd[205]);
       /* A441/A43: reserved [204] */
       seq_printf(s, "[203] Power class for 26MHz at 3.6V, pwr_cl_26_360: 0x%02x\n", ext_csd[203]);
       seq_printf(s, "[202] Power class for 52MHz at 3.6V, pwr_cl_52_360: 0x%02x\n", ext_csd[202]);
       seq_printf(s, "[201] Power class for 26MHz at 1.95V, pwr_cl_26_195: 0x%02x\n", ext_csd[201]);
       seq_printf(s, "[200] Power class for 52MHz at 1.95V, pwr_cl_52_195: 0x%02x\n", ext_csd[200]);

       /* A43: reserved [199:198] */
       if (ext_csd_rev >= 5) {
               seq_printf(s, "[199] Partition switching timing, partition_switch_time: 0x%02x\n", ext_csd[199]);
               seq_printf(s, "[198] Out-of-interrupt busy timing, out_of_interrupt_time: 0x%02x\n", ext_csd[198]);
       }

       /* A441/A43: reserved   [197] [195] [193] [190] [188]
        * [186] [184] [182] [180] [176] */

       if (ext_csd_rev >= 6)
		seq_printf(s, "[197] IO Driver Strength, driver_strength: 0x%02x\n", ext_csd[197]);

	seq_printf(s, "[196] Device type, device_type: 0x%02x\n", ext_csd[196]);
       seq_printf(s, "[194] CSD structure version, csd_structure: 0x%02x\n", ext_csd[194]);
       seq_printf(s, "[192] Extended CSD revision, ext_csd_rev: 0x%02x\n", ext_csd[192]);
       seq_printf(s, "[191] Command set, cmd_set: 0x%02x\n", ext_csd[191]);
       seq_printf(s, "[189] Command set revision, cmd_set_rev: 0x%02x\n", ext_csd[189]);
       seq_printf(s, "[187] Power class, power_class: 0x%02x\n", ext_csd[187]);
       seq_printf(s, "[185] High-speed interface timing, hs_timing: 0x%02x\n", ext_csd[185]);
       /* bus_width: ext_csd[183] not readable */
       seq_printf(s, "[181] Erased memory content, erased_mem_cont: 0x%02x\n", ext_csd[181]);
       seq_printf(s, "[179] Partition configuration, partition_config: 0x%02x\n", ext_csd[179]);
       seq_printf(s, "[178] Boot config protection, boot_config_prot: 0x%02x\n", ext_csd[178]);
	seq_printf(s, "[177] Boot bus Conditions, boot_bus_conditions: 0x%02x\n", ext_csd[177]);
       seq_printf(s, "[175] High-density erase group definition, erase_group_def: 0x%02x\n", ext_csd[175]);

       /* A43: reserved [174:0] */
       if (ext_csd_rev >= 5) {
		seq_printf(s, "[174] Boot write protection status registers, boot_wp_status: 0x%02x\n", ext_csd[174]);
               seq_printf(s, "[173] Boot area write protection register, boot_wp: 0x%02x\n", ext_csd[173]);
               /* A441: reserved [172] */
               seq_printf(s, "[171] User area write protection register, user_wp: 0x%02x\n", ext_csd[171]);
               /* A441: reserved [170] */
               seq_printf(s, "[169] FW configuration, fw_config: 0x%02x\n", ext_csd[169]);
               seq_printf(s, "[168] RPMB Size, rpmb_size_mult: 0x%02x\n", ext_csd[168]);
               seq_printf(s, "[167] Write reliability setting register, wr_rel_set: 0x%02x\n", ext_csd[167]);
               seq_printf(s, "[166] Write reliability parameter register, wr_rel_param: 0x%02x\n", ext_csd[166]);
               /* sanitize_start ext_csd[165]: not readable
                * bkops_start ext_csd[164]: only writable */
               seq_printf(s, "[163] Enable background operations handshake, bkops_en: 0x%02x\n", ext_csd[163]);
               seq_printf(s, "[162] H/W reset function, rst_n_function: 0x%02x\n", ext_csd[162]);
		seq_printf(s, "[161] HPI management, hpi_mgmt: 0x%02x\n", ext_csd[161]);
		seq_printf(s, "[160] Partitioning Support, partitioning_support: 0x%02x\n", ext_csd[160]);
               seq_printf(s, "[159:157] Max Enhanced Area Size, max_enh_size_mult: 0x%06x\n", (ext_csd[159] << 16) | (ext_csd[158] << 8) |ext_csd[157]);
               seq_printf(s, "[156] Partitions attribute, partitions_attribute: 0x%02x\n", ext_csd[156]);
               seq_printf(s, "[155] Partitioning Setting, partition_setting_completed: 0x%02x\n", ext_csd[155]);
               seq_printf(s, "[154:152] General Purpose Partition Size, gp_size_mult_4: 0x%06x\n", (ext_csd[154] << 16) |(ext_csd[153] << 8) | ext_csd[152]);
               seq_printf(s, "[151:149] General Purpose Partition Size, gp_size_mult_3: 0x%06x\n", (ext_csd[151] << 16) |(ext_csd[150] << 8) | ext_csd[149]);
               seq_printf(s, "[148:146] General Purpose Partition Size, gp_size_mult_2: 0x%06x\n", (ext_csd[148] << 16) |(ext_csd[147] << 8) | ext_csd[146]);
               seq_printf(s, "[145:143] General Purpose Partition Size, gp_size_mult_1: 0x%06x\n", (ext_csd[145] << 16) |(ext_csd[144] << 8) | ext_csd[143]);
               seq_printf(s, "[142:140] Enhanced User Data Area Size, enh_size_mult: 0x%06x\n", (ext_csd[142] << 16) |(ext_csd[141] << 8) | ext_csd[140]);
		seq_printf(s, "[139:136] Enhanced User Data Start Address, enh_start_addr: 0x%06x\n", (ext_csd[139] << 24) | (ext_csd[138] << 16) | (ext_csd[137] << 8) | ext_csd[136]);

               /* A441: reserved [135] */
               seq_printf(s, "[134] Bad Block Management mode, sec_bad_blk_mgmnt: 0x%02x\n", ext_csd[134]);
               /* A441: reserved [133:0] */
       }
       /* B45 */
       if (ext_csd_rev >= 6) {
               int j;
               /* tcase_support ext_csd[132] not readable */
		seq_printf(s, "[131] Periodic Wake-up, periodic_wakeup: 0x%02x\n", ext_csd[131]);
		seq_printf(s, "[130] Program CID CSD in DDR mode support, program_cid_csd_ddr_support: 0x%02x\n",
                          ext_csd[130]);

               for (j = 127; j >= 64; j--)
			seq_printf(s, "[127:64] Vendor Specific Fields, vendor_specific_field[%d]: 0x%02x\n",
                                  j, ext_csd[j]);

		seq_printf(s, "[63] Native sector size, native_sector_size: 0x%02x\n", ext_csd[63]);
		seq_printf(s, "[62] Sector size emulation, use_native_sector: 0x%02x\n", ext_csd[62]);
		seq_printf(s, "[61] Sector size, data_sector_size: 0x%02x\n", ext_csd[61]);
		seq_printf(s, "[60] 1st initialization after disabling sector size emulation, ini_timeout_emu: 0x%02x\n", ext_csd[60]);
		seq_printf(s, "[59] Class 6 commands control, class_6_ctrl: 0x%02x\n", ext_csd[59]);
		seq_printf(s, "[58] Number of addressed group to be Released, dyncap_needed: 0x%02x\n", ext_csd[58]);
		seq_printf(s, "[57:56] Exception events control, exception_events_ctrl: 0x%04x\n",
                          (ext_csd[57] << 8) | ext_csd[56]);
		seq_printf(s, "[55:54] Exception events status, exception_events_status: 0x%04x\n",
                          (ext_csd[55] << 8) | ext_csd[54]);
		seq_printf(s, "[53:52] Extended Partitions Attribute, ext_partitions_attribute: 0x%04x\n",
                          (ext_csd[53] << 8) | ext_csd[52]);
               for (j = 51; j >= 37; j--)
			seq_printf(s, "[51:37]Context configuration, context_conf[%d]: 0x%02x\n", j,
                                  ext_csd[j]);

		seq_printf(s, "[36] Packed command status, packed_command_status: 0x%02x\n", ext_csd[36]);
		seq_printf(s, "[35] Packed command failure index, packed_failure_index: 0x%02x\n", ext_csd[35]);
		seq_printf(s, "[34] Power Off Notification, power_off_notification: 0x%02x\n", ext_csd[34]);
		seq_printf(s, "[33] Control to turn the Cache On Off, cache_ctrl: 0x%02x\n", ext_csd[33]);
               /* flush_cache ext_csd[32] not readable */
               /*Reserved [31:0] */
       }
#endif
out_free:
#ifndef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	kfree(buf);
#endif
	kfree(ext_csd);
	return err;
}

#ifdef CONFIG_MACH_LGE
/* LGE_CHANGE
* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
* 2012-07-09, J1-FS@lge.com
*/

static int mmc_ext_csd_open(struct inode *inode, struct file *file)
#else
static ssize_t mmc_ext_csd_read(struct file *filp, char __user *ubuf,
				size_t cnt, loff_t *ppos)
#endif
{
#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	return single_open(file, mmc_ext_csd_read, inode->i_private);
#else
	char *buf = filp->private_data;

	return simple_read_from_buffer(ubuf, cnt, ppos,
				       buf, EXT_CSD_STR_LEN);
}

static int mmc_ext_csd_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
#endif
}

static ssize_t mmc_ext_csd_write(struct file *filp,const char __user *ubuf, size_t cnt, loff_t *ppos)
{
/*
	struct mmc_card *card = filp->private_data;
	int value;
	struct mmc_bkops_stats *bkops_stats;

	if (!card)
		return cnt;

	bkops_stats = &card->bkops_info.bkops_stats;

	//sscanf(ubuf, "%d", &value);
*/

	pr_info("%s \n", (char *)ubuf);

	return cnt;
}



static const struct file_operations mmc_dbg_ext_csd_fops = {
	.open		= mmc_ext_csd_open,
#ifdef CONFIG_MACH_LGE
	/* LGE_CHANGE
	* http://www.mail-archive.com/linux-mmc@vger.kernel.org/msg10669.html
	* 2012-07-09, J1-FS@lge.com
	*/
	.read		   = seq_read,
	.llseek 		= seq_lseek,
	.release		= single_release,
	.write 			= mmc_ext_csd_write,
#else
	.read		= mmc_ext_csd_read,
	.release	= mmc_ext_csd_release,
	.llseek		= default_llseek,
#endif
};

static int mmc_wr_pack_stats_open(struct inode *inode, struct file *filp)
{
	struct mmc_card *card = inode->i_private;

	filp->private_data = card;
	card->wr_pack_stats.print_in_read = 1;
	return 0;
}

#define TEMP_BUF_SIZE 256
static ssize_t mmc_wr_pack_stats_read(struct file *filp, char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	struct mmc_card *card = filp->private_data;
	struct mmc_wr_pack_stats *pack_stats;
	int i;
	int max_num_of_packed_reqs = 0;
	char *temp_buf;

	if (!card)
		return cnt;

	if (!card->wr_pack_stats.print_in_read)
		return 0;

	if (!card->wr_pack_stats.enabled) {
		pr_info("%s: write packing statistics are disabled\n",
			 mmc_hostname(card->host));
		goto exit;
	}

	pack_stats = &card->wr_pack_stats;

	if (!pack_stats->packing_events) {
		pr_info("%s: NULL packing_events\n", mmc_hostname(card->host));
		goto exit;
	}

	max_num_of_packed_reqs = card->ext_csd.max_packed_writes;

	temp_buf = kmalloc(TEMP_BUF_SIZE, GFP_KERNEL);
	if (!temp_buf)
		goto exit;

	spin_lock(&pack_stats->lock);

	snprintf(temp_buf, TEMP_BUF_SIZE, "%s: write packing statistics:\n",
		mmc_hostname(card->host));
	strlcat(ubuf, temp_buf, cnt);

	for (i = 1 ; i <= max_num_of_packed_reqs ; ++i) {
		if (pack_stats->packing_events[i]) {
			snprintf(temp_buf, TEMP_BUF_SIZE,
				 "%s: Packed %d reqs - %d times\n",
				mmc_hostname(card->host), i,
				pack_stats->packing_events[i]);
			strlcat(ubuf, temp_buf, cnt);
		}
	}

	snprintf(temp_buf, TEMP_BUF_SIZE,
		 "%s: stopped packing due to the following reasons:\n",
		 mmc_hostname(card->host));
	strlcat(ubuf, temp_buf, cnt);

	if (pack_stats->pack_stop_reason[EXCEEDS_SEGMENTS]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: exceed max num of segments\n",
			 mmc_hostname(card->host),
			 pack_stats->pack_stop_reason[EXCEEDS_SEGMENTS]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[EXCEEDS_SECTORS]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: exceed max num of sectors\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[EXCEEDS_SECTORS]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[WRONG_DATA_DIR]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: wrong data direction\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[WRONG_DATA_DIR]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[FLUSH_OR_DISCARD]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: flush or discard\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[FLUSH_OR_DISCARD]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[EMPTY_QUEUE]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: empty queue\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[EMPTY_QUEUE]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[REL_WRITE]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: rel write\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[REL_WRITE]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[THRESHOLD]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: Threshold\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[THRESHOLD]);
		strlcat(ubuf, temp_buf, cnt);
	}

	if (pack_stats->pack_stop_reason[LARGE_SEC_ALIGN]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: Large sector alignment\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[LARGE_SEC_ALIGN]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[RANDOM]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: random request\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[RANDOM]);
		strlcat(ubuf, temp_buf, cnt);
	}
	if (pack_stats->pack_stop_reason[FUA]) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: %d times: fua request\n",
			mmc_hostname(card->host),
			pack_stats->pack_stop_reason[FUA]);
		strlcat(ubuf, temp_buf, cnt);
	}

	spin_unlock(&pack_stats->lock);

	kfree(temp_buf);

	pr_info("%s", ubuf);

exit:
	if (card->wr_pack_stats.print_in_read == 1) {
		card->wr_pack_stats.print_in_read = 0;
		return strnlen(ubuf, cnt);
	}

	return 0;
}

static ssize_t mmc_wr_pack_stats_write(struct file *filp,
				       const char __user *ubuf, size_t cnt,
				       loff_t *ppos)
{
	struct mmc_card *card = filp->private_data;
	int value;

	if (!card)
		return cnt;

	sscanf(ubuf, "%d", &value);
	if (value) {
		mmc_blk_init_packed_statistics(card);
	} else {
		spin_lock(&card->wr_pack_stats.lock);
		card->wr_pack_stats.enabled = false;
		spin_unlock(&card->wr_pack_stats.lock);
	}

	return cnt;
}

static const struct file_operations mmc_dbg_wr_pack_stats_fops = {
	.open		= mmc_wr_pack_stats_open,
	.read		= mmc_wr_pack_stats_read,
	.write		= mmc_wr_pack_stats_write,
};

static int mmc_bkops_stats_open(struct inode *inode, struct file *filp)
{
	struct mmc_card *card = inode->i_private;

	filp->private_data = card;

	card->bkops_info.bkops_stats.print_stats = 1;
	return 0;
}

static ssize_t mmc_bkops_stats_read(struct file *filp, char __user *ubuf,
				     size_t cnt, loff_t *ppos)
{
	struct mmc_card *card = filp->private_data;
	struct mmc_bkops_stats *bkops_stats;
	int i;
	char *temp_buf;

	if (!card)
		return cnt;

	bkops_stats = &card->bkops_info.bkops_stats;

	if (!bkops_stats->print_stats)
		return 0;

	if (!bkops_stats->enabled) {
		pr_info("%s: bkops statistics are disabled\n",
			 mmc_hostname(card->host));
		goto exit;
	}

	temp_buf = kmalloc(TEMP_BUF_SIZE, GFP_KERNEL);
	if (!temp_buf)
		goto exit;

	spin_lock(&bkops_stats->lock);

	memset(ubuf, 0, cnt);

	snprintf(temp_buf, TEMP_BUF_SIZE, "%s: bkops statistics:\n",
		mmc_hostname(card->host));
	strlcat(ubuf, temp_buf, cnt);

	for (i = 0 ; i < BKOPS_NUM_OF_SEVERITY_LEVELS ; ++i) {
		snprintf(temp_buf, TEMP_BUF_SIZE,
			 "%s: BKOPS: due to level %d: %u\n",
		 mmc_hostname(card->host), i, bkops_stats->bkops_level[i]);
		strlcat(ubuf, temp_buf, cnt);
	}

	snprintf(temp_buf, TEMP_BUF_SIZE,
		 "%s: BKOPS: stopped due to HPI: %u\n",
		 mmc_hostname(card->host), bkops_stats->hpi);
	strlcat(ubuf, temp_buf, cnt);

	snprintf(temp_buf, TEMP_BUF_SIZE,
		 "%s: BKOPS: how many time host was suspended: %u\n",
		 mmc_hostname(card->host), bkops_stats->suspend);
	strlcat(ubuf, temp_buf, cnt);

	spin_unlock(&bkops_stats->lock);

	kfree(temp_buf);

	pr_info("%s", ubuf);

exit:
	if (bkops_stats->print_stats == 1) {
		bkops_stats->print_stats = 0;
		return strnlen(ubuf, cnt);
	}

	return 0;
}

static ssize_t mmc_bkops_stats_write(struct file *filp,
				      const char __user *ubuf, size_t cnt,
				      loff_t *ppos)
{
	struct mmc_card *card = filp->private_data;
	int value;
	struct mmc_bkops_stats *bkops_stats;

	if (!card)
		return cnt;

	bkops_stats = &card->bkops_info.bkops_stats;

	sscanf(ubuf, "%d", &value);
	if (value) {
		mmc_blk_init_bkops_statistics(card);
	} else {
		spin_lock(&bkops_stats->lock);
		bkops_stats->enabled = false;
		spin_unlock(&bkops_stats->lock);
	}

	return cnt;
}


static const struct file_operations mmc_dbg_bkops_stats_fops = {
	.open		= mmc_bkops_stats_open,
	.read		= mmc_bkops_stats_read,
	.write		= mmc_bkops_stats_write,
};


// jaehyek by written  from here.

#define JOB_NOTHING		5000
static DEFINE_MUTEX(mmc_wearout_lock);

struct mmc_test_card {
	struct mmc_card	*card;
	u8		*buffer;

};

struct mmc_lifehist 
{
	unsigned int seclife ;
	unsigned int looplife ;
	unsigned int levellife ;
};

struct mmc_wearout 
{ 
	unsigned int countrepeated ;		// count of for-out-loop
	unsigned int lifevalue ;			// emmc life value 
	struct mmc_lifehist* listlifehist ;				// emmc life value history
	unsigned int sizelifehist ;
	unsigned int indexlifehist ;		// infex for nex writing
	unsigned int countlifemeasure ;		// emmc life measure loop count
	unsigned int cmdindex ;				// selected command index 	
	unsigned int cmdstop ;				// bool value if loop stop or not
	unsigned int countsaveaddrs ;			// count of for-in-loop
	unsigned int addrblockstart ;		// for sequence write
	unsigned int incblockaddr ;		// for sequence write
	unsigned int *listaddrrandom ;		// fro random write
	unsigned int sizelistaddrrandom ;

	// paramerter for not saving multi write block(  ) 
	unsigned int countnotsavewrite ;
	unsigned int onetime;

	// normal operation parameter.
	unsigned int countloop;
	unsigned int param1;
	unsigned int param2;
	unsigned int result;
	

} ; 



static struct mmc_wearout mmc_wearout_debugfs = { 0} ;
static u8 * psaveblocks ;


struct mmc_wearout_test_case 
{
	const char *name;

	int (*prepare)(struct mmc_test_card *);
	int (*run)(struct mmc_test_card *);
	int (*cleanup)(struct mmc_test_card *);
};


static int mmc_wearout_busy(struct mmc_command *cmd)
{
	return !(cmd->resp[0] & R1_READY_FOR_DATA) ||
		(R1_CURRENT_STATE(cmd->resp[0]) == R1_STATE_PRG);
}


/*
 * Wait for the card to finish the busy state
 */
static int mmc_wearout_wait_busy(struct mmc_test_card *test)
{
	int ret, busy;
	struct mmc_command cmd = {0};

	busy = 0;
	do 
	{
		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = MMC_SEND_STATUS;
		cmd.arg = test->card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

		ret = mmc_wait_for_cmd(test->card->host, &cmd, 0);
		if (ret)
			break;

		if (!busy && mmc_wearout_busy(&cmd)) 
		{
			busy = 1;
			if (test->card->host->caps & MMC_CAP_WAIT_WHILE_BUSY)
				pr_info("%s: Warning: Host did not " "wait for busy state to end.\n",mmc_hostname(test->card->host));
		}
	} while (mmc_wearout_busy(&cmd));

	return ret;
}



/*
 * Fill in the mmc_request structure given a set of transfer parameters.
 */
static void mmc_wearout_prepare_mrq(struct mmc_test_card *test,
	struct mmc_request *mrq, struct scatterlist *sg, unsigned sg_len,
	unsigned dev_addr, unsigned blocks, unsigned blksz, int write)
{
	BUG_ON(!mrq || !mrq->cmd || !mrq->data || !mrq->stop);

	if (blocks > 1) 
	{
		mrq->cmd->opcode = write ? MMC_WRITE_MULTIPLE_BLOCK : MMC_READ_MULTIPLE_BLOCK;
	} 
	else 
	{
		mrq->cmd->opcode = write ? MMC_WRITE_BLOCK : MMC_READ_SINGLE_BLOCK;
	}

	mrq->cmd->arg = dev_addr;
	if (!mmc_card_blockaddr(test->card))
		mrq->cmd->arg <<= 9;

	mrq->cmd->flags = MMC_RSP_R1 | MMC_CMD_ADTC;

	if (blocks == 1)
		mrq->stop = NULL;
	else 
	{
		mrq->stop->opcode = MMC_STOP_TRANSMISSION;
		mrq->stop->arg = 0;
		mrq->stop->flags = MMC_RSP_R1B | MMC_CMD_AC;
	}

	mrq->data->blksz = blksz;
	mrq->data->blocks = blocks;
	mrq->data->flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
	mrq->data->sg = sg;
	mrq->data->sg_len = sg_len;

	mmc_set_data_timeout(mrq->data, test->card);
}

/*
 * Checks that a normal transfer didn't have any errors
 */
static int mmc_wearout_check_result(struct mmc_test_card *test, struct mmc_request *mrq)
{
	int ret;

	BUG_ON(!mrq || !mrq->cmd || !mrq->data);

	ret = 0;

	if (!ret && mrq->cmd->error)
		ret = mrq->cmd->error;
	if (!ret && mrq->data->error)
		ret = mrq->data->error;
	if (!ret && mrq->stop && mrq->stop->error)
		ret = mrq->stop->error;
	if (!ret && mrq->data->bytes_xfered !=
		mrq->data->blocks * mrq->data->blksz)
		ret = RESULT_FAIL;

	if (ret == -EINVAL)
		ret = RESULT_UNSUP_HOST;

	return ret;
}



static void mmc_wearout_get_lifevalue(struct mmc_test_card *test, struct mmc_lifehist *plifehist )
{
	u8 *ext_csd;
	size_t  size = 512 ;
	int err ;
	struct timespec ts ;



	ext_csd = kmalloc(size, GFP_KERNEL);
	if(!ext_csd)
	{
		pr_info("_______________ mmc malloc failed for ext_csd :  __________________\n" ) ;
		return ;
	}
		

	mmc_wearout_wait_busy(test);

	if (test->card->cid.manfid == CID_MANFID_HYNIX)
	{
		err = mmc_send_maker_cmd(test->card, MMC_SET_HYNIX_SPICIFIC_CMD, MMC_SET_HYNIX_ARG_FIRST);
		err = mmc_send_maker_cmd(test->card, MMC_SET_HYNIX_SPICIFIC_CMD, MMC_SET_HYNIX_ARG_SECOND);
		err = mmc_send_ext_csd(test->card, ext_csd);

		mmc_wearout_debugfs.lifevalue = ((u32 *)ext_csd)[15] ;
	}
	else
	{
		err = mmc_send_ext_csd(test->card, ext_csd);
		mmc_wearout_debugfs.lifevalue = (unsigned int )ext_csd[268] ;
	}

	if (err)
		mmc_wearout_debugfs.lifevalue = 0xffffffff ;

	getnstimeofday(&ts);

	plifehist->levellife = mmc_wearout_debugfs.lifevalue ;
	plifehist->seclife = ts.tv_sec ;

	kfree(ext_csd);
	
}

static u32 mmc_wearout_next_blockaddr(unsigned int loop)
{
	if ( mmc_wearout_debugfs.cmdindex == 0 )	// sequence write
	{
		return mmc_wearout_debugfs.addrblockstart + mmc_wearout_debugfs.incblockaddr * loop ;
	}
	else if ( mmc_wearout_debugfs.cmdindex == 1 || mmc_wearout_debugfs.cmdindex == 3 )	//random write or not-save random write
	{
		loop = loop %  mmc_wearout_debugfs.sizelistaddrrandom ;
		return mmc_wearout_debugfs.addrblockstart + mmc_wearout_debugfs.listaddrrandom[loop] ;
	}
	else
		return mmc_wearout_debugfs.addrblockstart;
}

static int mmc_wearout_backup_blocks(struct mmc_test_card *test)
{
	int ret;
	unsigned int size, loop, loopmax;
	u8 * ptemp ;
	unsigned long flags;
	struct scatterlist sg;
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_command stop = {0};
	struct mmc_data data = {0};

	

	if (test->card->host->max_blk_count == 1)
		return RESULT_UNSUP_HOST;

	size = PAGE_SIZE ;

	if (size < 1024)
		return RESULT_UNSUP_HOST;

	loopmax = mmc_wearout_debugfs.countsaveaddrs;
	if (!loopmax || !psaveblocks )
	{
		pr_info("_______________ countsaveaddrs == 0  __________________\n" ) ;
		mmc_wearout_debugfs.countsaveaddrs = 0 ;
		return RESULT_FAIL ;
	}

	if( mmc_wearout_debugfs.cmdindex == 1 && (!mmc_wearout_debugfs.listaddrrandom)) 
	{
		pr_info("_______________ listaddrrandom was not setuped .  __________________\n" ) ;
		return RESULT_FAIL ;

	}
	
	
	ptemp = psaveblocks ;		

	sg_init_one(&sg, test->buffer, size);

	mrq.cmd = &cmd;
	mrq.data = &data;
	mrq.stop = &stop;

	mmc_wearout_prepare_mrq(test, &mrq, &sg, 1, 0, size/512, 512, 0);

	// wait for the finish of the previous request
	mmc_wearout_wait_busy(test);

	for(loop = 0 ; loop < loopmax ; loop ++,ptemp += size )
	{
		cmd.arg = mmc_wearout_next_blockaddr(loop);
		if (!mmc_card_blockaddr(test->card))
			cmd.arg <<= 9;
	
		
		mmc_wait_for_req(test->card->host, &mrq);
		mmc_wearout_wait_busy(test);

		local_irq_save(flags);
		sg_copy_to_buffer(&sg, 1, ptemp, size);
		local_irq_restore(flags);

		pr_info("_______________ mmc backup loop : %d __________________\n", loop ) ;
		
		ret = mmc_wearout_check_result(test, &mrq);
		if (ret != 0)
			return ret ;
	}



	return ret ;
}


static int mmc_wearout_multi_write(struct mmc_test_card *test)
{
	int ret = 0 ;
	unsigned int size, loop, looplifeupdate ;
	struct mmc_lifehist prevlifehist, postlifehist;
	struct scatterlist sg;
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_command stop = {0};
	struct mmc_data data = {0};

	

	if (test->card->host->max_blk_count == 1)
		return RESULT_UNSUP_HOST;

	size = PAGE_SIZE ;

	if (size < 1024)
		return RESULT_UNSUP_HOST;

	memset( mmc_wearout_debugfs.listlifehist, 0, sizeof( struct mmc_lifehist) * mmc_wearout_debugfs.sizelifehist ) ;
	mmc_wearout_debugfs.indexlifehist = 0 ;

	sg_init_one(&sg, test->buffer, size);

	mrq.cmd = &cmd;
	mrq.data = &data;
	mrq.stop = &stop;
	

	mmc_wearout_prepare_mrq(test, &mrq, &sg, 1, 0, size/512, 512, 1);

	mmc_wearout_debugfs.cmdstop = 0 ; 
	mmc_wearout_debugfs.countrepeated = 0 ; 
	looplifeupdate = 0 ;
	
	mmc_wearout_get_lifevalue(test, &prevlifehist);
	prevlifehist.looplife = 0 ;

	while (!mmc_wearout_debugfs.cmdstop)
	{
		for(loop = 0 ; loop < mmc_wearout_debugfs.countsaveaddrs ; loop ++)
		{
			cmd.arg = mmc_wearout_next_blockaddr(loop);
			if (!mmc_card_blockaddr(test->card))
				cmd.arg <<= 9;
		
			// wait for the finish of the previous request
			//mmc_wearout_wait_busy(test);
			mmc_wait_for_req(test->card->host, &mrq);
			ret = mmc_wearout_check_result(test, &mrq);
			if (ret != 0)
			return ret ;

		}
		
		looplifeupdate ++ ;
		if ( looplifeupdate == mmc_wearout_debugfs.countlifemeasure ) 
		{
			mmc_wearout_get_lifevalue(test, &postlifehist);
			postlifehist.looplife = mmc_wearout_debugfs.countrepeated ;
			
			if ( (postlifehist.levellife > prevlifehist.levellife )  && 
				(mmc_wearout_debugfs.indexlifehist < mmc_wearout_debugfs.sizelifehist ))
			{
				struct mmc_lifehist *plifehist = &mmc_wearout_debugfs.listlifehist[mmc_wearout_debugfs.indexlifehist++];
				plifehist->levellife = prevlifehist.levellife;
				plifehist->looplife = postlifehist.looplife - prevlifehist.looplife ;
				plifehist->seclife = postlifehist.seclife - prevlifehist.seclife;

				//preserve the current lifehist for next computation.
				memcpy(&prevlifehist, &postlifehist, sizeof(struct mmc_lifehist));
			}
			looplifeupdate = 0 ;
		}
		mmc_wearout_debugfs.countrepeated ++ ;
	}

	return ret ;
}

static int mmc_wearout_restore_blocks(struct mmc_test_card *test)
{
	int ret = 0 ;
	unsigned int size, loop, loopmax;
	u8 * ptemp ;
	unsigned long flags;
	struct scatterlist sg;
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_command stop = {0};
	struct mmc_data data = {0};

	

	if (test->card->host->max_blk_count == 1)
		return RESULT_UNSUP_HOST;

	size = PAGE_SIZE ;

	sg_init_one(&sg, test->buffer, size);

	mrq.cmd = &cmd;
	mrq.data = &data;
	mrq.stop = &stop;

	

	mmc_wearout_prepare_mrq(test, &mrq, &sg, 1, 0, size/512, 512, 1);

	loopmax = mmc_wearout_debugfs.countsaveaddrs;
	ptemp = psaveblocks ;

	// wait for the finish of the previous request
	mmc_wearout_wait_busy(test);

	for(loop = 0 ; loop < loopmax ; loop ++,ptemp += size )
	{
		cmd.arg = mmc_wearout_next_blockaddr(loop);
		if (!mmc_card_blockaddr(test->card))
			cmd.arg <<= 9;
	
//		memcpy ( test->buffer, ptemp, size ) ;
		local_irq_save(flags);
		sg_copy_from_buffer(&sg, 1, ptemp, size);
		local_irq_restore(flags);
		
		mmc_wait_for_req(test->card->host, &mrq);
		mmc_wearout_wait_busy(test);

		pr_info("_______________ mmc restore loop : %d __________________\n", loop ) ;
		
		ret = mmc_wearout_check_result(test, &mrq);
		if (ret != 0)
			return ret ;
	}


	return ret ;
}


static int mmc_wearout_run_notsave_size_write(struct mmc_test_card *test)
{
	/*
	* check point : countnotsavewrite, listaddrrandom, sizelistaddrrandom, addrblockstart
	* preparing job : write dummy stuff from  addrblockstart while countnotsavewrite
	*
	*/
	
	int ret=0;
	unsigned int size, loop;
	struct mmc_lifehist prevlifehist, postlifehist;
	struct scatterlist sg;
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_command stop = {0};
	struct mmc_data data = {0};

	

	if (!(mmc_wearout_debugfs.countnotsavewrite && mmc_wearout_debugfs.addrblockstart  ))
	{
		pr_info("_______________ countnotsavewrite, addrblockstart was not setup :  __________________\n" ) ;
		return RESULT_FAIL;
	}

	size = PAGE_SIZE ;

	memset( mmc_wearout_debugfs.listlifehist, 0, sizeof( struct mmc_lifehist) * mmc_wearout_debugfs.sizelifehist ) ;
	mmc_wearout_debugfs.indexlifehist = 0 ;

	sg_init_one(&sg, test->buffer, size);

	mrq.cmd = &cmd;
	mrq.data = &data;
	mrq.stop = &stop;

	mmc_wearout_prepare_mrq(test, &mrq, &sg, 1, 0, size/512, 512, 1);

	// wait for the finish of the previous request

	mmc_wearout_debugfs.indexlifehist = 0 ;
	mmc_wearout_debugfs.cmdstop = 0 ; 
	mmc_wearout_debugfs.countrepeated = 0 ; 
	
	mmc_wearout_get_lifevalue(test, &prevlifehist);
	prevlifehist.looplife = 0 ;
	
	while ( !mmc_wearout_debugfs.cmdstop ) 
	{
		for(loop = 0 ; loop < mmc_wearout_debugfs.countnotsavewrite ; loop += 8 )
		{
			cmd.arg = mmc_wearout_debugfs.addrblockstart + loop   ;
			if (!mmc_card_blockaddr(test->card))
				cmd.arg <<= 9;
		
			// wait for the finish of the previous request
			//mmc_wearout_wait_busy(test);
			mmc_wait_for_req(test->card->host, &mrq);
			ret = mmc_wearout_check_result(test, &mrq);
			if (ret != 0)
				return ret ;
		}

		if ( (mmc_wearout_debugfs.countrepeated % mmc_wearout_debugfs.countlifemeasure ) == 0 )
		{
			mmc_wearout_get_lifevalue(test, &postlifehist);
			postlifehist.looplife = mmc_wearout_debugfs.countrepeated ;
			
			if ( (postlifehist.levellife > prevlifehist.levellife )  && 
				(mmc_wearout_debugfs.indexlifehist < mmc_wearout_debugfs.sizelifehist ))
			{
				struct mmc_lifehist *plifehist = &mmc_wearout_debugfs.listlifehist[mmc_wearout_debugfs.indexlifehist++];
				plifehist->levellife = prevlifehist.levellife;
				plifehist->looplife = postlifehist.looplife - prevlifehist.looplife ;
				plifehist->seclife = postlifehist.seclife - prevlifehist.seclife;

				//preserve the current lifehist for next computation.
				memcpy(&prevlifehist, &postlifehist, sizeof(struct mmc_lifehist));
			}
		}
		mmc_wearout_debugfs.countrepeated ++ ;
		if(mmc_wearout_debugfs.onetime)
		{
			mmc_wearout_debugfs.cmdstop = 1 ;
			mmc_wearout_debugfs.onetime= 0 ;
		}
	}

	return ret ;
}




static int mmc_wearout_run_notsave_random_write(struct mmc_test_card *test)
{
	int ret=0;
	unsigned int size, loop ;
	struct mmc_lifehist prevlifehist, postlifehist;
	struct scatterlist sg;
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_command stop = {0};
	struct mmc_data data = {0};

	if (!(mmc_wearout_debugfs.listaddrrandom && mmc_wearout_debugfs.sizelistaddrrandom && mmc_wearout_debugfs.addrblockstart  ))
	{
		pr_info("_______________ addrblockstart, listaddrrandom was not setup :  __________________\n" ) ;
		return RESULT_FAIL;
	}


	size = PAGE_SIZE ;

	memset( mmc_wearout_debugfs.listlifehist, 0, sizeof( struct mmc_lifehist) * mmc_wearout_debugfs.sizelifehist ) ;
	mmc_wearout_debugfs.indexlifehist = 0 ;

	sg_init_one(&sg, test->buffer, size);

	mrq.cmd = &cmd;
	mrq.data = &data;
	mrq.stop = &stop;
	

	mmc_wearout_prepare_mrq(test, &mrq, &sg, 1, 0, size/512, 512, 1);

	mmc_wearout_debugfs.indexlifehist = 0 ;
	mmc_wearout_debugfs.cmdstop = 0 ; 
	mmc_wearout_debugfs.countrepeated = 0 ; 
	
	mmc_wearout_get_lifevalue(test, &prevlifehist);
	prevlifehist.looplife = 0 ;

	while (!mmc_wearout_debugfs.cmdstop)
	{
		for(loop = 0 ; loop < mmc_wearout_debugfs.sizelistaddrrandom ; loop ++)
		{
			cmd.arg = mmc_wearout_next_blockaddr(loop);
			if (!mmc_card_blockaddr(test->card))
				cmd.arg <<= 9;
		
			// wait for the finish of the previous request
			//mmc_wearout_wait_busy(test);
			mmc_wait_for_req(test->card->host, &mrq);
			ret = mmc_wearout_check_result(test, &mrq);
			if (ret != 0)
				return ret ;
		}
		

		if ( (mmc_wearout_debugfs.countrepeated % mmc_wearout_debugfs.countlifemeasure ) == 0 )
		{
			mmc_wearout_get_lifevalue(test, &postlifehist);
			postlifehist.looplife = mmc_wearout_debugfs.countrepeated ;
			
			if ( (postlifehist.levellife > prevlifehist.levellife )  && 
				(mmc_wearout_debugfs.indexlifehist < mmc_wearout_debugfs.sizelifehist ))
			{
				struct mmc_lifehist *plifehist = &mmc_wearout_debugfs.listlifehist[mmc_wearout_debugfs.indexlifehist++];
				plifehist->levellife = prevlifehist.levellife;
				plifehist->looplife = postlifehist.looplife - prevlifehist.looplife ;
				plifehist->seclife = postlifehist.seclife - prevlifehist.seclife;

				//preserve the current lifehist for next computation.
				memcpy(&prevlifehist, &postlifehist, sizeof(struct mmc_lifehist));
			}
		}
		mmc_wearout_debugfs.countrepeated ++ ;
	}

	return ret ;
}



static int mmc_wearout_run_power_onoff(struct mmc_test_card *test)
{
	struct mmc_card *card = test->card;
	struct mmc_host *host = card->host;
	unsigned int countloop = mmc_wearout_debugfs.countloop;
	int err;
	int i ; 

	pr_info("_______________ run the routine ,mmc_wearout_run_power_onoff  __________________\n" ) ;

	if ( countloop == 0 ) 
	{
		countloop = 1 ; 
		mmc_wearout_debugfs.countloop = 1 ; 
	}
		
	for ( i = 0 ; i < countloop ; i ++ )
	{
		err = mmc_hw_reset_check(host);
		if (err)
			break;
	}

	
	if (!err)
		return RESULT_OK;

	if (err == -ENOSYS)
		return RESULT_FAIL;

	if (err != -EOPNOTSUPP)
		return err;

	if (!mmc_can_reset(card))
		return RESULT_UNSUP_CARD;

	return RESULT_UNSUP_HOST;
}

static int mmc_wearout_run_sleep(struct mmc_test_card *test)
{
	struct mmc_card *card = test->card;
	struct mmc_host *host = card->host;

	pr_info("_______________ run the routine ,mmc_wearout_run_sleep  __________________\n" ) ;

	return 	mmc_card_sleep(host);
}

static int mmc_wearout_run_awake(struct mmc_test_card *test)
{
	struct mmc_card *card = test->card;
	struct mmc_host *host = card->host;

	pr_info("_______________ run the routine ,mmc_wearout_run_awake  __________________\n" ) ;

	return 	mmc_card_awake(host);
}

static int mmc_wearout_run_flush_cache(struct mmc_test_card *test)
{
	struct mmc_card *card = test->card;

	pr_info("_______________ run the routine ,mmc_wearout_run_flush_cache  __________________\n" ) ;

	return 	mmc_flush_cache(card);
}

static int mmc_wearout_run_disable_cache(struct mmc_test_card *test)
{
	struct mmc_card *card = test->card;
	struct mmc_host *host = card->host;

	pr_info("_______________ run the routine ,mmc_wearout_run_disable_cache  __________________\n" ) ;

	return 	mmc_cache_ctrl(host, mmc_wearout_debugfs.param1);
}


static const struct mmc_wearout_test_case list_mmc_wearout_test_case[] = 
{
	{
		.name = "Multi-block sequence write( save blocks)",
		.prepare = mmc_wearout_backup_blocks,
		.run = mmc_wearout_multi_write,
		.cleanup = mmc_wearout_restore_blocks,
	},
	{
		.name = "Multi-block random write( save blocks)",
		.prepare = mmc_wearout_backup_blocks,
		.run = mmc_wearout_multi_write,
		.cleanup = mmc_wearout_restore_blocks,
	},
	{
		.name = "Multi-block size write(not save blocks)",
		.run = mmc_wearout_run_notsave_size_write,
	},
	{
		.name = "Multi-block random write(not save blocks)",
		.run = mmc_wearout_run_notsave_random_write,
	},
	
	{
			.name = "eMMC Power On/Off repeat",
			.run = mmc_wearout_run_power_onoff,
	},
	{
			.name = "eMMC Sleep",
			.run = mmc_wearout_run_sleep,
	},
	{
			.name = "eMMC Awake",
			.run = mmc_wearout_run_awake,
	},
	{
			.name = "eMMC Flush Cache",
			.run = mmc_wearout_run_flush_cache,
	},
	{
			.name = "eMMC enable/disable Cache",
			.run = mmc_wearout_run_disable_cache,
	},
	

};



static int mmc_wearout_countsaveaddrs_show(struct seq_file *sf, void *data)
{
	//struct mmc_card *card = (struct mmc_card *)sf->private;

	seq_printf(sf, "%d\n", mmc_wearout_debugfs.countsaveaddrs );
	return 0;
}

static int mmc_wearout_countsaveaddrs_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_wearout_countsaveaddrs_show, inode->i_private);
}

static ssize_t mmc_wearout_countsaveaddrs_write(struct file *file, const char __user *buf,
	size_t count, loff_t *pos)
{

	char lbuf[12];
	unsigned int countsaveaddrs;

	if (count >= sizeof(lbuf))
		return -EINVAL;

	if (copy_from_user(lbuf, buf, count))
		return -EFAULT;
	lbuf[count] = '\0';

	if (kstrtouint(lbuf, 10, &countsaveaddrs))
		return -EINVAL;



	if ( psaveblocks ) 
	{
		kfree (psaveblocks) ;
		psaveblocks = (u8*)0 ;
		pr_info("_______________  free memory  for psaveblocks  __________________\n" ) ;
	}

	if ( countsaveaddrs  == 0) 
	{
		mmc_wearout_debugfs.countsaveaddrs = 0 ;
		return count ;
	}

	
	psaveblocks = kmalloc(countsaveaddrs * PAGE_SIZE , GFP_KERNEL);
	if (!psaveblocks ) 
	{
		mmc_wearout_debugfs.countsaveaddrs = 0 ;
		pr_info("_______________ can't alloc memory : %d __________________\n", countsaveaddrs ) ;
		return RESULT_FAIL ;
	}	

	mmc_wearout_debugfs.countsaveaddrs = (unsigned int) countsaveaddrs ;

	return count;
}

static struct file_operations mmc_wearout_countsaveaddrs_fops = 
{
	.open		= mmc_wearout_countsaveaddrs_open,
	.read		= seq_read,
	.write		= mmc_wearout_countsaveaddrs_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static int mmc_wearout_listrandromblock_read(struct seq_file *s, void *data)
{
	//struct mmc_card *card = s->private;
	int i ; 

	for ( i = 0 ; i < mmc_wearout_debugfs.sizelistaddrrandom ; i ++)
	{
		seq_printf(s, "%08x,", mmc_wearout_debugfs.listaddrrandom[i] );
	}	

	return 0;
}


static int mmc_wearout_listrandromblock_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_wearout_listrandromblock_read, inode->i_private);

}

static ssize_t mmc_wearout_listrandromblock_write(struct file *file,const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	//struct seq_file *sf = (struct seq_file *)file->private_data;
	//struct mmc_card *card = (struct mmc_card *)sf->private;
	
	if ( mmc_wearout_debugfs.listaddrrandom  ) 
	{
		kfree ( mmc_wearout_debugfs.listaddrrandom ) ;
		mmc_wearout_debugfs.listaddrrandom = (unsigned int*) 0 ; 
		mmc_wearout_debugfs.sizelistaddrrandom = 0 ;
		pr_info("_______________  free memory  for listaddrrandom  __________________\n" ) ;
	}

	if ( cnt < 4 )
		return cnt ;


	
	mmc_wearout_debugfs.listaddrrandom  = kmalloc(cnt, GFP_KERNEL);
	if (!mmc_wearout_debugfs.listaddrrandom)
	{
		pr_info("_______________  fail to alloc  memory  for listaddrrandom  __________________\n" ) ;
		return -EFAULT ;
	}

	memcpy(mmc_wearout_debugfs.listaddrrandom, ubuf, cnt);
	mmc_wearout_debugfs.sizelistaddrrandom = cnt / sizeof(u32 ) ;

	return cnt;
}



static struct file_operations mmc_wearout_listrandromblock_fops = {
	.open		= mmc_wearout_listrandromblock_open,
	.read		   = seq_read,
	.llseek 		= seq_lseek,
	.release		= single_release,
	.write 			= mmc_wearout_listrandromblock_write,

};

static int mmc_wearout_lifehist_show(struct seq_file *sf, void *data)
{
	int i;
	struct mmc_lifehist *phist ; 

	//mutex_lock(&mmc_wearout_lock);			// remove the lock to read anytime .
	phist = mmc_wearout_debugfs.listlifehist ;
	
	for (i = 0; i < mmc_wearout_debugfs.indexlifehist ; i++)
	{
		seq_printf(sf, "%d:%d:%d\n", phist[i].seclife, phist[i].looplife, phist[i].levellife );
	}

	//mutex_unlock(&mmc_wearout_lock);

	return 0;
}

static int mmc_wearout_lifehist_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_wearout_lifehist_show, inode->i_private);
}


static struct file_operations mmc_wearout_lifehist_fops = 
{
	.open		= mmc_wearout_lifehist_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static int mmc_wearout_testlist_show(struct seq_file *sf, void *data)
{
	int i;

	mutex_lock(&mmc_wearout_lock);

	for (i = 0; i < ARRAY_SIZE(list_mmc_wearout_test_case); i++)
		seq_printf(sf, "%d:\t%s\n", i, list_mmc_wearout_test_case[i].name);

	mutex_unlock(&mmc_wearout_lock);

	return 0;
}

static int mmc_wearout_testlist_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_wearout_testlist_show, inode->i_private);
}

static struct file_operations mmc_wearout_testlist_fops = 
{
	.open		= mmc_wearout_testlist_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void mmc_wearout_testcmd_run(struct mmc_test_card *test, int testcase)
{
	int i, ret;

	pr_info("%s: Starting tests of card %s...\n", mmc_hostname(test->card->host), mmc_card_id(test->card));

	mmc_rpm_hold(test->card->host, &test->card->dev);
	mmc_claim_host(test->card->host);

	mmc_set_blocklen(test->card, 512);

	for (i = 0;i < ARRAY_SIZE(list_mmc_wearout_test_case);i++) 
	{

		if (i  != testcase)
			continue;

		pr_info("%s: Test case %d. %s...\n", mmc_hostname(test->card->host), i , list_mmc_wearout_test_case[i].name);

		if (list_mmc_wearout_test_case[i].prepare) 
		{
			ret = list_mmc_wearout_test_case[i].prepare(test);
			if (ret) 
			{
				pr_info("%s: Result: Prepare " "stage failed! (%d)\n", mmc_hostname(test->card->host), ret);
				continue;
			}
		}

		
		ret = list_mmc_wearout_test_case[i].run(test);
		mmc_wearout_debugfs.result = ret ;
		switch (ret) {
		case RESULT_OK:
			pr_info("%s: Result: OK\n", mmc_hostname(test->card->host));
			break;
		case RESULT_FAIL:
			pr_info("%s: Result: FAILED\n", mmc_hostname(test->card->host));
			break;
		case RESULT_UNSUP_HOST:
			pr_info("%s: Result: UNSUPPORTED " "(by host)\n", mmc_hostname(test->card->host));
			break;
		case RESULT_UNSUP_CARD:
			pr_info("%s: Result: UNSUPPORTED " "(by card)\n", mmc_hostname(test->card->host));
			break;
		default:
			pr_info("%s: Result: ERROR (%d)\n", mmc_hostname(test->card->host), ret);
		}


		if (list_mmc_wearout_test_case[i].cleanup) 
		{
			ret = list_mmc_wearout_test_case[i].cleanup(test);
			if (ret)
			{
				pr_info("%s: Warning: Cleanup " "stage failed! (%d)\n", mmc_hostname(test->card->host), ret);
			}
		}
	}

	mmc_release_host(test->card->host);
	mmc_rpm_release(test->card->host, &test->card->dev);

	mmc_wearout_debugfs.cmdindex = JOB_NOTHING ;

	pr_info("%s: Tests completed.\n", mmc_hostname(test->card->host));
}

static int mmc_wearout_testcmd_show(struct seq_file *sf, void *data)
{
	//struct mmc_card *card = (struct mmc_card *)sf->private;

	mutex_lock(&mmc_wearout_lock);

	seq_printf(sf, "%d\n", mmc_wearout_debugfs.cmdindex );

	mutex_unlock(&mmc_wearout_lock);

	return 0;
}

static int mmc_wearout_testcmd_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmc_wearout_testcmd_show, inode->i_private);
}

static ssize_t mmc_wearout_testcmd_write(struct file *file, const char __user *buf,
	size_t count, loff_t *pos)
{
	struct seq_file *sf = (struct seq_file *)file->private_data;
	struct mmc_card *card = (struct mmc_card *)sf->private;
	
	struct mmc_test_card *test;
	char lbuf[12];
	long testcase;

	if (count >= sizeof(lbuf))
		return -EINVAL;

	if (copy_from_user(lbuf, buf, count))
		return -EFAULT;
	lbuf[count] = '\0';

	if (strict_strtol(lbuf, 10, &testcase))
		return -EINVAL;

	mmc_wearout_debugfs.cmdindex = (unsigned int) testcase ;
	test = kzalloc(sizeof(struct mmc_test_card), GFP_KERNEL);
	if (!test)
		return -ENOMEM;


	test->card = card;
	test->buffer = kzalloc(BUFFER_SIZE, GFP_KERNEL);
	if (test->buffer) 
	{
		mutex_lock(&mmc_wearout_lock);
		mmc_wearout_testcmd_run(test, testcase);
		mutex_unlock(&mmc_wearout_lock);
	}

	kfree(test->buffer);
	kfree(test);

	mmc_wearout_debugfs.cmdstop = 0  ;
	return count;
}

static struct file_operations mmc_wearout_testcmd_fops = 
{
	.open		= mmc_wearout_testcmd_open,
	.read		= seq_read,
	.write		= mmc_wearout_testcmd_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};


int mmc_wearout_init(struct mmc_card *card)
{
	struct dentry *file = NULL;
	struct mmc_lifehist lifehist;
	struct mmc_test_card test ;

	test.card = card ;
	memset(&mmc_wearout_debugfs, 0, sizeof(mmc_wearout_debugfs));
	mmc_wearout_debugfs.cmdindex = JOB_NOTHING ;
	psaveblocks = (u8*)0 ; 
	
	//if (mmc_card_mmc(card) && (card->ext_csd.rev >= 5) )

		mmc_wearout_debugfs.countlifemeasure = 100 ;
		if (!debugfs_create_u32("countrepeated", S_IRUGO , card->debugfs_root, &mmc_wearout_debugfs.countrepeated))
			goto err;
		if (!debugfs_create_u32("lifevalue", S_IRUGO , card->debugfs_root, &mmc_wearout_debugfs.lifevalue))
			goto err;
		if (!debugfs_create_u32("cmdindex", S_IRUGO , card->debugfs_root, &mmc_wearout_debugfs.cmdindex))
			goto err;
		if (!debugfs_create_u32("cmdstop", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.cmdstop))
			goto err;
		if (!debugfs_create_u32("addrblockstart", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.addrblockstart))
			goto err;
		if (!debugfs_create_u32("incblockaddr", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.incblockaddr))
			goto err;
		if (!debugfs_create_u32("countlifemeasure", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.countlifemeasure))
			goto err;
		if (!debugfs_create_u32("sizelistaddrrandom", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.sizelistaddrrandom))
			goto err;
		if (!debugfs_create_u32("countnotsavewrite", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.countnotsavewrite))
			goto err;
		if (!debugfs_create_u32("sizelifehist", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.sizelifehist))
			goto err;
		if (!debugfs_create_u32("onetime", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.onetime))
			goto err;
		if (!debugfs_create_u32("indexlifehist", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.indexlifehist))
			goto err;
		if (!debugfs_create_u32("countloop", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.countloop))
			goto err;
		if (!debugfs_create_u32("param1", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.param1))
			goto err;
		if (!debugfs_create_u32("param2", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.param2))
			goto err;
		if (!debugfs_create_u32("result", S_IRUGO | S_IWUGO, card->debugfs_root, &mmc_wearout_debugfs.result))
			goto err;


		mmc_wearout_get_lifevalue(&test, &lifehist);
		mmc_wearout_debugfs.lifevalue = lifehist.levellife ;

		mmc_wearout_debugfs.indexlifehist = 0 ; 
		if ( card->cid.manfid == CID_MANFID_HYNIX)
		{
			mmc_wearout_debugfs.listlifehist = kzalloc(sizeof( struct mmc_lifehist) * 2000  , GFP_KERNEL );
				if (!mmc_wearout_debugfs.listlifehist)
					return -ENODEV;
			mmc_wearout_debugfs.sizelifehist = 2000 ;
		}
		else
		{
			mmc_wearout_debugfs.listlifehist = kzalloc(sizeof( struct mmc_lifehist) * 20  , GFP_KERNEL );
			if (!mmc_wearout_debugfs.listlifehist)
				return -ENODEV;

			mmc_wearout_debugfs.sizelifehist = 20 ;
		}
		pr_info( "mmc_wearout_debugfs.sizelifehist : %x \n ",mmc_wearout_debugfs.sizelifehist);
		pr_info( "mmc_wearout_debugfs.listlifehist : %x \n ",(unsigned int)mmc_wearout_debugfs.listlifehist);


		if (mmc_wearout_debugfs.listlifehist)
		{
			file = debugfs_create_file("listlifehist", S_IRUGO | S_IWUGO, card->debugfs_root, card, &mmc_wearout_lifehist_fops);
			if (IS_ERR_OR_NULL(file)) 
			{
				pr_info( "Can't create %s. Perhaps debugfs is disabled.\n","listlifehist");
				return -ENODEV;
			}
		}


		file = debugfs_create_file("countsaveaddrs", S_IRUGO | S_IWUGO, card->debugfs_root, card, &mmc_wearout_countsaveaddrs_fops);
		if (IS_ERR_OR_NULL(file)) 
		{
			pr_info( "Can't create %s. Perhaps debugfs is disabled.\n","countsaveaddrs");
			return -ENODEV;
		}

		

		file = debugfs_create_file("listaddrrandom", S_IRUGO | S_IWUGO, card->debugfs_root, card, &mmc_wearout_listrandromblock_fops);
		if (IS_ERR_OR_NULL(file)) 
		{
			pr_info( "Can't create %s. Perhaps debugfs is disabled.\n","listaddrrandom");
			return -ENODEV;
		}

		file = debugfs_create_file("listwearouttest", S_IRUGO , card->debugfs_root, card, &mmc_wearout_testlist_fops);
		if (IS_ERR_OR_NULL(file)) 
		{
			pr_info( "Can't create %s. Perhaps debugfs is disabled.\n","listwearouttest");
			return -ENODEV;
		}

		file = debugfs_create_file("cmdwearouttest", S_IRUGO | S_IWUGO, card->debugfs_root, card, &mmc_wearout_testcmd_fops);
		if (IS_ERR_OR_NULL(file)) 
		{
			pr_info( "Can't create %s. Perhaps debugfs is disabled.\n","cmdwearouttest");
			return -ENODEV;
		}
		

		return 0 ;


err:
	pr_info("wearout functions are  failed \n") ;
	return 0 ;

}


void mmc_add_card_debugfs(struct mmc_card *card)
{
	struct mmc_host	*host = card->host;
	struct dentry	*root;

	if (!host->debugfs_root)
		return;

	root = debugfs_create_dir(mmc_card_id(card), host->debugfs_root);
	if (IS_ERR(root))
		/* Don't complain -- debugfs just isn't enabled */
		return;
	if (!root)
		/* Complain -- debugfs is enabled, but it failed to
		 * create the directory. */
		goto err;

	card->debugfs_root = root;

	if (!debugfs_create_x32("state", S_IRUSR, root, &card->state))
		goto err;

	if (mmc_card_mmc(card) || mmc_card_sd(card))
		if (!debugfs_create_file("status", S_IRUSR, root, card,
					&mmc_dbg_card_status_fops))
			goto err;

	if (mmc_card_mmc(card))
		if (!debugfs_create_file("ext_csd", S_IRUGO|S_IWUGO, root, card,
					&mmc_dbg_ext_csd_fops))
			goto err;

	if (mmc_card_mmc(card) && (card->ext_csd.rev >= 6) &&
	    (card->host->caps2 & MMC_CAP2_PACKED_WR))
		if (!debugfs_create_file("wr_pack_stats", S_IRUSR, root, card,
					 &mmc_dbg_wr_pack_stats_fops))
			goto err;

	if (mmc_card_mmc(card) && (card->ext_csd.rev >= 5) &&
	    card->ext_csd.bkops_en)
		if (!debugfs_create_file("bkops_stats", S_IRUSR, root, card,
					 &mmc_dbg_bkops_stats_fops))
			goto err;

	mmc_wearout_init(card);

	return;

err:
	debugfs_remove_recursive(root);
	card->debugfs_root = NULL;
	dev_err(&card->dev, "failed to initialize debugfs\n");
}

void mmc_remove_card_debugfs(struct mmc_card *card)
{
	debugfs_remove_recursive(card->debugfs_root);
}