/*
 * Copyright (C) 2010
 *
 * ----------------------------------------------------------------------------
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 */

#include <common.h>
#include <asm/arch/reg_defs.h>
#include <asm/io.h>
#include <asm/hardware.h>		//for ka_xxx()

#if !defined(CONFIG_FLASH_CFI_DRIVER)
#include "../common/flash.c"
#endif

DECLARE_GLOBAL_DATA_PTR;

/*
static int null_delay(int n)
{
	volatile int i = n;
	volatile int j = 0;
	while (i-- > 0)
		j++;
	return j;
}
*/

//------------------------------------------------------------------------------
int board_init(void)
{
	//printf("board_init\n");
	/* arch number of the board */
	gd->bd->bi_arch_number = MACH_TYPE_DAVINCI_EVM;

	/* address of boot parameters */
	gd->bd->bi_boot_params = 0x0000100; //LINUX_BOOT_PARAM_ADDR;

	/* if workarounds are needed */
	ka_errata_workarounds();

	/* Disable SSI Clock */
	word_write(KA_REGIF_BASE, word_read(KA_REGIF_BASE) & 0xffdfffff);
	//buzzer_off();

	printf("Status %x\n", word_read(SDSW_M1_STATUS));

	/* Enable UART */
	/*word_write(UART_LCR,  0x83);
	word_write(UART_INTR, 0x00);
	word_write(UART_RECV, 0x28);	//baudrate=19200,12MHz
	word_write(UART_LCR,  0x03); */

	/* Close PLL */
	/* Power on required peripherals */
	//timer_init();
	//word_write(GPIO_OUTPUT, 0x66);

	return(0);
}

#ifdef CONFIG_MISC_INIT_R
int misc_init_r(void)
{
	//TODO: Placeholder
	//setenv ("bootcmd", "run set_bootargs; bootm 1ffc00; sf probe 0; sf read 100000 3200 19000; go 19000\0");
	return(0);
}
#endif

#ifdef CONFIG_SYS_PROGRAM_BIN
void detect_program_files(void);
#endif

#ifdef CONFIG_BOARD_LATE_INIT
extern void buzzer_off(void);

int board_late_init (void)
{
	u32 v1, v2;
	//do_remap(0x00000000, 0x01f00000);

#ifdef CONFIG_SYS_PROGRAM_BIN
	detect_program_files();
	setenv ("bootdelay", "5");
	//setenv ("bootf", "go 208000");
#else
	v2 = word_read(0x208000);

	if (v2 == 0xe1a00000 || v2 == 0xe3a00000) {
		printf("do thing\n");
		//setenv ("bootf", "mmc init; go 208000");
	}
	else {
		//setenv ("bootf", "mmc init; fatload mmc 1 208000 image; go 208000");
		setenv ("bootf", "run boot_sd");
	buzzer_off();
	}
#endif

#ifdef CONFIG_STAY_AT_UBOOT
	setenv("bootdelay", "-1");
#else
	setenv("bootdelay", "2");
#endif
	v1 = word_read(CONFIG_ENV_OFFSET - 0x400);
	if((v1 == 0x0) || (v1 == 0xffffffff))
		run_command("saveenv", 0);

	return (0);
}
#endif

#if !defined(CONFIG_FLASH_CFI_DRIVER)
unsigned long flash_init(void)
{
	//dummy function: we do not need to init flash
	return (0);
}
#endif

void do_remap(unsigned int src_base, unsigned int dest_base, int length)
{
	int i;

	//transfer data
	for(i = 0; i < length; i++)
		writel(readl(src_base + i), (dest_base + i));

	//set remap reg to compelete remap
	word_write(0xa0000010, 0x1);
	return ;
}



int dram_init(void)
{
	gd->bd->bi_dram[0].start = PHYS_SDRAM_1;
	gd->bd->bi_dram[0].size = PHYS_SDRAM_1_SIZE;
	/* dram_init must store complete ramsize in gd->ram_size */
	gd->ram_size = get_ram_size((long *)CONFIG_SYS_SDRAM_BASE,
			CONFIG_SYS_SDRAM_SIZE);

	return(0);
}

