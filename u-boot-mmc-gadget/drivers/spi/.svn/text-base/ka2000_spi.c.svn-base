/*
 * (C) Copyright 2009
 * Frank Bodammer <frank.bodammer@gcd-solutions.de>
 * (C) Copyright 2009 Semihalf, Grzegorz Bernacki
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

/* #define DEBUG */

#include <stdbool.h>
#include <common.h>
#include <asm/io.h>
#include <malloc.h>
#include <spi.h>
#include <asm/arch/reg_defs.h>
#include <spi_flash.h>

DECLARE_GLOBAL_DATA_PTR;


// SD Controller Registers
#define MULTE 		(0x1 << 14)
#define MIMSK 		(0x1 << 13)
#define TAGD  		(0x1 << 12)
#define SPIMW 		(0x1 << 11)
#define MSTR  		(0x1 << 10)
#define CSO   		(0x1 << 9)
#define ENSCK 		(0x1 << 8)
#define SMOD_PO 	(0x0 << 6)
#define SMOD_INT 	(0x1 << 6)
#define SMOD_DMA 	(0x2 << 6)
#define DRD   		(0x1 << 5)
#define DTD   		(0x1 << 4)
#define CSLV  		(0x1 << 3)
#define KEEP  		(0x1 << 2)
#define CPOL  		(0x1 << 1)
#define CPHA  		(0x1 << 0)
#define TMOD_BYTE  	(0x0 << 4)
#define TMOD_HWORD 	(0x1 << 4)
#define TMOD_WORD  	(0x2 << 4)


/* ------------------------------------------------------------------------- */

static void ka_ssi_WaitReady(void)
{
	int t = 5000;

	while ((word_read(SSI_STA) & 0x1) == 0) {
		t--;
		if (t == 0)
			printf("%s timeout.\n", __FUNCTION__);
	}
}

#define TMOD_MARK	(0x2 << 4)

/* set spi transfer bus width */
static u32 ssi_bits_per_word = 0;
static int ka_ssi_setup_TMOD(u8 bits_per_word)
{
	volatile u32 dat;

	if (ssi_bits_per_word == bits_per_word)
		return 0;

	ssi_bits_per_word = bits_per_word;

	ka_ssi_WaitReady();
	dat = word_read(SSI_STA);
	dat &= ~TMOD_MARK;

	if (bits_per_word <= 8)
	{
		dat |= TMOD_BYTE;
	}
	else if (bits_per_word <= 16)
	{
		dat |= TMOD_HWORD;
	}
	else if (bits_per_word <= 32)
	{
		dat |= TMOD_WORD;
	}
	else
	{
		printf("Incorrect TMOD: %d\n", bits_per_word);
		return -1;
	}

	ka_ssi_WaitReady();
	word_write(SSI_STA, dat);
	ka_ssi_WaitReady();

	return 0;
}


static void ka_ssi_cs(bool cs)
{
	u32 val = TAGD | SPIMW | MSTR | SMOD_PO | KEEP;

	if (cs == true)
		val |= CSO | ENSCK;


	ka_ssi_WaitReady();
	word_write(SSI_CON, val);
	ka_ssi_WaitReady();
}


/* ------------------------------------------------------------------------- */


struct soft_spi_slave {
	struct spi_slave slave;
	unsigned int mode;
};

static inline struct soft_spi_slave *to_soft_spi(struct spi_slave *slave) {
	return container_of(slave, struct soft_spi_slave, slave);
}


void spi_init (void)
{
	ka_ssi_WaitReady();

	/* TODO: check PCLK */
	/* baud rate = PCLK/2/(SSI_PRE+1), max_hz of flash is 25MHz. */
	word_write(SSI_PRE, 0x01);
}


int spi_cs_is_valid(unsigned int bus, unsigned int cs)
{
	return bus == 0 && cs == 0;
}


struct spi_slave *spi_setup_slave(unsigned int bus, unsigned int cs,
                                  unsigned int max_hz, unsigned int mode)
{
	struct soft_spi_slave *ss = NULL;

	if (!spi_cs_is_valid(bus, cs))
		return NULL;

	ss = malloc(sizeof(struct soft_spi_slave));
	if (!ss)
		return NULL;

	ss->slave.bus = bus;
	ss->slave.cs = cs;
	ss->mode = mode;

	/* Always use 8-bits mode. */
	ka_ssi_setup_TMOD(8);

	/* TODO: check PCLK */
	/* baud rate = PCLK/2/(SSI_PRE+1), max_hz of flash is 25MHz. */
	word_write(SSI_PRE, 0x01);

	/* TODO: Use max_hz to limit the SCK rate */

	return &ss->slave;
}

void spi_free_slave(struct spi_slave *slave)
{
	struct soft_spi_slave *ss = to_soft_spi(slave);

	free(ss);
}

int spi_claim_bus(struct spi_slave *slave)
{
	ka_ssi_cs(true);

	return 0;
}

void spi_release_bus(struct spi_slave *slave)
{
	ka_ssi_cs(false);
}


/* ------------------------------------------------------------------------- */


static int ka_ssi_tx(struct spi_slave *slave, unsigned int len,
                     const void *dout, unsigned long flags)
{
	unsigned char *data = (unsigned char *)dout;
	int i;


	// TODO: mode and length
	for (i = 0; i < len; i++) {
		ka_ssi_WaitReady();
		word_write(SSI_TDAT, data[i]);
	}

	/* Clear */
	ka_ssi_WaitReady();

	return 0;
}

static int ka_ssi_rx(struct spi_slave *slave, unsigned int len,
                     void *din, unsigned long flags)
{
	unsigned char *data = (unsigned char *)din;
	int i;

	/* Clear, Why here? */
	word_read(SSI_RDAT);

	// TODO: mode and length
	for (i = 0; i < len; i++) {
		ka_ssi_WaitReady();
		data[i] = byte_read(SSI_RDAT);
	}

	return 0;
}


int  spi_xfer(struct spi_slave *slave, unsigned int bitlen,
              const void *dout, void *din, unsigned long flags)
{
	unsigned int len;
	int ret = 0;
	int err = 0;

	if (bitlen == 0) {
		/* Finish any previously submitted transfers */
		ka_ssi_cs(false);
		return 0;
	}

#if 0 // Debug...
	debug("bitlen: %d ...\n", bitlen);
	if (bitlen <= 32 && dout) {
		int i;
		const unsigned char *data = (const unsigned char *)dout;
		debug("write: ");
		for (i = 0; i < bitlen / 8; i++)
		{
			debug("%02X ", data[i]);
		}
		debug("\n");
	}
#endif // Debug...

	// Do initialization?!
	if (flags & SPI_XFER_BEGIN) {
		ka_ssi_cs(true);
	}

	/*
	 * It's not clear how non-8-bit-aligned transfers are supposed to be
	 * represented as a stream of bytes...this is a limitation of
	 * the current SPI interface - here we terminate on receiving such a
	 * transfer request.
	 */
	if (bitlen % 8) {
		/* Errors always terminate an ongoing transfer */
		flags |= SPI_XFER_END;
		err = -1;
	} else {
		ka_ssi_setup_TMOD(8);

		len = bitlen / 8;

		if (dout ) {
			/*
			 * Need to gaurd 1st-boot-loader, u-boot.
			 * cf. cmd_sf.c
			 */
				ret = ka_ssi_tx(slave, len, dout, flags);
		}

		if (din) {
			ret = ka_ssi_rx(slave, len, din, flags);
		}
	}

	if (flags & SPI_XFER_END) {
		ka_ssi_cs(false);
	}

	return err;
}


