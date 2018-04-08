#define CONFIG_GENERIC_MMC
#ifdef CONFIG_GENERIC_MMC

#include <config.h>
#include <common.h>
#include <command.h>
#include <part.h>
#include <malloc.h>
#include <mmc.h>
#include <asm/errno.h>
#include <asm/io.h>

#if CONFIG_SYS_VER > 1 //#ifdef CONFIG_V34
#define SPECIAL_REG     0xa000a140
#else
#define SPECIAL_REG     0xa000a0c8
#endif

/*sd card */
#define SDR_BASE                  0xa000b000
#define SDR_Card_BLOCK_SET_REG    SDR_BASE + 0x00
#define SDR_CTRL_REG              SDR_BASE + 0x04
#define SDR_CMD_ARGUMENT_REG      SDR_BASE + 0x08
#define SDR_SPECIAL_CTRL_REG      SDR_BASE + 0x0C       //old:SDR_ADDRESS_REG
#define SDR_STATUS_REG            SDR_BASE + 0x10
#define SDR_Error_Enable_REG      SDR_BASE + 0x14
#define SDR_RESPONSE1_REG         SDR_BASE + 0x18
#define SDR_RESPONSE2_REG         SDR_BASE + 0x1C
#define SDR_RESPONSE3_REG         SDR_BASE + 0x20
#define SDR_RESPONSE4_REG         SDR_BASE + 0x24
#define SDR_DMA_TRAN_RESP_REG     SDR_BASE + 0x28
#define SDR_BUF_TRAN_CTRL_REG     SDR_BASE + 0x2C

#define SDR_DMA_SACH0_REG         SDR_BASE + 0x30
#define SDR_DMA_TCCH0_REG         SDR_BASE + 0x34
#define SDR_DMA_CTRCH0_REG        SDR_BASE + 0x38
#define SDR_DMA_DACH1_REG         SDR_BASE + 0x40
#define SDR_DMA_TCCH1_REG         SDR_BASE + 0x44
#define SDR_DMA_CTRCH1_REG        SDR_BASE + 0x48
#define SDR_DMA_INTS_REG          SDR_BASE + 0x4C
#define SDR_DMA_FIFO_STATUS_REG   SDR_BASE + 0x50

#define SDSW_BASE                0xa000a000
#define SDSW_M1_CTRL0            SDSW_BASE + 0x00
#define SDSW_M2_CTRL0            SDSW_BASE + 0x04
#define SDSW_M1_STATUS           SDSW_BASE + 0x08
#define SDSW_M2_STATUS           SDSW_BASE + 0x0c
#define SDSW_READ_SWDAT          SDSW_BASE + 0x18
#define SDSW_SW_CTRL0            SDSW_BASE + 0x1c
#define SDSW_TEST_REG            SDSW_BASE + 0x20

#define BLK_CNT         0x01
#define XFER_COUNT      0x200
#define BUF_BLK_CNT     (BLK_CNT << 16)
#define BUF_XFER_START  (0x1  << 2)

#define FIFO_XFER_DONE  0x1
#define SD_XFER_DONE    0x2
#define SD_CMD_DONE     0x4
#define SD_DATA_BOUND   0x8

/*
 * Sends a command out on the bus.  Takes the mmc pointer,
 * a command pointer, and an optional data pointer.
 */
static int
kmmc_send_cmd(struct mmc *mmc, struct mmc_cmd *cmd, struct mmc_data *data)
{
        unsigned int cmdidx = 0;
        unsigned int cmdresp = 0;
        unsigned int cmdarg = 0;
        int result = 0;
	int tranres = 0;
        int delay = 30;
        volatile int time_out = 1000+delay*50;
        unsigned int flag=0;
        unsigned int RCA0, RCA1, OCR1, OCR2;

        cmdidx = cmd->cmdidx;
        cmdresp = cmd->resp_type;
        cmdarg = cmd->cmdarg;

        if ((cmdresp & MMC_RSP_136) && (cmdresp & MMC_RSP_BUSY))
                return -1;
        else if (cmdresp & MMC_RSP_BUSY)
                flag |= ((0x3 << 10) | (0x1 << 12) | (0x1 << 13));
        else if (cmdidx == 0)
                flag |= ((0x1 << 12) | (0x1 << 13));
        else if ((cmdidx == 2) || (cmdidx == 9))
                flag |= ((0x1 << 10) | (0x1 << 12) | (0x1 << 13));
        else if (cmdidx == 0x12)
                flag |= ((0x1 << 11) | (0x1 << 12) | (0x1 << 13) | (0x1 << 0x9));
        else
                flag |= ((0x1 << 11) | (0x1 << 12) | (0x1 << 13));

        /* read tran_resp register */
        writel(readl(SDR_DMA_TRAN_RESP_REG), SDR_DMA_TRAN_RESP_REG);

        /* send the command */
        if (data && (cmdidx != 0x33) && (cmdidx != 0x06)) {
            if (data->flags & MMC_DATA_READ) {
                 flag |= 0x1 << 8;
                 flag |= 0x1 << 14;
            }
	    // setup block transfer
            if (data->blocks == 1)
            {       //single block
                    writel((0x1 | 0x1 << 8), SDR_Card_BLOCK_SET_REG);
	    }
	    else
	    {
                    writel((data->blocks | 0x1 << 8), SDR_Card_BLOCK_SET_REG);
	    }
        }

        writel(cmdarg, SDR_CMD_ARGUMENT_REG);

	if (mmc->bus_width == 1)
	        flag = flag | (cmd->flags << 8) | (cmdidx << 16) | 0x10 | (0x1  << 24);
	else
	        flag = flag | (cmd->flags << 8) | (cmdidx << 16) | 0x11 | (0x1  << 24);
        printf("cmdidx 0x%x flag 0x%x cmd->flags 0x%x cmdarg 0x%x\n", cmdidx, flag, cmd->flags, cmdarg);
        writel(flag, SDR_CTRL_REG);

//printf("DMA_TRAN_RESP_REG %x\n",readl(SDR_DMA_TRAN_RESP_REG));
        /* Polling for SDIO cmd done status */
        while( (readl(SDR_DMA_TRAN_RESP_REG) & SD_CMD_DONE) != SD_CMD_DONE) {
                if (time_out-- <= 0) {
                        printf("Time out - wait for 0x4 got %x (%d)\n", readl(SDR_DMA_TRAN_RESP_REG), delay);
			tranres = 1;
                        break;
                }
                udelay(20);
        }
//printf("DMA_TRAN_RESP_REG %x\n",readl(SDR_DMA_TRAN_RESP_REG));
	writel(SD_CMD_DONE, SDR_DMA_TRAN_RESP_REG);

        result = readl(SDR_STATUS_REG);

        /* check if CRC error */
        if (result & 0x200000) {
                printf("\n!!!! CRC Error\n");
        }

        /* check other error */
        if (result & 0x4f0000)
        { // skip crc error 0x200000
                printf("\nOther error status:%x\n", result);
		tranres = 1;
        }

	if (tranres == 1)
        {
		//error
                if (cmdidx == 8)
                {
                        // write to sdsw_sw_ctrl0
                        writel(1<<2, SDSW_SW_CTRL0);
                        // read m2 status
                        printf("a000a00c %08x\n", readl(0xa000a00c));
                }
		//return -1;
	}
	else 
	{
		//NO error
                if ((cmdidx == 6) && (cmdarg == 2))
                {
                        /* write to sdsw_sbw_reg */
                        writel(0x22, SPECIAL_REG);
                }
        }

        if (cmd->resp_type & MMC_RSP_PRESENT) {
                /*if (cmd->resp_type & MMC_RSP_136) {
                        cmd->response[0] = readl(SDR_RESPONSE1_REG);
                        cmd->response[1] = readl(SDR_RESPONSE2_REG);
                        cmd->response[2] = readl(SDR_RESPONSE3_REG);
                        cmd->response[3] = readl(SDR_RESPONSE4_REG);
                        printf("cmd->response is %x %x %x %x\n", cmd->response[0], cmd->response[1], cmd->response[2], cmd->response[3]);
                }
                else*/
                {
                        cmd->response[0] = readl(SDR_RESPONSE1_REG);
                        cmd->response[1] = readl(SDR_RESPONSE2_REG);
                        cmd->response[2] = readl(SDR_RESPONSE3_REG);
                        cmd->response[3] = readl(SDR_RESPONSE4_REG);
                        //printf("cmd->response is %x %x %x %x\n", cmd->response[0], cmd->response[1], cmd->response[2], cmd->response[3]);
                }
		udelay(400);
        }

        if (data && (data->flags & MMC_DATA_READ) && cmdidx != 0x33 && cmdidx !=0x6)
        {
                int k;
                int DMA_RADDR_start;
                volatile int time_out = 1000;

		udelay(200);
                printf("block %d blocksize %d flags 0x%x\n", data->blocks, data->blocksize, data->flags);
                /* switch to m2 */
                //writel(1, SDSW_M2_CTRL0);     

                if (data->blocks == 1)
                {       //single block
                        //Prepare to send SD command
                        //writel((0x00000001 | ((data->blocksize)<<16)), SDR_Card_BLOCK_SET_REG);

                        writel(0x1f, SDR_DMA_TRAN_RESP_REG); //clear all flags
                        writel(0x3, SDR_DMA_INTS_REG);   //clear interrupts
//printf("data->dest 0x%x\n", (unsigned int)(data->dest));
//printf("data dest is  %x %x\n",  *((data->dest) + 0x1fe), *((data->dest) + 0x1fe + 1));
                        DMA_RADDR_start = (unsigned int)(data->dest);   //compute FIFO addr
                        writel((data->blocksize), SDR_DMA_TCCH1_REG);
                        writel(DMA_RADDR_start, SDR_DMA_DACH1_REG);

                        /* only enable XFER whether single block or not the last block */
                        writel(((1 << 16) | BUF_XFER_START), SDR_BUF_TRAN_CTRL_REG);
                        udelay(100);

                        writel(0x3F,SDR_DMA_CTRCH1_REG);
                        writel(0x33,SDR_DMA_CTRCH1_REG);


                        //DMA transfer for all data completed
                        while((readl(SDR_DMA_INTS_REG) & 2) == 0) { //wait DMA transfer done
                                if (time_out-- <= 0) {
                                        printf("Wait dma 2 got %x time out\n", readl(SDR_DMA_INTS_REG));
                                        break;
                                }
                        }
                        writel(readl(SDR_DMA_INTS_REG), SDR_DMA_INTS_REG);
//printf("data dest is %x %x %x\n",  (unsigned int)(data->dest), *((data->dest) ), *((data->dest) + 1));
                }
		else
		{
			//multi block
                        //writel((data->blocks | 0x1 << 8), SDR_Card_BLOCK_SET_REG);

                        writel(0x1f, SDR_DMA_TRAN_RESP_REG); //clear all flags
                        writel(0x3, SDR_DMA_INTS_REG);   //clear interrupts

			for(k = 0; k < (data->blocks); k++) {

                          //start DMA to store read data
                          //start FIFO & DMA transfer
                          DMA_RADDR_start = (unsigned int)(data->dest + (512 * k));   //compute FIFO addr
                          writel((512), SDR_DMA_TCCH1_REG);
                          writel(DMA_RADDR_start, SDR_DMA_DACH1_REG);

                          /* only enable XFER whether single block or not the last block */
                          writel(((1 << 16) | BUF_XFER_START), SDR_BUF_TRAN_CTRL_REG);
                          udelay(100);

                          writel(0x3F,SDR_DMA_CTRCH1_REG);
                          writel(0x33,SDR_DMA_CTRCH1_REG);

                          //data moved to FIFO                        
                          delay = 100;
                          time_out = 1000+delay*50;
                          while( (readl(SDR_DMA_TRAN_RESP_REG) & FIFO_XFER_DONE) != FIFO_XFER_DONE) {
                            if (time_out-- <= 0) {
                              printf("Time out - wait for FIFO_XFER_DONE, got %x (%d)\n", readl(SDR_DMA_TRAN_RESP_REG), delay);
                              break;
                            }
                            udelay(2000);
                          }
                          writel(FIFO_XFER_DONE, SDR_DMA_TRAN_RESP_REG);

                          //DMA transfer for all data completed
                          while((readl(SDR_DMA_INTS_REG) & 2) == 0) { //wait DMA transfer done
                                if (time_out-- <= 0) {
                                        printf("Wait dma 2 got %x time out\n", readl(SDR_DMA_INTS_REG));
                                        break;
                                }
                          }
                          writel(readl(SDR_DMA_INTS_REG), SDR_DMA_INTS_REG);

                          //continue read next block from SD card
                          writel(0x1, SDR_SPECIAL_CTRL_REG);
//printf("data dest is %x %x %x\n", (unsigned int)(data->dest + (512*k)), *((data->dest) + (512*k)), *((data->dest) + (512*k + 1)));
			  printf(".");
			  udelay(20);
			} // for loop
#if 0
	                //last data read from SD card
        	        delay = 100;
                	time_out = 1000+delay*50;
	                while( (readl(SDR_DMA_TRAN_RESP_REG) & SD_XFER_DONE) != SD_XFER_DONE) 
			{
                          if (time_out-- <= 0) {
                                printf("Time out - wait for SD_XFER_DONE, got %x (%d)\n", readl(SDR_DMA_TRAN_RESP_REG), delay);
                                break;
                          }
        	          udelay(2000);
                	}
	                writel(SD_XFER_DONE, SDR_DMA_TRAN_RESP_REG);

	                //for CMD12(STOP_TRANMISSION)               
        	        delay = 100;
                	time_out = 1000+delay*50;
	                while( (readl(SDR_DMA_TRAN_RESP_REG) & SD_CMD_DONE) != SD_CMD_DONE) 
			{
                          if (time_out-- <= 0) {
                                printf("Time out - wait for SD_CMD_DONE, got %x (%d)\n", readl(SDR_DMA_TRAN_RESP_REG), delay);
                                break;
                          }
                          udelay(200);
        	        }
                	writel(SD_CMD_DONE, SDR_DMA_TRAN_RESP_REG);

	                //start DMA to store read data
        	        //start FIFO & DMA transfer
                	DMA_RADDR_start = (unsigned int)(data->dest + (512 * k));   //compute FIFO addr
	                writel((512 * 1), SDR_DMA_TCCH1_REG);
        	        writel(DMA_RADDR_start, SDR_DMA_DACH1_REG);

                	/* only enable XFER whether single block or not the last block */
	                writel(((1 << 16) | BUF_XFER_START), SDR_BUF_TRAN_CTRL_REG);
        	        udelay(100);

               		writel(0x3F,SDR_DMA_CTRCH1_REG);
	                writel(0x33,SDR_DMA_CTRCH1_REG);

        	        //last block of data moved to DMA
                	while((readl(SDR_DMA_INTS_REG) & 2) == 0) { //wait DMA transfer done
                          if (time_out-- <= 0) {
                                printf("Wait dma 2 got %x time out\n", readl(SDR_DMA_INTS_REG));
                                break;
                          }
                	}
                	writel(readl(SDR_DMA_INTS_REG), SDR_DMA_INTS_REG);
#endif
			writel((FIFO_XFER_DONE|SD_XFER_DONE), SDR_DMA_TRAN_RESP_REG);
	                writel(readl(SDR_DMA_INTS_REG), SDR_DMA_INTS_REG);
		} //multi block

	}
	return 0;
}

static void kmmc_set_ios(struct mmc *mmc)
{
	// set to 4 bit bus width
	if (mmc->bus_width == 1)
	{
		writel(0x0, SPECIAL_REG);
	}
	else
	{
		writel(0x22, SPECIAL_REG);
	}
        printf("SPECIAL_REG 0x%x\n", readl(SPECIAL_REG));
}

static int kmmc_init(struct mmc *mmc)
{
        unsigned int RCA0, RCA1;

        printf("M1 status 0x%x\n", readl(0xa000a008));
        RCA0 = readl(0xa000a09c) & 0xff;
        RCA1 = (readl(0xa000a09c) & 0xff00) >> 8;
        printf("RCA0 %x, RCA1 %x\n", RCA0, RCA1);

        //Initialize DMA
        writel(XFER_COUNT, SDR_DMA_TCCH0_REG);
        writel(XFER_COUNT, SDR_DMA_TCCH1_REG);
        writel((0x00000100|BLK_CNT), SDR_Card_BLOCK_SET_REG);
        writel(0x6f, SDR_Error_Enable_REG);                 //enable all errors

        writel(0xe0000000, 0xa000a0fc/*SDSW_M1_RDATA_TOUT_REG*/);
        writel(0, 0xa000a10c/*SDSW_DIRECT_CTRL_REG*/);

        writel(0x1f, SDR_DMA_TRAN_RESP_REG);
        writel(0x3, SDR_DMA_INTS_REG);

	writel(0xffff, 0xa0000028); //pclk enable (bit 10=sd host)

        //unset M1
        do
        {
                writel(0x0, SDSW_M1_CTRL0);
        } while ((readl(SDSW_M1_CTRL0) & 0x1) == 0x1);
        writel(0x8, SDSW_M1_CTRL0);
        //set M2
        do
        {
                writel(0x1, SDSW_M2_CTRL0);
        } while (!((readl(SDSW_M2_CTRL0) & 0x1) == 0x1));

        // write to sdsw_sw_ctrl0
        writel(1<<2, 0xa000a01c);
        // read m2 status
        printf("M2 status a000a00c %08x\n", readl(0xa000a00c));

        printf("0xa0000000 0x%x\n", readl(0xa0000000));
        printf("0xa0000014 0x%x\n", readl(0xa0000014));
        printf("0xa0000028 0x%x\n", readl(0xa0000028));
        printf("0xa000a0c8 0x%x\n", readl(0xa000a0c8));
        printf("0xa000a140 0x%x\n", readl(0xa000a140));

        printf("0xa000a000 0x%x\n", readl(0xa000a000));
        printf("0xa000a004 0x%x\n", readl(0xa000a004));
        printf("0xa000a00c 0x%x\n", readl(0xa000a00c));

	return 0;
}

int kagen2_mmc_init(bd_t *bis)
{
        struct mmc * mmc;

printf("kagen2_mmc_init\n");

        mmc = malloc(sizeof(struct mmc));
        if (!mmc)
                return -ENOMEM;
        memset(mmc, 0, sizeof(struct mmc));

        sprintf(mmc->name, "KAgen2");
        mmc->send_cmd = kmmc_send_cmd;
        mmc->set_ios = kmmc_set_ios;
        mmc->init = kmmc_init;
        mmc->getcd = NULL;

        mmc->f_max = 24000000;
        mmc->f_min = 400000;
        mmc->voltages = MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;
        mmc->host_caps = MMC_MODE_4BIT;
        mmc->host_caps |= MMC_MODE_HS_52MHz | MMC_MODE_HS ;
        mmc->b_max = 0;

        mmc_register(mmc);

        return 0;

}

int board_mmc_init(bd_t *bis)
{
       return kagen2_mmc_init(bis);
}

#endif
