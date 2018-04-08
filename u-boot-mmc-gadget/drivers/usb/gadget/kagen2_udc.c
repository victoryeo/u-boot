#include <common.h>
#include <command.h>
#include <config.h>
#include <net.h>
#include <malloc.h>
#include <asm/io.h>
#include <linux/types.h>

#include <asm/byteorder.h>
#include <asm/errno.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>

#include <usb_mass_storage.h>
#include <usbdescriptors.h>

#define NUM_ENDPOINTS   	6
#define EP_MAX_PACKET_SIZE      0x200
#define EP0_MAX_PACKET_SIZE     64
#define PAGE_SIZE       4096
#define QH_MAXNUM       32

static const char *reqname(unsigned r)
{
        switch (r) {
        case USB_REQ_GET_STATUS: return "GET_STATUS";
        case USB_REQ_CLEAR_FEATURE: return "CLEAR_FEATURE";
        case USB_REQ_SET_FEATURE: return "SET_FEATURE";
        case USB_REQ_SET_ADDRESS: return "SET_ADDRESS";
        case USB_REQ_GET_DESCRIPTOR: return "GET_DESCRIPTOR";
        case USB_REQ_SET_DESCRIPTOR: return "SET_DESCRIPTOR";
        case USB_REQ_GET_CONFIGURATION: return "GET_CONFIGURATION";
        case USB_REQ_SET_CONFIGURATION: return "SET_CONFIGURATION";
        case USB_REQ_GET_INTERFACE: return "GET_INTERFACE";
        case USB_REQ_SET_INTERFACE: return "SET_INTERFACE";
        default: return "*UNKNOWN*";
        }
}

static struct usb_endpoint_descriptor ep0_out_desc = {
        .bLength = sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0,
        .bmAttributes = USB_ENDPOINT_XFER_CONTROL,
};

static struct usb_endpoint_descriptor ep0_in_desc = {
        .bLength = sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_DIR_IN,
        .bmAttributes = USB_ENDPOINT_XFER_CONTROL,
};

static void ep_enable(int num, int in);
static int ka_pullup(struct usb_gadget *gadget, int is_on);
static int ka_ep_enable(struct usb_ep *ep,
                const struct usb_endpoint_descriptor *desc);
static int ka_ep_disable(struct usb_ep *ep);
static int ka_ep_queue(struct usb_ep *ep,
                struct usb_request *req, gfp_t gfp_flags);
static struct usb_request *
	ka_ep_alloc_request(struct usb_ep *ep, unsigned int gfp_flags);
static void ka_ep_free_request(struct usb_ep *ep, struct usb_request *_req);

static struct usb_gadget_ops ka_udc_ops = {
        .pullup = ka_pullup,
};

static struct usb_ep_ops ka_ep_ops = {
        .enable         = ka_ep_enable,
        .disable        = ka_ep_disable,
        .queue          = ka_ep_queue,
        .alloc_request  = ka_ep_alloc_request,
        .free_request   = ka_ep_free_request,
};

struct ka_ep {
        struct usb_ep ep;
	struct usb_request req;
        const struct usb_endpoint_descriptor *desc;
        struct list_head queue;
};

struct ka_request {
        struct usb_request req;
        struct list_head queue;
};

struct ka_udc {
        struct usb_gadget               gadget;
        struct usb_gadget_driver        *driver;
};

struct ept_queue_head {
        unsigned config;
        unsigned current; /* read-only */

        unsigned next;
        unsigned info;
        unsigned page0;
        unsigned page1;
        unsigned page2;
        unsigned page3;
        unsigned page4;
        unsigned reserved_0;

        unsigned char setup_data[8];

        unsigned reserved_1;
        unsigned reserved_2;
        unsigned reserved_3;
        unsigned reserved_4;
};

#define CONFIG_MAX_PKT(n)     ((n) << 16)

#define TERMINATE 1
#define INFO_BYTES(n)         ((n) << 16)
#define INFO_IOC              (1 << 15)
#define INFO_ACTIVE           (1 << 7)
#define INFO_HALTED           (1 << 6)
#define INFO_BUFFER_ERROR     (1 << 5)
#define INFO_TX_ERROR         (1 << 3)

static struct ka_ep ka_ep_g[2 * NUM_ENDPOINTS];

static struct ka_udc controller = {
        .gadget = {
                .ep0 = &ka_ep_g[0].ep,
                .name = "ka_udc",
        },
};

struct ept_queue_head *epts;

#define USB_REG_BASE 	0xb0000000
#define USB_IVECT 	USB_REG_BASE + 0x1a0

enum SPEED {
	LOWSPEED = 0,
	FULLSPEED = 1,
	HIGHSPEED = 2,
};

enum STATE {
	DEFAULT = 0,
	SUSPENDED
};

int         system_level = 0;
unsigned char   device_state = 0;            
unsigned char   device_speed = FULLSPEED;  

static void handle_ep_complete(struct ka_ep *ka_ep_p, struct ka_request *req)
{
        int num, in, len;
        num = ka_ep_p->desc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
        in = (ka_ep_p->desc->bEndpointAddress & USB_DIR_IN) != 0;
        if (num == 0)
                ka_ep_p->desc = &ep0_out_desc;

	len = ka_ep_p->req.length;
        printf("ept%d %s complete %x\n",
                        num, in ? "in" : "out", len);
     
	// call gadget code
	ka_ep_p->req.complete(&ka_ep_p->ep, &req->req);
        if (num == 0) {
                ka_ep_p->req.length = 0;
                usb_ep_queue(&ka_ep_p->ep, &req->req, 0);
                ka_ep_p->desc = &ep0_in_desc;
        }
}

#define SETUP(type, request) (((type) << 8) | (request))

static void handle_setup(int bmRequestType, int bRequest, int wValue, int wIndex, int wLength)
{
        struct usb_request *req = &ka_ep_g[0].req;
        struct usb_ctrlrequest r;
        int status = 0;
        int num, in, _num, _in, i;
        char *buf;

	r.bRequestType = bmRequestType;
	r.bRequest = bRequest;
	r.wValue = wValue;
	r.wIndex = wIndex;
	r.wLength = wLength;
        printf("handle setup %s, %x, %x index %x value %x len %x\n", reqname(r.bRequest),
            r.bRequestType, r.bRequest, r.wIndex, r.wValue, r.wLength);

        switch (SETUP(r.bRequestType, r.bRequest)) {
        case SETUP(USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE):
//printf("USB_RECIP_ENDPOINT\n");
                _num = r.wIndex & 15;
                _in = !!(r.wIndex & 0x80);

                if ((r.wValue == 0) && (r.wLength == 0)) {
                        req->length = 0;
                        for (i = 0; i < NUM_ENDPOINTS; i++) {
                                if (!ka_ep_g[i].desc)
                                        continue;
                                num = ka_ep_g[i].desc->bEndpointAddress
                                        & USB_ENDPOINT_NUMBER_MASK;
                                in = (ka_ep_g[i].desc->bEndpointAddress
                                                
& USB_DIR_IN) != 0;

                               if ((num == _num) && (in == _in)) {
                                        ep_enable(num, in);
                                        usb_ep_queue(controller.gadget.ep0,
                                                        req, 0);
                                        break;
                                }
                        }
                }
                return;

        case SETUP(USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS):
                /*
                 * write address delayed (will take effect
                 * after the next IN txn)
                 */
//printf("USB_RECIP_DEVICE\n");
                req->length = 0;
                usb_ep_queue(controller.gadget.ep0, req, 0);
                return;
        case SETUP(USB_DIR_IN | USB_RECIP_DEVICE, USB_REQ_GET_STATUS):
//printf("USB_DIR_IN\n");
                req->length = 2;
                buf = (char *)req->buf;
                buf[0] = 1 << USB_DEVICE_SELF_POWERED;
                buf[1] = 0;
                usb_ep_queue(controller.gadget.ep0, req, 0);
                return;
        }
        /* pass request up to the gadget driver */
        if (controller.driver)
                status = controller.driver->setup(&controller.gadget, &r);
        else
                status = -ENODEV;

        if (!status)
                return;

}

static void ep1_out()
{
	unsigned int val_arr[16];
	int len;
	int i, in, num;
	unsigned int val;

	//EP1 OUT IRQ
	//clear out fifo 0 to 7 empty irq
	val = readl(USB_REG_BASE + 0x190);
	val &= 0xff00ffff;
	val |= 0x00020000;		
	writel(val, USB_REG_BASE + 0x190);
		
	// clear Out 0 to 7 irq
	val = readl(USB_REG_BASE + 0x188);
	val &= 0xff00ffff;
	val |= 0x00020000;		
	writel(val, USB_REG_BASE + 0x188);

	// get byte cnt
        val = readl(USB_REG_BASE + 0x008);
	len = val & 0xFF;

	// read from fifo1 data
        for (i = 0; i < len/4; i++)
        {
              val_arr[i] = readl(USB_REG_BASE + 0x084);
              printf("0x%x ",val_arr[i]);
        }
        if ((len%4) != 0)
        {
              val_arr[i] = readl(USB_REG_BASE + 0x084);
        }

	//bulk out ep
        if (ka_ep_g[2].desc) {

		struct ka_request * req;
		struct usb_request * usbreq;	

		if (list_empty(&ka_ep_g[2].queue)) {
               		  printf(
                           "%s: RX DMA done : NULL REQ on OUT EP-1\n",
                          __func__);
                	  return;
		}
			
		req = list_entry(ka_ep_g[2].queue.next, struct ka_request, queue);

		if (req == NULL)
			return;
		req->req.length = len;
		req->req.actual = len;
		memcpy(req->req.buf, &val_arr[0], len);
		//ka_ep_g[2].desc->bEndpointAddress = 1;
                num = ka_ep_g[2].desc->bEndpointAddress
                                        & USB_ENDPOINT_NUMBER_MASK;
                in = (ka_ep_g[2].desc->bEndpointAddress
                                                & USB_DIR_IN) != 0;
		printf("epnum %d in %d\n", num, in);

		printf("len %d\n", len);
        	      
                handle_ep_complete(&ka_ep_g[2], req);
         }
         // OUT1CS
         val = readl(USB_REG_BASE + 0x008);
         val &= 0x00ffffff;
         writel(val, USB_REG_BASE + 0x008);
}

static void kaudc_isr(void)
{
	unsigned int val;

        val = readl(USB_IVECT);
	val = val & 0xFF;

	if (val == 0xd8)
	{
		//peripirq
		val = readl(USB_REG_BASE + 0x1bc);
		val &= 0xffffff00;
		val |= 0x00000011;		
		writel(val, USB_REG_BASE + 0x1bc);
	}
	else if (val == 0x10)
	{
		// usb reset
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xffffff00;
		val |= 0x00000010;		
		writel(val, USB_REG_BASE + 0x18c);
		system_level = 1;
		device_speed = FULLSPEED;
		device_state = DEFAULT;
	}
	else if (val == 0x14)
	{
		// usb hsirq
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xffffff00;
		val |= 0x00000020;		
		writel(val, USB_REG_BASE + 0x18c);
		device_speed = HIGHSPEED;
	}
	else if (val == 0x0C)
	{
		// suspend
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xffffff00;
		val |= 0x00000008;		
		writel(val, USB_REG_BASE + 0x18c);
		device_state = SUSPENDED;
	}
	else if (val == 0x08)
	{
		// setup token
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xffffff00;
		val |= 0x00000004;		
		writel(val, USB_REG_BASE + 0x18c);
	}
	else if (val == 0x00)
	{
		int bmRequestType;
		int bRequest;
		int bDescriptorType, bDescriptorIndex;
		unsigned int dst_addr;
		unsigned int rdata, rdata1, wLength, wValue, wIndex;

		// setup data valid
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xffffff00;
		val |= 0x00000001;		
		writel(val, USB_REG_BASE + 0x18c);

		// process the data
		//REQUEST TYPE (sdata0 < 80 06 01 01 00 00 45 00 > => 01 01 06 80  00 45 00 00)
		dst_addr = USB_REG_BASE + 0x180;  //setupdat
		rdata = readl(dst_addr);        
		dst_addr = USB_REG_BASE + 0x184;      
		rdata1 = readl(dst_addr);        

		bmRequestType = rdata & 0xff;
		bRequest      = (rdata >> 8) & 0xff;
		wValue 	      = (rdata >> 16) & 0xffff;
		wIndex 	      = rdata1 & 0xffff;
	      	wLength       = (rdata1 >> 16) & 0xffff; 

    	    	if(bmRequestType==0x80 &&  bRequest==0x06){
      			//wValue field specifies the descriptor type in the high byte and the descriptor index in the low byte.
		      	bDescriptorType  = rdata >> 24;
		      	bDescriptorIndex = rdata >> 16;
	    	}
		//-------------------
		//D.1 GET DESCRIPTOR
    		//-------------------
		if(bmRequestType==0x80 && bRequest==0x06){
	      		//D.1.1 DEVICE DECRIPTOR
		      	if (bDescriptorType==0x01) {
	    			if(wLength>0x12)
				{ 
					wLength=0x12;
				}
      			}
      			//D.1.2 CONFIGURATION DESCRIPTOR
      			else if(bDescriptorType==0x02) {
      			}
      			//D.1.3 STRING DESCRIPTOR
      			else if(bDescriptorType==0x03) {
      			}

		}
#if 1
		else if(bmRequestType==0x00 && bRequest==0x09) 
		{
//printf("handshake for set configuration\n");
			//clear IVECT reg
			val = readl(USB_IVECT);
			val &= 0xff00ffff;
			val |= 0x00110000;
		 	writel(val, USB_IVECT);	

	                val = readl(USB_IVECT);
                        val &= 0xff00ffff;
                        val |= 0x00710000;
                        writel(val, USB_IVECT);

                        val = readl(USB_IVECT);
                        val &= 0xff00ffff;
                        val |= 0x00010000;
                        writel(val, USB_IVECT);

                        val = readl(USB_IVECT);
                        val &= 0xff00ffff;
                        val |= 0x00610000;
                        writel(val, USB_IVECT);

			//handshake stage
	               	val = readl(USB_REG_BASE );
        	        val &= 0xff00ffff;
                	val |= 0x00120000;
               	 	writel(val, USB_REG_BASE );	
		}
#endif
		//printf("bmRequestType %x bRequest %x desctype %x\n", bmRequestType, bRequest, bDescriptorType);
		handle_setup(bmRequestType, bRequest, wValue, wIndex, wLength);
	}
	else if (val == 0x1c)
	{
		// EP0OUT TOKEN
		val = readl(USB_REG_BASE + 0x188);
		val &= 0xff00ffff;
		val |= 0x00010000;		
		writel(val, USB_REG_BASE + 0x188);
	}
	else if (val == 0x18)
	{
		// EP0IN TOKEN
		val = readl(USB_REG_BASE + 0x188);
		val &= 0xffffff00;
		val |= 0x00000001;		
		writel(val, USB_REG_BASE + 0x188);
	}
	else if (val == 0x2c)
	{
		// EP1 OUT PING
		val = readl(USB_REG_BASE + 0x18c);
		val &= 0xff00ffff;
		val |= 0x00020000;		
		writel(val, USB_REG_BASE + 0x18c);
	}
	else if (val == 0x28 || val == 0x34)
	{
printf("EPx OUT IRQ 0x%x\n", val);
	    	if (val == 0x28) 
	    	{
			ep1_out();
	    	}
                else if (val == 0x34)
                {
	                //EP2 OUT IRQ                
			val = readl(USB_REG_BASE + 0x190);
                	val &= 0xff00ffff;
        	        val |= 0x00040000;
	                writel(val, USB_REG_BASE + 0x190);

                	val = readl(USB_REG_BASE + 0x188);
        	        val &= 0xff00ffff;
	                val |= 0x00040000;
                	writel(val, USB_REG_BASE + 0x188);
                }
		val = readl(USB_IVECT);
	}
	else if (val == 0x24)
	{
		//EP1 in IRQ
		val = readl(USB_REG_BASE + 0x188);
		val &= 0xffffff00;
		val |= 0x00000002;		
		writel(val, USB_REG_BASE + 0x188);
	}
	else if (val == 0x20)
	{
		//EP0 PING
	}
	else
	{
		printf("unknown 0x%x\n", val);
	}
}

void kausb_lowlevel_init(void)
{
	unsigned int val;

    /* Clear all interrupt requests */
    writel(~0x0, 0xa0006018);
    writel(~0x0, 0xa000601c);
    
    /* Disable all interrupts */
    writel(~0x0, 0xa0006010);
    writel(~0x0, 0xa0006014);

    writel(~0x0, 0xa0006018);
    writel(~0x0, 0xa000601c);

    // setup priority
    //writel(0x1111111, 0xa0006020);
    //writel(0x11111111, 0xa0006024);

    writel(0, 0xa0006030);

    //set usb irq32 to edge trigger
    //writel(0x1, 0xa000602C);

    //unmask the irq 32 which is the usb irq
    writel((readl(0xa0006014) & 0xfffffffe), 0xa0006014);
    printf("6014 intc mask %x\n",readl(0xa0006014));

    //unmask the irq 1 and 22 which is the uart and timer 1 irq
    //writel((readl(0xa0006010) & 0xffbffffd), 0xa0006010);
    //printf("6010 intc mask %x\n",readl(0xa0006010));

    // set timer 1
    //printf("setup timer 1\n");
    //writel((0x33 << 16), 0xa0002000);
    //writel((0x5 << 20), 0xa0002008);
loopsta:

	val = readl(USB_IVECT);
	val &= 0x00ffffff;
	writel(val, USB_IVECT);
printf("USB_IVECT\n");

	// INXMAXPCKL - 0x3e2 INXMAXPCKH - 0x3e3
	val = readl(USB_REG_BASE + 0x3e0);
	val &= 0x0000ffff;
	val |= 0x02000000;
	writel(val, USB_REG_BASE + 0x3e0);
printf("USB_INXMAXPCKL\n");

	// OUTXMAXPCKL - 0x1e2 OUTXMAXPCKH - 0x1e3
	val = readl(USB_REG_BASE + 0x1e0);
	val &= 0x0000ffff;
	val |= 0x02000000;
	writel(val, USB_REG_BASE + 0x1e0);
printf("USB_OUTXMAXPCKL\n");

	// INXSTARTADDRL - 0x344 INXSTARTADDRH - 0x345
	val = readl(USB_REG_BASE + 0x344);
	val &= 0xffff0000;
	val |= 0x00000040;
	writel(val, USB_REG_BASE + 0x344);

	// OUTxSTARTADDRL - 0x304 OUTxSTARTADDRH - 0x305
	val = readl(USB_REG_BASE + 0x304);
	val &= 0xffff0000;
	val |= 0x00000240;
	writel(val, USB_REG_BASE + 0x304);
	
	// IN1CON - 0x00e
	val = readl(USB_REG_BASE + 0x00C);
	val &= 0xff00ffff;
	val |= 0x00880000;
	writel(val, USB_REG_BASE + 0x00C);

	// IN2CON - 0x016
	val = readl(USB_REG_BASE + 0x014);
	val &= 0xff00ffff;
	val |= 0x00880000;
	writel(val, USB_REG_BASE + 0x014);

	// OUT1CON - 0x00A
	val = readl(USB_REG_BASE + 0x008);
	val &= 0xff00ffff;
	val |= 0x00880000;
	writel(val, USB_REG_BASE + 0x008);

	// OUT2CON - 0x012
	val = readl(USB_REG_BASE + 0x010);
	val &= 0xff00ffff;
	val |= 0x00880000;
	writel(val, USB_REG_BASE + 0x010);

	// FIFOCTRL IN/OUT -0x1a8 for EP1
	val = readl(USB_REG_BASE + 0x1a8);
	val &= 0xffffff00;
	val |= 0x00000051;
	writel(val, USB_REG_BASE + 0x1a8);

	val = readl(USB_REG_BASE + 0x1a8);
	val &= 0xffffff00;
	val |= 0x00000001;
	writel(val, USB_REG_BASE + 0x1a8);

	// FIFOCTRL IN/OUT -0x1a8 for EP2
	val = readl(USB_REG_BASE + 0x1a8);
	val &= 0xffffff00;
	val |= 0x00000052;
	writel(val, USB_REG_BASE + 0x1a8);

	val = readl(USB_REG_BASE + 0x1a8);
	val &= 0xffffff00;
	val |= 0x00000002;
	writel(val, USB_REG_BASE + 0x1a8);

	// FIFORST togglerst - 0x1a0 for IN EP1
	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00110000;
	writel(val, USB_REG_BASE + 0x1a0);

	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00710000;
	writel(val, USB_REG_BASE + 0x1a0);

	// FIFORST togglerst - 0x1a0 for IN EP2
	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00120000;
	writel(val, USB_REG_BASE + 0x1a0);

	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00720000;
	writel(val, USB_REG_BASE + 0x1a0);

	// FIFORST togglerst - 0x1a0 for OUT EP1
	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00010000;
	writel(val, USB_REG_BASE + 0x1a0);

	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00610000;
	writel(val, USB_REG_BASE + 0x1a0);
	
	// FIFORST togglerst - 0x1a0 for OUT EP2
	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00020000;
	writel(val, USB_REG_BASE + 0x1a0);

	val = readl(USB_REG_BASE + 0x1a0);
	val &= 0xff00ffff;
	val |= 0x00620000;
	writel(val, USB_REG_BASE + 0x1a0);

	// INxIEN - 0x194 
	val = readl(USB_REG_BASE + 0x194);
	val &= 0xffffff00;
	val |= 0x00000004;
	writel(val, USB_REG_BASE + 0x194);

	// INxFULLIEN - 0x19C
	val = readl(USB_REG_BASE + 0x19C);
	val &= 0xffffff00;
	val |= 0x00000004;
	writel(val, USB_REG_BASE + 0x19C);

	// OUTxIEN 0x196
	val = readl(USB_REG_BASE + 0x194);
	val &= 0xff00ffff;
	val |= 0x00040000;
	writel(val, USB_REG_BASE + 0x194);

	// OUTxEMPTIEN 0x19E
	val = readl(USB_REG_BASE + 0x19C);
	val &= 0xff00ffff;
	val |= 0x00040000;
	writel(val, USB_REG_BASE + 0x19C);

	// OTG periph irq
	val = readl(USB_REG_BASE + 0x1C0);
	val &= 0xffffff00;
	val |= 0x00000010;
	writel(val, USB_REG_BASE + 0x1C0);

#if 0
{
	int loop = 0;
	val = readl(USB_IVECT);
	printf("check USB_IVECT 0x%x \n", val);
	while ((val&0xFF) !=0xD8) {
		val = readl(USB_REG_BASE + 0x1bc);
		printf("check USB_OTGST 0x%x \n", val);

		val = readl(USB_IVECT);
		printf("check USB_IVECT 0x%x \n", val);

		if (loop++ > 50)
		{
		  printf("reset USB\n");
		  // usb reset 1
		  writel((readl(0xa000003c) | 0x1), 0xa000003c);
		  // usb reset 0
		  writel((readl(0xa000003c) & 0xfffffffe), 0xa000003c);

		  // usb reset 0
		  writel((readl(0xa000002c) & 0xfffffffe), 0xa000002c);
		  // usb reset 1
		  writel((readl(0xa000002c) | 0x1), 0xa000002c);
		  goto loopsta;
		}
	};
}
#endif
val = readl(USB_REG_BASE + 0x1bc);
printf("check USB_OTGST 0x%x \n", val);

	// OTGIRQ
	//val = readl(USB_REG_BASE + 0x1bc);
	//val &= 0xffffff00;
	//val |= 0x00000010;
	//writel(val, USB_REG_BASE + 0x1bc);
//printf("check USB_OTGIRQ 0x%x \n", val);
	
	// intr init
	val = readl(USB_REG_BASE + 0x198);
	val &= 0xff00ff00;
	val |= 0x000700fd;
	writel(val, USB_REG_BASE + 0x198);

	val = readl(USB_REG_BASE + 0x194);
	val &= 0xff00ff00;
	val |= 0x00070007;
	writel(val, USB_REG_BASE + 0x194);
printf("check USB_IRQINIT 0x%x \n", val);

	//OTGFSM WAKEUPDP
	//val = readl(USB_REG_BASE + 0x1c0);
	//val &= 0xffffff00;
	//val |= 0x0000001f;
	//writel(val, USB_REG_BASE + 0x1c0);
//printf("check USB_OTGFSM 0x%x \n", val);

	// OTGCTRL
	//val = readl(USB_REG_BASE + 0x1bc);
	//val &= 0xff00ffff;
	//val |= 0x00300000;
	//writel(val, USB_REG_BASE + 0x1bc);
//printf("check USB_OTGCTRL 0x%x \n", val);	
}

static int kaudc_probe(void)
{
	int i;
	struct ept_queue_head *head;

	controller.gadget.ops = &ka_udc_ops;

        epts = memalign(PAGE_SIZE, QH_MAXNUM * sizeof(struct ept_queue_head));
        memset(epts, 0, QH_MAXNUM * sizeof(struct ept_queue_head));
        for (i = 0; i < 2 * NUM_ENDPOINTS; i++) {
                /*
                 * For item0 and item1, they are served as ep0
                 * out&in seperately
                 */
                head = epts + i;
  
                head->next = TERMINATE;
                head->info = 0;

        }

	INIT_LIST_HEAD(&controller.gadget.ep_list);
	ka_ep_g[0].ep.maxpacket = 64;
        ka_ep_g[0].ep.name = "ep0";
        ka_ep_g[0].desc = &ep0_in_desc;
        INIT_LIST_HEAD(&controller.gadget.ep0->ep_list);
        for (i = 0; i < 2 * NUM_ENDPOINTS; i++) {
                if (i != 0) {
                        ka_ep_g[i].ep.maxpacket = 512;
                        ka_ep_g[i].ep.name = "ep-";
                        list_add_tail(&ka_ep_g[i].ep.ep_list,
                                      &controller.gadget.ep_list);
                        ka_ep_g[i].desc = NULL;
			INIT_LIST_HEAD(&ka_ep_g[i].queue);
                }
                ka_ep_g[i].ep.ops = &ka_ep_ops;
        }

	return 0;
}

static struct usb_request *
ka_ep_alloc_request(struct usb_ep *ep, unsigned int gfp_flags)
{
        struct ka_ep *ka_ep = container_of(ep, struct ka_ep, ep);
        return &ka_ep->req;
}

static void ka_ep_free_request(struct usb_ep *ep, struct usb_request *_req)
{
        return;
}

static void ep_enable(int num, int in)
{

printf("%s %d\n", __func__, num);

	// enable endpoint
	/*val = readl(USB_REG_BASE + 0x00C + (num-1)*8);
	val &= 0xff00ffff;
	val |= 0x00880000;
	writel(val, USB_REG_BASE + 0x00C + (num-1)*8);*/

}

static int ka_ep_enable(struct usb_ep *ep,
                const struct usb_endpoint_descriptor *desc)
{
        struct ka_ep *ka_ep = container_of(ep, struct ka_ep, ep);
        int num, in;

        num = desc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
        in = (desc->bEndpointAddress & USB_DIR_IN) != 0;
        ep_enable(num, in);
        ka_ep->desc = desc;

	return 0;
}

static int ka_ep_disable(struct usb_ep *ep)
{
	return 0;
}

static void ep0_in(unsigned int phys, int len)
{
	unsigned int val32;
        unsigned int iter_num = 0;
	int i;

        // fill IN zero data buffer
        for (i = 0; i < len/4; i++)
        {
                val32 = *((unsigned int *)(phys + (i*4)));
printf("0x%x\n", val32);
                writel(val32, USB_REG_BASE + 0x100+(i*4));
        }
        if ((len%4) != 0)
        {
                val32 = *((unsigned int *)(phys + (i*4)));
printf("Last 0x%x\n", val32);
                writel(val32, USB_REG_BASE + 0x100+(i*4));
        }

        //udelay(20);
        // write to in0bc, arm the transfer
        val32 = readl(USB_REG_BASE);
        val32 &= 0xffff00ff;
        val32 |= len << 8;
        writel(val32, USB_REG_BASE);

        // check for IN0-7 IRQ
        val32 = readl(USB_REG_BASE + 0x188);
        while ((val32&0xff) != 0x01)
        {
                //printf("reg value is %x\n", value);
                val32 = readl(USB_REG_BASE + 0x188);
                if (iter_num++ > 40)
                        break;

        }
        // clear IN0-7 IRQ
        val32 &= 0xffffff00;
        val32 |= 0x00000001;
        writel(val32, USB_REG_BASE + 0x188);

        // handshake stage
        val32 = readl(USB_REG_BASE);
        val32 &= 0xff00ffff;
        val32 |= 0x00020000;  //clear HSNAK bit
        writel(val32, USB_REG_BASE);

	return;
}

static void ep1_in(unsigned int phys, int len)
{
	unsigned int iter_num = 0, len_num = 0;
	unsigned int val32;
	int i;
	int  num = 1;

	if (len > 512)	
	{
		// break down the data into chunks of 512 bytes
		len_num = len;
		do
		{
			// fill IN FIFO x data register
			for (i = 0; i < 512/4; i++)
		        {
                		val32 = *((unsigned int *)(phys + (512*iter_num) + (i*4)));
		                writel(val32, USB_REG_BASE + 0x84);
        		}

		        // write to inXbc, arm the transfer
		        val32 = readl(USB_REG_BASE + 0x0C );
		        val32 &= 0xffff0000;
		        val32 |= 512;
		        writel(val32, USB_REG_BASE + 0x0C );

		        // IN1CS reg
		        val32 = readl(USB_REG_BASE + 0x0C );
		        val32 &= 0x00ffffff;
		        writel(val32, USB_REG_BASE + 0x0C );

			// check for IN0-7 IRQ
		        val32 = readl(USB_REG_BASE + 0x188);
		        while (((val32&0xff) != 0x2) && ((val32&0xff) != 0x3) )
		        {
		                //printf("reg value is %x\n", value);
		                val32 = readl(USB_REG_BASE + 0x188);
		                if (iter_num++ > 40)
                		        break;
		        }
		        // clear IN0-7 IRQ
		        val32 &= 0xffffff00;
		        val32 |= 0x00000001 << num;
		        writel(val32, USB_REG_BASE + 0x188);

			len_num -= 512;
			iter_num++;
		} while (len_num > 0);	
	}
	else
	{
		// less than 512 bytes length
        	// fill IN FIFO x data register
	        for (i = 0; i < len/4; i++)
	        {
	                val32 = *((unsigned int *)(phys + (i*4)));
printf("0x%x\n", val32);
        	        writel(val32, USB_REG_BASE + 0x84);
	        }
	        if ((len%4) != 0)
        	{
        	        val32 = *((unsigned int *)(phys + (i*4)));
printf("Last 0x%x\n", val32);
        	        writel(val32, USB_REG_BASE + 0x84);
        	}
        	// write to inXbc, arm the transfer
		val32 = readl(USB_REG_BASE + 0x0C );
        	val32 &= 0xffff0000;
       	 	val32 |= len;
        	writel(val32, USB_REG_BASE + 0x0C );

		// IN1CS reg
        	val32 = readl(USB_REG_BASE + 0x0C );
        	val32 &= 0x00ffffff;
        	writel(val32, USB_REG_BASE + 0x0C );

        	/*val32 = readl(USB_REG_BASE + 0x0C );
        	val32 &= 0xffff00ff;
        	val32 |= len >> 16;
        	writel(val32, USB_REG_BASE + 0x0C );*/

        	// check for IN0-7 IRQ
        	val32 = readl(USB_REG_BASE + 0x188);
        	while (((val32&0xff) != 0x2) && ((val32&0xff) != 0x3) )
        	{	
			//printf("reg value is %x\n", value);
               		val32 = readl(USB_REG_BASE + 0x188);
			if (iter_num++ > 40)
				break;
        	}
        	// clear IN0-7 IRQ
        	val32 &= 0xffffff00;
        	val32 |= 0x00000001 << num;
        	writel(val32, USB_REG_BASE + 0x188);
	}

	return;
}

static int ka_ep_queue(struct usb_ep *ep,
                struct usb_request *req, gfp_t gfp_flags)
{
        struct ka_ep *ka_ep = container_of(ep, struct ka_ep, ep);
        unsigned int phys;
        int num, len, in;

	//bh_context = ka_ep->req.context;	
        num = ka_ep->desc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
        in = (ka_ep->desc->bEndpointAddress & USB_DIR_IN) != 0;
        phys = (unsigned)req->buf;
        len = req->length;

        printf("ept%d %s queue len %x, buffer %x\n",
                        num, in ? "in" : "out", len, phys);

        if ((len > 0) && (num == 0) && (in != 0))
        {
		ep0_in(phys, len);
		return 0;
        }
        else if ((len > 0) && (num == 1) && (in != 0))
        {
		ep1_in(phys, len);
		return len;
        }
    	else if (in == 0)
    	{

		// read from EPxOUT buffer
        	if (num == 1)
        	{
		struct ka_request * ka_req;
		struct usb_request * temp = &(ka_ep->req);
printf("temp %x %x %x", temp, req, req->context);
		temp = req;
printf(" temp %x %x\n", temp, &(ka_ep->req));
		printf("Victor %x %x\n", (u8 *)ka_ep, (u8 *)&ka_ep_g[2]);


		ka_req = container_of(temp, struct ka_request, req);
		list_add_tail(&ka_req->queue, &ka_ep->queue);
		}
    	}
	return 0;
}

static int ka_pullup(struct usb_gadget *gadget, int is_on)
{
	return 0;
}

int usb_gadget_handle_interrupts(void)
{
        u32 value;
	int temp;

        value = readl(USB_IVECT/*&udc->usbirq*/);
printf("usb_gadget_handle_interrupts read %x\n", value);
        if (value)
	{
		printf("usb_gadget intr 0x%x\n", value);
		printf("INTPND1 0x%x\n", readl(0xa0006018));
		printf("INTPND2 0x%x\n", readl(0xa000601C));

        __asm__ __volatile__("mrs %0, spsr\n"
                             : "=r" (temp)
                             :
                             : "memory");
        printf("temp 0x%x\n", temp);

	__asm__ __volatile__(
        "mov r2, #65\n"
        "ldr r3, =0xa0004000\n"
        "strb  r2, [r3]\n");


		kaudc_isr();
	}

	//writel(~0x0, 0xa0006018);
	//writel(~0x0, 0xa000601c);
	writel(0, 0xa0006030);
	//writel(0x1, 0xa000602C);
#if 0
	// check for IN0-7 IRQ
	value = readl(USB_REG_BASE + 0x188);
	if ((value&0xff) == 0x01)
	{
		// clear IN0-7 IRQ
		value &= 0xffffff00;
		value |= 0x00000001;		
		writel(value, USB_REG_BASE + 0x188);

		//handshake stage
		value = readl(USB_REG_BASE);
		value &= 0xff00ffff;
		value |= 0x00020000;
		writel(value, USB_REG_BASE);
	}
#endif		
        return value;
}

/*
  Register entry point for the peripheral controller driver.
*/
int usb_gadget_register_driver(struct usb_gadget_driver *driver)
{
        struct ka_udc *dev = &controller;
        int retval = 0;

        printf("%s: %s\n", __func__, "no name");

        if (!driver
            || (driver->speed != USB_SPEED_FULL
                && driver->speed != USB_SPEED_HIGH)
            || !driver->bind || !driver->disconnect || !driver->setup)
                return -EINVAL;
        if (!dev)
                return -ENODEV;

	if (!kaudc_probe())
	{
		kausb_lowlevel_init();	
	}

//printf("driver->bind\n");
        retval = driver->bind(&dev->gadget);
        if (retval) {
                printf("%s: bind to driver --> error %d\n",
                            dev->gadget.name, retval);
                return retval;
        }

        printf("Registered gadget driver %s\n", dev->gadget.name);
	controller.driver = driver;

        return 0;
}

/*********************************************************/

static int ums_read_sector(struct ums_device *ums_dev,
                           unsigned int n, void *buf);
static int ums_write_sector(struct ums_device *ums_dev,
                            unsigned int n, void *buf);
static void ums_get_capacity(struct ums_device *ums_dev,
                             long long int *capacity);

static struct ums_board_info ums_board = {
	.read_sector = ums_read_sector,
	.write_sector = ums_write_sector,
	.get_capacity = ums_get_capacity,
	.name = "GONI UMS disk",
	.ums_dev = {
		.mmc = NULL,
		.dev_num = 0,
		.offset = 0,
		.part_size = 0.
	},
};

static int ums_read_sector(struct ums_device *ums_dev,
			   unsigned int n, void *buf)
{
	if (ums_dev->mmc->block_dev.block_read(ums_dev->dev_num,
					      n + ums_dev->offset, 1, buf) != 1)
		return -1;

	return 0;
}

static int ums_write_sector(struct ums_device *ums_dev,
			    unsigned int n, void *buf)
{
	if (ums_dev->mmc->block_dev.block_write(ums_dev->dev_num,
					      n + ums_dev->offset, 1, buf) != 1)
		return -1;

	return 0;
}

static void ums_get_capacity(struct ums_device *ums_dev,
			     long long int *capacity)
{
	long long int tmp_capacity;

	tmp_capacity = (long long int) ((ums_dev->offset + ums_dev->part_size)
				* SECTOR_SIZE);

printf("get_capacity %16llx %x %x %16llx %x %x %16llx %x %x\n", tmp_capacity, ums_dev->offset, ums_dev->part_size, ums_dev->mmc->capacity, ums_dev->mmc->version, ums_board.ums_dev.mmc->version, ums_board.ums_dev.mmc->capacity, &(ums_board.ums_dev), ums_dev);
	*capacity = ums_dev->mmc->capacity - tmp_capacity;
}

#define MEMORYSTORAGE 0x100000
unsigned int diskinfo[8192] = {0};

static void disk_image_init(void)
{
  int i;
  unsigned int wdata;

  for(i=0;i<sizeof(diskinfo);i=i+4){ 
    wdata = diskinfo[i+3]<<24 | diskinfo[i+2]<<16 | diskinfo[i+1]<<8 | diskinfo[i];    
    writel(wdata, MEMORYSTORAGE+i);
  };
}

struct ums_board_info *board_ums_init(unsigned int dev_num, unsigned int offset,
				      unsigned int part_size)
{
	struct mmc *mmc;

#if 0
	mmc = find_mmc_device(dev_num);
	if (!mmc)
		return NULL;

	ums_board.ums_dev.mmc = mmc;
	ums_board.ums_dev.dev_num = dev_num;
	ums_board.ums_dev.offset = offset;
	ums_board.ums_dev.part_size = part_size;

	/* Init MMC */
	mmc_init(mmc);
#endif

printf("capacity %x %16llx\n", ums_board.ums_dev.mmc->version, ums_board.ums_dev.mmc->capacity);

	disk_image_init();

	return &ums_board;
}
