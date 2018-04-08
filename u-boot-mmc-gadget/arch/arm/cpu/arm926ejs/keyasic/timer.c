/*
 * Copyright (C) 2010
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/hardware.h>
#include <asm/arch/reg_defs.h>


/* 192M mode for test faster timer */
#define PRESCALE_V (128)
#define TIMER_PERIOD  0xffff // (0x1d4c)
#define T1_MUX 3
#define T2_MUX 0

/* Timer register bitfields */
#define TCR_ENAMODE_DISABLE          0x0
#define TCR_ENAMODE_ONESHOT          0x0
#define TCR_ENAMODE_PERIODIC         0x1

#define TCR_START_TIMER0	     (1 << 0)
#define TCR_START_TIMER1	     (1 << 20)

#define TCR_RELOAD_TIMER0	     (1 << 3)
#define TCR_RELOAD_TIMER1	     (1 << 22)

#define TCR_START_TIMER2	     (1 << 0)
#define TCR_RELOAD_TIMER2	     (1 << 2)
/* values for 'opts' field of struct timer_s */
#define TIMER_OPTS_DISABLED   0x00
#define TIMER_OPTS_ONESHOT    0x01
#define TIMER_OPTS_PERIODIC   0x02


#define TIMER_INTR		(0x1 << 22)
#define TIMER_TRIGGER_DMA	(0x1 << 24)

#define GPIO_OUTPUT 			 0xa0005008
#define INTC_INTPND1_ADDR		 0xa0006018

#define PRESCALER_MASK_T     (0xff << 16)
#define PRESCALER_VALUE_T(v)    ((v) << 16)
#define TRIGGER_MODE_MASK_T  (1 << 24)	//0x01000000
#define MUX0_MASK            (3 << 8)

#define TCR_START_TIMER TCR_START_TIMER1
#define TCR_RELOAD_TIMER TCR_RELOAD_TIMER1


#define SLOW_MODE		0
#define NORMAL_50_MODE		1
#define NORMAL_100_MODE		2
#define NORMAL_200_MODE		3
#define TIMER1_INTR		(0x1 << 22)



#define TIMER_LOAD_VAL	(CONFIG_SYS_HZ_CLOCK / CONFIG_SYS_HZ)
#define TIM_CLK_DIV	16

static ulong timestamp;
static int timer_inited = 0;

static void timer32_config(void)
{
    u32 tcr=0, tcfg0=0, tcfg1=0;

    tcfg0 = word_read(TCFG0);
    tcfg1 = word_read(TCFG1);

    tcr = ~(TCR_START_TIMER1) & word_read(TCON0);
    word_write(TCON0, tcr);			//disable timer
    word_write(TCNTB1, TIMER_PERIOD);					//reset counter to 0 (TCNTB0)

    /* set prescaler of T1 and T2 */
    word_write(TCFG0, (tcfg0 & ~PRESCALER_MASK_T) | PRESCALER_VALUE_T(PRESCALE_V));   	//setup prescaler (128) or T1 and T2
    // printf("TCFG0 %08x -> %08x\n", tcfg0, (tcfg0 & ~PRESCALER_MASK_T) | PRESCALER_VALUE_T);

    /* set trigger output mode and MUX */
    word_write(TCFG1, (word_read(TCFG1) | (T1_MUX << 8) | (T2_MUX << 10) | TRIGGER_MODE_MASK_T));  //timer 1 dma mode
	nop(); nop(); nop(); nop(); nop(); nop();
    word_write(TCFG1, word_read(TCFG1) & ~(TRIGGER_MODE_MASK_T));  //timer 1 interrupt mode

    tcr = word_read(TCON0);
    //ka2000_dbg("TCON0 %08x -> %08x\n", tcr, tcr | TCR_START_TIMER | TCR_RELOAD_TIMER);
    tcr |= TCR_START_TIMER1; // | TCR_RELOAD_TIMER1;
    word_write(TCON0, tcr);

    word_write(INTC_INTMSK1_ADDR, word_read(INTC_INTMSK1_ADDR) & ~TIMER1_INTR);
    timer_inited = 1;
}

int timer_init(void)
{
    // We do not need to initialize timer, dummy function
    timestamp = 0;
    if (timer_inited == 0)
    {
        timer32_config();
    }

    return(0);
}

unsigned int get_ka_tick(void)
{
    u32 t;

    if (timer_inited == 0)
    {
        timer32_config();
    }

    t = TIMER_PERIOD - word_read(TCNTO1);
    word_write(TCNTO1, TIMER_PERIOD);

    word_write(TCFG1, (word_read(TCFG1) | (T1_MUX << 8) | (T2_MUX << 10) | TRIGGER_MODE_MASK_T));  //timer 1 dma mode
	nop(); nop(); nop(); nop(); nop(); nop();
    word_write(TCFG1, word_read(TCFG1) & ~(TRIGGER_MODE_MASK_T));  //timer 1 interrupt mode

    return t >> 11;
}


void reset_timer(void)
{
    // dummy function
    timestamp = 0;
}

static ulong get_timer_raw(void)
{
    if (timer_inited == 0)
    {
        timestamp++;
    }

    timestamp += get_ka_tick();

    return timestamp;
}

ulong get_timer(ulong base)
{
    return((get_timer_raw() / (TIMER_LOAD_VAL / TIM_CLK_DIV)) - base);
}

void set_timer(ulong t)
{
    timestamp = t;
}

void __udelay(unsigned long usec)
{
    ulong tmo;
    ulong endtime;
    signed long diff;

    tmo = CONFIG_SYS_HZ_CLOCK / 1000;
    tmo *= usec;
    tmo /= (1000 * TIM_CLK_DIV);

    endtime = get_timer_raw() + tmo;

    do
    {
        ulong now = get_timer_raw();
        diff = endtime - now;
    }
    while (diff >= 0);
}

/*
 * This function is derived from PowerPC code (read timebase as long long).
 * On ARM it just returns the timer value.
 */
unsigned long long get_ticks(void)
{
    return(get_timer(0));
}

/*
 * This function is derived from PowerPC code (timebase clock frequency).
 * On ARM it returns the number of timer ticks per second.
 */
ulong get_tbclk(void)
{
    return CONFIG_SYS_HZ;
}
