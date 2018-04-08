/*
 * Miscelaneous KeyASIC functions.
 *
 * Copyright (C) 2012, Key ASIC Berhad.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
#define DEBUG
*/

#include <common.h>
#include <stdbool.h>
/*
#include <asm/arch/hardware.h>
#include <asm/io.h>
*/

DECLARE_GLOBAL_DATA_PTR;

static u32 logIdx = 0;
static char *logBuf = (char *)KA_LOGCON_ADDR;
static u32 logCnt = 0;
static bool ka_logcon_dumping = false;



void ka_logcon_init(void)
{
	memset((void *)KA_LOGCON_ADDR, 0, KA_LOGCON_SIZE);
}

u8 ka_logcon_isFull(void)
{
	return ka_logcon_dumping;
}

void ka_logcon_flush2file(void)
{
	char buff[64];
	u32 size;

	ka_logcon_dumping = true;

	if (logIdx) {
		size = logIdx;
		logIdx = 0;
	} else {
		size = KA_LOGCON_SIZE;
	}
	sprintf(buff, "mmc init; fatwrite mmc 1 %X %08d.txt %X",
		KA_LOGCON_ADDR, logCnt, size);
	run_command(buff, 0);
	logCnt++;
	ka_logcon_init();

	ka_logcon_dumping = false;
}

void ka_logcon_putc(const char c)
{
	if (c == '\n') {
		if (logIdx <= (KA_LOGCON_SIZE - 2)) {
			logBuf[logIdx++] = '\r';
			logBuf[logIdx++] = c;

			if (logIdx >= (KA_LOGCON_SIZE - 128)) {
				/* Force to flush at line-feed. */
				ka_logcon_flush2file();
				logIdx = 0;
				return;
			} else {
				return;
			}
		} else {
			/* Should not run to here */
			assert(0);
		}
	} else {
		logBuf[logIdx++] = c;
	}

	if (logIdx >= (KA_LOGCON_SIZE - 2)) {
		/* Reserve 2 bytes for "\r\n" */
		ka_logcon_flush2file();
		logIdx = 0;
	}
}

void ka_logcon_puts(const char *s)
{
	while (*s) {
		ka_logcon_putc (*s++);
	}
}


