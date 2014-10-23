/****************************************************************
* Super cool battery software
*
* scbs.h
*
* Kernel driver interface definitions
*
* Author: jonpry <jonpry@prymfg.com>
*
* License: GPL
*****************************************************************/
#ifndef SCBS_H
#define SCBS_H

#include <linux/ioctl.h>

struct scbs_update
{
	int awake;
	int sleep;
};

struct scbs_data_point
{
	int voltage;
	int charge;
	int discharge;
	int temperature;
	unsigned long long time;
	int sleep;
};

struct scbs_result
{
	int voltage;
	int capacity;
};

#define DEV_TYPE 0x9A

#define IOCTL_SCBS_SET_UPDATE 	_IOR(DEV_TYPE, 0, struct scbs_update*)
#define IOCTL_SCBS_GET_DATA	_IOW(DEV_TYPE, 1, struct scbs_data_point*)
#define IOCTL_SCBS_SET_RESULT	_IOR(DEV_TYPE, 2, struct scbs_result*)

#endif
