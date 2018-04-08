
#include <config.h>
#include <common.h>
#include <command.h>
#include <mmc.h>
#include <part.h>
#include <malloc.h>
#include <mmc.h>
#include <spi_flash.h>
#include <fat.h>
#include <asm/errno.h>
#include <asm/io.h>

#include <asm/arch/reg_defs.h>
//#include "ka2000-define.h"
#include "sdctrl.h"

#ifndef SCU_CHIP_ID_HI
#define SCU_CHIP_ID_HI 0xa0000030
#define SCU_CHIP_ID_LO 0xa0000034
#endif

#define DRIVER_NAME "ka2000-mmc"


static block_dev_desc_t mmc_blk_dev;

extern int m1_busy(void);

void test_switch_register(void)
{
	u32 v0[64];
	u32 v1[64];
	u32 r;
//	u32 val;
	int i;
//	int n = 24;
	r = SDSW_BASE;

	while (1) {
		for (i = 0; i < 64; i++) {
			v0[i]=v1[i];
			v1[i] = word_read(r+i*4);
		}

		for (i = 0; i < 64; i++) {
			if (v1[i] != v0[i])
				printf("i %d v0 %08x, v1 %08x", i, v0[i], v1[i]);
		}
	}

}

#ifndef CONFIG_GENERIC_MMC
block_dev_desc_t *mmc_get_dev(int dev)
{
	return (block_dev_desc_t *) &mmc_blk_dev;
}
#endif

static __inline__ int null_delay(int t)
{
	volatile int i = 0;
	volatile int j = 0;
	for (i = 0; i < t; i++) {
		for (j = 0; j < 5; j++); //nop();
	}
	return j;
}

#if 0
extern int m2_err;
uint32_t mmc_bread(int dev_num, uint32_t blknr, lbaint_t blkcnt, void *dst)
{
	int i = 0;
	int n = blkcnt;
	int m = 4;   //64
	int ret = 1;

	if (m2_err > 1)
		return 0;
	//switch_to_m2();
	for (i = 0; i + m < blkcnt; i += m) {
		ret = CardRead_single_pin((unsigned int)dst, blknr, m);
		if (m2_err > 2)
			return 0;

		null_delay(10);
		dst += m * 512;
		blknr += m;
		n -= m;
	}

	if (n > 0)
		ret = CardRead_single_pin((unsigned int)dst, blknr, n);
	switch_to_m1();
	ret = (ret==0) ? blkcnt : 0;
	return ret; //1;
}
#else
uint32_t mmc_bread(int dev_num, uint32_t blknr, lbaint_t blkcnt, void *dst)
{
	int ret = 1;
	int n = (int)blkcnt;
	int m = 8; //64
	int r = 50;

	//switch_to_m2();
	if(blkcnt <= 1)
		ret = SDCardRead((unsigned int)dst, blknr, (int)blkcnt, 1);
	else {
		for (n = blkcnt; n > 0; n -= m, r = 50) {
			if(n < m) m = n;
			while(r--) {
				if((ret = SDCardRead((unsigned int)dst, blknr, m, 0)) == 0)
					break;
				null_delay(10);
				printf("<%d.rf %d>" , r, blknr);
			}
			/*if((ret = SDCardRead((unsigned int)dst, blknr, m, 0)) != 0) {
			         printf("<r %d fail>" , blknr);
			        break;
			}*/
			null_delay(5); //10
			dst += m * 512;
			blknr += m;
		}
		//ret = SDCardRead((unsigned int)dst, blknr, (int)blkcnt, 0);
	}
	//switch_to_m1();
	ret = (ret==0) ? blkcnt : 0;
	return ret; //1;
}
#endif
uint32_t mmc_bwrite(int dev_num, uint32_t blknr, lbaint_t blkcnt, void *src)
{
	int ret = 1;
	int n = (int)blkcnt;
	int m = 8;
	int r = 50;

	if (blkcnt == 0)
	{
	    return 0;
	}
	if(blkcnt <= 1)
	{
	    ret = SDCardWrite((unsigned int)src, blknr, (int)blkcnt, 1);
	}
	else
	{
        for (n = blkcnt; n > 0; n -= m, r = 50)
        {
			if(n < m) m = n;
			while(r--)
			{
				if((ret = SDCardWrite((unsigned int)src, blknr, m, 0)) == 0)
					break;
				null_delay(10);
				printf("<%d.wr %d>" , r, blknr);
			}
			null_delay(5);
			src += m * 512;
			blknr += m;
		}
	}

	ret = (ret==0) ? blkcnt : 0;
	return ret;
}


#ifdef CONFIG_RECORD_CMD_ENABLE
u32 a[4096][10];
int ai = 0;
static u32 cmd13_count;
//SDSW_M1_CMD_FLG_REG2
#define ACMD6                   (0x1 << 6)
#define ACMD13                  (0x1 << 13)
#define ACMD22                  (0x1 << 22)
#define ACMD23                  (0x1 << 23)

//SDSW_M1_CMD_FLG_REG3
#define ACMD41                  (0x1 << 9)
#define ACMD42                  (0x1 << 10)
#define ACMD51                  (0x1 << 19)
extern void ka_logcon_flush2file(void);
extern void ka_logcon_puts(const char *s);

#ifdef CONFIG_RECORD_CMD_QUIET
#define printf ka2000_dbg
#endif

void ka2000_dbg(const char *fmt, ...)
{
	va_list va;
	char buff[256];

	va_start(va, fmt);
	vsprintf(buff, fmt, va);
	va_end(va);

	ka_logcon_puts(buff);

}

void print_m1_cmds()
{
	int i, j, n;
	u32 cmd;
	//u32 v;
	//char buf[256];
	char *p;

	printf("\n-- %3d   pre cmd    arg     resp0   repeat \n", cmd13_count);

	cmd13_count = 0;

	j = 0;
	n = ai;

	if (ai > 1000)
	{
	    j = ai - 1000;
	}
	for (i = j; i < ai; i++)
	{
        cmd = a[i][2]&0x7f;
        if (cmd == 0x7f)
            printf("%08x %3d  -- %08X %08x %6d\n", a[i][0], a[i][1]&0x7f,
			a[i][3], a[i][4], a[i][9]);
        else if (cmd >=64)
            printf("%08x %3d a%2d %08X %08x %6d\n", a[i][0], a[i][1]&0x7f, cmd - 64,
			a[i][3], a[i][4], a[i][9]);
        else
            printf("%08x %3d %3d %08X %08x %6d\n", a[i][0], a[i][1]&0x7f, cmd,
			a[i][3], a[i][4], a[i][9]);
	}

	ai = 0;
}
u64 j, jiffies = 0;
void save_m1_cmds(void)
{
	a[ai][0] = word_read(SDSW_M1_STATUS);
	a[ai][1] = word_read(SDSW_M1_PREV_CMD_REG);
	a[ai][2] = word_read(SDSW_M1_CURR_CMD_REG);  word_write(SDSW_M1_CURR_CMD_REG, 0xff);
	a[ai][3] = word_read(SDSW_M1_CURR_ARGU_REG); word_write(SDSW_M1_CURR_ARGU_REG, 0);
	a[ai][4] = word_read(SDSW_M1_CURR_RSP_REG1); word_write(SDSW_M1_CURR_RSP_REG1, 0);
	//a[ai][5] = word_read(SDSW_M1_CMD_FLG_REG0);  word_write(SDSW_M1_CMD_FLG_REG0, 0xffffffff);
	//a[ai][6] = word_read(SDSW_M1_CMD_FLG_REG1);  word_write(SDSW_M1_CMD_FLG_REG1, 0xffffffff);
	//a[ai][7] = word_read(SDSW_M1_CMD_FLG_REG2);  word_write(SDSW_M1_CMD_FLG_REG2, 0xffffffff);
	//a[ai][8] = word_read(SDSW_M1_CMD_FLG_REG3);  word_write(SDSW_M1_CMD_FLG_REG3, 0xffffffff);
    //word_write(SDSW_M1_PREV_CMD_REG, 0);

	//jiffies += 13;
	a[ai][9] = jiffies - j;
	j = jiffies;
	ai++;

	//if (ai >= 4096)
    //    ai = 0;
	if (ai >= 4096)
		print_m1_cmds();
}

void t1(void)
{
	static int i;
	u32 cmd = word_read(SDSW_M1_CURR_CMD_REG);
	if (cmd != 13)
	{
		printf("%2d %08x\n", word_read(SDSW_M1_CURR_CMD_REG), word_read(SDSW_M1_CURR_ARGU_REG));
	}
}

unsigned char tmpbuf[512];
void record_cmd(void)
{
	int i;
	u32 arg;
	u32 cmd = word_read(SDSW_M1_CURR_CMD_REG);
    int n, m;

	printf("record cmd...\n");
    jiffies++;

	//save_m1_cmds();
	//while (word_read(SDSW_M1_STATUS) != 0x20200804) save_m1_cmds();
    m = 0;
	for (i = 0; i < 65536; i++)
	{
	    jiffies++;
		save_m1_cmds();
		u32 cmd = word_read(SDSW_M1_CURR_CMD_REG);
		u32 arg = word_read(SDSW_M1_CURR_ARGU_REG);
		u32 status = word_read(SDSW_M1_STATUS);
		n = 0;

		while (cmd == word_read(SDSW_M1_CURR_CMD_REG) && arg == word_read(SDSW_M1_CURR_ARGU_REG))
		{
		    jiffies++;

			//if (cmd == 13) {
				n++;
				if (n > 10000)
					break;
			//}

			    m++;
			    if (m > 100000)
                {
                    //i = 65536;
					print_m1_cmds();
					m = 0;
                    break;
                }


		}
		if (n < 10000)
            m = 0;
	}

	print_m1_cmds();

	//CardReadsingle((unsigned int)tmpbuf, 4096);
}
#else
void record_cmd(void)
{

}
#endif

uint32_t mmc_sendcmd(int dev_num, unsigned char cmd, unsigned int arg, unsigned int ctrl, void *data)
{

	int ret = 1;
#ifdef CONFIG_RECORD_CMD_ENABLE
    if (cmd == 0xff)
    {
        record_cmd();
        return 0;
    }
#endif
	printf("\nmmc send SD CMD%d ARG:%08x .. ", cmd, arg);
	if (switch_to_m2() == 0)
		return ret;
	ret = HCmdNoData((arg>>24) & 0xff, (arg>>16) & 0xff, (arg>>8) & 0xff, arg & 0xff, (unsigned int)cmd, (ctrl>>8) & 0xff, ctrl & 0xff);
	//ret = HCmdNoData(arg & 0xff, (arg>>8) & 0xff, (arg>>16) & 0xff, (arg>>24) & 0xff, (unsigned int)cmd, (ctrl>>8) & 0xff, ctrl & 0xff);
	switch_to_m1();
	if(ret == 0) {
		if(ctrl & 0xC00) {
			printf("\nRSP:%08x ", word_read(SDR_RESPONSE1_REG));
			if((ctrl & 0xC00) == 0x400) //R2
				printf("%08x %08x %08x ", word_read(SDR_RESPONSE2_REG), word_read(SDR_RESPONSE3_REG), word_read(SDR_RESPONSE4_REG));
		}
		//printf(" .. passed.\n");
	}
	//else
	//printf(" failed.\n");
	return ret;
}

int CardWrite_single_pin(unsigned int blk_no, int blk_num, unsigned char *buffer);

/* find first device whose first partition is a DOS filesystem */
int find_fat_partition (block_dev_desc_t *dev_desc)
{
//	int i;
	int j;
	unsigned char *part_table;
	unsigned char buffer[512];
	unsigned int part_offset = 0;

	if (!dev_desc) {
		debug ("couldn't get ide device!\n");
		return (-1);
	}
	if (dev_desc->part_type == PART_TYPE_DOS) {
		if (dev_desc->
		    block_read (dev_desc->dev, 0, 1, (ulong *) buffer) != 1) {
			debug ("can't perform block_read!\n");
			return (-1);
		}
		part_table = &buffer[0x1be];	/* start with partition #4 */
		for (j = 0; j < 4; j++) {
			if ((part_table[4] == 1 ||	/* 12-bit FAT */
			     part_table[4] == 4 ||	/* 16-bit FAT */
			     part_table[4] == 6) &&	/* > 32Meg part */
			    part_table[0] == 0x80) {	/* bootable? */
				//curr_dev = i;
				part_offset = part_table[11];
				part_offset <<= 8;
				part_offset |= part_table[10];
				part_offset <<= 8;
				part_offset |= part_table[9];
				part_offset <<= 8;
				part_offset |= part_table[8];
				debug ("found partition start at %ld\n", (long int)part_offset);
				return (0);
			}
			part_table += 16;
		}
	}


	//debug ("no valid devices found!\n");
	return (-1);
}

unsigned long buf[4096];
unsigned long swctl;
int fat_register_device(block_dev_desc_t *dev_desc, int part_no);
int ka2000_sf_erase(struct spi_flash *flash, u32 offset, size_t len);
int ka2000_sf_read(struct spi_flash *flash, u32 offset, size_t len, void *buf);
int ka2000_sf_write(struct spi_flash *flash, u32 offset, size_t len, const void *buf);
int CardErase_single(unsigned int blk_no);

void rebuild_sd(void)
{
#ifdef CONFIG_SYS_PROGRAM_BIN
	/* Clear SD signature */
	memset(buf, 0, 512);

	CardWrite_single_pin(0, 1, buf);
	CardWrite_single_pin(6, 1, buf);
	CardWrite_single_pin(8, 1, buf);
	CardWrite_single_pin(30, 1, buf);
	CardWrite_single_pin(31, 1, buf);
	CardWrite_single_pin(238, 1, buf);
	CardWrite_single_pin(240, 1, buf);
	CardWrite_single_pin(262, 1, buf);

	/* rebuild mbr and fat */
	//CardWrite_single_pin(0, 1, mbr);
	//CardWrite_single_pin(8192, 1, buf);
#endif
}

extern void buzzer_off(void);
extern void buzzer_on(int tone);

int detect_program_bin(void)
{
//	int ret, count;
	int size;
	u32 buf[256/4];

	fat_register_device(&mmc_blk_dev,1);
	//buzzer_off();

	size = file_fat_read ("program.bin", (void *)buf, 256);

	if (size > 1 && (buf[0] & 0xffffff00) == 0xea000000) {
		buzzer_on(0);
		run_command("fatload mmc 1 e00000 program.bin; go e00000", 0);
		buzzer_off();
	}
	//buzzer_off();
	return 0;
}

#ifdef CONFIG_SYS_PROGRAM_BIN
int do_pretest(int *pretest)
{
	int size1 = -1;

	if(*pretest == 1)
		return 0;
	*pretest = 1;

	if ((size1 = file_fat_read ("preprog_chk.bin", NULL,  32)) != -1) {
		run_command("fatload mmc 1 100000 preprog_chk.bin; sf erase 100000 1000; sf write 100000 100000 400; sf read 30000 100000 400; sf erase 100000 1000\0", 0);
		if(memcmp((void *)0x100000, (void *)0x30000, 512) == 0)
			printf("pre-program check passed.\n");
		else
			return -1;
	}
	return 0;
}

void detect_program_files(void)
{
	int size = -1, program = 0;
	u32 buf[512];
	void *tmp;
	int chipRev = 0;
	int pretest = 0;

	if((word_read(SCU_CHIP_ID_HI) == 0x39323044) && (word_read(SCU_CHIP_ID_LO) == 0x004b4130))
		chipRev = 3;
	else if((word_read(SCU_CHIP_ID_HI) == 0x0) && (word_read(SCU_CHIP_ID_LO) == 0x0))
		chipRev = 2;

    printf("******          Program Mode (GEN%d)              ******\n", chipRev);

    while ((word_read(SDSW_M1_STATUS) & 0xff) != 0x04) {
        printf("status %08x, %02d, %8x %8x %8x\n", word_read(SDSW_M1_STATUS), word_read(SDSW_M1_CURR_CMD_REG), word_read(SDSW_M1_CURR_ARGU_REG), word_read(SDSW_M1_CURR_RSP_REG1));
    }

	if(m1_busy() == 0) {
		buzzer_on(0);

		run_command("sf probe 0; mmc init", 0);

		fat_register_device(&mmc_blk_dev,1);
		run_command("mmc init", 0);
		null_delay(10);

#ifdef CONFIG_SYS_ALLOW_PROGRAM_AUTOLOAD_TABLE
		if ((size = file_fat_read ("autoload.tbl", buf, 32)) != -1) {
			printf("found autoload.tbl, start program.\n");
			//printf("program autoload: %08x %08x %08x %08x\n", buf[0], buf[1], buf[2], buf[3]);
			run_command("sf read 1000 0 1000; md 1000 10; fatload mmc 1 1000 autoload.tbl; sf erase 0 1000; sf write 1000 0 1000; sf read 3000 0 1000; md 3000 10\0", 0);
		} else if (chipRev == 3) {
			run_command("sf read 1000 0 400", 0);
			if((*(unsigned int*)0x1004) != 0x00000300) {
				printf("enable delay chain\n");
				run_command("sf read 1000 0 1000; md 1000 10; mw 1004 00000300; sf erase 0 1000; sf write 1000 0 1000; sf read 3000 0 1000; md 3000 10\0", 0);
			}
		}
		null_delay(5);
#else

/*
		//"program_kernel=fatload mmc 1 208400 zImage; sf erase 200000 380000; sf write 200000 200000 380000\0" \
		//"program_uboot=sf read e00000 3000 20000; fatload mmc 1 e00200 u-boot.bin;  sf erase 3000 20000; sf write e00000 3000 20000\0" \
		//"program_mtd=fatload mmc 1 500000 mtd_jffs2.bin; sf erase 580000 280000; sf write 500000 580000 280000\0" \
*/

#ifdef CONFIG_SYS_ALLOW_PROGRAM_UBOOT
		if ((size = file_fat_read ("u-boot.bin", NULL,  32)) != -1) {
			if(do_pretest(&pretest) == -1)
				goto abort_program;
			//buzzer_on(0);
			printf("found u-boot.bin, start program.\n");
			run_command("sf read e00000 3000 20000; fatload mmc 1 e00200 u-boot.bin;  sf erase 3000 20000; sf write e00000 3000 20000\0", 0);
			//tmp = 0xe00000;
			//memset(tmp, 0, 0x20000);
			program++;
		}
#endif
#ifdef CONFIG_SYS_ALLOW_PROGRAM_BOOTLOADER
		if ((size = file_fat_read ("ka_bootldr.bin", NULL, 32)) != -1) {
			if(do_pretest(&pretest) == -1)
				goto abort_program;
			//buzzer_on(1);
			printf("found ka_bootldr.bin, start program.\n");
			//run_command("run 1st boot loader", 0);
			run_command(" sf read d00000 0 4000; fatload mmc 1 d00400 ka_bootldr.bin; sf erase 0 4000; sf write d00000 0 4000\0", 0);
			//size = file_fat_read ("ka_bootldr.bin", buf, 128 * 1024);
			tmp = 0xd00000;
			memset(tmp, 0, 0x4000);
			program++;
		}
#endif

		if(chipRev == 3) {
			if ((size = file_fat_read ("image3", NULL,  32)) != -1) {
				if(do_pretest(&pretest) == -1)
					goto abort_program;
				//buzzer_on(1);
				printf("found image3, start program.\n");
				if (size > 0x300000)
					printf("image3 oversize, size 0x%x > 0x300000\n", size);
				run_command("fatload mmc 1 208000 image3; sf erase 200000 300000; sf write 1ffc00 200000 300000\0", 0);
				/*run_command("fatload mmc 1 208000 image3\0", 0);
				if(pretest == 0)
				{
				    pretest = 1;
				    run_command("sf erase 100000 1000; sf write 208000 100000 1000; sf read 30000 100000 1000; sf erase 100000 1000\0", 0);
				    if(memcmp((void *)0x208000, (void *)0x30000, 512) != 0)
				    {
				        run_command("md 208000 10; md 30000 10\0", 0);
				        goto abort_program;
				    }
				}
				run_command("sf erase 200000 300000; sf write 1ffc00 200000 300000\0", 0);*/
				program++;
			}
		} else {
			if ((size = file_fat_read ("image", NULL,  32)) != -1) {
				if(do_pretest(&pretest) == -1)
					goto abort_program;
				//buzzer_on(1);
				printf("found image, start program.\n");
				if (size > 0x300000)
					printf("image oversize, size 0x%x > 0x300000\n", size);
				run_command("fatload mmc 1 208000 image; sf erase 200000 300000; sf write 1ffc00 200000 300000\0", 0);
				program++;
			}
		}
#if 0
		else if ((size = file_fat_read ("zImage", NULL, 32)) != -1) {
			//buzzer_on(1);
			printf("found zImage, start program.\n");
			run_command("fatload mmc 1 208000 zimage; sf erase 200000 300000; sf write 1ffc00 200000 300000\0", 0);
			program++;
		}
#endif

		if(chipRev == 3) {
			if ((size = file_fat_read ("initramfs3.gz", NULL, 32)) != -1) {
				if(do_pretest(&pretest) == -1)
					goto abort_program;
				//buzzer_on(1);
				printf("found initramfs3.gz, start program.\n");
				if (size > 0x300000)
					printf("initramfs3.gz oversize, size 0x%x > 0x300000\n", size);
				run_command("fatload mmc 1 500000 initramfs3.gz; sf erase 500000 300000; sf write 4ffc00 500000 300000\0", 0);
				program++;
			}
		} else {
			if ((size = file_fat_read ("initramfs.gz", NULL, 32)) != -1) {
				if(do_pretest(&pretest) == -1)
					goto abort_program;
				//buzzer_on(1);
				printf("found initramfs.gz, start program.\n");
				if (size > 0x300000)
					printf("initramfs.gz oversize, size 0x%x > 0x300000\n", size);
				run_command("fatload mmc 1 500000 initramfs.gz; sf erase 500000 300000; sf write 4ffc00 500000 300000\0", 0);
				program++;
			}
		}

		if ((size = file_fat_read ("mtd_jffs2.bin", NULL,  32)) != -1) {
			if(do_pretest(&pretest) == -1)
				goto abort_program;
			//buzzer_on(0);
			printf("found mtd_jffs2.bin, start program.\n");
			//run_command("fatload mmc 1 d00000 mtd_jffs2.bin; sf erase 700000 100000; sf write d00000 700000 100000\0", 0);
			run_command("fatload mmc 1 d00000 mtd_jffs2.bin; sf erase 80000 100000; sf write d00000 80000 100000\0", 0);

			tmp = 0xd00000;
			memset(tmp, 0, 0x100000);
			program++;
		}
#if 0
		if(chipRev == 3) {
			setenv ("bootargs", "root=/dev/ram0 rw console=ttyS0,38400n8 mem=32M");
			if (((tmp = getenv("misc_args")) == NULL) || (strcmp(tmp, "mem=32M") != 0))
				setenv ("misc_args", "mem=32M");
			run_command("saveenv", 0);
		}
#endif

#endif //CONFIG_SYS_ALLOW_PROGRAM_AUTOLOAD_TABLE
		//if(program == 1)
		//    rebuild_sd();
		buzzer_off();
		return;
	}
abort_program:
	printf("pre-program check failed! Abort programming.\n");
	run_command("md 100000 10; md 30000 10\0", 0);
	buzzer_off();
	buzzer_on(1);
	null_delay(10);
	buzzer_off();
}
#endif

#if 0

/* ------------------------------------------------------------------------- */
/* patch for ka2000_mmc.c call sf read/write/erase */
int ka2000_sf_erase(struct spi_flash *flash, u32 offset, size_t len)
{
    char cmd[256];
    /* sf erase offset [+]len */
    sprintf(cmd, "sf erase 0x%x 0x%x", offset, len);
    run_command(cmd, 0);
    return 0;
}

int ka2000_sf_read(struct spi_flash *flash, u32 offset, size_t len, void *buf)
{
    char cmd[256];
    /* sf read addr offset len */
    sprintf(cmd, "sf read 0x%x 0x%x 0x%x", (u32)buf, offset, len);
    run_command(cmd, 0);
    return 0;
}

int ka2000_sf_write(struct spi_flash *flash, u32 offset, size_t len, const void *buf)
{
    char cmd[256];
    /* sf write addr offset len */
    sprintf(cmd, "sf write 0x%x 0x%x 0x%x", (u32)buf, offset, len);
    run_command(cmd, 0);
    return 0;
}

int mmc_check_sw_signature(void)
{
//	unsigned long offset = 0;
//	unsigned long count = 0x100;
//	long size, size1;

	mmc_bread(0, 30, 1, (unsigned char *)buf);

	swctl = buf[0];

	if ((swctl & 0xffffff) == 0x544d53) {
		printf("******  Erase flash SWCTL Reg To Program Mode ******\n");
		ka2000_sf_read(NULL, 0x3000, 0x1000, buf);
		ka2000_sf_erase(NULL, 0x3000, 0x1000);
		buf[0] = 0xffffffff;

		ka2000_sf_write(NULL, 0x3000, 0x1000, buf);
		run_command("go 0", 0);
		return 1;
	}
	return 0;
}
#endif
int mmc_inited = 0;


int mmc_legacy_init(int verbose)
{
//	unsigned long fat_tag, mbr_tag;

	//test_switch_register();
	if (mmc_inited == 1)
		return 0;
	if (mmc_inited == 0) {
		debug("MMC Init Card ...\n");
		InitCardReader();
		//InitCard();
		if(InitCard())
			printf("InitCard failed\n");

		mmc_blk_dev.if_type = IF_TYPE_MMC;
		mmc_blk_dev.part_type = PART_TYPE_UNKNOWN; //PART_TYPE_DOS;
		mmc_blk_dev.dev = 1;
		mmc_blk_dev.lun = 0;
		mmc_blk_dev.type = 0;

		/* FIXME fill in the correct size (is set to 32MByte) */
		mmc_blk_dev.blksz = 512;
		mmc_blk_dev.lba = 0x100000;
		mmc_blk_dev.removable = 0;
		mmc_blk_dev.block_read = mmc_bread;
		mmc_blk_dev.block_write = mmc_bwrite;
		mmc_blk_dev.sd_cmd = mmc_sendcmd;
#ifndef CONFIG_SYS_PROGRAM_BIN
		buzzer_off();
#endif

#if 0
		if (find_fat_partition(&mmc_blk_dev) == 0) {
			check_mbr_and_fat();
		}
#endif
	}

	if (fat_register_device(&mmc_blk_dev, 1) != 0) {
		//printf("Could not register MMC fat device\n");
		init_part(&mmc_blk_dev);
	}

	if (mmc_inited == 0) {
		mmc_inited = 1;
		//ka2000_isp();
#if defined(CONFIG_RECORD_CMD_ENABLE) && defined(CONFIG_RECORD_CMD_AUTO)
        record_cmd();
#endif

#ifndef CONFIG_SYS_PROGRAM_BIN
		if (m1_busy() == 0)
			detect_program_bin();
#endif
	}

	mmc_inited = 1;

	return 0;
}
