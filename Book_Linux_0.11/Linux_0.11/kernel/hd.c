#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

/*
 * This code handles all hd-interrupts, and read/write requests to
 * the hard-disk. It is relatively straigthforward (not obvious maybe,
 * but interrupts never are), while still being efficient, and never
 * disabling interrupts (except to overcome possible race-condition).
 * The elevator block-seek algorithm doesn't need to disable interrupts
 * due to clever programming.
 */

/* Max read/write errors/sector */
#define MAX_ERRORS	5
#define MAX_HD		2
#define NR_REQUEST	32

/*
 *  This struct defines the HD's and their types.
 *  Currently defined for CP3044's, ie a modified
 *  type 17.
 */
static struct hd_i_struct{
	int head,sect,cyl,wpcom,lzone,ctl;
	} hd_info[]= { HD_TYPE };

#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))

static struct hd_struct {
	long start_sect;
	long nr_sects;
} hd[5*MAX_HD]={{0,0},};			//一个硬盘可分5个区，共有2个硬盘

static struct hd_request {
	int hd;		/* -1 if no request */
	int nsector;
	int sector;
	int head;
	int cyl;
	int cmd;
	int errors;
	struct buffer_head * bh;
	struct hd_request * next;
} request[NR_REQUEST];

#define IN_ORDER(s1,s2) \
((s1)->hd<(s2)->hd || (s1)->hd==(s2)->hd && \
((s1)->cyl<(s2)->cyl || (s1)->cyl==(s2)->cyl && \
((s1)->head<(s2)->head || (s1)->head==(s2)->head && \
((s1)->sector<(s2)->sector))))

static struct hd_request * this_request = NULL;

static int sorting=0;

static void do_request(void);
static void reset_controller(void);
static void rw_abs_hd(int rw,unsigned int nr,unsigned int sec,unsigned int head,
	unsigned int cyl,struct buffer_head * bh);
void hd_init(void);

#define port_read(port,buf,nr) \
//下面的内嵌汇编的意思是：
//	movl	port	%edx
//	movl	buf	%edi
//	movl	nr	%ecx
//	cld
//	rep	insw		//从edx指定的port传送字到es：edi指定的buf中
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")	//其中outsw是从ds：si指定的buf中传送字到edx指定的port

extern void hd_interrupt(void);			//在system_call.S中定义

static struct task_struct * wait_for_request=NULL;

static inline void lock_buffer(struct buffer_head * bh)
{
	if (bh->b_lock)				//此缓冲区是否上锁
		printk("hd.c: buffer multiply locked\n");
	bh->b_lock=1;
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("hd.c: free buffer being unlocked\n");
	bh->b_lock=0;
	wake_up(&bh->b_wait);
}

static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);		//进入等待对列
	sti();
}

void rw_hd(int rw, struct buffer_head * bh)	//缓冲区和磁盘之间交换数据
{
	unsigned int block,dev;
	unsigned int sec,head,cyl;

	block = bh->b_blocknr << 1;			//默认1块=1024字节，二个扇区
	dev = MINOR(bh->b_dev);
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects)	//是否超过了分区，此分区是否满了
		return;
	block += hd[dev].start_sect;		//此分区的开始扇区+block，即在硬盘中定开始的位置
	dev /= 5;					//看看是哪个硬盘
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),	
		"r" (hd_info[dev].sect));					//确定扇区号
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));					//确定光头号
	rw_abs_hd(rw,dev,sec+1,head,cyl,bh);	//实际写盘函数
}

/* This may be used only once, enforced by 'static int callable' */
int sys_setup(void)
{
	static int callable = 1;
	int i,drive;
	struct partition *p;

	if (!callable)
		return -1;
	callable = 0;
	for (drive=0 ; drive<NR_HD ; drive++) {
		rw_abs_hd(READ,drive,1,0,0,(struct buffer_head *) start_buffer);	//读取引导扇区512字节到start_buffer中
		if (!start_buffer->b_uptodate) {
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		if (start_buffer->b_data[510] != 0x55 || (unsigned char)
		    start_buffer->b_data[511] != 0xAA) {				//分析引导扇区是否合法，合法应为55AA
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		p = 0x1BE + (void *)start_buffer->b_data;		//指针指向分区信息的开始位置
		for (i=1;i<5;i++,p++) {				//保存分区信息
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
	}
	printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	mount_root();				//在super.c中定义
	return (0);
}

/*
 * This is the pointer to a routine to be executed at every hd-interrupt.
 * Interesting way of doing things, but should be rather practical.
 */
void (*do_hd)(void) = NULL;

static int controller_ready(void)
{
	int retries=1000;

	while (--retries && (inb(HD_STATUS)&0xc0)!=0x40);	//0x40是第六个位置1为驱动器准备好，0x1f7端口是读取硬盘状态
	return (retries);
}

static int win_result(void)
{
	int i=inb(HD_STATUS);	//读取硬盘状态

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);	//0x1f1是硬盘错误寄存器，检查上一个命令是否出错
	return (1);
}

static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");

	if (drive>1 || head>15)	//只支持1个硬盘和14个头
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	do_hd = intr_addr;		//硬盘写或读操作指针
	outb(_CTL,HD_CMD);		//0x3f6是硬盘控制寄存器，置0为控制器为普通操作
	port=HD_DATA;			//0x1f0硬盘数据寄存器
	outb_p(_WPCOM,++port);	//0x1f1，写预补偿，在从磁柱300X4处开始写
	outb_p(nsect,++port);	//0x1f2是硬盘扇区记数寄存器，将要传送的扇区数放入该寄存器
	outb_p(sect,++port);		//0x1f3是硬盘扇区号寄存器，记录当前的扇区号
	outb_p(cyl,++port);		//0x1f4硬盘磁柱低寄存器，保存起始磁柱号的低8字节
	outb_p(cyl>>8,++port);	//0x1f5硬盘磁柱高寄存器
	outb_p(0xA0|(drive<<4)|head,++port);	//0x1f6硬盘驱动器和磁头寄存器，这里选择主盘（高4位为1010），低4位为磁头号
	outb(cmd,++port);		//0x1f7硬盘命令输出寄存器，主要向硬盘发出命令
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 100000; i++)	//循环，直到驱动器为READY_STAT或超时
		if (READY_STAT == (inb(HD_STATUS) & (BUSY_STAT | READY_STAT)))
			break;
	i = inb(HD_STATUS);			//再读硬盘状态
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == READY_STAT | SEEK_STAT)	//如果处于读状态或寻找状态，OK
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);		//0x3f6是硬盘适配器控制寄存器，第二位置1为重启控制器
	for(i = 0; i < 1000; i++) nop();	//等待一会儿
	outb(0,HD_CMD);		//使驱动器处于普通状态
	for(i = 0; i < 10000 && drive_busy(); i++) /* nothing */;
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if((i = inb(ERR_STAT)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,_SECT,_SECT,_HEAD-1,_CYL,WIN_SPECIFY,&do_request);	//do_request是重新处理请求队列
}

void unexpected_hd_interrupt(void)
{
	panic("Unexpected HD interrupt\n\r");
}

static void bad_rw_intr(void)
{
	int i = this_request->hd;

	if (this_request->errors++ >= MAX_ERRORS) {	//如果错误超过有效的最大错误数，放弃这次请求
		this_request->bh->b_uptodate = 0;
		unlock_buffer(this_request->bh);
		wake_up(&wait_for_request);
		this_request->hd = -1;
		this_request=this_request->next;
	}
	reset_hd(i);
}

static void read_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		return;
	}
	port_read(HD_DATA,this_request->bh->b_data+
		512*(this_request->nsector&1),256);	//是从b_data指定位置读512字节
	this_request->errors = 0;
	if (--this_request->nsector)		//如果还有要传的数据，直接返回，不从请求队列中删除该请求
		return;
//以下6行代码是指数据传送完毕，就从请求队列中删除该请求
	this_request->bh->b_uptodate = 1;
	this_request->bh->b_dirt = 0;
	wake_up(&wait_for_request);
	unlock_buffer(this_request->bh);
	this_request->hd = -1;
	this_request=this_request->next;
	do_request();				//处理其他请求
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();		//如果有错误，重启驱动器，再继续处理请求
		return;
	}
	if (--this_request->nsector) {
		port_write(HD_DATA,this_request->bh->b_data+512,256);	//向硬盘数据寄存器写数据
		return;
	}
	this_request->bh->b_uptodate = 1;
	this_request->bh->b_dirt = 0;
	wake_up(&wait_for_request);
	unlock_buffer(this_request->bh);
	this_request->hd = -1;
	this_request=this_request->next;
	do_request();				//处理下一个请求
}

static void do_request(void)
{
	int i,r;

	if (sorting)
		return;
	if (!this_request) {
		do_hd=NULL;
		return;
	}
	if (this_request->cmd == WIN_WRITE) {
		hd_out(this_request->hd,this_request->nsector,this_request->
			sector,this_request->head,this_request->cyl,
			this_request->cmd,&write_intr);
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)	//循环，直到控制器指示准备好
			/* nothing */ ;
		if (!r) {					//如果超时驱动器仍未准备好，重设该驱动器
			reset_hd(this_request->hd);
			return;
		}
		port_write(HD_DATA,this_request->bh->b_data+
			512*(this_request->nsector&1),256);		//传送数据
	} else if (this_request->cmd == WIN_READ) {
		hd_out(this_request->hd,this_request->nsector,this_request->
			sector,this_request->head,this_request->cyl,
			this_request->cmd,&read_intr);
	} else
		panic("unknown hd-command");
}

/*
 * add-request adds a request to the linked list.
 * It sets the 'sorting'-variable when doing something
 * that interrupts shouldn't touch.
 */
static void add_request(struct hd_request * req)
{
	struct hd_request * tmp;

	if (req->nsector != 2)
		panic("nsector!=2 not implemented");
/*
 * Not to mess up the linked lists, we never touch the two first
 * entries (not this_request, as it is used by current interrups,
 * and not this_request->next, as it can be assigned to this_request).
 * This is not too high a price to pay for the ability of not
 * disabling interrupts.
 */
	sorting=1;
	if (!(tmp=this_request))
		this_request=req;
	else {
		if (!(tmp->next))
			tmp->next=req;
		else {
			tmp=tmp->next;
			for ( ; tmp->next ; tmp=tmp->next)
				if ((IN_ORDER(tmp,req) ||
				    !IN_ORDER(tmp,tmp->next)) &&
				    IN_ORDER(req,tmp->next))
					break;
			req->next=tmp->next;
			tmp->next=req;
		}
	}
	sorting=0;
/*
 * NOTE! As a result of sorting, the interrupts may have died down,
 * as they aren't redone due to locking with sorting=1. They might
 * also never have started, if this is the first request in the queue,
 * so we restart them if necessary.
 */
	if (!do_hd)
		do_request();
}

void rw_abs_hd(int rw,unsigned int nr,unsigned int sec,unsigned int head,
	unsigned int cyl,struct buffer_head * bh)					//根据硬盘参数做磁盘读写
{
	struct hd_request * req;

	if (rw!=READ && rw!=WRITE)
		panic("Bad hd command, must be R/W");
	lock_buffer(bh);
repeat:
	for (req=0+request ; req<NR_REQUEST+request ; req++)
		if (req->hd<0)					//hd=-1表示没有请求
			break;
	if (req==NR_REQUEST+request) {				//不能超过最大的请求数
		sleep_on(&wait_for_request);
		goto repeat;
	}
	req->hd=nr;
	req->nsector=2;						//==1024字节
	req->sector=sec;
	req->head=head;
	req->cyl=cyl;
	req->cmd = ((rw==READ)?WIN_READ:WIN_WRITE);
	req->bh=bh;
	req->errors=0;
	req->next=NULL;
	add_request(req);
	wait_on_buffer(bh);						//进入等待
}

void hd_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {				//初始化请求队列
		request[i].hd = -1;
		request[i].next = NULL;
	}
	for (i=0 ; i<NR_HD ; i++) {					//初始化硬盘分区
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;
	}
	set_trap_gate(0x2E,&hd_interrupt);				//登记硬盘中断处理程序，中断号为0x2E
	outb_p(inb_p(0x21)&0xfb,0x21);				//开放IRQ2（中断控制器1）
	outb(inb_p(0xA1)&0xbf,0xA1);				//开放IRQ14，硬盘控制器
}
