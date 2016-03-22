/*
 *  drivers/cpufreq/cpufreq_ondemand.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_FREQUENCY_UP_THRESHOLD		(80)
#define MIN_FREQUENCY_UP_THRESHOLD		(11)
#define MAX_FREQUENCY_UP_THRESHOLD		(100)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
static unsigned int def_sampling_rate;
#define MIN_SAMPLING_RATE_RATIO			(2)
/* for correct statistics, we need at least 10 ticks between each measure */
#define MIN_STAT_SAMPLING_RATE 			\
			(MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10))
#define MIN_SAMPLING_RATE			\
			(def_sampling_rate / MIN_SAMPLING_RATE_RATIO)
#define MAX_SAMPLING_RATE			(500 * def_sampling_rate)
#define DEF_SAMPLING_RATE_LATENCY_MULTIPLIER	(1000)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

static void do_dbs_timer(void *data);

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	struct cpufreq_policy *cur_policy;
	struct work_struct work;
	struct cpufreq_frequency_table *freq_table;
	unsigned int freq_lo;
	unsigned int freq_lo_jiffies;
	unsigned int freq_hi_jiffies;
	int cpu;
	unsigned int enable:1,
	             sample_type:1;
};

/* 定义每CPU全局变量: cpu_dbs_info */
static DEFINE_PER_CPU(struct cpu_dbs_info_s, cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * DEADLOCK ALERT! There is a ordering requirement between cpu_hotplug
 * lock and dbs_mutex. cpu_hotplug lock should always be held before
 * dbs_mutex. If any function that can potentially take cpu_hotplug lock
 * (like __cpufreq_driver_target()) is being called with dbs_mutex taken, then
 * cpu_hotplug lock should be taken before that. Note that cpu_hotplug lock
 * is recursive for the same process. -Venki
 */
static DEFINE_MUTEX(dbs_mutex);

static struct workqueue_struct	*kondemand_wq;

static struct dbs_tuners {
	unsigned int sampling_rate;	/* cpu工作队列运行超时时间 */
	unsigned int up_threshold;	/* cpu负载阀值 */
	unsigned int ignore_nice;
	unsigned int powersave_bias;	/* <此值当设置1时，当cpu满载运行时，就会一直运行于高频率上> */
} dbs_tuners_ins = {
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.ignore_nice = 0,
	.powersave_bias = 0,
};
/**ltl
 * 功能: 获取cpu空闲时间
 * 参数:
 * 返回值:
 * 说明: 从开机启动时刻到当前时刻的时间统计
 */
static inline cputime64_t get_cpu_idle_time(unsigned int cpu)
{
	cputime64_t idle_time;
	cputime64_t cur_jiffies;
	cputime64_t busy_time;

	cur_jiffies = jiffies64_to_cputime64(get_jiffies_64()); /* 当前时刻 */
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user, /* user和system花费时间 */
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq); /* 加上中断时间 */
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq); /* 加上软中断 */
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);

	if (!dbs_tuners_ins.ignore_nice) {
		busy_time = cputime64_add(busy_time,
				kstat_cpu(cpu).cpustat.nice);
	}

	idle_time = cputime64_sub(cur_jiffies, busy_time); /* 空闲时间=当前时刻 - cpu运行时间 */
	return idle_time;
}

/*
 * Find right freq to be set now with powersave_bias on.
 * Returns the freq_hi to be used right now and will set freq_hi_jiffies,
 * freq_lo, and freq_lo_jiffies in percpu area for averaging freqs.
 */
static unsigned int powersave_bias_target(struct cpufreq_policy *policy,
					  unsigned int freq_next,
					  unsigned int relation)
{
	unsigned int freq_req, freq_reduc, freq_avg;
	unsigned int freq_hi, freq_lo;
	unsigned int index = 0;
	unsigned int jiffies_total, jiffies_hi, jiffies_lo;
	struct cpu_dbs_info_s *dbs_info = &per_cpu(cpu_dbs_info, policy->cpu);

	if (!dbs_info->freq_table) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_next;
	}

	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_next,
			relation, &index);
	freq_req = dbs_info->freq_table[index].frequency;
	freq_reduc = freq_req * dbs_tuners_ins.powersave_bias / 1000;
	freq_avg = freq_req - freq_reduc;

	/* Find freq bounds for freq_avg in freq_table */
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_H, &index);
	freq_lo = dbs_info->freq_table[index].frequency;
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_L, &index);
	freq_hi = dbs_info->freq_table[index].frequency;

	/* Find out how long we have to be in hi and lo freqs */
	if (freq_hi == freq_lo) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_lo;
	}
	jiffies_total = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	jiffies_hi = (freq_avg - freq_lo) * jiffies_total;
	jiffies_hi += ((freq_hi - freq_lo) / 2);
	jiffies_hi /= (freq_hi - freq_lo);
	jiffies_lo = jiffies_total - jiffies_hi;
	dbs_info->freq_lo = freq_lo;
	dbs_info->freq_lo_jiffies = jiffies_lo;
	dbs_info->freq_hi_jiffies = jiffies_hi;
	return freq_hi;
}

static void ondemand_powersave_bias_init(void)
{
	int i;
	for_each_online_cpu(i) {
		struct cpu_dbs_info_s *dbs_info = &per_cpu(cpu_dbs_info, i);
		dbs_info->freq_table = cpufreq_frequency_get_table(i);
		dbs_info->freq_lo = 0;
	}
}

/************************** sysfs interface ************************/
static ssize_t show_sampling_rate_max(struct cpufreq_policy *policy, char *buf)
{
	return sprintf (buf, "%u\n", MAX_SAMPLING_RATE);
}

static ssize_t show_sampling_rate_min(struct cpufreq_policy *policy, char *buf)
{
	return sprintf (buf, "%u\n", MIN_SAMPLING_RATE);
}

#define define_one_ro(_name)		\
static struct freq_attr _name =		\
__ATTR(_name, 0444, show_##_name, NULL)

define_one_ro(sampling_rate_max);
define_one_ro(sampling_rate_min);

/* cpufreq_ondemand Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct cpufreq_policy *unused, char *buf)				\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(up_threshold, up_threshold);
show_one(ignore_nice_load, ignore_nice);
show_one(powersave_bias, powersave_bias);

/* 文件/sys/devices/system/cpu/cpu1/cpufreq/ondemand/sampling_rate的写函数 */
static ssize_t store_sampling_rate(struct cpufreq_policy *unused,
		const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	mutex_lock(&dbs_mutex);
	if (ret != 1 || input > MAX_SAMPLING_RATE
		     || input < MIN_SAMPLING_RATE) {
		mutex_unlock(&dbs_mutex);
		return -EINVAL;
	}

	dbs_tuners_ins.sampling_rate = input;
	mutex_unlock(&dbs_mutex);

	return count;
}
/* 文件/sys/devices/system/cpu/cpu1/cpufreq/ondemand/up_threshold的写函数 */
static ssize_t store_up_threshold(struct cpufreq_policy *unused,
		const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	mutex_lock(&dbs_mutex);
	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
			input < MIN_FREQUENCY_UP_THRESHOLD) {
		mutex_unlock(&dbs_mutex);
		return -EINVAL;
	}

	dbs_tuners_ins.up_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_ignore_nice_load(struct cpufreq_policy *policy,
		const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if ( ret != 1 )
		return -EINVAL;

	if ( input > 1 )
		input = 1;

	mutex_lock(&dbs_mutex);
	if ( input == dbs_tuners_ins.ignore_nice ) { /* nothing to do */
		mutex_unlock(&dbs_mutex);
		return count;
	}
	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j);
		dbs_info->prev_cpu_wall = get_jiffies_64();
	}
	mutex_unlock(&dbs_mutex);

	return count;
}
/* /sys/devices/system/cpu/cpu1/cpufreq/ondemand/powersave_bias的处理函数 */
static ssize_t store_powersave_bias(struct cpufreq_policy *unused,
		const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 1000)
		input = 1000;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.powersave_bias = input;
	ondemand_powersave_bias_init();
	mutex_unlock(&dbs_mutex);

	return count;
}

#define define_one_rw(_name) \
static struct freq_attr _name = \
__ATTR(_name, 0644, show_##_name, store_##_name)

define_one_rw(sampling_rate);
define_one_rw(up_threshold);
define_one_rw(ignore_nice_load);
define_one_rw(powersave_bias);

static struct attribute * dbs_attributes[] = {
	&sampling_rate_max.attr,
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&up_threshold.attr,
	&ignore_nice_load.attr,
	&powersave_bias.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "ondemand",
};

/************************** sysfs end ************************/

#ifndef CONFIG_XEN
/**ltl
 * 功能: 计算cpu负债 
 * 参数:
 * 返回值:
 * 说明:
 */
static int dbs_calc_load(struct cpu_dbs_info_s *this_dbs_info)
{
	struct cpufreq_policy *policy;
	cputime64_t cur_jiffies;
	unsigned int total_ticks, idle_ticks;
	unsigned int j;
	int load;

	this_dbs_info->freq_lo = 0;
	policy = this_dbs_info->cur_policy;
	cur_jiffies = jiffies64_to_cputime64(get_jiffies_64());
	total_ticks = (unsigned int) cputime64_sub(cur_jiffies,
			this_dbs_info->prev_cpu_wall); /* 从开机到此刻的总tick */
	this_dbs_info->prev_cpu_wall = get_jiffies_64();

	if (!total_ticks)
		return 200;
	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate, we look for a the lowest
	 * frequency which can sustain the load while keeping idle time over
	 * 30%. If such a frequency exist, we try to decrease to this frequency.
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of current frequency
	 */

	/* Get Idle Time */
	idle_ticks = UINT_MAX;
	for_each_cpu_mask(j, policy->cpus) {
		cputime64_t total_idle_ticks;
		unsigned int tmp_idle_ticks;
		struct cpu_dbs_info_s *j_dbs_info;

		j_dbs_info = &per_cpu(cpu_dbs_info, j);
		total_idle_ticks = get_cpu_idle_time(j); /* 从开机启动起，此cpu总空闲时间  */
		tmp_idle_ticks = (unsigned int) cputime64_sub(total_idle_ticks,
				j_dbs_info->prev_cpu_idle); /* 最近一次空闲的空闲时间 */
		j_dbs_info->prev_cpu_idle = total_idle_ticks; 

		if (tmp_idle_ticks < idle_ticks)
			idle_ticks = tmp_idle_ticks; /* 此次的idle时间 */
	}
	load = (100 * (total_ticks - idle_ticks)) / total_ticks;
	return load;
}
#else

#include <xen/interface/platform.h>
static int dbs_calc_load(struct cpu_dbs_info_s *this_dbs_info)
{
	int load = 0;
	dom0_op_t op;
	uint64_t idletime[NR_CPUS];
	struct cpufreq_policy *policy;
	unsigned int j;
	cpumask_t cpumap;

	policy = this_dbs_info->cur_policy;
	cpumap = policy->cpus;

	op.cmd = DOM0_getidletime;
	set_xen_guest_handle(op.u.getidletime.cpumap_bitmap, (uint8_t *) cpus_addr(cpumap));
	op.u.getidletime.cpumap_nr_cpus = NR_CPUS;
	set_xen_guest_handle(op.u.getidletime.idletime, idletime);
	if (HYPERVISOR_dom0_op(&op))
		return 200;

	for_each_cpu_mask(j, cpumap) {
		cputime64_t total_idle_nsecs, tmp_idle_nsecs;
		cputime64_t total_wall_nsecs, tmp_wall_nsecs;
		struct cpu_dbs_info_s *j_dbs_info;
		unsigned long tmp_load, tmp_wall_msecs, tmp_idle_msecs;

		j_dbs_info = &per_cpu(cpu_dbs_info, j);
		total_idle_nsecs = idletime[j];
		tmp_idle_nsecs = cputime64_sub(total_idle_nsecs,
				j_dbs_info->prev_cpu_idle);
		total_wall_nsecs = op.u.getidletime.now;
		tmp_wall_nsecs = cputime64_sub(total_wall_nsecs,
				j_dbs_info->prev_cpu_wall);

		if (tmp_wall_nsecs == 0)
			return 200;

		j_dbs_info->prev_cpu_wall = total_wall_nsecs;
		j_dbs_info->prev_cpu_idle = total_idle_nsecs;

		/* Convert nsecs to msecs and clamp times to sane values. */
		do_div(tmp_wall_nsecs, 1000000);
		tmp_wall_msecs = tmp_wall_nsecs;
		do_div(tmp_idle_nsecs, 1000000);
		tmp_idle_msecs = tmp_idle_nsecs;
		if (tmp_wall_msecs == 0)
			tmp_wall_msecs = 1;
		if (tmp_idle_msecs > tmp_wall_msecs)
			tmp_idle_msecs = tmp_wall_msecs;

		tmp_load = (100 * (tmp_wall_msecs - tmp_idle_msecs)) /
				tmp_wall_msecs;
		tmp_wall_msecs = tmp_wall_nsecs;
		
		load = max(load, min(100, (int) tmp_load));
	}
	return load;
}
#endif
/**ltl
 * 功能: 根据cpu的负载来设置cpu的频率，当负载高于80时，设置CPU最高频率，如果低于80，则向下设置频率
 * 参数:
 * 返回值:
 * 说明:
 */
static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	int load=0;

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return;

	policy = this_dbs_info->cur_policy;
	load = dbs_calc_load(this_dbs_info); /* 计算cpu的负载 */
	if (load > 100) 
		return;
	/* cpu负载高干80，则把cpu频率设置成最高 */
	/* Check for frequency increase */
	if (load > dbs_tuners_ins.up_threshold/*80*/) {
		/* if we are already at full speed then break out early */
		if (!dbs_tuners_ins.powersave_bias) {
			if (policy->cur == policy->max)
				return;
			/* 把cpu的运行频率设置成最大 */
			__cpufreq_driver_target(policy, policy->max,
				CPUFREQ_RELATION_H);
		} else {
			int freq = powersave_bias_target(policy, policy->max,
					CPUFREQ_RELATION_H);
			__cpufreq_driver_target(policy, freq,
				CPUFREQ_RELATION_L);
		}

		return;
	}
	/* cpu负载低于80的情况 */
	/* Check for frequency decrease */
	/* if we cannot reduce the frequency anymore, break out early */
	if (policy->cur == policy->min)
		return;

	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, we focus 10 points under the threshold.
	 */
	if (load < (dbs_tuners_ins.up_threshold - 10)) {
		unsigned int freq_next, freq_cur;

		freq_cur = __cpufreq_driver_getavg(policy);
		if (!freq_cur)
			freq_cur = policy->cur;
		/* 下一频率 */
		freq_next = (freq_cur * load) /
			(dbs_tuners_ins.up_threshold - 10);
		/* 设置cpu的频率 */
		if (!dbs_tuners_ins.powersave_bias) {
			__cpufreq_driver_target(policy, freq_next,
					CPUFREQ_RELATION_L);
		} else {
			int freq = powersave_bias_target(policy, freq_next,
					CPUFREQ_RELATION_L);
			__cpufreq_driver_target(policy, freq,
				CPUFREQ_RELATION_L);
		}
	}
}
/**ltl
 * 功能: cpu采用任务处理函数
 */
static void do_dbs_timer(void *data)
{
	unsigned int cpu = smp_processor_id();
	struct cpu_dbs_info_s *dbs_info = &per_cpu(cpu_dbs_info, cpu);
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	delay -= jiffies % delay;

	if (lock_policy_rwsem_write(cpu) < 0)
		return;

	if (!dbs_info->enable) {
		unlock_policy_rwsem_write(cpu);
		return;
	}

	/* Common NORMAL_SAMPLE setup */
	INIT_WORK(&dbs_info->work, do_dbs_timer, (void *)DBS_NORMAL_SAMPLE);
	if (!dbs_tuners_ins.powersave_bias ||
	    (unsigned long) data == DBS_NORMAL_SAMPLE) {
		lock_cpu_hotplug();
		dbs_check_cpu(dbs_info);	/* 根据CPU负载去设置cpu的频率 */
		unlock_cpu_hotplug();
		if (dbs_info->freq_lo) {
			/* Setup timer for SUB_SAMPLE */
			INIT_WORK(&dbs_info->work, do_dbs_timer,
					(void *)DBS_SUB_SAMPLE);
			delay = dbs_info->freq_hi_jiffies;
		}
	} else { /* 只有第一次和设置freq_lo才会执行此分支 */
		__cpufreq_driver_target(dbs_info->cur_policy,
	                        	dbs_info->freq_lo,
	                        	CPUFREQ_RELATION_H);
	}
	/* 重新开启工作队列 */
	queue_delayed_work_on(cpu, kondemand_wq, &dbs_info->work, delay);
	unlock_policy_rwsem_write(cpu);
}
/* 开启工作队列 */
static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	dbs_info->enable = 1;
	ondemand_powersave_bias_init();
	dbs_info->sample_type = DBS_NORMAL_SAMPLE;
	INIT_WORK(&dbs_info->work, do_dbs_timer, dbs_info);	/* 初始化任务 */
	queue_delayed_work_on(dbs_info->cpu, kondemand_wq, &dbs_info->work,
	                      delay); /* 开启任务 */
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work(&dbs_info->work);
}
/**ltl
 * 功能: ondemand调节器的处理函数
 * 参数:
 * 返回值:
 * 说明:
 */
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START: /* 调节器开启 */
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		if (this_dbs_info->enable) /* Already enabled */
			break;

		mutex_lock(&dbs_mutex);
		dbs_enable++;
		/* 创建目录/sys/devices/system/cpu/cpu1/cpufreq/ondemand及目录下的文件 */
		rc = sysfs_create_group(&policy->kobj, &dbs_attr_group);
		if (rc) {
			dbs_enable--;
			mutex_unlock(&dbs_mutex);
			return rc;
		}

		for_each_cpu_mask(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j);
			j_dbs_info->prev_cpu_wall = get_jiffies_64();
		}
		this_dbs_info->cpu = cpu;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			unsigned int latency;
			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;

			def_sampling_rate = latency *
					DEF_SAMPLING_RATE_LATENCY_MULTIPLIER;

			if (def_sampling_rate < MIN_STAT_SAMPLING_RATE)
				def_sampling_rate = MIN_STAT_SAMPLING_RATE;

			dbs_tuners_ins.sampling_rate = def_sampling_rate;	/* cpu工作队列运行超时时间  */
		}
		dbs_timer_init(this_dbs_info);	/* 开启采样工作队列 */

		mutex_unlock(&dbs_mutex);
		break;

	case CPUFREQ_GOV_STOP:	/* 调节器停止 */
		mutex_lock(&dbs_mutex);
		dbs_timer_exit(this_dbs_info);
		sysfs_remove_group(&policy->kobj, &dbs_attr_group);
		dbs_enable--;
		mutex_unlock(&dbs_mutex);

		break;

	case CPUFREQ_GOV_LIMITS:	/* 调节器限制 */
		mutex_lock(&dbs_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
			                        policy->max,
			                        CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
			                        policy->min,
			                        CPUFREQ_RELATION_L);
		mutex_unlock(&dbs_mutex);
		break;
	}
	return 0;
}
/* ondemand调节器对象 */
struct cpufreq_governor cpufreq_gov_ondemand = {
	.name			= "ondemand",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency = TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};
EXPORT_SYMBOL(cpufreq_gov_ondemand);

static int __init cpufreq_gov_dbs_init(void)
{
	kondemand_wq = create_workqueue("kondemand");
	if (!kondemand_wq) {
		printk(KERN_ERR "Creation of kondemand failed\n");
		return -EFAULT;
	}
	return cpufreq_register_governor(&cpufreq_gov_ondemand);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_ondemand);
	destroy_workqueue(kondemand_wq);
}


MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_DESCRIPTION("'cpufreq_ondemand' - A dynamic cpufreq governor for "
                   "Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

module_init(cpufreq_gov_dbs_init);
module_exit(cpufreq_gov_dbs_exit);

