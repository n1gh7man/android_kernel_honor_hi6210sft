/*
 *  linux/drivers/mmc/core/sd.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  Copyright (C) 2005-2007 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/stat.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include "core.h"
#include "bus.h"
#include "mmc_ops.h"
#include "sd.h"
#include "sd_ops.h"
#ifdef CONFIG_MMC_PASSWORDS
#include "lock.h"
#endif
#ifdef CONFIG_HUAWEI_SDCARD_DSM
#include <linux/mmc/dsm_sdcard.h>
struct dsm_sdcard_cmd_log dsm_sdcard_cmd_logs[] = 
{
	{"CMD8 : ",				0},
	{"CMD55 : ",    		0},
	{"ACMD41: ",			0},
	{"CMD2_RESP0 : ",		0},
	{"CMD2_RESP1 : ",		0},
	{"CMD2_RESP2 : ",		0},
	{"CMD2_RESP3 : ",		0},
	{"CMD3 : ",				0},
	{"CMD9_RESP0 : ",		0},
	{"CMD9_RESP1 : ",		0},
	{"CMD9_RESP2 : ",		0},
	{"CMD9_RESP3 : ",		0},
	{"CMD7 : ",				0},
	{"CMD6_CMDERR : ",      0},
	{"CMD6_DATERR : ",      0},
	{"Report Uevent : ",	0},
};

#endif

#ifdef CONFIG_HW_SD_HEALTH_DETECT
static unsigned int g_sd_speed_class = 0;
#endif

static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int tacc_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

#define UNSTUFF_BITS(resp,start,size)					\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
void mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	memset(&card->cid, 0, sizeof(struct mmc_cid));

	/*
	 * SD doesn't currently have a version field so we will
	 * have to assume we can parse this.
	 */
	card->cid.manfid		= UNSTUFF_BITS(resp, 120, 8);
	card->cid.oemid			= UNSTUFF_BITS(resp, 104, 16);
	card->cid.prod_name[0]		= UNSTUFF_BITS(resp, 96, 8);
	card->cid.prod_name[1]		= UNSTUFF_BITS(resp, 88, 8);
	card->cid.prod_name[2]		= UNSTUFF_BITS(resp, 80, 8);
	card->cid.prod_name[3]		= UNSTUFF_BITS(resp, 72, 8);
	card->cid.prod_name[4]		= UNSTUFF_BITS(resp, 64, 8);
	card->cid.hwrev			= UNSTUFF_BITS(resp, 60, 4);
	card->cid.fwrev			= UNSTUFF_BITS(resp, 56, 4);
	card->cid.serial		= UNSTUFF_BITS(resp, 24, 32);
	card->cid.year			= UNSTUFF_BITS(resp, 12, 8);
	card->cid.month			= UNSTUFF_BITS(resp, 8, 4);

	card->cid.year += 2000; /* SD cards year offset */
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	csd_struct = UNSTUFF_BITS(resp, 126, 2);

	switch (csd_struct) {
	case 0:
		m = UNSTUFF_BITS(resp, 115, 4);
		e = UNSTUFF_BITS(resp, 112, 3);
		csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
		csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		e = UNSTUFF_BITS(resp, 47, 3);
		m = UNSTUFF_BITS(resp, 62, 12);
		csd->capacity	  = (1 + m) << (e + 2);

		csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
		csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
		csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
		csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
		csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
		csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
		csd->write_partial = UNSTUFF_BITS(resp, 21, 1);

		if (UNSTUFF_BITS(resp, 46, 1)) {
			csd->erase_size = 1;
		} else if (csd->write_blkbits >= 9) {
			csd->erase_size = UNSTUFF_BITS(resp, 39, 7) + 1;
			csd->erase_size <<= csd->write_blkbits - 9;
		}
		break;
	case 1:
		/*
		 * This is a block-addressed SDHC or SDXC card. Most
		 * interesting fields are unused and have fixed
		 * values. To avoid getting tripped by buggy cards,
		 * we assume those fixed values ourselves.
		 */
		mmc_card_set_blockaddr(card);

		csd->tacc_ns	 = 0; /* Unused */
		csd->tacc_clks	 = 0; /* Unused */

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);
		csd->c_size	  = UNSTUFF_BITS(resp, 48, 22);

		/* SDXC cards have a minimum C_SIZE of 0x00FFFF */
		if (csd->c_size >= 0xFFFF)
			mmc_card_set_ext_capacity(card);

		m = UNSTUFF_BITS(resp, 48, 22);
		csd->capacity     = (1 + m) << 10;

		csd->read_blkbits = 9;
		csd->read_partial = 0;
		csd->write_misalign = 0;
		csd->read_misalign = 0;
		csd->r2w_factor = 4; /* Unused */
		csd->write_blkbits = 9;
		csd->write_partial = 0;
		csd->erase_size = 1;
		break;
	default:
		pr_err("%s: unrecognised CSD structure version %d\n",
			mmc_hostname(card->host), csd_struct);
		return -EINVAL;
	}

	card->erase_size = csd->erase_size;

	return 0;
}

/*
 * Given a 64-bit response, decode to our card SCR structure.
 */
static int mmc_decode_scr(struct mmc_card *card)
{
	struct sd_scr *scr = &card->scr;
	unsigned int scr_struct;
	u32 resp[4];

	resp[3] = card->raw_scr[1];
	resp[2] = card->raw_scr[0];

	scr_struct = UNSTUFF_BITS(resp, 60, 4);
	if (scr_struct != 0) {
		pr_err("%s: unrecognised SCR structure version %d\n",
			mmc_hostname(card->host), scr_struct);
		return -EINVAL;
	}

	scr->sda_vsn = UNSTUFF_BITS(resp, 56, 4);
	scr->bus_widths = UNSTUFF_BITS(resp, 48, 4);
	if (scr->sda_vsn == SCR_SPEC_VER_2)
		/* Check if Physical Layer Spec v3.0 is supported */
		scr->sda_spec3 = UNSTUFF_BITS(resp, 47, 1);

	if (UNSTUFF_BITS(resp, 55, 1))
		card->erased_byte = 0xFF;
	else
		card->erased_byte = 0x0;

	if (scr->sda_spec3)
		scr->cmds = UNSTUFF_BITS(resp, 32, 2);
	return 0;
}

/*
 * Fetch and process SD Status register.
 */
static int mmc_read_ssr(struct mmc_card *card)
{
	unsigned int au, es, et, eo;
	int err, i;
	u32 *ssr;

	if (!(card->csd.cmdclass & CCC_APP_SPEC)) {
		pr_warning("%s: card lacks mandatory SD Status "
			"function.\n", mmc_hostname(card->host));
		return 0;
	}

	ssr = kmalloc(64, GFP_KERNEL);
	if (!ssr)
		return -ENOMEM;

	err = mmc_app_sd_status(card, ssr);
	if (err) {
		pr_warning("%s: problem reading SD Status "
			"register.\n", mmc_hostname(card->host));
		err = 0;
		goto out;
	}

	for (i = 0; i < 16; i++)
		ssr[i] = be32_to_cpu(ssr[i]);

	card->ssr.speed_class = UNSTUFF_BITS(ssr, 440 - 384, 8);

#ifdef CONFIG_HW_SD_HEALTH_DETECT
	g_sd_speed_class = card->ssr.speed_class;
#endif
	/*
	 * UNSTUFF_BITS only works with four u32s so we have to offset the
	 * bitfield positions accordingly.
	 */
	au = UNSTUFF_BITS(ssr, 428 - 384, 4);
	if (au > 0 && au <= 9) {
		card->ssr.au = 1 << (au + 4);
		es = UNSTUFF_BITS(ssr, 408 - 384, 16);
		et = UNSTUFF_BITS(ssr, 402 - 384, 6);
		eo = UNSTUFF_BITS(ssr, 400 - 384, 2);
		if (es && et) {
			card->ssr.erase_timeout = (et * 1000) / es;
			card->ssr.erase_offset = eo * 1000;
		}
	} else {
		pr_warning("%s: SD Status: Invalid Allocation Unit "
			"size.\n", mmc_hostname(card->host));
	}
out:
	kfree(ssr);
	return err;
}

/*
 * Fetches and decodes switch information
 */
static int mmc_read_switch(struct mmc_card *card)
{
	int err;
	u8 *status;

	if (card->scr.sda_vsn < SCR_SPEC_VER_1)
		return 0;

	if (!(card->csd.cmdclass & CCC_SWITCH)) {
		pr_warning("%s: card lacks mandatory switch "
			"function, performance might suffer.\n",
			mmc_hostname(card->host));
		return 0;
	}

	err = -EIO;

	status = kmalloc(64, GFP_KERNEL);
	if (!status) {
		pr_err("%s: could not allocate a buffer for "
			"switch capabilities.\n",
			mmc_hostname(card->host));
		return -ENOMEM;
	}

	/*
	 * Find out the card's support bits with a mode 0 operation.
	 * The argument does not matter, as the support bits do not
	 * change with the arguments.
	 */
	err = mmc_sd_switch(card, 0, 0, 0, status);
	if (err) {
		/*
		 * If the host or the card can't do the switch,
		 * fail more gracefully.
		 */
		if (err != -EINVAL && err != -ENOSYS && err != -EFAULT)
			goto out;

		pr_warning("%s: problem reading Bus Speed modes.\n",
			mmc_hostname(card->host));
		err = 0;

		goto out;
	}

	if (status[13] & SD_MODE_HIGH_SPEED)
		card->sw_caps.hs_max_dtr = HIGH_SPEED_MAX_DTR;

	if (card->scr.sda_spec3) {
		card->sw_caps.sd3_bus_mode = status[13];
		/* Driver Strengths supported by the card */
		card->sw_caps.sd3_drv_type = status[9];
	    printk(KERN_ERR "%s:card support %s %s %s %s %s\n",
	        mmc_hostname(card->host),(card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR12 ? "SDR12":""),(card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR25 ? "SDR25":""),
	        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR50 ? "SDR50":""),(card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR104 ? "SDR104":""),
	        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_DDR50 ? "DDR50":""));
		printk(KERN_ERR "%s:card driver %s %s %s %s\n",
			mmc_hostname(card->host),(card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_B ? "TYPE_B":""),
			(card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_A ? "TYPE_A":""),
			(card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_C ? "TYPE_C":""),
			(card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_D ? "TYPE_D":""));
	}
	else
	{
	    printk(KERN_ERR "%s:card not support spec3\n",mmc_hostname(card->host));
	}

out:
	kfree(status);

	return err;
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
int mmc_sd_switch_hs(struct mmc_card *card)
{
	int err;
	u8 *status;

	if (card->scr.sda_vsn < SCR_SPEC_VER_1)
		return 0;

	if (!(card->csd.cmdclass & CCC_SWITCH))
		return 0;

	if (!(card->host->caps & MMC_CAP_SD_HIGHSPEED))
		return 0;

	if (card->sw_caps.hs_max_dtr == 0)
		return 0;

	err = -EIO;

	status = kmalloc(64, GFP_KERNEL);
	if (!status) {
		pr_err("%s: could not allocate a buffer for "
			"switch capabilities.\n", mmc_hostname(card->host));
		return -ENOMEM;
	}

	err = mmc_sd_switch(card, 1, 0, 1, status);
	if (err)
		goto out;

	if ((status[16] & 0xF) != 1) {
		pr_warn("%s: Problem switching card into high-speed mode!, status:%x\n",
			mmc_hostname(card->host), (status[16] & 0xF));
		err = -EBUSY;
	} else {
		err = 1;
	}

	/* SPI mode doesn't define CMD19 */
	if (!mmc_host_is_spi(card->host) && card->host->ops->execute_tuning) {
		mmc_host_clk_hold(card->host);
		card->host->ops->execute_tuning(card->host,
						      MMC_SEND_TUNING_BLOCK);
		mmc_host_clk_release(card->host);
	}

out:
	kfree(status);

	return err;
}

static int sd_select_driver_type(struct mmc_card *card, u8 *status)
{
	int host_drv_type = SD_DRIVER_TYPE_B;
	int card_drv_type = SD_DRIVER_TYPE_B;
	int drive_strength;
	int err;

	/*
	 * If the host doesn't support any of the Driver Types A,C or D,
	 * or there is no board specific handler then default Driver
	 * Type B is used.
	 */
	if (!(card->host->caps & (MMC_CAP_DRIVER_TYPE_A | MMC_CAP_DRIVER_TYPE_C
	    | MMC_CAP_DRIVER_TYPE_D)))
		return 0;

	if (!card->host->ops->select_drive_strength)
		return 0;

	if (card->host->caps & MMC_CAP_DRIVER_TYPE_A)
		host_drv_type |= SD_DRIVER_TYPE_A;

	if (card->host->caps & MMC_CAP_DRIVER_TYPE_C)
		host_drv_type |= SD_DRIVER_TYPE_C;

	if (card->host->caps & MMC_CAP_DRIVER_TYPE_D)
		host_drv_type |= SD_DRIVER_TYPE_D;

	if (card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_A)
		card_drv_type |= SD_DRIVER_TYPE_A;

	if (card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_C)
		card_drv_type |= SD_DRIVER_TYPE_C;

	if (card->sw_caps.sd3_drv_type & SD_DRIVER_TYPE_D)
		card_drv_type |= SD_DRIVER_TYPE_D;

	/*
	 * The drive strength that the hardware can support
	 * depends on the board design.  Pass the appropriate
	 * information and let the hardware specific code
	 * return what is possible given the options
	 */
	mmc_host_clk_hold(card->host);
	drive_strength = card->host->ops->select_drive_strength(
		card->sw_caps.uhs_max_dtr,
		host_drv_type, card_drv_type);
	mmc_host_clk_release(card->host);

	err = mmc_sd_switch(card, 1, 2, drive_strength, status);
	if (err)
		return err;

	if ((status[15] & 0xF) != drive_strength) {
		pr_warning("%s: Problem setting drive strength!\n",
			mmc_hostname(card->host));
		return 0;
	}

	mmc_set_driver_type(card->host, drive_strength);

	return 0;
}

static void sd_update_bus_speed_mode(struct mmc_card *card)
{
	/*
	 * If the host doesn't support any of the UHS-I modes, fallback on
	 * default speed.
	 */
	if (!mmc_host_uhs(card->host)) {
		card->sd_bus_speed = 0;
		printk(KERN_ERR "%s:host not support uhs-1\n",mmc_hostname(card->host));
		return;
	}
	printk(KERN_ERR "%s:host support %s %s %s %s %s\n",
	mmc_hostname(card->host),(card->host->caps & MMC_CAP_UHS_SDR12 ? "SDR12":""),(card->host->caps & MMC_CAP_UHS_SDR25 ? "SDR25":""),
	(card->host->caps & MMC_CAP_UHS_SDR50 ? "SDR50":""),(card->host->caps & MMC_CAP_UHS_SDR104 ? "SDR104":""),
	(card->host->caps & MMC_CAP_UHS_DDR50 ? "DDR50":""));
	if ((card->host->caps & MMC_CAP_UHS_SDR104) &&
	    (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR104)) {
			card->sd_bus_speed = UHS_SDR104_BUS_SPEED;
	} else if ((card->host->caps & MMC_CAP_UHS_DDR50) &&
		   (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_DDR50)) {
			card->sd_bus_speed = UHS_DDR50_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50)) && (card->sw_caps.sd3_bus_mode &
		    SD_MODE_UHS_SDR50)) {
			card->sd_bus_speed = UHS_SDR50_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR25)) &&
		   (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR25)) {
			card->sd_bus_speed = UHS_SDR25_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR25 |
		    MMC_CAP_UHS_SDR12)) && (card->sw_caps.sd3_bus_mode &
		    SD_MODE_UHS_SDR12)) {
			card->sd_bus_speed = UHS_SDR12_BUS_SPEED;
	}
}

static int sd_set_bus_speed_mode(struct mmc_card *card, u8 *status)
{
	int err;
	unsigned int timing = 0;

	switch (card->sd_bus_speed) {
	case UHS_SDR104_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR104;
		card->sw_caps.uhs_max_dtr = UHS_SDR104_MAX_DTR;
		break;
	case UHS_DDR50_BUS_SPEED:
		timing = MMC_TIMING_UHS_DDR50;
		card->sw_caps.uhs_max_dtr = UHS_DDR50_MAX_DTR;
		break;
	case UHS_SDR50_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR50;
		card->sw_caps.uhs_max_dtr = UHS_SDR50_MAX_DTR;
		break;
	case UHS_SDR25_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR25;
		card->sw_caps.uhs_max_dtr = UHS_SDR25_MAX_DTR;
		break;
	case UHS_SDR12_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR12;
		card->sw_caps.uhs_max_dtr = UHS_SDR12_MAX_DTR;
		break;
	default:
		return 0;
	}

	err = mmc_sd_switch(card, 1, 0, card->sd_bus_speed, status);
	if (err)
		return err;

	if ((status[16] & 0xF) != card->sd_bus_speed) {
		pr_warn("%s: Problem setting bus speed mode(%u)! max_dtr:%u, timing:%u, status:%x\n",
			mmc_hostname(card->host), card->sd_bus_speed,
			card->sw_caps.uhs_max_dtr, timing, (status[16] & 0xF));
		err = -EBUSY;
	} else {
		mmc_set_timing(card->host, timing);
		mmc_set_clock(card->host, card->sw_caps.uhs_max_dtr);
	}

	return err;
}

/* Get host's max current setting at its current voltage */
static u32 sd_get_host_max_current(struct mmc_host *host)
{
	u32 voltage, max_current;

	voltage = 1 << host->ios.vdd;
	switch (voltage) {
	case MMC_VDD_165_195:
		max_current = host->max_current_180;
		break;
	case MMC_VDD_29_30:
	case MMC_VDD_30_31:
		max_current = host->max_current_300;
		break;
	case MMC_VDD_32_33:
	case MMC_VDD_33_34:
		max_current = host->max_current_330;
		break;
	default:
		max_current = 0;
	}

	return max_current;
}

static int sd_set_current_limit(struct mmc_card *card, u8 *status)
{
	int current_limit = SD_SET_CURRENT_NO_CHANGE;
	int err;
	u32 max_current;

	/*
	 * Current limit switch is only defined for SDR50, SDR104, and DDR50
	 * bus speed modes. For other bus speed modes, we do not change the
	 * current limit.
	 */
	if ((card->sd_bus_speed != UHS_SDR50_BUS_SPEED) &&
	    (card->sd_bus_speed != UHS_SDR104_BUS_SPEED) &&
	    (card->sd_bus_speed != UHS_DDR50_BUS_SPEED))
		return 0;

	/*
	 * Host has different current capabilities when operating at
	 * different voltages, so find out its max current first.
	 */
	max_current = sd_get_host_max_current(card->host);

	/*
	 * We only check host's capability here, if we set a limit that is
	 * higher than the card's maximum current, the card will be using its
	 * maximum current, e.g. if the card's maximum current is 300ma, and
	 * when we set current limit to 200ma, the card will draw 200ma, and
	 * when we set current limit to 400/600/800ma, the card will draw its
	 * maximum 300ma from the host.
	 */
	if (max_current >= 800)
		current_limit = SD_SET_CURRENT_LIMIT_800;
	else if (max_current >= 600)
		current_limit = SD_SET_CURRENT_LIMIT_600;
	else if (max_current >= 400)
		current_limit = SD_SET_CURRENT_LIMIT_400;
	else if (max_current >= 200)
		current_limit = SD_SET_CURRENT_LIMIT_200;

	if (current_limit != SD_SET_CURRENT_NO_CHANGE) {
		err = mmc_sd_switch(card, 1, 3, current_limit, status);
		if (err)
			return err;

		if (((status[15] >> 4) & 0x0F) != current_limit)
			pr_warning("%s: Problem setting current limit!\n",
				mmc_hostname(card->host));

	}

	return 0;
}

/*
 * UHS-I specific initialization procedure
 */
#ifdef CONFIG_MMC_PASSWORDS
int mmc_sd_init_uhs_card(struct mmc_card *card)
#else
static int mmc_sd_init_uhs_card(struct mmc_card *card)
#endif
{
	int err;
	u8 *status;

	if (!card->scr.sda_spec3)
		return 0;

	if (!(card->csd.cmdclass & CCC_SWITCH))
		return 0;

	status = kmalloc(64, GFP_KERNEL);
	if (!status) {
		pr_err("%s: could not allocate a buffer for "
			"switch capabilities.\n", mmc_hostname(card->host));
		return -ENOMEM;
	}

	/* Set 4-bit bus width */
	if ((card->host->caps & MMC_CAP_4_BIT_DATA) &&
	    (card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {
		err = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
		if (err)
			goto out;

		mmc_set_bus_width(card->host, MMC_BUS_WIDTH_4);
	}

	/*
	 * Select the bus speed mode depending on host
	 * and card capability.
	 */
	sd_update_bus_speed_mode(card);

	/* Set the driver strength for the card */
	err = sd_select_driver_type(card, status);
	if (err)
		goto out;

	/* Set current limit for the card */
	err = sd_set_current_limit(card, status);
	if (err)
		goto out;

	/* Set bus speed mode of the card */
	err = sd_set_bus_speed_mode(card, status);
	if (err)
		goto out;

	/* SPI mode doesn't define CMD19 */
	if (!mmc_host_is_spi(card->host) && card->host->ops->execute_tuning) {
		mmc_host_clk_hold(card->host);
		err = card->host->ops->execute_tuning(card->host,
						      MMC_SEND_TUNING_BLOCK);
		mmc_host_clk_release(card->host);
	}

out:
	kfree(status);

	return err;
}

MMC_DEV_ATTR(cid, "%08x%08x%08x%08x\n", card->raw_cid[0], card->raw_cid[1],
	card->raw_cid[2], card->raw_cid[3]);
MMC_DEV_ATTR(csd, "%08x%08x%08x%08x\n", card->raw_csd[0], card->raw_csd[1],
	card->raw_csd[2], card->raw_csd[3]);
MMC_DEV_ATTR(scr, "%08x%08x\n", card->raw_scr[0], card->raw_scr[1]);
MMC_DEV_ATTR(date, "%02d/%04d\n", card->cid.month, card->cid.year);
MMC_DEV_ATTR(erase_size, "%u\n", card->erase_size << 9);
MMC_DEV_ATTR(preferred_erase_size, "%u\n", card->pref_erase << 9);
MMC_DEV_ATTR(fwrev, "0x%x\n", card->cid.fwrev);
MMC_DEV_ATTR(hwrev, "0x%x\n", card->cid.hwrev);
MMC_DEV_ATTR(manfid, "0x%06x\n", card->cid.manfid);
MMC_DEV_ATTR(name, "%s\n", card->cid.prod_name);
MMC_DEV_ATTR(oemid, "0x%04x\n", card->cid.oemid);
MMC_DEV_ATTR(serial, "0x%08x\n", card->cid.serial);
MMC_DEV_ATTR(speed_class, "0x%08x\n", card->ssr.speed_class);
MMC_DEV_ATTR(state, "0x%08x\n", card->state);

static struct attribute *sd_std_attrs[] = {
	&dev_attr_cid.attr,
	&dev_attr_csd.attr,
	&dev_attr_scr.attr,
	&dev_attr_date.attr,
	&dev_attr_erase_size.attr,
	&dev_attr_preferred_erase_size.attr,
	&dev_attr_fwrev.attr,
	&dev_attr_hwrev.attr,
	&dev_attr_manfid.attr,
	&dev_attr_name.attr,
	&dev_attr_oemid.attr,
	&dev_attr_serial.attr,
	&dev_attr_speed_class.attr,
	&dev_attr_state.attr,
	NULL,
};

static struct attribute_group sd_std_attr_group = {
	.attrs = sd_std_attrs,
};

static const struct attribute_group *sd_attr_groups[] = {
	&sd_std_attr_group,
	NULL,
};

struct device_type sd_type = {
	.groups = sd_attr_groups,
};

#ifdef CONFIG_MMC_PASSWORDS
/*
 * Adds sysfs entries as relevant.
 */
static int mmc_sd_sysfs_add(struct mmc_host *host, struct mmc_card *card)
{
	int ret;

	ret = mmc_lock_add_sysfs(card);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

/*
 * Removes the sysfs entries added by mmc_sysfs_add().
 */
static void mmc_sd_sysfs_remove(struct mmc_host *host, struct mmc_card *card)
{
	mmc_lock_remove_sysfs(card);
}
#endif
/*
 * Fetch CID from card.
 */
int mmc_sd_get_cid(struct mmc_host *host, u32 ocr, u32 *cid, u32 *rocr)
{
	int err;
	u32 max_current;
	int retries = 10;

try_again:
	if (!retries) {
		ocr &= ~SD_OCR_S18R;
		pr_warning("%s: Skipping voltage switch\n",
			mmc_hostname(host));
	}

	/*
	 * Since we're changing the OCR value, we seem to
	 * need to tell some cards to go back to the idle
	 * state.  We wait 1ms to give cards time to
	 * respond.
	 */
	mmc_go_idle(host);

	/*
	 * If SD_SEND_IF_COND indicates an SD 2.0
	 * compliant card and we should set bit 30
	 * of the ocr to indicate that we can handle
	 * block-addressed SDHC cards.
	 */
	err = mmc_send_if_cond(host, ocr);
	if (!err)
	{
		ocr |= SD_OCR_CCS;
		printk(KERN_ERR "%s: this sd support SDHC or later \n",mmc_hostname(host));
	}

	/*
	 * If the host supports one of UHS-I modes, request the card
	 * to switch to 1.8V signaling level. If the card has failed
	 * repeatedly to switch however, skip this.
	 */
	if (retries && mmc_host_uhs(host))
		ocr |= SD_OCR_S18R;

	/*
	 * If the host can supply more than 150mA at current voltage,
	 * XPC should be set to 1.
	 */
	max_current = sd_get_host_max_current(host);
	if (max_current > 150)
		ocr |= SD_OCR_XPC;

	err = mmc_send_app_op_cond(host, ocr, rocr);
	if (err)
	{
	    printk(KERN_ERR "%s:send acmd41 to get ocr fail,err=%d\n",mmc_hostname(host),err);
		return err;
	}
	printk(KERN_ERR "%s:send acmd41 with ocr:0x%x,get rocr:0x%x\n",mmc_hostname(host),ocr,*rocr);

	/*
	 * In case CCS and S18A in the response is set, start Signal Voltage
	 * Switch procedure. SPI mode doesn't support CMD11.
	 */
	if (!mmc_host_is_spi(host) && rocr &&
	   ((*rocr & 0x41000000) == 0x41000000)) {
	   printk(KERN_ERR "%s:support uhs-1,and accept 1.8V switching request\n",mmc_hostname(host));
		err = mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_180);
		if (err == -EAGAIN) {
			retries--;
			goto try_again;
		} else if (err) {
			retries = 0;
			goto try_again;
		}
	}

	if (mmc_host_is_spi(host))
		err = mmc_send_cid(host, cid);
	else
		err = mmc_all_send_cid(host, cid);

	return err;
}

int mmc_sd_get_csd(struct mmc_host *host, struct mmc_card *card)
{
	int err;
#ifdef CONFIG_HUAWEI_SDCARD_DSM
	int buff_len;
	char *log_buff;
#endif
	/*
	 * Fetch CSD from card.
	 */
	err = mmc_send_csd(card, card->raw_csd);
#ifdef CONFIG_HUAWEI_SDCARD_DSM
	if(!strcmp(mmc_hostname(host), "mmc1"))
	{
		 dsm_sdcard_cmd_logs[DSM_SDCARD_CMD9_R0].value = card->raw_csd[0];
		 dsm_sdcard_cmd_logs[DSM_SDCARD_CMD9_R1].value = card->raw_csd[1];
		 dsm_sdcard_cmd_logs[DSM_SDCARD_CMD9_R2].value = card->raw_csd[2];
		 dsm_sdcard_cmd_logs[DSM_SDCARD_CMD9_R3].value = card->raw_csd[3];
	}
	
	if (err)
	{
		if(-ENOMEDIUM != err && -ETIMEDOUT != err
		&& !strcmp(mmc_hostname(host), "mmc1") && !dsm_client_ocuppy(sdcard_dclient))
		{
			log_buff = dsm_sdcard_get_log(DSM_SDCARD_CMD9_R3,err);
			buff_len = strlen(log_buff);
			dsm_client_copy(sdcard_dclient,log_buff,buff_len + 1);
			dsm_client_notify(sdcard_dclient, DSM_SDCARD_CMD9_RESP_ERR);

		}
	
		return err;
	}
#else
	if (err)
		return err;
#endif	

	err = mmc_decode_csd(card);
	if (err)
		return err;

	return 0;
}

int mmc_sd_setup_card(struct mmc_host *host, struct mmc_card *card,
	bool reinit)
{
	int err;
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	int retries;
#endif

	if (!reinit) {
		/*
		 * Fetch SCR from card.
		 */
		err = mmc_app_send_scr(card, card->raw_scr);
		if (err)
			{
			printk(KERN_ERR "%s:send acmd51 to get scr fail when init sd first time,err=%d,check data0!!!!\n",mmc_hostname(host),err);
			return err;
			}
		err = mmc_decode_scr(card);
		if (err)
			return err;

		/*
		 * Fetch and process SD Status register.
		 */
		err = mmc_read_ssr(card);
		if (err)
			{
			printk(KERN_ERR "%s:send cmd13 to get ssr fail when init sd first time,err=%d\n",mmc_hostname(host),err);
			return err;
			}
		/* Erase init depends on CSD and SSR */
		mmc_init_erase(card);

		/*
		 * Fetch switch information from card.
		 */
#ifdef CONFIG_MMC_PARANOID_SD_INIT
		for (retries = 1; retries <= 3; retries++) {
			err = mmc_read_switch(card);
			if (!err) {
				if (retries > 1) {
					printk(KERN_WARNING
					       "%s: recovered\n", 
					       mmc_hostname(host));
				}
				break;
			} else {
				printk(KERN_WARNING
				       "%s: read switch failed (attempt %d)\n",
				       mmc_hostname(host), retries);
			}
		}
#else
		err = mmc_read_switch(card);
#endif

		if (err)
			{
			printk(KERN_ERR "%s:send cmd6 fail when init sd first time,err=%d\n",mmc_hostname(host),err);
			return err;
			}
	}

	/*
	 * For SPI, enable CRC as appropriate.
	 * This CRC enable is located AFTER the reading of the
	 * card registers because some SDHC cards are not able
	 * to provide valid CRCs for non-512-byte blocks.
	 */
	if (mmc_host_is_spi(host)) {
		err = mmc_spi_set_crc(host, use_spi_crc);
		if (err)
			return err;
	}

	/*
	 * Check if read-only switch is active.
	 */
	if (!reinit) {
		int ro = -1;

		if (host->ops->get_ro) {
			mmc_host_clk_hold(card->host);
			ro = host->ops->get_ro(host);
			mmc_host_clk_release(card->host);
		}

		if (ro < 0) {
			pr_warning("%s: host does not "
				"support reading read-only "
				"switch. assuming write-enable.\n",
				mmc_hostname(host));
		} else if (ro > 0) {
			mmc_card_set_readonly(card);
		}
	}

	return 0;
}

unsigned mmc_sd_get_max_clock(struct mmc_card *card)
{
	unsigned max_dtr = (unsigned int)-1;

	if (mmc_card_highspeed(card)) {
		if (max_dtr > card->sw_caps.hs_max_dtr)
			max_dtr = card->sw_caps.hs_max_dtr;
	} else if (max_dtr > card->csd.max_dtr) {
		max_dtr = card->csd.max_dtr;
	}

	return max_dtr;
}

#ifdef CONFIG_MMC_PASSWORDS
void mmc_sd_go_highspeed(struct mmc_card *card)
{
	mmc_card_set_highspeed(card);//temp remove this
	mmc_set_timing(card->host, MMC_TIMING_SD_HS);
}
#endif

/*
 * Handle the detection and initialisation of a card.
 *
 * In the case of a resume, "oldcard" will contain the card
 * we're trying to reinitialise.
 */
int mmc_sd_init_card(struct mmc_host *host, u32 ocr,
	struct mmc_card *oldcard)
{
	struct mmc_card *card;
	int err;
	u32 cid[4];
	u32 rocr = 0;
#ifdef CONFIG_MMC_PASSWORDS
	u32 status = 0;
#endif

	BUG_ON(!host);
	WARN_ON(!host->claimed);

	err = mmc_sd_get_cid(host, ocr, cid, &rocr);
	if (err)
		{
		 printk(KERN_ERR "%s:send cmd2 to get cid fail,err=%d\n",mmc_hostname(host),err);
		return err;
		}
    //end:dangjian
	if (oldcard) {
		if (memcmp(cid, oldcard->raw_cid, sizeof(cid)) != 0)
			{
			  printk(KERN_ERR "%s:new cid don't match old cid in resume!!\n",mmc_hostname(host));
			  printk(KERN_ERR "new cid:0x%08x,%08x,%08x,%08x\n",cid[0],cid[1],cid[2],cid[3]);
			  printk(KERN_ERR "old cid:0x%08x,%08x,%08x,%08x\n",oldcard->raw_cid[0],oldcard->raw_cid[1],oldcard->raw_cid[2],oldcard->raw_cid[3]);
			return -ENOENT;
			}
		card = oldcard;
	} else {
		/*
		 * Allocate card structure.
		 */
		card = mmc_alloc_card(host, &sd_type);
		if (IS_ERR(card))
		{
		    printk(KERN_ERR "%s:mmc_alloc_card failed\n",mmc_hostname(host));
			return PTR_ERR(card);
		}
		card->type = MMC_TYPE_SD;
		memcpy(card->raw_cid, cid, sizeof(card->raw_cid));
	}

	/*
	 * For native busses:  get card RCA and quit open drain mode.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_send_relative_addr(host, &card->rca);
		if (err)
			{
			 printk(KERN_ERR "%s:send cmd3 to get RCA fail,err=%d\n",mmc_hostname(host),err);
			goto free_card;
			}
	}
#ifdef CONFIG_MMC_PASSWORDS
    /* whether voltage switch just for sd card */
	if (rocr & SD_ROCR_S18A)
        card->swith_voltage = true;
    else
        card->swith_voltage = false;

    printk("%s, sd card voltage swith(3.3v--> 1.8v) is %d\n", __func__, card->swith_voltage);

	/*
	 * Check if card is locked.
    */
	err = mmc_send_status(card, &status);
	if (err)
		goto free_card;
	if (status & R1_CARD_IS_LOCKED)
	//	mmc_card_set_locked(card);
	{
		mmc_card_set_encrypted(card);
		mmc_card_set_locked(card);
	}
#endif

	if (!oldcard) {
		err = mmc_sd_get_csd(host, card);
		if (err)
			{
			 /*
			  *send cmd9 to get csd information(e.g.block length,card capacity etc)
			 */
			printk(KERN_ERR "%s:send cmd9 to get csd fail when init sd first time,err=%d\n",mmc_hostname(host),err);
			goto free_card;
			}

		mmc_decode_cid(card);
	}

	/*
	 * Select card, as all following commands rely on that.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_select_card(card);
		if (err)
			{
			printk(KERN_ERR "%s:send cmd7 to select sd fail ,err=%d\n",mmc_hostname(host),err);
			goto free_card;
			}
	}
#if CONFIG_MMC_PASSWORDS
	if (status & R1_CARD_IS_LOCKED) {
		if(card->auto_unlock )
		{
			if(card->unlock_pwd[0] > 0 )
			{
				//unlock sd card
				err = mmc_lock_unlock_by_buf(card,  card->unlock_pwd+1,(int)card->unlock_pwd[0], MMC_LOCK_MODE_UNLOCK);
				if(err)
				{
					printk("[SDLOCK] %s unlock failed \n",__func__);
				}
				else
				{
					printk("[SDLOCK] %s unlock success \n",__func__);

					if(!mmc_card_locked(card))
					{
						card->auto_unlock = false;
						printk("[SDLOCK] %s unlock success and sdcard status is unlocked.\n",__func__);
					}
					else
					{
						printk("[SDLOCK] %s unlock success but sdcard status is locked, abnormal status.\n",__func__);
					}

				}
				//Check if card is locked
				err = mmc_send_status(card, &status);
				if (err)
				{
					printk("[SDLOCK] %s resume sd card exception /n",__func__);
					goto free_card;
				}
			}
			else
			{
				printk("[SDLOCK] %s unlock password is null\n",__func__);
			}
		} 

		if (status & R1_CARD_IS_LOCKED)
		{
			printk(KERN_WARNING "[SDLOCK] sdcard is locked\n");
			goto locked_card;
		}
		else
		{
			printk(KERN_WARNING "[SDLOCK] sdcard resume to unlocked\n");
		}
	}
	else
	{
		printk(KERN_WARNING "[SDLOCK] sdcard is unlocked\n");
	}
#endif

	err = mmc_sd_setup_card(host, card, oldcard != NULL);
	if (err)
	{
	    printk(KERN_ERR "%s:mmc_sd_setup_card fail\n",mmc_hostname(host));
		goto free_card;
	}

	/* Initialization sequence for UHS-I cards */
	if (rocr & SD_ROCR_S18A) {
		err = mmc_sd_init_uhs_card(card);
		if (err)
			{
			printk(KERN_ERR "%s:send cmd6 to uhs-1 sd fail,err=%d\n",mmc_hostname(host),err);
			goto free_card;
			}
		/* Card is an ultra-high-speed card */
		mmc_card_set_uhs(card);
	} else {
		/*
		 * Attempt to change to high-speed (if supported)
		 */
		err = mmc_sd_switch_hs(card);
		if (err > 0)
			mmc_sd_go_highspeed(card);
		else if (err)
			{
			printk(KERN_ERR "%s:send cmd6 into high-speed mode fail(not uhs-1) ,err=%d\n",mmc_hostname(host),err);
			goto free_card;
			}
		/*
		 * Set bus speed.
		 */
		mmc_set_clock(host, mmc_sd_get_max_clock(card));

		/*
		 * Switch to wider bus (if supported).
		 */
		if ((host->caps & MMC_CAP_4_BIT_DATA) &&
			(card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {
			err = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
			if (err)
				{
				printk(KERN_ERR "%s:send acmd6 into 4bit mode fail(not uhs-1),err=%d\n",mmc_hostname(host),err);
				goto free_card;
				}
			mmc_set_bus_width(host, MMC_BUS_WIDTH_4);
		}
	}
#ifdef CONFIG_MMC_PASSWORDS
locked_card:
#endif
	host->card = card;
	return 0;

free_card:
	if (!oldcard)
		mmc_remove_card(card);

	return err;
}

/*
 * Host is being removed. Free up the current card.
 */
static void mmc_sd_remove(struct mmc_host *host)
{
	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_remove_card(host->card);
	host->card = NULL;
}

/*
 * Card detection - card is alive.
 */
static int mmc_sd_alive(struct mmc_host *host)
{
	return mmc_send_status(host->card, NULL);
}

/*
 * Card detection callback from host.
 */
static void mmc_sd_detect(struct mmc_host *host)
{
	int err = 0;
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	int retries = 5;
#endif

	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_get_card(host->card);

	/*
	 * Just check if our card has been removed.
	 */
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	while(retries) {
		err = mmc_send_status(host->card, NULL);
		if (err) {
			retries--;
			udelay(5);
			continue;
		}
		break;
	}
	if (!retries) {
		printk(KERN_ERR "%s(%s): Unable to re-detect card (%d)\n",
		       __func__, mmc_hostname(host), err);
	}
#else
	err = _mmc_detect_card_removed(host);
#endif

	mmc_put_card(host->card);

	if (err) {
		mmc_sd_remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		mmc_power_off(host);
		mmc_release_host(host);
	}
}

/*
 * Suspend callback from host.
 */
static int mmc_sd_suspend(struct mmc_host *host)
{
	int err = 0;

	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_claim_host(host);
	if (!mmc_host_is_spi(host))
		err = mmc_deselect_cards(host);
#ifdef CONFIG_MMC_PASSWORDS	
	/*if sd is unlock , set auto unlock flag , so system resume auto unlock sd card */
	if(!mmc_card_locked(host->card))
	{
		pr_err("%s: [SDLOCK] sdcard is unlocked on suspend and set auto_unlock = true. \n", mmc_hostname(host));
		host->card->auto_unlock = true;
	}
	host->card->state &= ~(MMC_STATE_LOCKED | MMC_STATE_ENCRYPT);
#endif	
	host->card->state &= ~MMC_STATE_HIGHSPEED;
	mmc_release_host(host);

	return err;
}

/*
 * Resume callback from host.
 *
 * This function tries to determine if the same card is still present
 * and, if so, restore all state to it.
 */
static int mmc_sd_resume(struct mmc_host *host)
{
	int err;
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	int retries;
#endif

	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_claim_host(host);
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	retries = 5;
	while (retries) {
		err = mmc_sd_init_card(host, host->ocr, host->card);

		if (err) {
			printk(KERN_ERR "%s: Re-init card rc = %d (retries = %d)\n",
			       mmc_hostname(host), err, retries);
			mdelay(5);
			retries--;
			continue;
		}
		break;
	}
#else
	err = mmc_sd_init_card(host, host->ocr, host->card);
#endif
	mmc_release_host(host);

	return err;
}

/*
 * Callback for runtime_suspend.
 */
static int mmc_sd_runtime_suspend(struct mmc_host *host)
{
	int err;

	if (!(host->caps & MMC_CAP_AGGRESSIVE_PM))
		return 0;

	mmc_claim_host(host);

	err = mmc_sd_suspend(host);
	if (err) {
		pr_err("%s: error %d doing aggessive suspend\n",
			mmc_hostname(host), err);
		goto out;
	}
	mmc_power_off(host);

out:
	mmc_release_host(host);
	return err;
}

/*
 * Callback for runtime_resume.
 */
static int mmc_sd_runtime_resume(struct mmc_host *host)
{
	int err;

	if (!(host->caps & MMC_CAP_AGGRESSIVE_PM))
		return 0;

	mmc_claim_host(host);

	mmc_power_up(host);
	err = mmc_sd_resume(host);
	if (err)
		pr_err("%s: error %d doing aggessive resume\n",
			mmc_hostname(host), err);

	mmc_release_host(host);
	return 0;
}

static int mmc_sd_power_restore(struct mmc_host *host)
{
	int ret;

	host->card->state &= ~MMC_STATE_HIGHSPEED;
	mmc_claim_host(host);
	ret = mmc_sd_init_card(host, host->ocr, host->card);
	mmc_release_host(host);

	return ret;
}

static const struct mmc_bus_ops mmc_sd_ops = {
	.remove = mmc_sd_remove,
	.detect = mmc_sd_detect,
	.suspend = NULL,
	.resume = NULL,
	.power_restore = mmc_sd_power_restore,
	.alive = mmc_sd_alive,
};

static const struct mmc_bus_ops mmc_sd_ops_unsafe = {
	.remove = mmc_sd_remove,
	.detect = mmc_sd_detect,
	.runtime_suspend = mmc_sd_runtime_suspend,
	.runtime_resume = mmc_sd_runtime_resume,
	.suspend = mmc_sd_suspend,
	.resume = mmc_sd_resume,
	.power_restore = mmc_sd_power_restore,
	.alive = mmc_sd_alive,
#ifdef CONFIG_MMC_PASSWORDS
	.sysfs_add = mmc_sd_sysfs_add,
	.sysfs_remove = mmc_sd_sysfs_remove,
#endif
};

static void mmc_sd_attach_bus_ops(struct mmc_host *host)
{
	const struct mmc_bus_ops *bus_ops;

	if (!mmc_card_is_removable(host))
		bus_ops = &mmc_sd_ops_unsafe;
	else
		bus_ops = &mmc_sd_ops;
	mmc_attach_bus(host, bus_ops);
}

/*
 * Starting point for SD card init.
 */
int mmc_attach_sd(struct mmc_host *host)
{
	int err;
	u32 ocr = 0;
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	int retries;
#endif

	BUG_ON(!host);
	WARN_ON(!host->claimed);

	err = mmc_send_app_op_cond(host, 0, &ocr);
	if (err)
		return err;

	mmc_sd_attach_bus_ops(host);
	if (host->ocr_avail_sd)
		host->ocr_avail = host->ocr_avail_sd;

	/*
	 * We need to get OCR a different way for SPI.
	 */
	if (mmc_host_is_spi(host)) {
		mmc_go_idle(host);

		err = mmc_spi_read_ocr(host, 0, &ocr);
		if (err)
			goto err;
	}

	/*
	 * Sanity check the voltages that the card claims to
	 * support.
	 */
	if (ocr & 0x7F) {
		pr_warning("%s: card claims to support voltages "
		       "below the defined range. These will be ignored.\n",
		       mmc_hostname(host));
		ocr &= ~0x7F;
	}

	if ((ocr & MMC_VDD_165_195) &&
	    !(host->ocr_avail_sd & MMC_VDD_165_195)) {
		pr_warning("%s: SD card claims to support the "
		       "incompletely defined 'low voltage range'. This "
		       "will be ignored.\n", mmc_hostname(host));
		ocr &= ~MMC_VDD_165_195;
	}

	host->ocr = mmc_select_voltage(host, ocr);

	/*
	 * Can we support the voltage(s) of the card(s)?
	 */
	if (!host->ocr) {
		err = -EINVAL;
		goto err;
	}

	/*
	 * Detect and init the card.
	 */
#ifdef CONFIG_MMC_PARANOID_SD_INIT
	retries = 5;
	while (retries) {
		err = mmc_sd_init_card(host, host->ocr, NULL);
		if (err) {
			retries--;
			continue;
		}
		break;
	}

	if (!retries) {
		printk(KERN_ERR "%s: mmc_sd_init_card() failure (err = %d)\n",
		       mmc_hostname(host), err);
		goto err;
	}
#else
	err = mmc_sd_init_card(host, host->ocr, NULL);
	if (err)
		goto err;
#endif

	mmc_release_host(host);
	err = mmc_add_card(host->card);
	mmc_claim_host(host);
	if (err)
		goto remove_card;

	return 0;

remove_card:
	mmc_release_host(host);
	mmc_remove_card(host->card);
	host->card = NULL;
	mmc_claim_host(host);
err:
	mmc_detach_bus(host);

	pr_err("%s: error %d whilst initialising SD card\n",
		mmc_hostname(host), err);

	return err;
}

#ifdef CONFIG_HW_SD_HEALTH_DETECT
unsigned int mmc_get_sd_speed()
{
   unsigned int speed = 0;
   switch(g_sd_speed_class){
   case 0x00:
        speed = 0;
        break;
   case 0x01:
        speed = 2;
        break;
   case 0x02:
        speed = 4;
        break;
   case 0x03:
        speed = 6;
        break;
   case 0x04:
        speed = 10;
        break;
   default:
        speed = 2;
}
   return speed;
}
#endif

#ifdef CONFIG_HUAWEI_SDCARD_DSM
char *dsm_sdcard_get_log(int cmd,int err)
{	
	int i;
	int ret = 0;
	int buff_size = sizeof(g_dsm_log_sum);
	char *dsm_log_buff = g_dsm_log_sum;
	 
	memset(g_dsm_log_sum,0,buff_size);
	
	ret = snprintf(dsm_log_buff,buff_size,"Err : %d\n",err);
	dsm_log_buff += ret;
	buff_size -= ret;
	
	for(i = 0; i <= cmd; i++)
	{
		
		ret = snprintf(dsm_log_buff,buff_size,
		"%s%08x\n",dsm_sdcard_cmd_logs[i].log,dsm_sdcard_cmd_logs[i].value);/* [false alarm]*/
		if(ret > buff_size -1)
		{
			printk(KERN_ERR "Buff size is not enough\n");
			printk(KERN_ERR "%s",g_dsm_log_sum);
			return g_dsm_log_sum;
		}
		
		dsm_log_buff += ret;
		buff_size -= ret;
	}

	pr_debug("DSM_DEBUG %s",g_dsm_log_sum);
	
	return g_dsm_log_sum;
		
}
EXPORT_SYMBOL(dsm_sdcard_get_log);
#endif
