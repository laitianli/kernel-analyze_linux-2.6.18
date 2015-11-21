#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/sysdev.h>
#include <linux/bitops.h>

#include <asm/8253pit.h>
#include <asm/atomic.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/timer.h>
#include <asm/pgtable.h>
#include <asm/delay.h>
#include <asm/desc.h>
#include <asm/apic.h>
#include <asm/arch_hooks.h>
#include <asm/i8259.h>

#include <io_ports.h>

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
	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)) &&
							irq_desc[irq].action)
		enable_8259A_irq(irq);
}

#define shutdown_8259A_irq	disable_8259A_irq

static void mask_and_ack_8259A(unsigned int);

unsigned int startup_8259A_irq(unsigned int irq)
{ 
	enable_8259A_irq(irq);
	return 0; /* never anything pending */
}
//8259A中断控制器描述符
static struct hw_interrupt_type i8259A_irq_type = {
	.typename = "XT-PIC",
	.startup = startup_8259A_irq,//启动中断线
	.shutdown = shutdown_8259A_irq,//关闭中断线
	.enable = enable_8259A_irq,//启用中断线
	.disable = disable_8259A_irq,//禁用中断线
	.ack = mask_and_ack_8259A,
	.end = end_8259A_irq,
};

/*
 * 8259A PIC functions to handle ISA devices:
 */

/*
 * This contains the irq mask for both 8259A irq controllers,
 */
unsigned int cached_irq_mask = 0xffff;

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
		outb(cached_slave_mask, PIC_SLAVE_IMR);
	else
		outb(cached_master_mask, PIC_MASTER_IMR);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void enable_8259A_irq(unsigned int irq)
{
	unsigned int mask = ~(1 << irq);
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask &= mask;
	if (irq & 8)
		outb(cached_slave_mask, PIC_SLAVE_IMR);
	else
		outb(cached_master_mask, PIC_MASTER_IMR);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

int i8259A_irq_pending(unsigned int irq)
{
	unsigned int mask = 1<<irq;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&i8259A_lock, flags);
	if (irq < 8)
		ret = inb(PIC_MASTER_CMD) & mask;
	else
		ret = inb(PIC_SLAVE_CMD) & (mask >> 8);
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
		outb(0x0B,PIC_MASTER_CMD);	/* ISR register */
		value = inb(PIC_MASTER_CMD) & irqmask;
		outb(0x0A,PIC_MASTER_CMD);	/* back to the IRR register */
		return value;
	}
	outb(0x0B,PIC_SLAVE_CMD);	/* ISR register */
	value = inb(PIC_SLAVE_CMD) & (irqmask >> 8);
	outb(0x0A,PIC_SLAVE_CMD);	/* back to the IRR register */
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
		inb(PIC_SLAVE_IMR);	/* DUMMY - (do we need this?) */
		outb(cached_slave_mask, PIC_SLAVE_IMR);
		outb(0x60+(irq&7),PIC_SLAVE_CMD);/* 'Specific EOI' to slave */
		outb(0x60+PIC_CASCADE_IR,PIC_MASTER_CMD); /* 'Specific EOI' to master-IRQ2 */
	} else {
		inb(PIC_MASTER_IMR);	/* DUMMY - (do we need this?) */
		outb(cached_master_mask, PIC_MASTER_IMR);
		outb(0x60+irq,PIC_MASTER_CMD);	/* 'Specific EOI to master */
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
	outb(0xff, PIC_MASTER_IMR);	/* mask all of 8259A-1 */
	outb(0xff, PIC_SLAVE_IMR);	/* mask all of 8259A-1 */
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

/**ltl
8259A通过两个端口来进行数据传送，对于单块的8259A或者是级连中的8259A_1来说，这两个端口是0x20和0x21。
对于8259A_2来说，这两个端口是0xA0和0xA1。
*/

void init_8259A(int auto_eoi)
{
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	/*把两级的中断控制器的(ICW1)中断屏蔽字都置1，即屏蔽所有的外部中断，因为此时系统还没有初始化完毕，不能接收外部设备的任何请求*/
	outb(0xff, PIC_MASTER_IMR);	/* mask all of 8259A-1 */
	outb(0xff, PIC_SLAVE_IMR);	/* mask all of 8259A-2 */

	/*
	 * outb_p - this has to work on a wide range of PC hardware.
	 */
	/*送0x11到ICW1,启动初始化编程，00x11表示外部中断请求信号为上升沿有效，系统中有多片的8259A芯片，还要向ICW4送数据*/
	outb_p(0x11, PIC_MASTER_CMD);	/* ICW1: select 8259A-1 init */
	/*送0x20到ICW2，写入高五位作为中断向量的高5位，低3位根据中断源(管脚)填入中断号0~7，即把IRQ0~IRQ7映射到20~0x27*/
	outb_p(0x20 + 0, PIC_MASTER_IMR);	/* ICW2: 8259A-1 IR0-7 mapped to 0x20-0x27 */

	/*送0x4到ICW3,ICW3是级联命令字，0x4表示8259A_1是主片*/
	outb_p(1U << PIC_CASCADE_IR, PIC_MASTER_IMR);	/* 8259A-1 (the master) has a slave on IR2 */
	if (auto_eoi)	/* master does Auto EOI */
		outb_p(MASTER_ICW4_DEFAULT | PIC_ICW4_AEOI, PIC_MASTER_IMR);
	else		/* master expects normal EOI */
		outb_p(MASTER_ICW4_DEFAULT, PIC_MASTER_IMR);
	//0x11初始化8259A_2
	outb_p(0x11, PIC_SLAVE_CMD);	/* ICW1: select 8259A-2 init */
	//用ICW2把8259A_2的IRQ0~IRQ7映射0x28~0x2f
	outb_p(0x20 + 8, PIC_SLAVE_IMR);	/* ICW2: 8259A-2 IR0-7 mapped to 0x28-0x2f */
	//送0x4到ICW3，表示8259A_2是从片，并连接到8259A_1的2号管脚上
	outb_p(PIC_CASCADE_IR, PIC_SLAVE_IMR);	/* 8259A-2 is a slave on master's IR2 */
	//把0x1送到ICW4
	outb_p(SLAVE_ICW4_DEFAULT, PIC_SLAVE_IMR); /* (slave's support for AEOI in flat mode is to be investigated) */
	if (auto_eoi)
		/*
		 * in AEOI mode we just have to mask the interrupt
		 * when acking.
		 */
		i8259A_irq_type.ack = disable_8259A_irq;
	else
		i8259A_irq_type.ack = mask_and_ack_8259A;

	udelay(100);		/* wait for 8259A to initialize */

	outb(cached_master_mask, PIC_MASTER_IMR); /* restore master IRQ mask */
	outb(cached_slave_mask, PIC_SLAVE_IMR);	  /* restore slave IRQ mask */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}

/*
 * Note that on a 486, we don't want to do a SIGFPE on an irq13
 * as the irq is unreliable, and exception 16 works correctly
 * (ie as explained in the intel literature). On a 386, you
 * can't use exception 16 due to bad IBM design, so we have to
 * rely on the less exact irq13.
 *
 * Careful.. Not only is IRQ13 unreliable, but it is also
 * leads to races. IBM designers who came up with it should
 * be shot.
 */
 

static irqreturn_t math_error_irq(int cpl, void *dev_id, struct pt_regs *regs)
{
	extern void math_error(void __user *);
	outb(0,0xF0);
	if (ignore_fpu_irq || !boot_cpu_data.hard_math)
		return IRQ_NONE;
	math_error((void __user *)regs->eip);
	return IRQ_HANDLED;
}

/*
 * New motherboards sometimes make IRQ 13 be a PCI interrupt,
 * so allow interrupt sharing.
 */
static struct irqaction fpu_irq = { math_error_irq, 0, CPU_MASK_NONE, "fpu", NULL, NULL };
/**ltl
功能:在系统初始化区间，对中断描述符进行初始化
*/
void __init init_ISA_irqs (void)
{
	int i;

#ifdef CONFIG_X86_LOCAL_APIC
	init_bsp_APIC();
#endif
	init_8259A(0);
	/*
	1.从这里可以看出，在初始化区间，所有的中断都是disable,同时depth=1
	2.还没有指定中断处理函数，handle_irq = NULL
	*/
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc[i].status = IRQ_DISABLED;//
		irq_desc[i].action = NULL;
		irq_desc[i].depth = 1;
		/*对中断控制器的描述有两种:一种是8259A,另一种是其它的控制器*/
		if (i < 16) {
			/*
			 * 16 old-style INTA-cycle interrupts:
			 */
			irq_desc[i].chip = &i8259A_irq_type;
		} else {
			/*
			 * 'high' PCI IRQs filled in on demand
			 */
			irq_desc[i].chip = &no_irq_type;
		}
	}
}

void __init init_IRQ(void)
{
	int i;

	/* all the set up before the call gates are initialised */
	pre_intr_init_hook();

	/*
	 * Cover the whole vector space, no vector can escape
	 * us. (some of these will be overridden and become
	 * 'special' SMP interrupts)
	 */
	//对中断门(32~255)进行初始化
	for (i = 0; i < (NR_VECTORS - FIRST_EXTERNAL_VECTOR); i++) {
		int vector = FIRST_EXTERNAL_VECTOR + i;
		if (i >= NR_IRQS/*=224*/)
			break;
		if (vector != SYSCALL_VECTOR) //排除系统门0x80,因为系统门已经在trap_int时初始化过。
			set_intr_gate(vector, interrupt[i]);//interrupt是怎么定义的呢? 是IRQLIST_16
	}

	/* setup after call gates are initialised (usually add in
	 * the architecture specific gates)
	 */
	intr_init_hook();

	/*
	 * Set the clock to HZ Hz, we already have a valid
	 * vector now:
	 */
	setup_pit_timer();

	/*
	 * External FPU? Set up irq13 if so, for
	 * original braindamaged IBM FERR coupling.
	 */
	if (boot_cpu_data.hard_math && !cpu_has_fpu)
		setup_irq(FPU_IRQ, &fpu_irq);

	irq_ctx_init(smp_processor_id());
}
