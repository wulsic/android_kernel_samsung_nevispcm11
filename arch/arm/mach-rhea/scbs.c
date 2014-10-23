/****************************************************************
* Super cool battery software
*
* scbs.c
*
* Kernel driver
*
* Author: jonpry <jonpry@prymfg.com>
*
* License: GPL
*****************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/circ_buf.h>
#include <linux/quickwakeup.h>
#include <linux/time.h>
#include <linux/scbs.h>

#define SUPER_QUICK

#define RING_BUF_SIZE (16 * 1024)

u64 msm_timer_get_time(void);
//int htc_get_batt_smem_info(struct battery_info_reply *buffer);
int msmrtc_rewake(int diff);
void msmrtc_maxdiff(u32 diff);
void msm_timer_update_sleep(void);

struct class *scbs_class;
dev_t scbs_devno;

static struct cdev scbs_cdev;
static struct device *scbs_device;
static struct scbs_update scbs_update;
static struct scbs_result scbs_result;
static struct scbs_data_point scbs_data_point;

static DECLARE_WAIT_QUEUE_HEAD(transfer_queue);

static spinlock_t consumer_lock;
static spinlock_t producer_lock;

static struct timer_list timer;

int scbs_capacity;
int use_scbs;

/* EXPORT_SYMBOL_NOVERS(scbs_capacity);
EXPORT_SYMBOL_NOVERS(use_scbs); */

struct scbs_buffer
{
	struct scbs_data_point *buf;
	u32 size;
	u32 head;
	u32 tail;
};

static struct scbs_buffer buffer;

static void get_data_point(void)
{
	struct scbs_data_point item;
	unsigned long head;
	unsigned long tail;

	//Caller must check that data is present
	spin_lock(&consumer_lock);

	head = ACCESS_ONCE(buffer.head);
	tail = buffer.tail;

	if (CIRC_CNT(head, tail, buffer.size) >= 1) {
	/* read index before reading contents at that index */
		smp_read_barrier_depends();

		/* extract one item from the buffer */
		item = buffer.buf[tail];

		scbs_data_point = item;

		smp_mb(); /* finish reading descriptor before incrementing tail */

		buffer.tail = (tail + 1) & (buffer.size - 1);
	}

	spin_unlock(&consumer_lock);
}

//static void set_data_point(int v, int c, int d, u64 time, int temp, int sleep)

	//unsigned long head, tail;
	//spin_lock(&producer_lock);

	//head = buffer.head;
	//tail = ACCESS_ONCE(buffer.tail);

	//if (CIRC_SPACE(head, tail, buffer.size) >= 1) {
		/* insert one item into the buffer */
		//struct scbs_data_point *item = &buffer.buf[head];

		//item->voltage = v;
		//item->charge = c;
		//item->discharge = d;
		//item->time = time;
		//item->temperature = temp;
		//item->sleep = sleep;

		//smp_wmb(); /* commit the item before incrementing the head */

		//buffer.head = (head + 1) & (buffer.size - 1);

		/* wake_up() will make sure that the head is committed before
		 * waking anyone up */
//		wake_up(consumer);
	//}

	///spin_unlock(&producer_lock);
//}

void take_measurement(int sleep)
{
		//u32 fixed_vol;
		//u32 fixed_c;
		//u32 fixed_d;
		//Take measurement
		//u64 ctime = msm_timer_get_time();

		//struct battery_info_reply batt_info;
		//htc_get_batt_smem_info(&batt_info);

#ifdef SCBS_DUMP_SMEM
		int i;
		for(i=0; i < 8; i++)
			printk("scbs smem dump u16: %d, %d\n", i,  *(volatile u16*)(MSM_SHARED_RAM_BASE + 0xfc110 + i * 2));
		for(i=0; i < 8; i++)
			printk("scbs smem dump u32: %d, %d\n", i,  *(volatile u32*)(MSM_SHARED_RAM_BASE + 0xfc110 + i * 4));
#endif

		//fixed_vol = (batt_info.batt_vol * (1<<16)) / 1000;
		//fixed_c = (batt_info.batt_current * (1<<16)) / 1000;
		//fixed_d = (batt_info.batt_discharge * (1<<16)) / 1000;

//		printk("scbs_take_measure: %d, %d, %d, %d, %d, %d, %llu, %d\n", batt_info.batt_vol, batt_info.batt_current, batt_info.batt_discharge, fixed_vol, fixed_c, fixed_d, ctime, sleep);

		//set_data_point(fixed_vol, fixed_c, fixed_d, ctime, batt_info.batt_tempRAW, sleep);
}

void wake_time_fn(unsigned long arg)
{
//	printk("scbs: wake_time_fn %lu\n",arg);

	if(!scbs_update.awake)
		return;

	init_timer(&timer);
	timer.function = wake_time_fn;
	timer.expires += scbs_update.awake / 10;
	add_timer(&timer);

	take_measurement(0);

	wake_up_interruptible(&transfer_queue);
}

static int scbs_open(struct inode *inode, struct file *filp)
{
	//Just succeed, we only use IOCTL
	//u64 ctime = msm_timer_get_time();
	//struct  timeval tv = ns_to_timeval//(ctime);

	//printk("scbs: secs %d, ctime %llu\n", (int)tv.tv_sec, ctime);
	return 0;
}

static int scbs_release(struct inode *inode, struct file *filp)
{
	use_scbs = false;
	scbs_update.awake=0;
	scbs_update.sleep=0;
	printk("scbs: shutdown\n");
	return 0;
}


static long scbs_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	int rc = 0;

	switch (cmd) {

	case  IOCTL_SCBS_GET_DATA :
//		printk("scbs: got request\n");

		while(wait_event_interruptible(transfer_queue, (CIRC_CNT(buffer.head, buffer.tail, buffer.size)>=1))){}

//		printk("scbs: data available\n");
		get_data_point();
//		printk("scbs_send_data: %d, %d, %d, %llu\n", scbs_data_point.voltage, scbs_data_point.charge,
//			scbs_data_point.discharge, scbs_data_point.time);
		rc = copy_to_user((void*) arg, &scbs_data_point, sizeof(scbs_data_point));
		break;

	case IOCTL_SCBS_SET_UPDATE:
		rc = copy_from_user(&scbs_update, (void *) arg,
				    sizeof(scbs_update));
		if (rc < 0)
			break;

		printk("scbs: enabled wake: %d, sleep: %d\n", scbs_update.awake, scbs_update.sleep);

		init_timer(&timer);
		timer.expires = scbs_update.awake / 10 + jiffies;
		timer.function = wake_time_fn;
		add_timer(&timer);

		//msmrtc_maxdiff(scbs_update.sleep / 1000);

		printk("scbs: done creating timer\n");
		break;

	case IOCTL_SCBS_SET_RESULT:
		rc = copy_from_user(&scbs_result, (void *) arg,
				    sizeof(scbs_result));
		if (rc < 0)
			break;

		scbs_capacity = scbs_result.voltage;
		use_scbs=true;
		break;

	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

#ifdef SUPER_QUICK
void scbs_do(void)
#else
int quick_check(void)
#endif
{
//	printk("scbs: check\n");

	if(scbs_update.sleep)
	{
		//mdelay(5);

		take_measurement(1);
		//msm_timer_update_sleep();
		//msmrtc_rewake(5);
	}

#ifndef SUPER_QUICK
	return 0;
#endif
}
#ifdef SUPER_QUICK
EXPORT_SYMBOL(scbs_do);

int quick_check(void)
{
	return 0;
}
#endif

int quick_callback(void)
{
//	printk("scbs: callback\n");
	return 0;
}

static struct file_operations scbs_fops = {
	.owner	 = THIS_MODULE,
	.open	 = scbs_open,
	.release = scbs_release,
	.unlocked_ioctl	 = scbs_ioctl,
};

static struct quickwakeup_ops quick_ops = {
	//.qw_callback = quick_callback,
	//.qw_check = quick_check,
};

int scbs_init_devices(void)
{
	int rc;
	int major;

	printk("Super cool battery software km driver\n");

	/* Create a device node */
	scbs_class = class_create(THIS_MODULE, "scbs");
	if (IS_ERR(scbs_class)) {
		rc = -ENOMEM;
		printk(KERN_ERR
		       "scbs: failed to create scbs class\n");
		goto fail;
	}

	rc = alloc_chrdev_region(&scbs_devno, 0, 1, "scbs");
	if (rc < 0) {
		printk(KERN_ERR
		       "scbs: Failed to alloc chardev region (%d)\n", rc);
		goto fail_destroy_class;
	}

	major = MAJOR(scbs_devno);
	scbs_device = device_create(scbs_class, NULL,
					 scbs_devno, NULL, "%d",
					 0);
	if (IS_ERR(scbs_device)) {
		rc = -ENOMEM;
		goto fail_unregister_cdev_region;
	}

	cdev_init(&scbs_cdev, &scbs_fops);
	scbs_cdev.owner = THIS_MODULE;

	rc = cdev_add(&scbs_cdev, scbs_devno, 1);
	if (rc < 0)
		goto fail_destroy_device;

	spin_lock_init(&consumer_lock);
	spin_lock_init(&producer_lock);

	buffer.size = RING_BUF_SIZE;
	buffer.buf = kmalloc(RING_BUF_SIZE*sizeof(struct scbs_data_point),GFP_KERNEL );

	return quickwakeup_register(&quick_ops);

fail_destroy_device:
	device_destroy(scbs_class, scbs_devno);
fail_unregister_cdev_region:
	unregister_chrdev_region(scbs_devno,1);
fail_destroy_class:
	class_destroy(scbs_class);
fail:
	return rc;
}

void scbs_exit_devices(void)
{
	cdev_del(&scbs_cdev);
	device_destroy(scbs_class, scbs_devno);
	unregister_chrdev_region(scbs_devno, 1);
	class_destroy(scbs_class);
}

module_init(scbs_init_devices);
module_exit(scbs_exit_devices);
