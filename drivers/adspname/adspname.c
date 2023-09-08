/* #############################################################################
 *****************************************************************************
 Copyright (c) 2005, Advantech eAutomation Division
 Advantech Co., Ltd.
 Linux DIO driver
 THIS IS AN UNPUBLISHED WORK CONTAINING CONFIDENTIAL AND PROPRIETARY
 INFORMATION WHICH IS THE PROPERTY OF ADVANTECH AUTOMATION CORP.

 ANY DISCLOSURE, USE, OR REPRODUCTION, WITHOUT WRITTEN AUTHORIZATION FROM
 ADVANTECH AUTOMATION CORP., IS STRICTLY PROHIBITED.
 *****************************************************************************
#############################################################################

File: 	         adspname.c
Author:          Joshua Lan
Version:         2.00 <09/19/2017>

Change log:      Version 1.00 <01/16/2006> Joshua Lan
                   - Inivial version
                 Version 2.00 <09/19/2017> Ji Xu
                   - Update to support detect Advantech product name
				     in UEFI BIOS(DMI).
                   - Add some new ioctl item.
                 Version 2.01 <11/02/2017> Ji Xu
                   - Fixed some complier warnings in ISO C90 environment.
                 Version 2.02 <03/22/2018> Ji Xu
                   - Add some new device title.
                 Version 2.03 <09/05/2018> Jianfeng.dai
                   - Add /proc/board interface for device name
                 Version 2.04 <09/13/2020> Jianfeng.dai
                   - use kernel API to get DMI information

Description:     This is a virtual driver to detect Advantech module.
Status: 	     works

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted, provided
that the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.  This software is provided "as is" without express or
implied warranty.
----------------------------------------------------------------------------*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/ioctl.h>
#include <linux/dmi.h>
#include <asm/io.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/param.h>		
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) ((a)*65536+(b)*256+(c))
#endif

#define ADVANTECH_ADSPNAME_VER   "2.04"
#define ADVANTECH_ADSPNAME_DATE  "09/13/2020" 

#define ADSPNAME_MAGIC 		'p'
#define GETPNAME			_IO(ADSPNAME_MAGIC, 1)
#define CHKADVBOARD			_IO(ADSPNAME_MAGIC, 2)

//#define ADVANTECH_DEBUG
#ifdef ADVANTECH_DEBUG
#define DEBUGPRINT printk
#else
#define DEBUGPRINT(a, ...)
#endif

#define DEVICE_NODE_NAME 		"adspname"
#define DEVICE_CLASS_NAME 		"adspname"
#define DEVICE_FILE_NAME 		"adspname"
#define ADVSPNAME_MAJOR 		0
/****************************************/
#define _ADVANTECH_BOARD_NAME_LENGTH        64
#define BIOS_START_ADDRESS		0xF0000
#define BIOS_MAP_LENGTH			0xFFFF

#define AWARD_BIOS_NAME                     "Phoenix - AwardBIOS"
#define AWARD_BIOS_NAME_ADDRESS             0x000FE061
#define AWARD_BIOS_NAME_LENGTH              strlen(AWARD_BIOS_NAME)
#define AWARD_ADVANTECH_BOARD_ID_ADDRESS    0x000FEC80
#define AWARD_ADVANTECH_BOARD_ID_LENGTH     32
#define AWARD_ADVANTECH_BOARD_NAME_ADDRESS  0x000FE0C1
#define AWARD_ADVANTECH_BOARD_NAME_LENGTH   _ADVANTECH_BOARD_NAME_LENGTH

#define AMI_BIOS_NAME                       "AMIBIOS"
#define AMI_BIOS_NAME_ADDRESS               0x000FF400
#define AMI_BIOS_NAME_LENGTH                strlen(AMI_BIOS_NAME)
#define AMI_ADVANTECH_BOARD_ID_ADDRESS      0x000FE840
#define AMI_ADVANTECH_BOARD_ID_LENGTH       32
#define AMI_ADVANTECH_BOARD_NAME_LENGTH     _ADVANTECH_BOARD_NAME_LENGTH
/***************************************/

static unsigned char is_adv_dev[8] = "yes";
static unsigned char no_adv_dev[8] = "no";
static unsigned char board_id[_ADVANTECH_BOARD_NAME_LENGTH];
static bool check_result = false;
struct class *my_class;
static unsigned char *uc_ptaddr;
//static unsigned char *uc_epsaddr;
static int adspname_major = ADVSPNAME_MAJOR;
struct adspname_cdev *ads_cdev;

struct adspname_cdev {
	struct cdev dev;
	spinlock_t adspname_spinlock;
};

typedef struct _adv_bios_info {
	int eps_table;
	unsigned short eps_length;
	unsigned int *eps_address;
	unsigned short eps_types;
	char *baseboard_product_name[32];
} adv_bios_info;
//static adv_bios_info adspname_info;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
static ssize_t adspname_ioctl (struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg )
#else
static long adspname_ioctl (struct file *file, unsigned int cmd, unsigned long arg )
#endif
{  
	DEBUGPRINT("in ioctl()\n");

	switch ( cmd )
	{
	case GETPNAME:
		DEBUGPRINT(KERN_INFO "Board name is: %s\n", board_id);
		copy_to_user((void*)arg, board_id, sizeof(board_id));
		break;
	case CHKADVBOARD:
		DEBUGPRINT(KERN_INFO "Board name is: %s\n", board_id);
		if(check_result)
			copy_to_user((void*)arg, is_adv_dev, sizeof(is_adv_dev));
		else
			copy_to_user((void*)arg, no_adv_dev, sizeof(no_adv_dev));
		break;

	default:
		return -1;
	}
	return 0;
}

static int adspname_open (
		struct inode *inode, 
		struct file *file )
{
	struct adspname_cdev *dev;

	DEBUGPRINT("in open()\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	MOD_INC_USE_COUNT;
#endif

	dev = container_of(inode->i_cdev,struct adspname_cdev,dev);
	file->private_data = dev;
	return 0;
}

static int adspname_release (
		struct inode *inode, 
		struct file *file )
{
	DEBUGPRINT("in release\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	MOD_DEC_USE_COUNT;
#endif
	return 0;
}

static int board_proc_show(struct seq_file *seq, void *v)
{
   char boardname[60]; 
   DEBUGPRINT("in board_proc_show()\n");
   memset(boardname,0,sizeof(boardname));
   strcat(boardname,board_id);
   strcat(boardname,"\n"); 
   seq_puts(seq, boardname );
   return 0;        
}

static int board_proc_open(struct inode *inode, struct file *file)
{
   return  single_open(file, board_proc_show, inode->i_private);
} 

static const struct proc_ops  board_proc_ops = {
 .proc_open  = board_proc_open,
 .proc_read  = seq_read,
  //.write  = mytest_proc_write,
 .proc_lseek  = seq_lseek,
 .proc_release = single_release,
};


static struct file_operations adspname_fops = {
owner:		THIS_MODULE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
	ioctl:		adspname_ioctl,
#else
	unlocked_ioctl: adspname_ioctl,
#endif
	open:		adspname_open,
	release:	adspname_release,
};

void adspname_cleanup ( void )
{
	dev_t devno=MKDEV(adspname_major,0);
	device_destroy(my_class,devno);
	class_destroy(my_class);
	cdev_del(&ads_cdev->dev);
	kfree(ads_cdev);
	unregister_chrdev_region(devno,1);
	if(check_result) {
		remove_proc_entry("board",NULL);
	}
	DEBUGPRINT("in cleanup\n");
}

static char * _MapIoSpace(unsigned int HWAddress, unsigned long Size)
{
	char *              ioPortBase = NULL;
	ioPortBase = ioremap(HWAddress, Size);
	return (ioPortBase);
}

static void _UnMapIoSpace(char * BaseAddress, unsigned long Size)
{
	iounmap(BaseAddress);
}

static bool _IsBiosMatched(unsigned int BiosAddress, char *pBiosName, int BiosNameLen)
{
	bool bMatched = false;
	char * pVMem;

	pVMem = _MapIoSpace(BiosAddress, BiosNameLen);
	if(!pVMem) return false;
	if(!strncmp((char *)pVMem, pBiosName, BiosNameLen))
		bMatched = true;
	_UnMapIoSpace(pVMem, BiosNameLen);

	return bMatched;
}

int adspname_init ( void )
{ 
	dev_t devno;
	int ret;
	int loopc = 0;
	int length = 0;
	//int type0_str = 0;
	//int type1_str = 0;
	//int i = 0;
	int is_advantech = 0;
	const char * vendor_id;
	const char * product_id;
	struct proc_dir_entry *entry;   
	
	if (adspname_major) {
		devno = MKDEV(adspname_major,0);
		ret = register_chrdev_region(devno,1,DEVICE_NODE_NAME);
		if (ret < 0) {
			DEBUGPRINT("register fail!\r\n");
			goto exit0;
		}
	} else {
		ret = alloc_chrdev_region(&devno,0,1,DEVICE_NODE_NAME);
		if (ret < 0) {
			DEBUGPRINT("register fail!\r\n");
			goto exit0;
		}
		adspname_major = MAJOR(devno);
	}

	ads_cdev = kmalloc(sizeof(struct adspname_cdev),GFP_KERNEL);
	if (!ads_cdev) {
		ret = -ENOMEM;
		goto exit1;
	}

	memset(ads_cdev,0,sizeof(struct adspname_cdev));
	spin_lock_init(&ads_cdev->adspname_spinlock);
	cdev_init(&ads_cdev->dev,&adspname_fops);
	ads_cdev->dev.owner = THIS_MODULE;
	ret = cdev_add(&ads_cdev->dev,devno,1);
	if (ret < 0) {
		DEBUGPRINT("cdev add fail!\r\n");
		goto exit2;
	}

	my_class = class_create(THIS_MODULE,DEVICE_CLASS_NAME);
	if (IS_ERR(my_class)) {
		DEBUGPRINT("class create fail!\r\n");
		goto exit3;
	}

	device_create(my_class,NULL,devno,"%s",DEVICE_FILE_NAME);

	//printk(KERN_INFO "=====================================================\n");
	printk(KERN_INFO "Product Name Detecting driver V%s [%s]\n", 
			ADVANTECH_ADSPNAME_VER, ADVANTECH_ADSPNAME_DATE);
	printk(KERN_INFO "Advantech eAutomation Division.\n");
	//printk(KERN_INFO "=====================================================\n");
	
 

	vendor_id = dmi_get_system_info(DMI_SYS_VENDOR);
	if (memcmp(vendor_id,"Advantech", sizeof("Advantech")) == 0 )
	{
		DEBUGPRINT(KERN_INFO "UEFI BIOS\n");
		is_advantech = true;
	}

	if (is_advantech) {
		check_result = true;
		product_id = dmi_get_system_info(DMI_PRODUCT_NAME);
		length = 0;
		while ( (product_id[length] != ' ') 
				&& (length < _ADVANTECH_BOARD_NAME_LENGTH)) {
			length += 1;
		}
		memset(board_id, 0, sizeof(board_id));
	        memmove(board_id, product_id, length);
		printk(KERN_INFO "This Product is: %s\n", board_id);

		if (memcmp(board_id, "MIC-3329", sizeof("MIC-3329")) != 0 )
		{
			printk(KERN_ERR "Error: Not Advantech MIC-3329 Product.\n");
			panic("Not Advantech MIC-3329 Product.\n");
		}

		entry = proc_create("board", 0x0444, NULL, &board_proc_ops);
		if (NULL == entry)
		{
			DEBUGPRINT(KERN_INFO "Count not create /proc/board file!\n");
		}
		return 0;
	} else {
		printk(KERN_ERR "Error: Not Advantech Product.\n");
		panic("Not Advantech Product.\n");
	}

	//old BIOS
	if (_IsBiosMatched(AWARD_BIOS_NAME_ADDRESS, AWARD_BIOS_NAME, AWARD_BIOS_NAME_LENGTH)) {
		// If Award BIOS
		DEBUGPRINT(KERN_INFO "Award BIOS\n");
	} else if (_IsBiosMatched(AMI_BIOS_NAME_ADDRESS, AMI_BIOS_NAME, AMI_BIOS_NAME_LENGTH)) {
		// If AMI BIOS
		DEBUGPRINT(KERN_INFO "AMI BIOS\n");
	} 
	else
	{
		DEBUGPRINT(KERN_INFO "unknow BIOS\n");
		goto exit3;
	}

	if (!(uc_ptaddr = ioremap(BIOS_START_ADDRESS, BIOS_MAP_LENGTH))) {
		printk(KERN_ERR "Error: ioremap_nocache() \n");
		return -ENXIO;
	}


        /*
	// Try to Read the product name from UEFI BIOS(DMI) EPS table
	for (loopc = 0; loopc < BIOS_MAP_LENGTH; loopc++) {
		if (uc_ptaddr[loopc] == '_' 
					&& uc_ptaddr[loopc+0x1] == 'S' 
					&& uc_ptaddr[loopc+0x2] == 'M' 
					&& uc_ptaddr[loopc+0x3] == '_'
					&& uc_ptaddr[loopc+0x10] == '_'
					&& uc_ptaddr[loopc+0x11] == 'D'
					&& uc_ptaddr[loopc+0x12] == 'M'
					&& uc_ptaddr[loopc+0x13] == 'I'
					&& uc_ptaddr[loopc+0x14] == '_'
					) {
			DEBUGPRINT(KERN_INFO "UEFI BIOS \n");
			adspname_info.eps_table = 1;
			break;
		}
	}

	memset(board_id, 0, sizeof(board_id));
	check_result = false;
	// If EPS table exist, read type1(system information)
	if (adspname_info.eps_table) {
		if (!(uc_epsaddr = (char *)ioremap(((unsigned int *)&uc_ptaddr[loopc+0x18])[0], 
						((unsigned short *)&uc_ptaddr[loopc+0x16])[0]))) {
			if (!(uc_epsaddr = (char *)ioremap(((unsigned int *)&uc_ptaddr[loopc+0x18])[0], 
							((unsigned short *)&uc_ptaddr[loopc+0x16])[0]))) {
				printk(KERN_ERR "Error: both ioremap_nocache() and ioremap_cache() exec failed! \n");
				return -ENXIO;
			}
		}
		type0_str = (int)uc_epsaddr[1];
		for (i = type0_str; i < (type0_str+512); i++) {
			if (uc_epsaddr[i] == 0 && uc_epsaddr[i+1] == 0 && uc_epsaddr[i+2] == 1) {
				type1_str = i + uc_epsaddr[i+3];
				break;
			}
		}
		for (i = type1_str; i < (type1_str+512); i++) {
			if (uc_epsaddr[i] == 'A' && uc_epsaddr[i+1] == 'd' && uc_epsaddr[i+2] == 'v' 
					&& uc_epsaddr[i+3] == 'a' && uc_epsaddr[i+4] == 'n' && uc_epsaddr[i+5] == 't' 
					&& uc_epsaddr[i+6] == 'e' && uc_epsaddr[i+7] == 'c' &&uc_epsaddr[i+8] == 'h') {
				check_result = true;
				is_advantech = 1;
				DEBUGPRINT(KERN_INFO "match Advantech \n");
			}
			if (uc_epsaddr[i] == 0) {
				i++;
				type1_str = i;
				break;
			}
		}
		length = 2;
		while ((uc_epsaddr[type1_str + length] != 0)
				&& (length < _ADVANTECH_BOARD_NAME_LENGTH)) {
			length += 1;
		}
		memmove(board_id, &uc_epsaddr[type1_str], length);
		iounmap((void *)uc_epsaddr);
		if (is_advantech) {
			iounmap(( void* )uc_ptaddr);
			printk(KERN_INFO "This Advantech Product is: %s\n", board_id);
			entry = proc_create("board", 0x0444, NULL, &board_proc_ops);
			if (NULL == entry)
			{
			DEBUGPRINT(KERN_INFO "Count not create /proc/board file!\n");
			}
			return 0;
		}
	}                         while ((uc_ptaddr[loopc + length] != ' ')
                                        && (length < _ADVANTECH_BOARD_NAME_LENGTH)) {
                                length += 1;
                        }

	*/
	// It is an old BIOS, read from 0x000F0000
	for (loopc = 0; loopc < BIOS_MAP_LENGTH; loopc++) {
		if ((uc_ptaddr[loopc]=='T' && uc_ptaddr[loopc+1]=='P' && uc_ptaddr[loopc+2]=='C')
				|| (uc_ptaddr[loopc]=='A' && uc_ptaddr[loopc+1]=='D' && uc_ptaddr[loopc+2]=='A' && uc_ptaddr[loopc+3]=='M')
				|| (uc_ptaddr[loopc]=='P' && uc_ptaddr[loopc+1]=='P' && uc_ptaddr[loopc+2]=='C')
				|| (uc_ptaddr[loopc]=='U' && uc_ptaddr[loopc+1]=='N' && uc_ptaddr[loopc+2]=='O')
				|| (uc_ptaddr[loopc]=='E' && uc_ptaddr[loopc+1]=='C' && uc_ptaddr[loopc+2]=='U')
				|| (uc_ptaddr[loopc]=='I' && uc_ptaddr[loopc+1]=='T' && uc_ptaddr[loopc+2]=='A')
				|| (uc_ptaddr[loopc]=='A' && uc_ptaddr[loopc+1]=='I' && uc_ptaddr[loopc+2]=='M' && uc_ptaddr[loopc+3]=='C')
				|| (uc_ptaddr[loopc]=='A' && uc_ptaddr[loopc+1]=='P' && uc_ptaddr[loopc+2]=='A' && uc_ptaddr[loopc+3]=='X')
				|| (uc_ptaddr[loopc]=='M' && uc_ptaddr[loopc+1]=='I' && uc_ptaddr[loopc+2]=='O')) {
			check_result = true;
			length = 2;
			while ((uc_ptaddr[loopc + length] != ' ') 
					&& (length < _ADVANTECH_BOARD_NAME_LENGTH)) {
				length += 1;
			}
			break;
		}
	}
	
	if(check_result) {
		memset(board_id, 0, sizeof(board_id));
		memmove(board_id, uc_ptaddr+loopc, length);
		DEBUGPRINT(KERN_INFO "loopc: %d\n", loopc);
		printk(KERN_INFO "This Advantech Product is: %s\n", board_id);
		entry = proc_create("board", 0x0444, NULL, &board_proc_ops);
		if (NULL == entry)
		{
			DEBUGPRINT(KERN_INFO "Count not create /proc/board file!\n");
		}
	} else {
		DEBUGPRINT(KERN_INFO "Is no advantech device!\n");
	}
 

	iounmap(( void* )uc_ptaddr);
	return 0;

exit3:
	cdev_del(&ads_cdev->dev);
exit2:
	kfree(ads_cdev);
exit1:
	unregister_chrdev_region(devno,1);
exit0:
	return -1;
}

module_init( adspname_init );
module_exit( adspname_cleanup );

MODULE_DESCRIPTION("Driver for ADVANTECH Devices Detecting");
MODULE_AUTHOR("ji.xu@advantech.com");
MODULE_VERSION("2.0.4");
MODULE_LICENSE("GPL");

