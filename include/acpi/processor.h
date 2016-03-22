#ifndef __ACPI_PROCESSOR_H
#define __ACPI_PROCESSOR_H

#include <linux/kernel.h>
#include <linux/cpu.h>

#include <asm/acpi.h>

#define ACPI_PROCESSOR_BUSY_METRIC	10

#define ACPI_PROCESSOR_MAX_POWER	8
#define ACPI_PROCESSOR_MAX_C2_LATENCY	100
#define ACPI_PROCESSOR_MAX_C3_LATENCY	1000

#define ACPI_PROCESSOR_MAX_THROTTLING	16
#define ACPI_PROCESSOR_MAX_THROTTLE	250	/* 25% */
#define ACPI_PROCESSOR_MAX_DUTY_WIDTH	4

#define ACPI_PDC_REVISION_ID		0x1

#define ACPI_PSD_REV0_REVISION		0 /* Support for _PSD as in ACPI 3.0 */
#define ACPI_PSD_REV0_ENTRIES		5

#define ACPI_TSD_REV0_REVISION		0
#define ACPI_TSD_REV0_ENTRIES		5

/*
 * Types of coordination defined in ACPI 3.0. Same macros can be used across
 * P, C and T states
 */
#define DOMAIN_COORD_TYPE_SW_ALL	0xfc
#define DOMAIN_COORD_TYPE_SW_ANY	0xfd
#define DOMAIN_COORD_TYPE_HW_ALL	0xfe

/* Power Management */

struct acpi_processor_cx;

struct acpi_power_register {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 reserved;
	u64 address;
} __attribute__ ((packed));

struct acpi_processor_cx_policy {
	u32 count;
	struct acpi_processor_cx *state;
	struct {
		u32 time;
		u32 ticks;
		u32 count;
		u32 bm;
	} threshold;
};
/* the struction of processor C states */
struct acpi_processor_cx {
	u8 valid;		/* is valid */
	u8 type;		/* 1=C1,2=C2,3=C3 */
	u32 address;	/* the io port */
	u32 latency;	/* the warest-case latency when switch between C states */
	u32 latency_ticks; /* US_TO_PM_TIMER_TICKS(latency) */
	u32 power;		/* the average of power consumption */
	u32 usage;
	u64 time;
	struct acpi_processor_cx_policy promotion; /* 记录下一级别，例如: C2.promotion=C3, C1.promotion=C2*/
	struct acpi_processor_cx_policy demotion;  /* 记录上一级别, 例如: C2.demotion=C1, C3.demotion=C2 */
};
/* the struct of processor power */
struct acpi_processor_power {
	struct acpi_processor_cx *state; /* the current state, the first is C1 state */
	unsigned long bm_check_timestamp;
	u32 default_state;
	u32 bm_activity;
	int count;	/* the number of the C state in states array */
	struct acpi_processor_cx states[ACPI_PROCESSOR_MAX_POWER]; /* C states infomations */
};

/* Performance Management */

struct acpi_psd_package {
	acpi_integer num_entries;
	acpi_integer revision;
	acpi_integer domain;
	acpi_integer coord_type;
	acpi_integer num_processors;
} __attribute__ ((packed));

struct acpi_pct_register {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 reserved;
	u64 address;
} __attribute__ ((packed));

struct acpi_processor_px {
	acpi_integer core_frequency;	/* megahertz */
	acpi_integer power;	/* milliWatts */
	acpi_integer transition_latency;	/* microseconds */
	acpi_integer bus_master_latency;	/* microseconds */
	acpi_integer control;	/* control value */
	acpi_integer status;	/* success indicator */
};

struct acpi_processor_performance {
	unsigned int state;	/* 当前使用的P state的下标，值区间为(0 ~ state_count) */
	unsigned int platform_limit;
	struct acpi_pct_register control_register;	/* 状态寄存器(_PCT) */
	struct acpi_pct_register status_register;	/* 控制寄存器(_PCT) */
	unsigned int state_count;/* _PSS对象中元素个数，即states数组大小 */
	struct acpi_processor_px *states; /* _PSS对象 */
	struct acpi_psd_package domain_info;
	cpumask_t shared_cpu_map;
	unsigned int shared_type;
};

/* Throttling Control */

struct acpi_tsd_package {
	acpi_integer num_entries;
	acpi_integer revision;
	acpi_integer domain;
	acpi_integer coord_type;
	acpi_integer num_processors;
} __attribute__ ((packed));
/* the struction that describe the _TSS object */
struct acpi_processor_tx_tss {
	acpi_integer freqpercentage;	/* percentage in this T state*/
	acpi_integer power;	/* milliWatts */
	acpi_integer transition_latency;	/* microseconds */
	acpi_integer control;	/* control value */
	acpi_integer status;	/* success indicator */
};

struct acpi_ptc_register {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 reserved;
	u64 address;
} __attribute__ ((packed));

struct acpi_processor_tx {
	u16 power;
	u16 performance;
};

struct acpi_processor;
/* the struction processor throttling  */
struct acpi_processor_throttling {
	unsigned int state;	/* 当前的T状态(为state_tss数组下标) */
	unsigned int platform_limit;
	struct acpi_pct_register control_register;
	struct acpi_pct_register status_register;
	unsigned int state_count;	/* the number of T state */
	struct acpi_processor_tx_tss *states_tss;	/* describe the _TSS object*/
	struct acpi_tsd_package domain_info;
	cpumask_t shared_cpu_map;
	int (*acpi_processor_get_throttling) (struct acpi_processor * pr);
	int (*acpi_processor_set_throttling) (struct acpi_processor * pr,
					      int state);

	u32 address;
	u8 duty_offset;
	u8 duty_width;
	u8 tsd_valid_flag;
	unsigned int shared_type;
	struct acpi_processor_tx states[ACPI_PROCESSOR_MAX_THROTTLING]; /* T states array */
};

/* Limit Interface */

struct acpi_processor_lx {
	int px;			/* performace state */
	int tx;			/* throttle level */
};

struct acpi_processor_limit {
	struct acpi_processor_lx state;	/* current limit */
	struct acpi_processor_lx thermal;	/* thermal limit */
	struct acpi_processor_lx user;	/* user limit */
};

struct acpi_processor_flags {
	u8 power:1;			/* 如果此processor至少支持C2,C3两个状态，则设置此值(acpi_processor_get_power_info()函数中设置) */
	u8 performance:1;
	u8 throttling:1;	/* 最否有TState(acpi_processor_get_throttling_info()函数中设置) */
	u8 limit:1;			/* 如果支持TState，则设置此标志(在acpi_processor_get_limit_info()中设置) */
	u8 bm_control:1; 	/* 是否有总线仲裁控制权(在acpi_processor_get_info()函数中设置) */
	u8 bm_check:1;		/* 是否已经校验C1,C2,C3各个状态(在函数acpi_processor_power_init_bm_check设置) */
	u8 has_cst:1;		/* 表示processor支持_CST对象(在acpi_processor_get_power_info_cst函数中设置)*/
	u8 power_setup_done:1; /* 标识power初始化是否完成(acpi_processor_power_init函数设置) */
};

struct acpi_processor {
	acpi_handle handle;
	u32 acpi_id;	/* ACPI id */
	u32 id;		    /* CPU ID */
	u32 pblk;
	int performance_platform_limit;/* _PPC对象返回值，表示PState中从第个几P状态起有效 */
	int throttling_platform_limit;/* _TPC对象返回值，表示TState中从第个几T状态起有效 */
	struct acpi_processor_flags flags;	/* mask the processor support furture. */
	struct acpi_processor_power power;	/* processor power infomation(C states) */
	struct acpi_processor_performance *performance;	/* cpu性能信息 */
	struct acpi_processor_throttling throttling; /* processor throttling information(T state) */
	struct acpi_processor_limit limit;

	/* the _PDC objects for this processor, if any */
	struct acpi_object_list *pdc;
};

struct acpi_processor_errata {
	u8 smp;
	struct {
		u8 throttle:1;
		u8 fdma:1;
		u8 reserved:6;
		u32 bmisx;
	} piix4;
};

extern int acpi_processor_preregister_performance(
		struct acpi_processor_performance **performance);

extern int acpi_processor_register_performance(struct acpi_processor_performance
					       *performance, unsigned int cpu);
extern void acpi_processor_unregister_performance(struct
						  acpi_processor_performance
						  *performance,
						  unsigned int cpu);

/* note: this locks both the calling module and the processor module
         if a _PPC object exists, rmmod is disallowed then */
int acpi_processor_notify_smm(struct module *calling_module);

/* for communication between multiple parts of the processor kernel module */
extern struct acpi_processor *processors[NR_CPUS];
extern struct acpi_processor_errata errata;

void arch_acpi_processor_init_pdc(struct acpi_processor *pr);

#ifdef ARCH_HAS_POWER_INIT
void acpi_processor_power_init_bm_check(struct acpi_processor_flags *flags,
					unsigned int cpu);
#else
static inline void acpi_processor_power_init_bm_check(struct
						      acpi_processor_flags
						      *flags, unsigned int cpu)
{
	flags->bm_check = 1;
	return;
}
#endif

/* in processor_perflib.c */

#ifdef CONFIG_CPU_FREQ
void acpi_processor_ppc_init(void);
void acpi_processor_ppc_exit(void);
int acpi_processor_ppc_has_changed(struct acpi_processor *pr);
#else
static inline void acpi_processor_ppc_init(void)
{
	return;
}
static inline void acpi_processor_ppc_exit(void)
{
	return;
}
static inline int acpi_processor_ppc_has_changed(struct acpi_processor *pr)
{
	static unsigned int printout = 1;
	if (printout) {
		printk(KERN_WARNING
		       "Warning: Processor Platform Limit event detected, but not handled.\n");
		printk(KERN_WARNING
		       "Consider compiling CPUfreq support into your kernel.\n");
		printout = 0;
	}
	return 0;
}
#endif				/* CONFIG_CPU_FREQ */

/* in processor_throttling.c */
int acpi_processor_tstate_has_changed(struct acpi_processor *pr);
int acpi_processor_get_throttling_info(struct acpi_processor *pr);
int acpi_processor_set_throttling(struct acpi_processor *pr, int state);
extern struct file_operations acpi_processor_throttling_fops;

/* in processor_idle.c */
int acpi_processor_power_init(struct acpi_processor *pr,
			      struct acpi_device *device);
int acpi_processor_cst_has_changed(struct acpi_processor *pr);
int acpi_processor_power_exit(struct acpi_processor *pr,
			      struct acpi_device *device);

/* in processor_thermal.c */
int acpi_processor_get_limit_info(struct acpi_processor *pr);
extern struct file_operations acpi_processor_limit_fops;

#ifdef CONFIG_CPU_FREQ
void acpi_thermal_cpufreq_init(void);
void acpi_thermal_cpufreq_exit(void);
#else
static inline void acpi_thermal_cpufreq_init(void)
{
	return;
}
static inline void acpi_thermal_cpufreq_exit(void)
{
	return;
}
#endif

#endif
