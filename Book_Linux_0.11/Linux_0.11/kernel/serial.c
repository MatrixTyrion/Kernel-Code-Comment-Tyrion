/*
 *	serial.c
 *
 * This module implements the rs232 io functions
 *	void rs_write(struct tty_struct * queue);
 *	void rs_init(void);
 * and all interrupts pertaining to serial IO.
 */

#include <linux/tty.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/io.h>

#define WAKEUP_CHARS (TTY_BUF_SIZE/4)

extern void rs1_interrupt(void);
extern void rs2_interrupt(void);

static void init(int port)
{
	outb_p(0x80,port+3);	/* set DLAB of line control reg */		//线控制寄存器，将第7位置1是开放除法因子锁存器地址位
	outb_p(0x30,port);	/* LS of divisor (48 -> 2400 bps */	//将波特率设为2400（低有效位）
	outb_p(0x00,port+1);	/* MS of divisor */				//将波特率的最高有效位保存入该寄存器
	outb_p(0x03,port+3);	/* reset DLAB */				//将DLAB设为0，幀设为8位（通常是8位）
	outb_p(0x0b,port+4);	/* set DTR,RTS, OUT_2 */			//设置数据终端准备好线（DTR）的状态，设置请求发送线（RTS）的状态，out_2用于开放中断请求
	outb_p(0x0d,port+1);	/* enable all intrs but writes */
	(void)inb(port);	/* read data port to reset things (?) */	//获取接收缓冲区数据字节
}

void rs_init(void)
{
	set_intr_gate(0x24,rs1_interrupt);			//串行口端口3F8的中断好是0x24
	set_intr_gate(0x23,rs2_interrupt);			//串行口端口2F8的中断好是0x23
	init(tty_table[1].read_q.data);			//初始化2个串行端口
	init(tty_table[2].read_q.data);
	outb(inb_p(0x21)&0xE7,0x21);			//控制中断控制器对中断请求线0--7的操作
}

/*
 * This routine gets called when tty_write has put something into
 * the write_queue. It must check wheter the queue is empty, and
 * set the interrupt register accordingly
 *
 *	void _rs_write(struct tty_struct * tty);
 */
void rs_write(struct tty_struct * tty)
{
	cli();
	if (!EMPTY(tty->write_q))
		outb(inb_p(tty->write_q.data+1)|0x02,tty->write_q.data+1);	//将寄存器的第2位（传送位）置1，开放传送中断
	sti();
}
