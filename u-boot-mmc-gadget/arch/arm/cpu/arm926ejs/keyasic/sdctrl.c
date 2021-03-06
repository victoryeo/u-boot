/* SD Controller
 *
 * SD card must be written to 6 blks to prevent file system for message lost.
 *
 */
#include <config.h>
#include <common.h>
#include <mmc.h>
#include <asm/errno.h>
#include <asm/arch/hardware.h>
#include <part.h>

#include <asm/arch/reg_defs.h>

#include "sdctrl.h"

#define M2_INIT_CARD 1

//#define show_str_en
//#define simulation
#define SINGLE_BLOCK

//#define SIYO_READER
//#define GEN3
#if CONFIG_SYS_VER > 1 //#ifdef CONFIG_V34
#define SPECIAL_REG     0xa000a140
#else
#define SPECIAL_REG     0xa000a0c8
#endif

#define DMA_WADDR 	0x00b10000
#define DMA_RADDR 	0x00b10000
#define EN_INTR	  	(0x1  << 24)
#define BLK_CNT		0x01
#define XFER_COUNT	0x200
#define BUF_BLK_CNT	(BLK_CNT << 16)
#define BUF_XFER_START	(0x1  << 2)
#define BUF_WRITE	(0x1  << 1)
#define FIFO_XFER_DONE	0x1
#define SD_XFER_DONE	0x2
#define SD_CMD_DONE	0x4
#define SD_DATA_BOUND	0x8
#define DMA_CH0_INTR	0x1
#define DMA_CH1_INTR	0x2

#define SD_CLOCK_EN		(0x1 << 13)
#define SDSW_CLOCK_EN		(0x1 << 17)
#define SDIO_CLOCK_EN		(0x1 << 9)

int HighCap;
int RCA0, RCA1;
unsigned int OCR1, OCR2, RSP1;
int m1_m2 = 1;

/* for interrupt in ISR */
volatile int CRXferDone;
volatile int CRCardErr;
volatile int CRXferBDone;
volatile int CRCmdDone;
volatile int CRXferFinish;

//#define show_str_en
//#define show_str(a, b, c, d);  printf("%x %d %s", b, d, a);
#ifdef show_str_en
#define DBG printf
//#define show_str(a, b, c, d);  printf("%d, %d, %s", b, d, a );
#else
#define DBG(...); {}
//void show_str(char *start,unsigned int input,char *end,int end_len){}
#endif

int m1_err = 0;
#ifdef SIYO_READER
int special_cont = 0;
#endif

void buzzer_off(void);
void buzzer_on(int tone);

#if M2_INIT_CARD
void switch_to_m1_sw(void)
{
	return 0;
}

int switch_to_m2_sw(int flag)
{
    return 0;
}
#endif

void InitCardReader(void)
{
#ifdef show_str_en
	show_str("Enter InitCardReader().\n",0,"\n",1);
#endif

	//word_write(SCU_CLK_SRC_CTL, word_read(SCU_CLK_SRC_CTL) | SD_CLOCK_EN | SDSW_CLOCK_EN | SDIO_CLOCK_EN);
	//word_write(0xa0000014, 0x1);

	//Initialize DMA
	word_write(SDR_DMA_TCCH0_REG, XFER_COUNT);
	word_write(SDR_DMA_TCCH1_REG, XFER_COUNT);
	word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));
	word_write(SDR_Error_Enable_REG, 0x6f);			//enable all errors

	word_write(SDSW_M1_RDATA_TOUT_REG, 0xe000000);
	word_write(SDSW_DIRECT_CTRL_REG, 0);

	word_write(SDR_DMA_TRAN_RESP_REG, 0x1f);
	word_write(SDR_DMA_INTS_REG, 0x3);
	word_write(0xa0000028, 0xffff); //pclk enable (bit 10=sd host)

	//word_write(0xa000a01c, 0x2);
	//word_write(SPECIAL_REG, 0x2);
}

//------------------------------------------------------------------------------
static int null_delay(int t)
{
	volatile int i = 0;
	int j = 0;
	for (i = 0; i < t; i++) {
		j++;//nop();
	}
	return j;
}
//------------------------------------------------------------------------------

int m1_busy(void)
{
	if (m1_err)
		return 0;

	if ((word_read(SDSW_M1_STATUS) & 0x3f3f0000) != 0x20200000) {
		if (m1_m2 == 2)
			switch_to_m1();
		return 1;
	}

	return 0;
}

//------------------------------------------------------------------------------

int wait_m1_ready(int time_out)
{
	volatile int t = time_out;

	while (t-- > 0) {
		RCA0 = word_read(0xa000a09c) & 0xff;
		RCA1 = (word_read(0xa000a09c) & 0xff00) >> 8;
		//printf("Status %x RCA0 %x, RCA1 %x\n", word_read(SDSW_M1_STATUS), RCA0, RCA1);
		if (word_read(SDSW_M1_STATUS) == 0x20200804)
			return 1;

		//word_write(0xa000a01c, (1<<3) | (1<<2));
	}
	printf("Wait m1 timeout - Status %x RCA0 %x, RCA1 %x\n", word_read(SDSW_M1_STATUS), RCA0, RCA1);

	m1_err = 1;

	//while (1) {}
	return 0;
}

//------------------------------------------------------------------------------
void command_read_pre_clear_status(void)
{
	word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));

	word_write(SDR_DMA_TRAN_RESP_REG, (FIFO_XFER_DONE|SD_XFER_DONE|SD_CMD_DONE));	//clear all flags
	word_write(SDR_DMA_INTS_REG, 0x3);							//clear interrupts
}

//------------------------------------------------------------------------------
int command_read_single_block(unsigned int blk_flag)
{
	unsigned int LBA0, LBA1, LBA2;

	LBA0 = (( blk_flag) & 0x000000ff);
	LBA1 = (( blk_flag) & 0x0000ff00) >> 8;
	LBA2 = (( blk_flag) & 0x00ff0000) >> 16;

	// CMD17 (READ_SINGLE_BLOCK)
	// [31:0]data addr, R1
	if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x11, 0x79, 0x11))
		return 1;
	return 0;
}
//------------------------------------------------------------------------------
int CardRead_single_pin(unsigned int buf_dest_addr, unsigned int blk_no, int blk_num)
{
	unsigned int i;
	int blk_flag=0;
	unsigned int LBA0, LBA1, LBA2,DMA_RADDR_start;

#ifdef show_str_en
	show_str("Enter CardRead_single_pin().\n",0,"\n",1);
#endif
	//printf("r %d %d @%x\n", blk_no, blk_num, buf_dest_addr);
	/*word_write(SDR_CTRL_REG, word_read(SDR_CTRL_REG) | 0x40);
	for(i=0; i<100; i++); */
	if (m1_busy())
		goto  fail;

	for(i = 0; i<blk_num ; i++) {
		if (switch_to_m2() == 0)
			goto fail;
		WaitBusReady(0x0c);

		blk_flag = blk_no + i;

#ifdef show_str_en
		show_str("Send Read Block CMD. Blk\n",blk_flag,"\n",1);
#endif
		CRXferDone = 0;
		CRCardErr = 0;
		CRXferBDone = 0;
		CRXferFinish = 0;
		word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));

#ifdef simulation
		LBA0 = (( blk_flag * 512) & 0x000000ff);
		LBA1 = (( blk_flag * 512) & 0x0000ff00) >> 8;
		LBA2 = (( blk_flag * 512) & 0x00ff0000) >> 16;
#else
		LBA0 = (( blk_flag) & 0x000000ff);
		LBA1 = (( blk_flag) & 0x0000ff00) >> 8;
		LBA2 = (( blk_flag) & 0x00ff0000) >> 16;
#endif

		word_write(SDR_DMA_TRAN_RESP_REG, (FIFO_XFER_DONE|SD_XFER_DONE|SD_CMD_DONE));	//clear all flags
		word_write(SDR_DMA_INTS_REG, 0x3);							//clear interrupts

		// CMD17 (READ_SINGLE_BLOCK)
		// [31:0]data addr, R1
		if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x11, 0x79,0x11))
			goto fail;

		if(i > 0) { //the (i>0)th blk
			DMA_RADDR_start = DMA_RADDR + (512 * (i-1));

			//start DMA
			word_write(SDR_DMA_TCCH1_REG, XFER_COUNT);
			word_write(SDR_DMA_DACH1_REG, DMA_RADDR_start);
			word_write(SDR_DMA_CTRCH1_REG, 0x3F);
			word_write(SDR_DMA_CTRCH1_REG, 0x33);
			word_write(SDR_BUF_TRAN_CTRL_REG, BUF_BLK_CNT | BUF_XFER_START);

			//wait sd card transfer done for i-th block, transfer finish for (i-1)th block
			if(WaitResp(FIFO_XFER_DONE|SD_XFER_DONE, 0))
				goto fail;
			WaitDMAIntr(DMA_CH1_INTR);			//data moved to memory

		} else if(i==0) {
			if(WaitResp(SD_XFER_DONE, 0))			//first block of data read from SD card
				goto fail;
		}
		switch_to_m1();
	}
	if((i-1)==(blk_num-1)) {
		printf(".");
		//if (switch_to_m2() == 0)
		//    return 0;
		CardRead_dma(buf_dest_addr, blk_flag, (i-1));
		//   if ((word_read(SDSW_M1_STATUS) & 0x3f3f0000) != 0x20200000)
		//        switch_to_m1();
	}
//pass:
	switch_to_m1();
	return 0;
fail:
#ifdef show_str_en
	show_str("CardRead_single() Fail.\n",0,"\n",1);
#endif
	switch_to_m1();
	return 1;
}

int CardRead_dma(unsigned int buf_dest_addr, unsigned int blk_no, int blk_order)
{
	int DMA_RADDR_start, i;

	//start FIFO & DMA transfer
	DMA_RADDR_start = DMA_RADDR + (512 * (blk_order));
	word_write(SDR_DMA_DACH1_REG, DMA_RADDR_start);
	word_write(SDR_DMA_CTRCH1_REG,0x3F);
	word_write(SDR_DMA_CTRCH1_REG,0x33);
	word_write(SDR_BUF_TRAN_CTRL_REG, (BUF_BLK_CNT | BUF_XFER_START));

	if(WaitResp(FIFO_XFER_DONE, 0))		//last block of data moved to FIFO
		goto fail;
	WaitDMAIntr(DMA_CH1_INTR);			//last block of data moved to DMA

	//write all read data to destination buffer
	for (i = 0; i <= blk_order; i++)
		write_512_buffer(buf_dest_addr + (512*(i)), DMA_RADDR + (512*(i)));
	return 0;

fail:
	return 1;
}

void write_512_buffer(unsigned int dest_addr, unsigned int DMA_RADDR_start)
{
//	unsigned int data;
	unsigned int data_temp;
	int j;

	for(j=0; j<128; j++) {
		data_temp = word_read( DMA_RADDR_start + (j*4));
		/*
		            data  = ((data_temp & 0xff000000) >> 24)|
		                    ((data_temp & 0x00ff0000) >> 8 )|
		                    ((data_temp & 0x0000ff00) << 8 )|
		                    ((data_temp & 0x000000ff) << 24);
		*/
		word_write(dest_addr + (j*4), data_temp);
#ifdef show_str_en_v
		show_str("bufRd:\n",data_temp,"\n",1);
#endif
	}
}

int CardErase_single(unsigned int blk_no)
{
	unsigned int LBA0, LBA1, LBA2;

#ifdef show_str_en
	show_str("Enter CardErase_single().\n",blk_no,"\n",1);
#endif
	if (switch_to_m2() == 0)
		return 0;
	//Wait SD card turn to transfer state (0xc: wait SD card exist, DAT[0] free)
	WaitBusReady(0x0c);

	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	CRXferFinish = 0;

#ifdef simulation
	LBA0 = (( blk_no * 512) & 0x000000ff);
	LBA1 = (( blk_no * 512) & 0x0000ff00) >> 8;
	LBA2 = (( blk_no * 512) & 0x00ff0000) >> 16;
#else
	LBA0 = ( blk_no & 0x000000ff);
	LBA1 = ( blk_no & 0x0000ff00) >> 8;
	LBA2 = ( blk_no & 0x00ff0000) >> 16;
#endif

	word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));

	// CMD32 (ERASE WR BLOCK START ADDR - set the addr of 1st write block to be erased)
	// [31:0]data addr, R1
	if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x20, 0x38, 0x11))
		goto fail;

	// CMD33 (ERASE WR BLOCK END ADDR - set the addr of last write block to be erased)
	// [31:0]data addr, R1
	if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x21, 0x38, 0x11))
		goto fail;

	// CMD38 (ERASE - erase all selected write blocks)
	// [31:0]stuff bits, R1b
	if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x26, 0x38, 0x11))
		goto fail;

	//wait sd card transfer done
	if(WaitResp(SD_XFER_DONE, 0))
		goto fail;

	//clear transfer done and finish flags
	word_write(SDR_DMA_TRAN_RESP_REG, SD_XFER_DONE);

#ifdef show_str_en
	show_str("CardErase_single() Passed.\n",blk_no,"\n",1);
#endif
	switch_to_m1();
	return 0;
fail:
#ifdef show_str_en
	show_str("CardErase_single() Failed.\n",blk_no,"\n",1);
#endif
	switch_to_m1();
	return 1;
}

extern int switch_to_m2_sw(int flag);
int m2_err = 0;
int switch_to_m2(void)
{
	int time_out = 100;
	int i;
	int res;
	//printf("->M2\n");

	if (m1_err)
		return 0;
#ifdef SIYO_READER
	if(special_cont) {
		do {
			word_write(SDSW_M1_CTRL0, 0x0);
		} while((word_read(SDSW_M1_CTRL0) & 0x1) == 0x1);
		goto tom2;
	}
#endif

//#ifndef GEN3
	res = switch_to_m2_sw(0);
	if(!res)
		m1_m2 = 2;
	return !res; // !switch_to_m2_sw();
//#endif

#ifdef DEBUG
	//show_str("Switch to M2\n",0,"\n",1);
#endif
	if ((SDSW_M2_CTRL0) == 1 && word_read(SDSW_M1_CTRL0) == 0) //if (m1_m2 == 2)
		return 1;
	for (i = 0; i < 10; i++) {
		if ((word_read(SDSW_M1_STATUS) & 0x3f3f0000) == 0x20200000)
			break;
	}
	if ((word_read(SDSW_M1_STATUS) & 0x3f3f0000) != 0x20200000)
		return 0;

	while ((word_read(SDSW_M1_CTRL0) & 0x1) == 0x1) { 	//deselect m1
		if ((word_read(SDSW_M1_STATUS) & 0x3f3f0000) != 0x20200000)
			return 0;
		if (time_out-- <= 0) {
			m2_err++;
			//printf("switch to m2 timeout\n");
			switch_to_m1();
			return 0;
		}

		word_write(SDSW_M1_CTRL0, 0x0);
	}
//tom2:
	word_write(SDSW_M2_CTRL0, 0x1); 			//select m2
	m1_m2 = 2;
	return 1;
}

extern void switch_to_m1_sw(void);

int do_switch_to_m1(void)
{
	volatile int time_out = 100000;
	//printf("->M1\n");
	switch_to_m1_sw();

#ifdef DEBUG
	//show_str("Switch to M1\n",0,"\n",1);
#endif

	while ((word_read(SDSW_M2_CTRL0) & 0x1) == 0x1) { 	//deselect m2
		if (time_out-- <= 0) {
			printf("switch to m1 timeout\n");
			break;
		}
		word_write(SDSW_M2_CTRL0, 0x0);
	}

	word_write(SDSW_M1_CTRL0, 0x1); 			//select m1

	time_out = 1000;
	while ((word_read(SDSW_M2_CTRL0) & 0x1) == 0x1) { 	//deselect m2
		if (time_out-- <= 0) {
			printf("switch to m1 timeout\n");
			break;
		}
		word_write(SDSW_M2_CTRL0, 0x0);
	}

	word_write(SDSW_M1_CTRL0, 0x1); 			//select m1
	null_delay(100);

	m1_m2 = 1;
	return 1;
}

int switch_to_m1(void)
{
	volatile int time_out = 100000;
	//printf("->M1\n");
	if (m1_err)
		return 0;
//#ifndef GEN3
	switch_to_m1_sw();
	m1_m2 = 1;
	return 1;
//#endif
#ifdef DEBUG
	//show_str("Switch to M1\n",0,"\n",1);
#endif
	if ((SDSW_M2_CTRL0) == 0 && word_read(SDSW_M1_CTRL0) == 1) //if (m1_m2 == 1)
		return 1;

	while ((word_read(SDSW_M2_CTRL0) & 0x1) == 0x1) { 	//deselect m2
		if (time_out-- <= 0) {
			printf("switch to m1 timeout\n");
			break;
		}
		word_write(SDSW_M2_CTRL0, 0x0);
	}

	word_write(SDSW_M1_CTRL0, 0x1); 			//select m1

	time_out = 1000;
	while ((word_read(SDSW_M2_CTRL0) & 0x1) == 0x1) { 	//deselect m2
		if (time_out-- <= 0) {
			printf("switch to m1 timeout\n");
			break;
		}
		word_write(SDSW_M2_CTRL0, 0x0);
	}

	word_write(SDSW_M1_CTRL0, 0x1); 			//select m1
	null_delay(100);

	m1_m2 = 1;
	return 1;
}

#if M2_INIT_CARD
//#define DEBUG_SD_CMD
#ifdef DEBUG_SD_CMD
    #define DBG_CMD printf
#else
    #define DBG_CMD(...); {}
#endif
#define swap_data(a)    (a = (((a & 0xff) << 24) + ((a & 0xff00) << 8) + ((a & 0xff0000) >> 8) + ((a & 0xff000000) >> 24)))


u32 csd[4];


#define at_m1_mode() ((word_read(SDSW_M1_CTRL0) & 0x1) == 0x1)
#define at_m2_mode() ((word_read(SDSW_M2_CTRL0) & 0x1) == 0x1)

#define set_m1_mode() word_write(SDSW_M1_CTRL0, 0x1)
#define set_m2_mode() word_write(SDSW_M2_CTRL0, 0x1)
#define force_set_m1_mode() word_write(SDSW_M1_CTRL0, 0x5)
#define force_unset_m1_mode() word_write(SDSW_M1_CTRL0, 0x8)

#define unset_m1_mode() word_write(SDSW_M1_CTRL0, 0x0)
#define unset_m2_mode() word_write(SDSW_M2_CTRL0, 0x0)


//------------------------------------------------------------------------------
int command_9_get_csd()
{
	printf("\n\ncommand_9_get_csd()\n");

    word_write(SDR_Card_BLOCK_SET_REG, 1 | (16 << 16));          //1 block, 4 bytes data transfer
    word_write(SDR_Error_Enable_REG, 0x6f);                  //enable all errors
	////==============================================================================================//
    // CMD7
    if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 7, 0x38, 0x11))
         ;//return -1;

    if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 9, 0x79, 0x11))  // command 9
         ;//return -1;

    csd[0] = word_read(SDR_RESPONSE4_REG);
    csd[1] = word_read(SDR_RESPONSE3_REG);
    csd[2] = word_read(SDR_RESPONSE2_REG);
    csd[3] = word_read(SDR_RESPONSE1_REG);

    printf("csd at r4-r1 %08x %08x %08x %08x\n", word_read(SDR_RESPONSE4_REG), word_read(SDR_RESPONSE3_REG), word_read(SDR_RESPONSE2_REG), word_read(SDR_RESPONSE1_REG));
    swap_data(csd[0]);swap_data(csd[1]);swap_data(csd[2]);swap_data(csd[3]);

    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 7, 0x38, 0x11))
         return 1;
	return 0;
}

//------------------------------------------------------------------------------

int init_m2(void)
{
	volatile int timeout = 1000000;

    DBG_CMD("unset m1\n");
	do { unset_m1_mode(); } while(at_m1_mode());
	force_unset_m1_mode();
	DBG_CMD("set m2\n");
	do { set_m2_mode(); } while(!at_m2_mode());
#if 0
	word_write(0xa000a01c, 0x8);
    while ((word_read(0xa000a000) & 0x1) == 0x1)
    {
    	word_write(0xa000a01c, 0x8);
        word_write(0xa000a000, 0x0);

        if (timeout-- <= 0)
        {
        	printf("\nInit SD time out.\n");
        	return -1;
		}
    }

    word_write(0xa000a004, 0x1);
#endif
    printf("\nInit SD Host done\n");
    return 0;
}
//------------------------------------------------------------------------------
int InitCard(void)
{

    printf("M2 InitCard.\n");
     //word_write(0xa0000008, 0x101010); //temp set
    if (init_m2() == -1)
       ;// return 0;

    if (word_read(0xa000a00c) == 0x20200804)
    {
        //return 0;
    }
    word_write(SPECIAL_REG, 0x22);
    //wait for power up
    //null_delay(10);

	//dump_regs();
    //send_cmd60_61();
    DBG_CMD("Send CMD 0\n");
    // CMD0 (GO_IDLE_STATE - reset SD memory card)
    // [31:0]stuff bits, no resp
    if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x00, 0x30,0x10))
       goto fail;
    null_delay(100);
    RCA0 = 0;
    RCA1 = 0;
    DBG_CMD("Send CMD 8\n");
    // CMD8 (SEND_IF_COND - send SD memory card interface condition)
    // [31:12]reserved, [11:8]supply voltage, [7:0]check pattern, R7
    if(HCmdNoData(0x00, 0x00, 0x01, 0xaa, 0x08, 0x38, 0x10))
    {
//printf("HCmdNoData Command 8 error\n");
        if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 40, 0x30,0x10))
           goto fail;
		if(HCmdNoData(0x00, 0x00, 0x01, 0xaa, 0x08, 0x38, 0x10))
    	{
    	    DBG_CMD("Check Card is SD Ver 1\n");
        	// ACMD41 (SD_SEND_OP_COND - send host capacity support (HCS) info & request operating condition reg (OCR) content)
        	if(InCaIsSDVer(1,0))
            goto fail;
        }
	    else
	    {
	        DBG_CMD("Check Card is SD Ver 2\n");
	        // to check if card ver 2.0x -ACMD41
	        if(InCaIsSDVer(2, 0x40))
	            goto fail;
	    }
    }
    else
    {
printf("HCmdNoData Command 8 ok\n");
        DBG_CMD("Check Card is SD Ver 2\n");
        // to check if card ver 2.0x -ACMD41
        if(InCaIsSDVer(2, 0x40))
            goto fail;
    }
    null_delay(100);
    /*
opcode= 2, arg=0x00000000, cmdctl=0x034, flags=0x067
opcode= 3, arg=0x00000000, cmdctl=0x038, flags=0x075
*/
    DBG_CMD("Send CMD 2 - send CID\n");
    // CMD2 (ALL_SEND_CID - ask any connected card to send CID num on CMD line)
    // [31:0]stuff bits, R2
    if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x02, 0x34, 0x10))
        goto fail;

    printf("CID r4-r1 %08x %08x %08x %08x\n", word_read(SDR_RESPONSE4_REG), word_read(SDR_RESPONSE3_REG), word_read(SDR_RESPONSE2_REG), word_read(SDR_RESPONSE1_REG));


    DBG_CMD("Send CMD 3 - SEND_RELATIVE_ADDR\n");
    // CMD3 (SEND_RELATIVE_ADDR - ask card to publish new relative address (RCA))
    // [31:0]stuff bits, R6
    if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x03, 0x38, 0x10))
        goto fail;

    RCA0 = (word_read(SDR_RESPONSE1_REG) & 0x00ff0000) >> 16;
    RCA1 = (word_read(SDR_RESPONSE1_REG) & 0xff000000) >> 24;

    printf("RCA0 %X\n",RCA0);
    printf("RCA1 %X\n",RCA1);
    /*
opcode= 9, arg=0xc7b30000, cmdctl=0x034, flags=0x007
opcode= 7, arg=0xc7b30000, cmdctl=0x038, flags=0x015
*/
    DBG_CMD("Send CMD 9 get CSD\n");
    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x09, 0x34, 0x10))
        goto fail;
    else
        WaitBusReady(0x8c);
    csd[0] = word_read(SDR_RESPONSE4_REG);
    csd[1] = word_read(SDR_RESPONSE3_REG);
    csd[2] = word_read(SDR_RESPONSE2_REG);
    csd[3] = word_read(SDR_RESPONSE1_REG);

    printf("CSD r4-r1 %08x %08x %08x %08x\n", word_read(SDR_RESPONSE4_REG), word_read(SDR_RESPONSE3_REG), word_read(SDR_RESPONSE2_REG), word_read(SDR_RESPONSE1_REG));
    swap_data(csd[0]);swap_data(csd[1]);swap_data(csd[2]);swap_data(csd[3]);
    DBG_CMD("Send CMD 7 - SELECT/DESELECT_CARD \n");
    // CMD7 (SELECT/DESELECT_CARD - toggle card btw standby/disconnect & transfer/programming state)
    // [31:16]RCA, [15:0]stuff, R1b
    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x07, 0x38, 0x10))  //orginal ctrl is 0x3c
        goto fail;
    else
        WaitBusReady(0x8c);
/*
opcode=55, arg=0xc7b30000, cmdctl=0x038, flags=0x095
opcode=51, arg=0x00000000, cmdctl=0x038, flags=0x0b5
*/
    null_delay(100);
    word_write(SDR_RESPONSE1_REG, 0);
    word_write(SDR_RESPONSE2_REG, 0);
    word_write(SDR_RESPONSE3_REG, 0);
    word_write(SDR_RESPONSE4_REG, 0);
    DBG_CMD("Send CMD 55\n");
    // CMD55
    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x10))
		goto fail;

    DBG_CMD("Send ACMD 51 - SEND_SCR\n");
    // ACMD51
    // [31:2]stuff, [1:0]bus width (00=1bit, 10=4bits), R1
    if(HCmdNoData(0x00, 0x00, 0x00, 0x02, 51, 0x38, 0x10))
		goto fail;
    printf("SCR r2-r1 %08x %08x\n", word_read(SDR_RESPONSE2_REG), word_read(SDR_RESPONSE1_REG));

/*opcode= 6, arg=0x00fffff1, cmdctl=0x038, flags=0x0b5		*/
    if(HCmdNoData(0x00, 0xff, 0xff, 0xf1, 0x06, 0x38, 0x10))
		goto fail;
/*
opcode=55, arg=0xc7b30000, cmdctl=0x038, flags=0x095
opcode= 6, arg=0x00000002, cmdctl=0x038, flags=0x015
*/
    DBG_CMD("Send CMD 55\n");
    // CMD55
    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x10))
		goto fail;

    printf("Send ACMD 6\n");
    // ACMD6 (SET_BUS_WIDTH - define data bus width for data transfer)
    // [31:2]stuff, [1:0]bus width (00=1bit, 10=4bits), R1
    if(HCmdNoData(0x00, 0x00, 0x00, 0x02, 0x06, 0x38, 0x10))
		goto fail;
    //test();
    DBG("InitCard() Passed.\n");

    //command_55_get_scr();
    //test_cmd27();
#if 1
    //test_user_define_cmd(60);
    //test_user_define_cmd(61);

    //test_multi_cmd55();
    //test_acmd7();
#endif
    return 0;
fail:
    printf("InitCard() Failed.\n");
#if 0
    //for (i = 0; i < 100000; i++)
    {
        if (ReInitCard() == 0)
            return 0;
    }
#endif
    return 1;
}
#else
int InitCard(void)
{
//	int j;
	int waitCnt=5;
//	unsigned int reg, lastRdArg=0;

#ifdef show_str_en
	show_str("Enter InitCard().\n",0,"\n",1);
#endif
	word_write(SDSW_M1_CMD_FLG_REG0, 0xffffffff);     //clear all cmd flags
	word_write(SDSW_M1_CMD_FLG_REG1, 0xffffffff);
	word_write(SDSW_M1_CMD_FLG_REG2, 0xffffffff);
	word_write(SDSW_M1_CMD_FLG_REG3, 0xffffffff);
	word_write(0xa000a01c, 0x2);
#if CONFIG_SYS_VER > 1 //#ifdef CONFIG_V34
	word_write(SPECIAL_REG, 0x22);
#else
	word_write(SPECIAL_REG, 0x2);
#endif
	word_write(SDSW_M1_CTRL0, 0x1);
	word_write(SDSW_M2_CTRL0, 0x0);
	debug("(m1:%x, m2:%x, sw:%x@%x:%x, bw:%x)\n", word_read(0xa000a008), word_read(0xa000a00c), word_read(0xa0000014), word_read(0xa000a000), word_read(0xa000a004), word_read(SDSW_M1_SBW_REG));
//wait_again:
	if(wait_m1_ready(500000)) {        //eee
		//printf("(%d)m1 ready", waitCnt);
		word_write(SDSW_M1_CSR_REG, word_read(SDSW_M1_CSR_REG) | 0x100);        //set Ready For Data bit
	} else
		printf("(%d)m1 not ready", waitCnt);
#ifdef SIYO_READER
	printf("(m1:%x, m2:%x, sw:%x@%x:%x, bw:%x)\n", word_read(0xa000a008), word_read(0xa000a00c), word_read(0xa0000014), word_read(0xa000a000), word_read(0xa000a004), word_read(SDSW_M1_SBW_REG));
	for(reg = 0xa000a000; reg<=0xa000a12c; reg+=4)
		printf("[%08x]%08x\n", reg, word_read(reg));
#endif

	/*
	    RCA0 = word_read(0xa000a09c) & 0xff;
	    RCA1 = (word_read(0xa000a09c) & 0xff00) >> 8;
	    printf("RCA0 %x, RCA1 %x\n", RCA0, RCA1);
	    word_write(0xa000a01c, 0x2);
	#ifdef GEN3
	    word_write(SPECIAL_REG, 0x22);
	#else
	    word_write(SPECIAL_REG, 0x2);
	#endif*/

	printf("Status %x\n", word_read(SDSW_M1_STATUS));
#ifdef SIYO_READER
	if((word_read(SDSW_M1_STATUS) != 0x20200804) && --waitCnt) {
		printf("curr cmd:%x %x, prev cmd: %x\n", word_read(0xa000a040), word_read(0xa000a044), word_read(0xa000a030));
		if(word_read(SDSW_M1_STATUS) == 0x20100815) {
			if(waitCnt == 4) lastRdArg = word_read(0xa000a044);
			else if(word_read(0xa000a044) == lastRdArg) {
				printf("conditional continue\n");
				m1_err = 0;
				special_cont = 1;
				goto cond_cont;
			}
		}
		goto wait_again;
	} else if(!waitCnt) {
		printf("m1 still busy, m2 abort!\n");
		goto fail;
	}
#endif

	if (m1_err)
		goto fail;
	// if (m1_busy())
	//     return;
//cond_cont:
	if (word_read(SDSW_M1_STATUS) != 0x20200800) {
		/* m1 idle ready */
		RCA0 = word_read(0xa000a09c) & 0xff;
		RCA1 = (word_read(0xa000a09c) & 0xff00) >> 8;
		printf("RCA0 %x, RCA1 %x\n", RCA0, RCA1);
		word_write(0xa000a01c, 0x2);
#if CONFIG_SYS_VER > 1 //#ifdef CONFIG_V34
		word_write(SPECIAL_REG, 0x22);
#else
		word_write(SPECIAL_REG, 0x2);
#endif
		goto pass;
	}
	//switch_to_m1();
	//WaitBusReady(0x8c);
	/*
	    switch_to_m2();
	    for(j=0; j<100000; j++);
	    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x07, 0x3C, 0x11)) {
		RCA0 = (word_read(0xa000a09c) & 0xff00) >> 8;
		RCA1 = word_read(0xa000a09c) & 0xff;
		if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x07, 0x3C, 0x11))
	     		goto fail;
	    }
	    else
	        WaitBusReady(0x8c);
	    if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x0D, 0x38, 0x11))
		goto fail;
	    switch_to_m1();
	*/
#if M2_INIT_CARD //1
	switch_to_m2();

	//wait for power up
	int j;
#ifdef simulation
	for(j=0; j<10; j++);
#else
	for(j=0; j<100000; j++);
#endif

	// CMD0 (GO_IDLE_STATE - reset SD memory card)
	// [31:0]stuff bits, no resp
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x00, 0x30,0x11))
		goto fail;

	RCA0 = 0;
	RCA1 = 0;

	// CMD8 (SEND_IF_COND - send SD memory card interface condition)
	// [31:12]reserved, [11:8]supply voltage, [7:0]check pattern, R7
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	if(HCmdNoData(0x00, 0x00, 0x01, 0xaa, 0x08, 0x38, 0x11)) {
		// ACMD41 (SD_SEND_OP_COND - send host capacity support (HCS) info & request operating condition reg (OCR) content)
		if(InCaIsSDVer(1,0))
			goto fail;
	} else {
		// to check if card ver 2.0x -ACMD41
		if(InCaIsSDVer(2, 0x40))
			goto fail;
	}

	// CMD2 (ALL_SEND_CID - ask any connected card to send CID num on CMD line)
	// [31:0]stuff bits, R2
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x02, 0x34, 0x11))
		goto fail;

	// CMD3 (SEND_RELATIVE_ADDR - ask card to publish new relative address (RCA))
	// [31:0]stuff bits, R6
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x03, 0x38, 0x11))
		goto fail;

	RCA0 = (word_read(SDR_RESPONSE1_REG) & 0x00ff0000) >> 16;
	RCA1 = (word_read(SDR_RESPONSE1_REG) & 0xff000000) >> 24;

#ifdef show_str_en
	show_str("RCA0 \n",RCA0,"\n",1);
	show_str("RCA1 \n",RCA1,"\n",1);
#endif
	word_write(0xa000a09c, RCA0 | (RCA1 << 8));

	// CMD7 (SELECT/DESELECT_CARD - toggle card btw standby/disconnect & transfer/programming state)
	// [31:16]RCA, [15:0]stuff, R1b
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x07, 0x3C, 0x11))
		goto fail;
	else
		WaitBusReady(0x8c);

#ifdef simulation
#else
	// CMD55
	if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x11))
		goto fail;

	// ACMD6 (SET_BUS_WIDTH - define data bus width for data transfer)
	// [31:2]stuff, [1:0]bus width (00=1bit, 10=4bits), R1
	if(HCmdNoData(0x00, 0x00, 0x00, 0x02, 0x06, 0x38, 0x11))
		goto fail;
#endif

#endif

pass:
#ifdef show_str_en
	show_str("InitCard() Passed.\n",0,"\n",1);
#endif
	return 0;
fail:
#ifdef show_str_en
	show_str("InitCard() Failed.\n",0,"\n",1);
#endif
	return 1;
}
#endif
//------------------------------------------------------------------------------
int WaitResp(unsigned int expResp, int delay)
{
//	volatile unsigned int i;
	volatile unsigned int result = 0, res = 0;
	volatile int time_out = 1000+delay*50;

	DBG("Enter WaitResp from %d to %d\n", word_read(SDR_DMA_TRAN_RESP_REG), expResp);

	while( (word_read(SDR_DMA_TRAN_RESP_REG) & expResp) != expResp ) {
		if (time_out-- <= 0) {
			printf("Time out - WaitResp %x got %x (%d)\n", expResp, word_read(SDR_DMA_TRAN_RESP_REG), delay);
			res = 1;
			break;
		}
		null_delay(2);
	}

//printf("DMA_TRAN_RESP_REG %x\n",word_read(SDR_DMA_TRAN_RESP_REG));

	word_write(SDR_DMA_TRAN_RESP_REG, expResp);

	result = word_read(SDR_STATUS_REG);
	/* check if CRC error */
	if (result & 0x200000) {
		printf("\n!!!! CRC Error\n");
	}

	/* check other error */
	if( (result & 0x4f0000)) { // skip crc error 0x200000
		printf("WaitResp() error status:%x\n", result);
		res = 1;
	} /*else
		res = 0;*/

	return res;
}
//------------------------------------------------------------------------------
void WaitDMAIntr(unsigned int channel)
{
	volatile int time_out = 1000;
	DBG("wait dma channel %d\n", channel);
	while((word_read(SDR_DMA_INTS_REG) & channel) == 0) { //wait DMA transfer done
		if (time_out-- <= 0) {
			printf("Wait dma %d got %x time out\n", channel, word_read(SDR_DMA_INTS_REG));
			break;
		}
	}
	DBG("SDR_DMA_INTS_REG %x\n", word_read(SDR_DMA_INTS_REG));
	//word_write(SDR_DMA_INTS_REG, channel);                 	//reset DMA interrupt pin
	word_write(SDR_DMA_INTS_REG, word_read(SDR_DMA_INTS_REG));
}
//------------------------------------------------------------------------------
int WaitCardReady()
{
	// CMD13 (SEND_STATUS - card sends its status register)
	// [31:16]RCA, [15:0]stuff bits, R1
	if (HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x0D, 0x38, 0x11))
		goto fail;
	else
		return ~( (word_read(SDR_RESPONSE1_REG) & 0x00000001));
fail:
	printf("WaitCardReady() Failed.\n");
	return 1;
}
//------------------------------------------------------------------------------
int WaitBusReady(unsigned int expStatus)
{
	unsigned int CR_BUS_STS;
	volatile int time_out = 100000; //10000
	do {
		CR_BUS_STS = word_read(SDR_STATUS_REG);
		if (time_out-- <= 0) {
			printf("WaitBusReady timeout\n");
			break;
		}
	} while(  ((CR_BUS_STS >> 8) & expStatus ) != expStatus);

	//DBG("Exit WaitBusReady().\n");
	return(time_out == 0);
}
//------------------------------------------------------------------------------
int InCaIsSDVer(int Ver, int CCap)
{
//	int i;
#ifdef simulation
	for(i=0; i<10; i++);
#else
	//for(i=0; i<50000; i++);
#endif
	if (m1_err)
		return 0;
	if(Ver==1) {
#ifdef show_str_en
		show_str("InCaIsSDVer1x().\n", 0, "\n",1);
#endif
		HighCap=0;
		OCR1=0;
		OCR2=0;
#ifdef simulation
#else
		if(ACMD41tillReady(0x00) == 1)
			goto fail;
#endif
	} else {
		/* to check if card ver 2.0x -ACMD41*/
		/* CMD55 */
#ifdef show_str_en
		show_str("InCaIsSDVer2x().\n", 0, "\n",1);
#endif
		if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x10))
			goto fail;

		/* CMD41 */
#ifdef show_str_en
		show_str("Send ACMD41.\n",0,"\n",1);
#endif

#ifdef simulation
		if(HCmdNoData(0x80, 0xff, 0x80, 0x00, 0x29, 0x38, 0x10))
			goto fail;
#else
		if(HCmdNoData(CCap, 0xff, 0x80, 0x00, 0x29, 0x38, 0x10))
			goto fail;
#endif

		OCR1 = (word_read(SDR_RESPONSE1_REG) & 0x0000ff00);
		OCR2 = (word_read(SDR_RESPONSE1_REG) & 0x00ff0000) >> 16;

#ifdef show_str_en
		show_str("OCR1 \n",OCR1,"\n",1);
		show_str("OCR2 \n",OCR2,"\n",1);
#endif

#ifdef simulation
#else
		if(ACMD41tillReady(0x40) == 1)
			goto fail;
#endif
	}

//pass:
	return 0;
fail:
#ifdef show_str_en
	show_str("InCaIsSDVer() Fail.",0,"\n",1);
#endif
	return 1;
}

//------------------------------------------------------------------------------
int ACMD41tillReady(int CCap)
{
	int retry = 100;
	if (m1_err)
		return 0;
	while(1) {
		/* ACMD commands are preceded by CMD55 (APP_CMD) */
		// [31:16]RCA, [15:0]stuff bits, R1 (0x40 = HC)
		if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x10))
			goto fail;

		// ACMD41 (SD_SEND_OP_COND - send host capacity support (HCS) info & request operating condition reg (OCR) content)
		// [31]reserved, [30]HCS(OCR[30]), [29:24]reserved, [23:0]Vdd voltage window(OCR[23:0]), R3
		if(HCmdNoData(CCap, OCR2, OCR1, 0x00, 0x29, 0x38, 0x10)) {
			if (retry-- > 0)
				continue;
			else
				goto fail;
		}


		HighCap = word_read(SDR_RESPONSE1_REG);
#ifdef show_str_en
		show_str("HighCap \n",HighCap,"\n",1);
#endif
		if( (HighCap & 0x80000000) == 0x80000000)  //To check RSP3 7th bit == 1
			goto pass;
	}
pass:
	return 0;
fail:
#ifdef show_str_en
	show_str("ACMD41tillReady() Fail.",0,"\n",1);
#endif
	return 1;
}

int HCmdNoData(unsigned int CMD_ARG3,unsigned int CMD_ARG2, unsigned int CMD_ARG1, unsigned int CMD_ARG0, unsigned int CMD, unsigned int CTRL, unsigned int reader_set)
{
	unsigned int flag=0;
	unsigned int CMD_ARG;

	CRCardErr = 0;
	CRCmdDone = 0;
	CRXferDone = 0;
	CRXferBDone = 0;
	CRXferFinish = 0;

	//if (m1_busy())
	//    goto  fail;
	if (m1_err)
		return 0;
	word_write(SDR_DMA_TRAN_RESP_REG, word_read(SDR_DMA_TRAN_RESP_REG));

	//cmd arguments
	CMD_ARG = (CMD_ARG3 << 24) |  (CMD_ARG2 << 16) | (CMD_ARG1 << 8) | CMD_ARG0;
	word_write(SDR_CMD_ARGUMENT_REG, CMD_ARG);

	// when write CTRL_REG, SD will ready to send out cmd
	flag = flag | (CTRL << 8) | (CMD << 16) | reader_set | EN_INTR;   //bit 0-0x01: card bus width : 4 bit;

//printf("HCmdNoData flag 0x%x\n", flag);
	word_write(SDR_CTRL_REG, flag);
	//printf("cmd%d:%x f:%x\n", CMD, CMD_ARG, flag);

#ifdef show_str_en
	show_str("Sent CMD, wait Done: \n", CMD,"\n",1);
#endif

	//Polling
	if(WaitResp(SD_CMD_DONE, 30)) {
		if (CMD == 8) {
			word_write(SDSW_SW_CTRL0, 1<<2);
			printf("a000a00c %08x\n", word_read(0xa000a00c));
			goto fail;
		} else
			goto fail;
	} else if((CTRL & 0xC) == 0)		//bit[11:10], 00=no response
		goto pass;

	//Read response if command is expecting response
#ifdef show_str_en
	RSP1 = word_read(SDR_RESPONSE1_REG);
	show_str("RESP1 \n", RSP1, "\n",1);
#endif

//ISR
//    while(CRCmdDone == 0){
//          if( (CRCardErr & 0x01) == 0x01)
//             {
//                  //show_str("CRCardErr.\n",0,"\n",1);
//                  goto fail;
//             }
//    }

pass:
#ifdef show_str_en
	show_str("Send CMD Passed! CMD: \n",CMD,"\n",1);
#endif
	if((CMD==6) && (CMD_ARG==2))
		word_write(SPECIAL_REG, 0x22);
	return 0;
fail:
#ifdef show_str_en
	show_str("Send CMD Failed! CMD: \n",CMD,"\n",1);
	RSP1 =  word_read(SDR_STATUS_REG);
	show_str("Status: \n",RSP1,"\n",1);
#endif
	return 1;
}

#if 0
void show_str(char *start,unsigned int input,char *end,int end_len)
{
#if 0

	int i;
	unsigned int temp, flag;

	for(i=0; ; i++) {
		if(start[i]=='\n')
			break;
		sim_uart_tx(start[i]);
	}

	for(i=7; i>=0; i--) {
		temp=0xf0000000 >> ((7-i)*4);
		flag = (input & temp) >> (4*i);

		if(flag > 0x09)
			sim_uart_tx( (0x37 + flag));
		else
			sim_uart_tx(0x30 + flag);
	}

	for(i=0; i<end_len; i++)
		sim_uart_tx(end[i]);
#endif
}
#endif

void sim_uart_tx(char out)
{
	/* Send message */
	while((byte_read(UART_LINE) & 0x20) != 0x20) {}; //check LSR bit5
	byte_write(UART_RECV ,out);
}

int CardWrite_cmd(unsigned int blk_no, int blk_order)
{
	unsigned int LBA0, LBA1, LBA2;
	if (m1_err)
		return 0;
#ifdef show_str_en
	show_str("Enter CardWrite_single().\n",0,"\n",1);
#endif

	//CMD24 (WRITE_BLOCK)
#ifdef show_str_en
	show_str("Send Write Block CMD.\n",0,"\n",1);
#endif
	CRXferDone = 0;
	CRCardErr = 0;
	CRXferBDone = 0;
	CRXferFinish = 0;

	if (m1_busy())
		goto fail;

	word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));
	//HCmdData(0x01,0x00, 0x01, 0x01);  //blk_size, blk_Len_LowB, blk_Len_HighB,blk_num(1),sector;

#ifdef simulation
	LBA0 = (( blk_no * 512) & 0x000000ff);
	LBA1 = (( blk_no * 512) & 0x0000ff00) >> 8;
	LBA2 = (( blk_no * 512) & 0x00ff0000) >> 16;
#else
	LBA0 = (( blk_no) & 0x000000ff);
	LBA1 = (( blk_no) & 0x0000ff00) >> 8;
	LBA2 = (( blk_no) & 0x00ff0000) >> 16;
#endif

	// CMD24 (WRITE BLOCK - write the (blk_flag-1)th blk)
	// [31:0]data addr, R1
	if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x18, 0x78,0x11))
		goto fail;

	//wait sd card transfer done
	if(WaitResp(SD_XFER_DONE, 0))
		goto fail;

#ifdef show_str_en
	show_str("WRITE data to BLK \n", blk_order,"\n",1);
#endif

//pass:
	return 0;
fail:
#ifdef show_str_en
	show_str("CardWrite_single() Fail.\n",0,"\n",1);
#endif
	return 1;
}
int CardWrite_single_pin(unsigned int blk_no, int blk_num, unsigned char *buffer)
{
	unsigned int i;
//	unsigned int j, k, data;
//	unsigned char m;
	int blk_flag=0;
//	int blkWritten = 0;
	unsigned int LBA0, LBA1, LBA2,DMA_ADDR_start;

	if (m1_err)
		return 0;
	if (switch_to_m2() == 0)
		goto fail;
#ifdef show_str_en
	show_str("Enter CardWrite().\n",0,"\n",1);
#endif
	memcpy((void *)DMA_WADDR, buffer, 512 * blk_num);

	for(i = 0; i<blk_num ; i++) {
		//Wait SD card turn to transfer state (0xc: wait SD card exist, DAT[0] free)
		WaitBusReady(0x0c);
		blk_flag = blk_no + i;

		//Transfer data to FIFO
		DMA_ADDR_start = DMA_WADDR + (512 * i);
		word_write(SDR_DMA_SACH0_REG, DMA_ADDR_start);		//set DMA Ch0 source addr
		//for(k=0, m=0; (k<(512*BLK_CNT)); k+=4, m=k) 			//write ascending numbers to memory
		//{
		//     data = ((m+3) << 24) | ((m+2) << 16) | ((m+1) << 8) | m;
		//     word_write( DMA_ADDR_start+k, data);
		//}
		word_write(SDR_DMA_CTRCH0_REG,0x3F); 			//set DMA Ch0 control to start DMA
		word_write(SDR_DMA_CTRCH0_REG,0x33); 			//reset DMA Ch0 control
		word_write(SDR_BUF_TRAN_CTRL_REG, (BUF_BLK_CNT | BUF_WRITE | BUF_XFER_START));  //set FIFO control & start transfer
		WaitDMAIntr(DMA_CH0_INTR);					//wait for data move to memory

#ifdef SINGLE_BLOCK
		if(i>0) {
#else
		if(WaitResp(FIFO_XFER_DONE, 0))  			//wait for data move to FIFO
			goto fail;
		blk_flag = blk_no + 1;
#endif
			//Transfer data to SD card
			CRXferDone = 0;
			CRCardErr = 0;
			CRXferBDone = 0;
			CRXferFinish = 0;
#ifdef simulation
			LBA0 = (( (blk_flag-1) * 512) & 0x000000ff);
			LBA1 = (( (blk_flag-1) * 512) & 0x0000ff00) >> 8;
			LBA2 = (( (blk_flag-1) * 512) & 0x00ff0000) >> 16;
#else
			LBA0 = (( blk_flag-1) & 0x000000ff);
			LBA1 = (( blk_flag-1) & 0x0000ff00) >> 8;
			LBA2 = (( blk_flag-1) & 0x00ff0000) >> 16;
#endif
			word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|BLK_CNT));
#ifdef show_str_en
			show_str("Send Write Block CMD. Blk:\n",blk_flag,"\n",1);
#endif

#ifdef SINGLE_BLOCK
			// CMD24 (WRITE BLOCK - write the (blk_flag-1)th blk)
			// [31:0]data addr, R1
			if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x18, 0x78,0x11))
				goto fail;
#else
			// CMD25 (WRITE MULTIPLE BLOCK - write the (blk_flag-1)th blk until interrupted by CMD12)
			// [31:0]data addr, R1
			if(HCmdNoData(0x00, LBA2, LBA1, LBA0, 0x19, 0x7A,0x11))
				goto fail;
#endif
			//wait sd card transfer done and fifo transfer finish
#ifdef SINGLE_BLOCK
			if(WaitResp(FIFO_XFER_DONE|SD_XFER_DONE, 0))		//wait data write to FIFO & SD card done
				goto fail;
#else
			if(WaitResp(FIFO_XFER_DONE, 0))   			//(eee:still needed?)
				goto fail;
			for(i=0; (i<(BLK_CNT-1)); i++) {
				if(WaitResp(SD_DATA_BOUND, 2000))			//current block write to SD card
					goto fail;
				word_write(SDR_ADDRESS_REG, 0x1);			//continue write next block
			}
			if(WaitResp(SD_XFER_DONE, 0))				//all blocks transferred to SD card
				goto fail;
			if(WaitResp(SD_CMD_DONE, 0))				//for CMD12 (STOP_TRANSMISSION)
				goto fail;
			WaitBusReady(0x0c);					//wait SD card exist and not busy

			// confirm write was successful
			// CMD55
			if(HCmdNoData(RCA1, RCA0, 0x00, 0x00, 0x37, 0x38, 0x11))
				goto fail;

			// ACMD22 (SEND_NUM_WR_BLOCKS - send num of written (no error) blocks. Responds with 32bit+CRC data blocks)
			// [31:0]stuff bits
			word_write(SDR_Card_BLOCK_SET_REG, 0x00040001);    	 //1 block, 4 bytes data transfer
			word_write(SDR_Error_Enable_REG, 0x6f);                  //enable all errors
			if(HCmdNoData(0x00, 0x00, 0x00, 0x00, 0x16, 0x79, 0x11))
				goto fail;
			if(WaitResp(SD_XFER_DONE, 0))				 //wait SD data transfer done
				goto fail;

			//transfer data to FIFO
			word_write(SDR_DMA_TCCH1_REG, 0x4);
			word_write(SDR_DMA_DACH1_REG, DMA_RADDR);
			word_write(SDR_DMA_CTRCH1_REG, 0x3F);
			word_write(SDR_DMA_CTRCH1_REG, 0x33);
			word_write(SDR_BUF_TRAN_CTRL_REG, 0x00010000 | BUF_XFER_START);
			if(WaitResp(FIFO_XFER_DONE, 0))				//data moved to FIFO
				goto fail;

			//read returned data & compare with expected block count
			data = word_read(DMA_RADDR);
			blkWritten = ((data & 0xff000000)>>24) | ((data & 0xff0000)>>8) | ((data & 0xff00)<<8) | ((data & 0xff)<<24);
			if(blkWritten != BLK_CNT) {
#ifdef show_str_en
				show_str("Block written NOT as expected.\n", blkWritten,"\n",1);
#endif
				goto fail;
			} else
#ifdef show_str_en
				show_str("Block Written: \n", blkWritten,"\n",1);
#endif

#ifdef simulation
			// CMD7 (SELECT/DESELECT_CARD - toggle card btw standby/disconnect & transfer/programming state)
			// [31:16]RCA, [15:0]stuff, R1b
			CRXferDone = 0;
			CRCardErr = 0;
			CRXferBDone = 0;
			if(HCmdNoData(0x12, 0x34, 0x00, 0x00, 0x07, 0x3C, 0x11))
				goto fail;     		//else WaitBusReady();
#endif
			goto pass;
#endif

#ifdef SINGLE_BLOCK
		} else if(i==0) { //Wait for DMA to complete block 0 transfer
#ifdef show_str_en
			show_str("Write data to BLK \n", i,"\n",1);
#endif
			if(WaitResp(FIFO_XFER_DONE, 0))		//wait for data move to FIFO
				goto fail;
		}
#endif
	}

#ifdef SINGLE_BLOCK
	if( (i-1) == (blk_num-1))
	{
		//clear flags & write last block to SD
		word_write(SDR_DMA_TRAN_RESP_REG, (FIFO_XFER_DONE|SD_XFER_DONE));
		CardWrite_cmd(blk_flag, (i-1));
	}
#endif

//pass:
	//clear transfer done and finish flags
	word_write(SDR_DMA_TRAN_RESP_REG,(FIFO_XFER_DONE|SD_XFER_DONE));
#ifdef show_str_en
	show_str("CardWrite() Pass.\n",0,"\n",1);
#endif
	switch_to_m1();

	return 0;
fail:
#ifdef show_str_en
	show_str("CardWrite() Fail.\n",0,"\n",1);
#endif
	switch_to_m1();
	return 1;
}


int SendWriteCmd(unsigned int cmdarg, int single)
{
	DBG("Enter SendWriteCmd(). BLK%d\n", cmdarg);
	if (m1_err)
		return 0;
	//Transfer data to SD card
	if(single) {
		// CMD24 (WRITE BLOCK - write the (cur_blk-1)th blk)
		// [31:0]data addr, R1
		if(HCmdNoData(((cmdarg>>24)&0xff), ((cmdarg>>16)&0xff), ((cmdarg>>8)&0xff), (cmdarg&0xff), 0x18, 0x78, 0x11))
			goto fail;
	} else { //Multi Block
		// CMD25 (WRITE MULTIPLE BLOCK - write all blocks until interrupted by CMD12)
		// [31:0]data addr, R1
		//if(HCmdNoData(cmdarg, 0x19, 0x7A, 0x11))
		if(HCmdNoData(((cmdarg>>24)&0xff), ((cmdarg>>16)&0xff), ((cmdarg>>8)&0xff), (cmdarg&0xff), 0x19, 0x7A, 0x11))
			goto fail;
	}

	return 0;
fail:
	DBG("SendWriteCmd(Blk %d) Failed!\n", cmdarg);
	return 1;
}


int SDCardWrite(unsigned int buf_src_addr, unsigned int cmd_arg, int blk_num, int single)
{
	unsigned int i;
//	unsigned int data;
	int cur_blk=0;
//	int blkWritten=0;
	unsigned int DMA_ADDR_start;
//	unsigned int cmdarg=0;
	//unsigned int *pBuf=NULL;
	if (m1_err)
		return 0;
	//if (m1_busy())
	//    goto fail;

	//DBG("Write(s%d) %d blk from %x to %x\n", single, blk_num, buf_src_addr, cmd_arg);
	debug("w.%s %d %d\n", (single?"s":"m"), cmd_arg, blk_num);
	word_write(SDR_BUF_TRAN_CTRL_REG, 0);   //clear buffer transfer start bit

	if(!single) { /* Multi Block Write */
		if (switch_to_m2() == 0)
			goto fail;
		if(WaitBusReady(0x0c))	//0xc=SD card exist, DAT0 free
			goto fail;
		//Init FIFO
		word_write(SDR_DMA_TCCH0_REG, (512 * blk_num));  		//Transfer Count
		word_write(SDR_DMA_SACH0_REG, buf_src_addr);			//Start Addr, DMA_WADDR

		//Transfer write data to FIFO
		//for(i = 0; i < blk_num ; i++) read_512_buffer(buf_src_addr + (512 * i), DMA_WADDR + (512 * i));
		word_write(SDR_DMA_CTRCH0_REG,0x3F); 			//set DMA CH0 control to start DMA
		word_write(SDR_DMA_CTRCH0_REG,0x33); 			//reset DMA CH0 control
		word_write(SDR_BUF_TRAN_CTRL_REG, ((blk_num << 16) | BUF_WRITE | BUF_XFER_START));
		//WaitDMAIntr(DMA_CH0_INTR);					//wait for data move to memory
		if(WaitResp(FIFO_XFER_DONE, 0)) 				//Wait until data moved to FIFO
			goto fail;
		word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|blk_num));
		SendWriteCmd(cmd_arg, single);

		for(i=0; (i<(blk_num-1)); i++) {
			if(WaitResp(SD_DATA_BOUND, 20000))				//current block written to SD card
				goto fail;
			word_write(SDR_SPECIAL_CTRL_REG, 0x1);			//continue write next block
		}
		if(WaitResp(SD_XFER_DONE, 1000))				//all blocks transferred to SD card
			goto fail;
		if(WaitResp(SD_CMD_DONE, 1000))					//for CMD12 (STOP_TRANSMISSION)
			goto fail;
		WaitBusReady(0x0c);
		switch_to_m1();

#if 0
		//confirm write was successful with ACMD22
		cmdarg = (RCA1 << 24) | (RCA0 << 16) | 0x0;
		if(HCmdNoData(cmdarg, 0x37, 0x38, 0x11))	//CMD55
			goto fail;
		// ACMD22 (SEND_NUM_WR_BLOCKS - send num of written (no error) blocks. Responds with 32bit+CRC data blocks)
		// [31:0]stuff bits
		word_write(SDR_Card_BLOCK_SET_REG, 0x00040001);    	 	//1 block, 4 bytes data transfer
		word_write(SDR_Error_Enable_REG, 0x6f);                  	//enable all errors
		if(HCmdNoData(0x00, 0x16, 0x79, 0x11))
			goto fail;
		if(WaitResp(SD_XFER_DONE, 50))
			goto fail;
		pBuf = (unsigned int *)kmalloc(128, GFP_KERNEL);
		word_write(SDR_DMA_TCCH1_REG, 0x4);
		word_write(SDR_DMA_DACH1_REG, (unsigned int)pBuf);	//DMA_RADDR
		word_write(SDR_DMA_CTRCH1_REG, 0x3F);
		word_write(SDR_DMA_CTRCH1_REG, 0x33);
		word_write(SDR_BUF_TRAN_CTRL_REG, 0x00010000 | BUF_XFER_START);
		if(WaitResp(FIFO_XFER_DONE, 0))
			goto fail;
		data = *pBuf;
		blkWritten = ((data & 0xff000000)>>24) | ((data & 0xff0000)>>8) | ((data & 0xff00)<<8) | ((data & 0xff)<<24);
		if(blkWritten != blk_num) {
			DBG("Block written NOT as expected. Exp: %d, Actual: %d.\n", blk_num, blkWritten);
			goto fail;
		}
#endif
		goto pass;
	}

	/* Single block Write */
	for(i = 0; i < blk_num ; i++) {
		if (switch_to_m2() == 0)
			goto fail;
		if(WaitBusReady(0x0c))					//0xC=SD card exist, DAT0 free
			goto fail;
		//Init FIFO
		cur_blk = cmd_arg + i; //1124-2:16a
		word_write(SDR_DMA_TCCH0_REG, 512);  			//Transfer Count
		DMA_ADDR_start = buf_src_addr + (512 * i);		//DMA_WADDR
		word_write(SDR_DMA_SACH0_REG, DMA_ADDR_start);		//Start Addr

		//Transfer write data to FIFO
		//read_512_buffer(buf_src_addr + (512 * i), DMA_ADDR_start);
		//word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|0x1));
		word_write(SDR_DMA_CTRCH0_REG,0x3F); 			//set DMA CH0 control to start DMA
		word_write(SDR_DMA_CTRCH0_REG,0x33); 			//reset DMA CH0 control
		word_write(SDR_BUF_TRAN_CTRL_REG, ((0x1 << 16) | BUF_WRITE | BUF_XFER_START));
		null_delay(10);

		WaitDMAIntr(DMA_CH0_INTR);					//wait for data move to memory
		word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|0x1));
		if(i > 0) {
			SendWriteCmd(cur_blk, 1);
			if(WaitResp(FIFO_XFER_DONE|SD_XFER_DONE, 1000)) {
				DBG("wait fifo_xfer_done and sd_xfer_done error \n");
				goto fail;
			}
		} else if(i==0) {
			if(WaitResp(FIFO_XFER_DONE, 1000)) {			//Wait for DMA to complete block 0 transfer
				DBG("wait FIFO_XFER_DONE error\n");
				goto fail;
			}
		}
		switch_to_m1();

	} //single block for loop

	//Transfer last/only block of write data to SD Card
	if(i == blk_num) {
		if (switch_to_m2() == 0)
			goto fail;
		word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|0x1));
		SendWriteCmd(cur_blk, 1);
		if(WaitResp(SD_XFER_DONE, 3000)) {
			DBG("end blk write error \n");
			goto fail;
		}
		switch_to_m1();
	}

pass:
	word_write(SDR_DMA_TRAN_RESP_REG,(FIFO_XFER_DONE|SD_XFER_DONE));
	word_write(SDR_DMA_INTS_REG, word_read(SDR_DMA_INTS_REG));
	DBG("SDCardWrite() Passed.\n");
	return 0;
fail:
	DBG("SDCardWrite() Failed!\n");
	DBG("Write(s%d) %d blk from %x to %x\n", single, blk_num, buf_src_addr, cmd_arg);
	word_write(SDR_DMA_TRAN_RESP_REG,(FIFO_XFER_DONE|SD_XFER_DONE));
	word_write(SDR_DMA_INTS_REG, word_read(SDR_DMA_INTS_REG));
	return 1;
}
#if 0
int ReadDma(unsigned int blk_no, unsigned int blk_cnt, unsigned int startAddr)
{
	int DMA_RADDR_start;
	if (m1_err)
		return 0;
	//start FIFO & DMA transfer
	DMA_RADDR_start = startAddr + (512 * blk_no);	//compute FIFO addr
	word_write(SDR_DMA_TCCH1_REG, (512 * blk_cnt));
	word_write(SDR_DMA_DACH1_REG, DMA_RADDR_start);

	word_write(SDR_DMA_CTRCH1_REG,0x3F);
	word_write(SDR_DMA_CTRCH1_REG,0x33);
	word_write(SDR_BUF_TRAN_CTRL_REG, ((blk_cnt << 16) | BUF_XFER_START));

	return 0;
}
#else
int ReadDma(unsigned int blk_no, unsigned int blk_cnt, unsigned int startAddr)
{
	int DMA_RADDR_start;
	if (m1_err)
		return 0;


	//start FIFO & DMA transfer
	DMA_RADDR_start = startAddr + (512 * blk_no);	//compute FIFO addr
	word_write(SDR_DMA_TCCH1_REG, (512 * blk_cnt));
	word_write(SDR_DMA_DACH1_REG, DMA_RADDR_start);

	/* only enable XFER whether single block or not the last block */
	if (blk_no < blk_cnt || blk_cnt == 1) {
		word_write(SDR_BUF_TRAN_CTRL_REG, ((blk_cnt << 16) | BUF_XFER_START));
		null_delay(1);
	}

	word_write(SDR_DMA_CTRCH1_REG,0x3F);
	word_write(SDR_DMA_CTRCH1_REG,0x33);

	return 0;
}
#endif

int SDCardRead(unsigned int buf_dest_addr, unsigned int cmd_arg, int blk_num, int single)
{
	unsigned int i, k;
	int cur_blk=0;
	if (m1_err)
		return 0;
	//DBG("Read(s%d) %d blk from blk %x to addr %x\n", single, blk_num, cmd_arg, buf_dest_addr);
	//if (m1_busy())    //0401
	//    goto fail;
	//printf("r.%s %d %d\n", (single?"s":"m"), cmd_arg, blk_num);
	printf(".");
	if (switch_to_m2() == 0)      //0401
		goto fail;

	for(i=0; i<blk_num; i++) {
		if(WaitBusReady(0x0c))
			goto fail;
		cur_blk = cmd_arg + i;

		//Prepare to send SD command
		if(single)
			word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|0x1));
		else
			word_write(SDR_Card_BLOCK_SET_REG,(0x00000100|blk_num));

		word_write(SDR_DMA_TRAN_RESP_REG, 0x1f); //clear all flags
		word_write(SDR_DMA_INTS_REG, 0x3);						 //clear interrupts

#ifdef SIYO_READER
		if(special_cont && (word_read(SDSW_M1_STATUS) == 0x20100815)) {
			//send cmd12
			if(HCmdNoData(0, 0, 0, 0, 12, 0x3c, 0x11))
				goto fail;
			if(WaitResp(SD_CMD_DONE, 100))  				//for CMD12(STOP_TRANMISSION)
				//goto fail;
				printf("cmd12 failed\n");
			special_cont = 0;
		}
#endif

		if(single) {
			// CMD17 (READ_SINGLE_BLOCK)
			// [31:0]data addr, R1
			if(HCmdNoData(((cur_blk>>24)&0xff), ((cur_blk>>16)&0xff), ((cur_blk>>8)&0xff), (cur_blk&0xff), 0x11, 0x79, 0x11))
				goto fail;

			if(WaitResp(SD_XFER_DONE, 1000))			//first block of data read from SD card
				goto fail;

			ReadDma(i, 0x1, buf_dest_addr);					//start DMA to store read data
			//if(WaitResp(FIFO_XFER_DONE|SD_XFER_DONE, 1000))
			//    goto fail;
			WaitDMAIntr(DMA_CH1_INTR);				//data moved to memory
		} else { //Multi Block
			// CMD18 (READ_MULTIPLE_BLOCK) - continuously transfer data blocks from card to host until STOP_TRANSMISSION cmd
			// [31:0]data addr, R1
			//DBG("multi blk read blk_no %x total blk_num %x\n", cmd_arg, blk_num);
			if(HCmdNoData(((cmd_arg>>24)&0xff), ((cmd_arg>>16)&0xff), ((cmd_arg>>8)&0xff), (cmd_arg&0xff), 0x12, 0x7B, 0x11))
				goto fail;
			null_delay(100);


			for(k=0; k<blk_num-1; k++) {
				if(WaitResp(SD_DATA_BOUND, 2000))				//first block of data read from SD card (500)
					goto fail;

				word_write(SDR_SPECIAL_CTRL_REG, 0x1);		//continue read next block from SD card

				ReadDma(k, 1, buf_dest_addr);	//start DMA to store read data

				if(WaitResp(FIFO_XFER_DONE, 100))			//data moved to FIFO
					goto fail;

				WaitDMAIntr(2);			//DMA transfer for all data completed
			}

			if(WaitResp(SD_XFER_DONE, 100))				//last data read from SD card
				goto fail;

			if(WaitResp(SD_CMD_DONE, 100))  				//for CMD12(STOP_TRANMISSION)
				goto fail;

			//if(WaitResp(FIFO_XFER_DONE, 100))				//last block of data moved to FIFO
			//    goto fail;

			//WaitDMAIntr(DMA_CH1_INTR);			//DMA transfer for all data completed
			//switch_to_m1();

			//if (switch_to_m2() == 0)      //0401
			//    goto fail;
			ReadDma(k, 0x1, buf_dest_addr);

			//if(WaitResp(FIFO_XFER_DONE, 100))                     		//last block of data moved to FIFO
			//    goto fail;

			WaitDMAIntr(2);                          		//last block of data moved to DMA
			//switch_to_m1();

			goto strdata;
		}

	} //for loop

strdata:
	switch_to_m1(); //0401
	//write all read data to destination buffer
	//for (j = 0; j < blk_num; j++) write_512_buffer(buf_dest_addr + (512*(j)), DMA_RADDR + (512*(j)));
	word_write(SDR_DMA_TRAN_RESP_REG,(FIFO_XFER_DONE|SD_XFER_DONE));
	word_write(SDR_DMA_INTS_REG, word_read(SDR_DMA_INTS_REG));
	return 0;
fail:
	switch_to_m1();
	printf("SDCardRead() Failed!\n");
	printf("Read(s%d) %d blk from blk %x to addr %x\n", single, blk_num, cmd_arg, buf_dest_addr);
	word_write(SDR_DMA_TRAN_RESP_REG,(FIFO_XFER_DONE|SD_XFER_DONE));
	word_write(SDR_DMA_INTS_REG, word_read(SDR_DMA_INTS_REG));
	return 1;
}
