#include <linux/linkage.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/sysdev.h>
#include <linux/bitops.h>

#include <asm/acpi.h>
#include <asm/atomic.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/hw_irq.h>
#include <asm/pgtable.h>
#include <asm/delay.h>
#include <asm/desc.h>
#include <asm/apic.h>

/*
 * Common place to define all x86 IRQ vectors
 *
 * This builds up the IRQ handler stubs using some ugly macros in irq.h
 *
 * These macros create the low-level assembly IRQ routines that save
 * register context and call do_IRQ(). do_IRQ() then does all the
 * operations that are needed to keep the AT (or SMP IOAPIC)
 * interrupt-controller happy.
 */
/* 每个中断的处理函数实现
 * 宏张开如下:
 * void IRQ0x40_interrupt(void); __asm__( "\n.p2align\n" "IRQ" "0x40" "_interrupt:\n\t" "push $~(" "0x40" ") ; " "jmp common_interrupt");
 */
#define BI(x,y) \
	BUILD_IRQ(x##y)

#define BUILD_16_IRQS(x) \
	BI(x,0) BI(x,1) BI(x,2) BI(x,3) \
	BI(x,4) BI(x,5) BI(x,6) BI(x,7) \
	BI(x,8) BI(x,9) BI(x,a) BI(x,b) \
	BI(x,c) BI(x,d) BI(x,e) BI(x,f)

#define BUILD_15_IRQS(x) \
	BI(x,0) BI(x,1) BI(x,2) BI(x,3) \
	BI(x,4) BI(x,5) BI(x,6) BI(x,7) \
	BI(x,8) BI(x,9) BI(x,a) BI(x,b) \
	BI(x,c) BI(x,d) BI(x,e)

/*
 * ISA PIC or low IO-APIC triggered (INTA-cycle or APIC) interrupts:
 * (these are usually mapped to vectors 0x20-0x2f)
 */
BUILD_16_IRQS(0x0)

#ifdef CONFIG_X86_LOCAL_APIC
/*
 * The IO-APIC gives us many more interrupt sources. Most of these 
 * are unused but an SMP system is supposed to have enough memory ...
 * sometimes (mostly wrt. hw bugs) we get corrupted vectors all
 * across the spectrum, so we really want to be prepared to get all
 * of these. Plus, more powerful systems might have more than 64
 * IO-APIC registers.
 *
 * (these are usually mapped into the 0x30-0xff vector range)
 */
		   BUILD_16_IRQS(0x1) BUILD_16_IRQS(0x2) BUILD_16_IRQS(0x3)
BUILD_16_IRQS(0x4) BUILD_16_IRQS(0x5) BUILD_16_IRQS(0x6) BUILD_16_IRQS(0x7)
BUILD_16_IRQS(0x8) BUILD_16_IRQS(0x9) BUILD_16_IRQS(0xa) BUILD_16_IRQS(0xb)
BUILD_16_IRQS(0xc) BUILD_16_IRQS(0xd)

#ifdef CONFIG_PCI_MSI
	BUILD_15_IRQS(0xe)
#endif

#endif

#undef BUILD_16_IRQS
#undef BUILD_15_IRQS
#undef BI


#define IRQ(x,y) \
	IRQ##x##y##_interrupt

#define IRQLIST_16(x) \
	IRQ(x,0), IRQ(x,1), IRQ(x,2), IRQ(x,3), \
	IRQ(x,4), IRQ(x,5), IRQ(x,6), IRQ(x,7), \
	IRQ(x,8), IRQ(x,9), IRQ(x,a), IRQ(x,b), \
	IRQ(x,c), IRQ(x,d), IRQ(x,e), IRQ(x,f)

#define IRQLIST_15(x) \
	IRQ(x,0), IRQ(x,1), IRQ(x,2), IRQ(x,3), \
	IRQ(x,4), IRQ(x,5), IRQ(x,6), IRQ(x,7), \
	IRQ(x,8), IRQ(x,9), IRQ(x,a), IRQ(x,b), \
	IRQ(x,c), IRQ(x,d), IRQ(x,e)
/**ltl
 * 功能:中断函数赋值，
 * 每个中断函数的形式如下:void IRQ0x40_interrupt(void); __asm__( "\n.p2align\n" "IRQ" "0x40" "_interrupt:\n\t" "push $~(" "0x40" ") ; " "jmp common_interrupt");
 * 注:此文件最好用宏展开看(make arch/x86_64/kernel/i8259.i)
 */
void (*interrupt[NR_IRQS])(void) = {
	IRQLIST_16(0x0),

#ifdef CONFIG_X86_IO_APIC
			 IRQLIST_16(0x1), IRQLIST_16(0x2), IRQLIST_16(0x3),
	IRQLIST_16(0x4), IRQLIST_16(0x5), IRQLIST_16(0x6), IRQLIST_16(0x7),
	IRQLIST_16(0x8), IRQLIST_16(0x9), IRQLIST_16(0xa), IRQLIST_16(0xb),
	IRQLIST_16(0xc), IRQLIST_16(0xd)

#ifdef CONFIG_PCI_MSI
	, IRQLIST_15(0xe)
#endif

#endif
};

#undef IRQ
#undef IRQLIST_16
#undef IRQLIST_14

/*
 * This is the 'legacy' 8259A Programmable Interrupt Controller,
 * present in the majority of PC/AT boxes.
 * plus some generic x86 specific things if generic specifics makes
 * any sense at all.
 * this file should become arch/i386/kernel/irq.c when the old irq.c
 * moves to arch independent land
 */

DEFINE_SPINLOCK(i8259A_lock);

static void end_8259A_irq (unsigned int irq)
{
	if (irq > 256) { 
		char var;
		printk("return %p stack %p ti %p\n", __builtin_return_address(0), &var, task_thread_info(current));

		BUG(); 
	}

	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)) &&
	    irq_desc[irq].action)
		enable_8259A_irq(irq);
}

#define shutdown_8259A_irq	disable_8259A_irq

static void mask_and_ack_8259A(unsigned int);

static unsigned int startup_8259A_irq(unsigned int irq)
{ 
	enable_8259A_irq(irq);
	return 0; /* never anything pending */
}

static struct hw_interrupt_type i8259A_irq_type = {
	.typename = "XT-PIC",
	.startup = startup_8259A_irq,
	.shutdown = shutdown_8259A_irq,
	.enable = enable_8259A_irq,
	.disable = disable_8259A_irq,
	.ack = mask_and_ack_8259A,
	.end = end_8259A_irq,
};

/*
 * 8259A PIC functions to handle ISA devices:
 */

/*
 * This contains the irq mask for both 8259A irq controllers,
 */
static unsigned int cached_irq_mask = 0xffff;

#define __byte(x,y) 	(((unsigned char *)&(y))[x])
#define cached_21	(__byte(0,cached_irq_mask))
#define cached_A1	(__byte(1,cached_irq_mask))

/*
 * Not all IRQs can be routed through the IO-APIC, eg. on certain (older)
 * boards the timer interrupt is not really connected to any IO-APIC pin,
 * it's fed to the master 8259A's IR0 line only.
 *
 * Any '1' bit in this mask means the IRQ is routed through the IO-APIC.
 * this 'mixed mode' IRQ handling costs nothing because it's only used
 * at IRQ setup time.
 */
unsigned long io_apic_irqs;

void disable_8259A_irq(unsigned int irq)
{
	unsigned int mask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask |= mask;
	if (irq & 8)
		outb(cached_A1,0xA1);
	else
		outb(cached_21,0x21);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void enable_8259A_irq(unsigned int irq)
{
	unsigned int mask = ~(1 << irq);
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask &= mask;
	if (irq & 8)
		outb(cached_A1,0xA1);
	else
		outb(cached_21,0x21);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

int i8259A_irq_pending(unsigned int irq)
{
	unsigned int mask = 1<<irq;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&i8259A_lock, flags);
	if (irq < 8)
		ret = inb(0x20) & mask;
	else
		ret = inb(0xA0) & (mask >> 8);
	spin_unlock_irqrestore(&i8259A_lock, flags);

	return ret;
}

void make_8259A_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	io_apic_irqs &= ~(1<<irq);
	irq_desc[irq].chip = &i8259A_irq_type;
	enable_irq(irq);
}

/*
 * This function assumes to be called rarely. Switching between
 * 8259A registers is slow.
 * This has to be protected by the irq controller spinlock
 * before being called.
 */
static inline int i8259A_irq_real(unsigned int irq)
{
	int value;
	int irqmask = 1<<irq;

	if (irq < 8) {
		outb(0x0B,0x20);		/* ISR register */
		value = inb(0x20) & irqmask;
		outb(0x0A,0x20);		/* back to the IRR register */
		return value;
	}
	outb(0x0B,0xA0);		/* ISR register */
	value = inb(0xA0) & (irqmask >> 8);
	outb(0x0A,0xA0);		/* back to the IRR register */
	return value;
}

/*
 * Careful! The 8259A is a fragile beast, it pretty
 * much _has_ to be done exactly like this (mask it
 * first, _then_ send the EOI, and the order of EOI
 * to the two 8259s is important!
 */
static void mask_and_ack_8259A(unsigned int irq)
{
	unsigned int irqmask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	/*
	 * Lightweight spurious IRQ detection. We do not want
	 * to overdo spurious IRQ handling - it's usually a sign
	 * of hardware problems, so we only do the checks we can
	 * do without slowing down good hardware unnecessarily.
	 *
	 * Note that IRQ7 and IRQ15 (the two spurious IRQs
	 * usually resulting from the 8259A-1|2 PICs) occur
	 * even if the IRQ is masked in the 8259A. Thus we
	 * can check spurious 8259A IRQs without doing the
	 * quite slow i8259A_irq_real() call for every IRQ.
	 * This does not cover 100% of spurious interrupts,
	 * but should be enough to warn the user that there
	 * is something bad going on ...
	 */
	if (cached_irq_mask & irqmask)
		goto spurious_8259A_irq;
	cached_irq_mask |= irqmask;

handle_real_irq:
	if (irq & 8) {
		inb(0xA1);		/* DUMMY - (do we need this?) */
		outb(cached_A1,0xA1);
		outb(0x60+(irq&7),0xA0);/* 'Specific EOI' to slave */
		outb(0x62,0x20);	/* 'Specific EOI' to master-IRQ2 */
	} else {
		inb(0x21);		/* DUMMY - (do we need this?) */
		outb(cached_21,0x21);
		outb(0x60+irq,0x20);	/* 'Specific EOI' to master */
	}
	spin_unlock_irqrestore(&i8259A_lock, flags);
	return;

spurious_8259A_irq:
	/*
	 * this is the slow path - should happen rarely.
	 */
	if (i8259A_irq_real(irq))
		/*
		 * oops, the IRQ _is_ in service according to the
		 * 8259A - not spurious, go handle it.
		 */
		goto handle_real_irq;

	{
		static int spurious_irq_mask;
		/*
		 * At this point we can be sure the IRQ is spurious,
		 * lets ACK and report it. [once per IRQ]
		 */
		if (!(spurious_irq_mask & irqmask)) {
			printk(KERN_DEBUG "spurious 8259A interrupt: IRQ%d.\n", irq);
			spurious_irq_mask |= irqmask;
		}
		atomic_inc(&irq_err_count);
		/*
		 * Theoretically we do not have to handle this IRQ,
		 * but in Linux this does not cause problems and is
		 * simpler for us.
		 */
		goto handle_real_irq;
	}
}
/**ltl
 * 功能: 初始化8259A芯片
 * 参数:
 * 返回值:
 * 说明:
 */
void init_8259A(int auto_eoi)
{
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);

	outb(0xff, 0x21);	/* mask all of 8259A-1 */
	outb(0xff, 0xA1);	/* mask all of 8259A-2 */

	/*
	 * outb_p - this has to work on a wide range of PC hardware.
	 */
	outb_p(0x11, 0x20);	/* ICW1: select 8259A-1 init */
	outb_p(0x20 + 0, 0x21);	/* ICW2: 8259A-1 IR0-7 mapped to 0x20-0x27 */
	outb_p(0x04, 0x21);	/* 8259A-1 (the master) has a slave on IR2 */
	if (auto_eoi)
		outb_p(0x03, 0x21);	/* master does Auto EOI */
	else
		outb_p(0x01, 0x21);	/* master expects normal EOI */

	outb_p(0x11, 0xA0);	/* ICW1: select 8259A-2 init */
	outb_p(0x20 + 8, 0xA1);	/* ICW2: 8259A-2 IR0-7 mapped to 0x28-0x2f */
	outb_p(0x02, 0xA1);	/* 8259A-2 is a slave on master's IR2 */
	outb_p(0x01, 0xA1);	/* (slave's support for AEOI in flat mode
				    is to be investigated) */

	if (auto_eoi)
		/*
		 * in AEOI mode we just have to mask the interrupt
		 * when acking.
		 */
		i8259A_irq_type.ack = disable_8259A_irq;
	else
		i8259A_irq_type.ack = mask_and_ack_8259A;

	udelay(100);		/* wait for 8259A to initialize */

	outb(cached_21, 0x21);	/* restore master IRQ mask */
	outb(cached_A1, 0xA1);	/* restore slave IRQ mask */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}

static char irq_trigger[2];
/**
 * ELCR registers (0x4d0, 0x4d1) control edge/level of IRQ
 */
static void restore_ELCR(char *trigger)
{
	outb(trigger[0], 0x4d0);
	outb(trigger[1], 0x4d1);
}

static void save_ELCR(char *trigger)
{
	/* IRQ 0,1,2,8,13 are marked as reserved */
	trigger[0] = inb(0x4d0) & 0xF8;
	trigger[1] = inb(0x4d1) & 0xDE;
}

static int i8259A_resume(struct sys_device *dev)
{
	init_8259A(0);
	restore_ELCR(irq_trigger);
	return 0;
}

static int i8259A_suspend(struct sys_device *dev, pm_message_t state)
{
	save_ELCR(irq_trigger);
	return 0;
}

static int i8259A_shutdown(struct sys_device *dev)
{
	/* Put the i8259A into a quiescent state that
	 * the kernel initialization code can get it
	 * out of.
	 */
	outb(0xff, 0x21);	/* mask all of 8259A-1 */
	outb(0xff, 0xA1);	/* mask all of 8259A-1 */
	return 0;
}

static struct sysdev_class i8259_sysdev_class = {
	set_kset_name("i8259"),
	.suspend = i8259A_suspend,
	.resume = i8259A_resume,
	.shutdown = i8259A_shutdown,
};

static struct sys_device device_i8259A = {
	.id	= 0,
	.cls	= &i8259_sysdev_class,
};

static int __init i8259A_init_sysfs(void)
{
	int error = sysdev_class_register(&i8259_sysdev_class);
	if (!error)
		error = sysdev_register(&device_i8259A);
	return error;
}

device_initcall(i8259A_init_sysfs);

/*
 * IRQ2 is cascade interrupt to second interrupt controller
 */

static struct irqaction irq2 = { no_action, 0, CPU_MASK_NONE, "cascade", NULL, NULL};
/**ltl
 * 功能: 设置中断描述符中的中断控制器芯片的处理函数集
 * 参数:
 * 返回值:
 * 说明: 1.初始化local apic
 *	    2.初始化8259A芯片
 *	    3.初始化中断描述符全局数组irq_desc[NR_IRQS]. 主要是用来设定中断控制器的处理函数集
 * 		 注:小于16的中断向量使用i8259的处理函数集，其它的先不定义。
 */
void __init init_ISA_irqs (void)
{
	int i;

#ifdef CONFIG_X86_LOCAL_APIC
	init_bsp_APIC();		/* 初始化Local apic */
#endif
	init_8259A(0); 			/* 初始化8259A芯片 */
	
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc[i].status = IRQ_DISABLED; /* <什么时候使能呢?> */
		irq_desc[i].action = NULL;
		irq_desc[i].depth = 1;

		if (i < 16) {
			/*
			 * 16 old-style INTA-cycle interrupts:
			 */
			irq_desc[i].chip = &i8259A_irq_type; /* 使用8259A的处理函数集 */
		} else {
			/*
			 * 'high' PCI IRQs filled in on demand
			 */
			irq_desc[i].chip = &no_irq_type;
		}
	}
}

void apic_timer_interrupt(void);
void spurious_interrupt(void);
void error_interrupt(void);
void reschedule_interrupt(void);
void call_function_interrupt(void);
void invalidate_interrupt0(void);
void invalidate_interrupt1(void);
void invalidate_interrupt2(void);
void invalidate_interrupt3(void);
void invalidate_interrupt4(void);
void invalidate_interrupt5(void);
void invalidate_interrupt6(void);
void invalidate_interrupt7(void);
void thermal_interrupt(void);
void threshold_interrupt(void);
void i8254_timer_resume(void);
/**
 * 功能:
 * 参数:
 * 返回值:
 * 说明: <这个函数的实现要查看intel体系架构文档>
 */
static void setup_timer_hardware(void)
{
	outb_p(0x34,0x43);		/* binary, mode 2, LSB/MSB, ch 0 */
	udelay(10);
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	udelay(10);
	outb(LATCH >> 8 , 0x40);	/* MSB */
}

static int timer_resume(struct sys_device *dev)
{
	setup_timer_hardware();
	return 0;
}

void i8254_timer_resume(void)
{
	setup_timer_hardware();
}

static struct sysdev_class timer_sysclass = {
	set_kset_name("timer_pit"),
	.resume		= timer_resume,
};

static struct sys_device device_timer = {
	.id		= 0,
	.cls		= &timer_sysclass,
};

static int __init init_timer_sysfs(void)
{
	int error = sysdev_class_register(&timer_sysclass);
	if (!error)
		error = sysdev_register(&device_timer);
	return error;
}

device_initcall(init_timer_sysfs);
/**ltl
 * 功能: 初始化中断
 * 参数:
 * 返回值:
 * 说明: 将大于32的中断向量都加入到中断路由表中，其默认的中断处理函数为interrupt[x]，
 *       在IOAPIC或者MSI初始化或者使能时，会重新设定此中断向量的中断处理函数。如:get_new_vector()/ioapic_register_intr()
 *       注: 在中断路由表中，外设中断号从32号起，而在中断号描述符irq_desc表，外设的中断号是0，这两者是如何关联起来? (下标一致)
 */
void __init init_IRQ(void)
{
	int i;
	/* 初始化中断控制器和设置中断描述符中的中断芯片处理函数集 */
	init_ISA_irqs();
	/*
	 * Cover the whole vector space, no vector can escape
	 * us. (some of these will be overridden and become
	 * 'special' SMP interrupts)
	 */
	/* 设置大于32的中断向量到中断路由表中(0~19:intel用来处理异常，20~31: intel保留) */
	for (i = 0; i < (NR_VECTORS - FIRST_EXTERNAL_VECTOR); i++) {
		int vector = FIRST_EXTERNAL_VECTOR + i; /* 中断号从32开始 */
		if (i >= NR_IRQS)
			break;
		/* 除了0x80向量(系统调用在trap_init已经添加过)，其它的都添加到中断路由表中 */
		if (vector != IA32_SYSCALL_VECTOR) 
			set_intr_gate(vector, interrupt[i]);
	}

#ifdef CONFIG_SMP
	/*
	 * IRQ0 must be given a fixed assignment and initialized,
	 * because it's used before the IO-APIC is set up.
	 */
	set_intr_gate(FIRST_DEVICE_VECTOR, interrupt[0]); /* 将0x31加入到中断向量表中 */

	/*
	 * The reschedule interrupt is a CPU-to-CPU reschedule-helper
	 * IPI, driven by wakeup.
	 */
	set_intr_gate(RESCHEDULE_VECTOR, reschedule_interrupt);

	/* IPIs for invalidation */
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+0, invalidate_interrupt0);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+1, invalidate_interrupt1);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+2, invalidate_interrupt2);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+3, invalidate_interrupt3);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+4, invalidate_interrupt4);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+5, invalidate_interrupt5);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+6, invalidate_interrupt6);
	set_intr_gate(INVALIDATE_TLB_VECTOR_START+7, invalidate_interrupt7);

	/* IPI for generic function call */
	set_intr_gate(CALL_FUNCTION_VECTOR, call_function_interrupt);
#endif	
	set_intr_gate(THERMAL_APIC_VECTOR, thermal_interrupt);
	set_intr_gate(THRESHOLD_APIC_VECTOR, threshold_interrupt);

#ifdef CONFIG_X86_LOCAL_APIC
	/* 时钟中断(jiffies_64)通过此中断更新 */
	/* self generated IPI for local APIC timer */
	set_intr_gate(LOCAL_TIMER_VECTOR, apic_timer_interrupt); 

	/* IPI vectors for APIC spurious and error interrupts */
	set_intr_gate(SPURIOUS_APIC_VECTOR, spurious_interrupt);
	set_intr_gate(ERROR_APIC_VECTOR, error_interrupt);
#endif

	/*
	 * Set the clock to HZ Hz, we already have a valid
	 * vector now:
	 */
	setup_timer_hardware(); /* 开启定时器 */

	if (!acpi_ioapic) /* 在没有解析到ioapic信息情况下 */
		setup_irq(2, &irq2); /* 由于主8259A芯片的pin-2是连接另一个8259A芯片的输出引脚(连接8259A从片) */
}
