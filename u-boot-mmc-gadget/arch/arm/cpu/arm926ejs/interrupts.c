#include <asm/hardware.h>

#include <asm/io.h>

/* require interrupts for the S3C4510B */
# ifdef CONFIG_USE_IRQ
#if 0
struct _irq_handler {
        void                *m_data;
        void (*m_func)( void *data);
};

static struct _irq_handler IRQ_HANDLER[64];
#endif

void do_irq (struct pt_regs *pt_regs)
{
#if 0
        unsigned int pending;

	pending = readl(0xa0006018);
        IRQ_HANDLER[pending].m_func( IRQ_HANDLER[pending].m_data);
#endif
	printf("do_irq\n");
        /* clear pending interrupt */
	writel(0x0, 0xa0006030);
}

#if 0
static void default_isr( void *data) {
	printf ("default_isr():  called for IRQ %d\n", (int)data);
}
#endif

int arch_interrupt_init (void)
{
#if 0
	int i;

	/* install default interrupt handlers */
	for ( i = 0; i < 64; i++) {
		IRQ_HANDLER[i].m_data = (void *)i;
		IRQ_HANDLER[i].m_func = default_isr;
	}
#endif
	/*disable all interrupts*/
	writel(~0x0, 0xa0006010);
	writel(~0x0, 0xa0006014);
	/* clear all interrupts*/
	writel(~0x0, 0xa0006018);
	writel(~0x0, 0xa000601C);

	//writel(0x0, 0xa0006008);
	//writel(0x0, 0xa000600C);

	/* clear any pending interrupts */
	writel(0x0, 0xa0006030);

printf("Interrupt init is done\n");
	return 0;
}
#endif

