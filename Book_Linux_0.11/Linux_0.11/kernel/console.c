/*
 *	console.c
 *
 * This module implements the console io functions
 *	'void con_init(void)'
 *	'void con_write(struct tty_queue * queue)'
 * Hopefully this will be a rather complete VT102 implementation.
 *
 */

/*
 *  NOTE!!! We sometimes disable and enable interrupts for a short while
 * (to put a word in video IO), but this will work even for keyboard
 * interrupts. We know interrupts aren't enabled when getting a keyboard
 * interrupt, as we use trap-gates. Hopefully all is well.
 */

#include <linux/sched.h>
#include <linux/tty.h>
#include <asm/io.h>
#include <asm/system.h>

#define SCREEN_START 0xb8000	//0Xb8000---0XbFFFF是显存的地址
#define SCREEN_END   0xc0000
#define LINES 25
#define COLUMNS 80
#define NPAR 16

extern void keyboard_interrupt(void);

static unsigned long origin=SCREEN_START;
static unsigned long scr_end=SCREEN_START+LINES*COLUMNS*2;
static unsigned long pos;
static unsigned long x,y;
static unsigned long top=0,bottom=LINES;
static unsigned long lines=LINES,columns=COLUMNS;
static unsigned long state=0;
static unsigned long npar,par[NPAR];
static unsigned long ques=0;
static unsigned char attr=0x07;

/*
 * this is what the terminal answers to a ESC-Z or csi0c
 * query (= vt100 response).
 */
#define RESPONSE "\033[?1;2c"		//\033=0X1B=ESC键

static inline void gotoxy(unsigned int new_x,unsigned int new_y)
{
	if (new_x>=columns || new_y>=lines)
		return;
	x=new_x;
	y=new_y;
	pos=origin+((y*columns+x)<<1);	//乘2是因为描述位置是字
}

static inline void set_origin(void)
{
	cli();
//下面二个寄存器处理屏幕滚动，寄存器12是开始地址的高字，13是低字
//0x3d4是6845索引寄存器，它可以选择18个寄存器，但必须在读/写0x3d5之前，18个寄存器中前10个是处理垂直和水平显示参数的，不要动
	outb_p(12,0x3d4);
	outb_p(0xff&((origin-SCREEN_START)>>9),0x3d5);
	outb_p(13,0x3d4);
	outb_p(0xff&((origin-SCREEN_START)>>1),0x3d5);
	sti();
}

static void scrup(void)
{
	if (!top && bottom==lines) {
		origin += columns<<1;
		pos += columns<<1;
		scr_end += columns<<1;
		if (scr_end>SCREEN_END) {
//以下的内嵌汇编意思是：
//	movl	0X0720		%eax		//其中0X0720中的07表示属性为灰，20代表空白键
//	movl	(line-1)*columns>>1	%ecx
//	movl	SCREEN_START	%edi
//	movl	origin		%esi
//	cld					//地址增量
//	rep	movsl				//ds:si --->es:di
//	movl	_columns	%ecx
//	rep	stosw				//将eax中的内容传入ES：DI中
			__asm__("cld\n\t"
				"rep\n\t"
				"movsl\n\t"
				"movl _columns,%1\n\t"
				"rep\n\t"
				"stosw"
				::"a" (0x0720),
				"c" ((lines-1)*columns>>1),
				"D" (SCREEN_START),
				"S" (origin)
				:"cx","di","si");
			scr_end -= origin-SCREEN_START;
			pos -= origin-SCREEN_START;
			origin = SCREEN_START;
		} else {
//以下的内嵌汇编意思是：
//	movl	0X07200720	%eax
//	movl	columns>>1	%ecx		//因为传送的是双字，所以要除2
//	movl	scr_end-(columns<<1)	%edi
//	cld
//	rep	stosl				//双字传送
			__asm__("cld\n\t"
				"rep\n\t"
				"stosl"
				::"a" (0x07200720),
				"c" (columns>>1),
				"D" (scr_end-(columns<<1))
				:"cx","di");
		}
		set_origin();
	} else {
//以下的内嵌汇编意思是：
//	movl	0X0720		%eax
//	movl	(bottom-top-1)*columns>>1	%ecx
//	movl	origin+(columns<<1)*top		%edi
//	movl	origin+(columns<<1)*(top+1)		%esi
//	cld
//	rep	movsl
//	movl	_columns	%ecx
//	rep	stosw
		__asm__("cld\n\t"
			"rep\n\t"
			"movsl\n\t"
			"movl _columns,%%ecx\n\t"
			"rep\n\t"
			"stosw"
			::"a" (0x0720),
			"c" ((bottom-top-1)*columns>>1),
			"D" (origin+(columns<<1)*top),
			"S" (origin+(columns<<1)*(top+1))
			:"cx","di","si");
	}
}

static void scrdown(void)
{
//以下的内嵌汇编意思是：
//	movl	0X0720		%eax
//	movl	(bottom-top-1)*columns	%ecx
//	movl	(origin+(columns<<1)*(bottom-1)-4)	%esi
//	std				//地址减量
//	rep	movsl
//	addl	$2	%%edi
//	movl	_columns	%ecx
//	stosw
	__asm__("std\n\t"
		"rep\n\t"
		"movsl\n\t"
		"addl $2,%%edi\n\t"	/* %edi has been decremented by 4 */
		"movl _columns,%%ecx\n\t"
		"rep\n\t"
		"stosw"
		::"a" (0x0720),
		"c" ((bottom-top-1)*columns>>1),
		"D" (origin+(columns<<1)*bottom-4),
		"S" (origin+(columns<<1)*(bottom-1)-4)
		:"ax","cx","di","si");
}

static void lf(void)
{
	if (y+1<bottom) {
		y++;
		pos += columns<<1;
		return;
	}
	scrup();			//如果超过了底部，屏向上走一行
}

static void ri(void)
{
	if (y>top) {
		y--;
		pos -= columns<<1;
		return;
	}
	scrdown();			//如果y<top，就是超过了屏的顶部，就向下走一行
}

static void cr(void)
{
	pos -= x<<1;
	x=0;
}

static void del(void)
{
	if (x) {
		pos -= 2;
		x--;
		*(unsigned short *)pos = 0x0720;	//将删除的这个字填为空白，属性为灰
	}
}

static void csi_J(int par)		//这是处理整个显示区的擦除函数
{
	long count __asm__("cx");
	long start __asm__("di");

	switch (par) {
		case 0:	/* erase from cursor to end of display */
			count = (scr_end-pos)>>1;
			start = pos;
			break;
		case 1:	/* erase from start to cursor */
			count = (pos-origin)>>1;
			start = origin;
			break;
		case 2: /* erase whole display */
			count = columns*lines;
			start = origin;
			break;
		default:
			return;
	}
//以下的内嵌汇编意思是：
//	movl	count	%ecx
//	movl	start	%edi
//	movl	0X0720	%eax
//	cld
//	rep	stosw
	__asm__("cld\n\t"
		"rep\n\t"
		"stosw\n\t"
		::"c" (count),
		"D" (start),"a" (0x0720)
		:"cx","di");
}

static void csi_K(int par)
{
	long count __asm__("cx");
	long start __asm__("di");

	switch (par) {
		case 0:	/* erase from cursor to end of line */
			if (x>=columns)
				return;
			count = columns-x;
			start = pos;
			break;
		case 1:	/* erase from start of line to cursor */
			start = pos - (x<<1);
			count = (x<columns)?x:columns;
			break;
		case 2: /* erase whole line */
			start = pos - (x<<1);
			count = columns;
			break;
		default:
			return;
	}
	__asm__("cld\n\t"			//同上
		"rep\n\t"
		"stosw\n\t"
		::"c" (count),
		"D" (start),"a" (0x0720)
		:"cx","di");
}

void csi_m(void)				//修改部分区域的字符属性
{
	int i;

	for (i=0;i<=npar;i++)
		switch (par[i]) {
			case 0:attr=0x07;break;
			case 1:attr=0x0f;break;
			case 4:attr=0x0f;break;
			case 7:attr=0x70;break;
			case 27:attr=0x07;break;
		}
}

static inline void set_cursor(void)
{
	cli();
//寄存器14控制光标位置（高位字），寄存器15控制低位字
	outb_p(14,0x3d4);
	outb_p(0xff&((pos-SCREEN_START)>>9),0x3d5);
	outb_p(15,0x3d4);
	outb_p(0xff&((pos-SCREEN_START)>>1),0x3d5);
	sti();
}

static void respond(struct tty_struct * tty)
{
	char * p = RESPONSE;

	cli();
	while (*p) {
		PUTCH(*p,tty->read_q);
		p++;
	}
	sti();
	copy_to_cooked(tty);			//此函数在tty_io.c中定义
}

static void insert_char(void)
{
	int i=x;
	unsigned short tmp,old=0x0720;
	unsigned short * p = (unsigned short *) pos;

	while (i++<columns) {		//将光标的当前位置填空为0X0720，然后循环，把当前位置的老的字符向后移，然后将后面的所有字符向后移一位置
		tmp=*p;
		*p=old;
		old=tmp;
		p++;
	}
}

static void insert_line(void)
{
	int oldtop,oldbottom;

	oldtop=top;
	oldbottom=bottom;
	top=y;				//top是当前光标位置的Y轴，即为当前屏幕的第Y行
	bottom=lines;
	scrdown();
	top=oldtop;
	bottom=oldbottom;
}

static void delete_char(void)
{
	int i;
	unsigned short * p = (unsigned short *) pos;

	if (x>=columns)
		return;
	i = x;				//X为当前光标的X轴，即为当前屏幕的X列
	while (++i < columns) {
		*p = *(p+1);
		p++;
	}
	*p=0x0720;			//将最后一个字符填为0X0720
}

static void delete_line(void)
{
	int oldtop,oldbottom;

	oldtop=top;
	oldbottom=bottom;
	top=y;
	bottom=lines;
	scrup();
	top=oldtop;
	bottom=oldbottom;
}

static void csi_at(int nr)
{
	if (nr>columns)
		nr=columns;
	else if (!nr)			//如果nr为0，那么nr=1，这样是保证插入一个字符
		nr=1;
	while (nr--)
		insert_char();
}

static void csi_L(int nr)
{
	if (nr>lines)
		nr=lines;
	else if (!nr)
		nr=1;
	while (nr--)
		insert_line();
}

static void csi_P(int nr)
{
	if (nr>columns)
		nr=columns;
	else if (!nr)
		nr=1;
	while (nr--)
		delete_char();
}

static void csi_M(int nr)
{
	if (nr>lines)
		nr=lines;
	else if (!nr)
		nr=1;
	while (nr--)
		delete_line();
}

static int saved_x=0;
static int saved_y=0;

static void save_cur(void)
{
	saved_x=x;
	saved_y=y;
}

static void restore_cur(void)
{
	x=saved_x;
	y=saved_y;
	pos=origin+((y*columns+x)<<1);		//还原光标位置
}

void con_write(struct tty_struct * tty)
{
	int nr;
	char c;

	nr = CHARS(tty->write_q);			//tty->write_q中有多少没有被处理的字符
	while (nr--) {
		GETCH(tty->write_q,c);
		switch(state) {			//state默认为0
			case 0:
				if (c>31 && c<127) {		//是有效可写字符？
					if (x>=columns) {	//需要换行吗？
						x -= columns;	//将光标X轴指向本行的行首
						pos -= columns<<1;
						lf();		//将光标指向下一行
					}
//下面内嵌汇编的意思是：
//	movl	c	%eax
//	movb	0X70	%ah
//	movw	%ax	*pos
					__asm__("movb _attr,%%ah\n\t"
						"movw %%ax,%1\n\t"
						::"a" (c),"m" (*(short *)pos)	//其中m表示操作数是内存变量
						:"ax");
					pos += 2;		//指向下一个位置
					x++;
				} else if (c==27)		//27=0X1B，是否为ESC键
					state=1;
				else if (c==10 || c==11 || c==12)
					lf();
				else if (c==13)		//13=0X0D，是否为ENTER键
					cr();
				else if (c==ERASE_CHAR(tty))
					del();
				else if (c==8) {		//8是空白键（BACKSPACE）
					if (x) {
						x--;
						pos -= 2;
					}
				} else if (c==9) {		//TAB键
					c=8-(x&7);		//只能跳过最大8个字符
					x += c;
					pos += c<<1;
					if (x>columns) {
						x -= columns;
						pos -= columns<<1;
						lf();
					}
					c=9;
				}
				break;
			case 1:
				state=0;
				if (c=='[')
					state=2;
				else if (c=='E')
					gotoxy(0,y+1);	//到下行的开头位置
				else if (c=='M')
					ri();			//光标到上行的当前位置
				else if (c=='D')
					lf();			//光标到下行的当前位置
				else if (c=='Z')
					respond(tty);
				else if (x=='7')
					save_cur();
				else if (x=='8')
					restore_cur();
				break;
			case 2:
				for(npar=0;npar<NPAR;npar++)
					par[npar]=0;
				npar=0;
				state=3;
				if (ques=(c=='?'))
					break;
			case 3:
				if (c==';' && npar<NPAR-1) {
					npar++;
					break;
				} else if (c>='0' && c<='9') {
					par[npar]=10*par[npar]+c-'0';
					break;
				} else state=4;
			case 4:				//光标控制
				state=0;
				switch(c) {
					case 'G': case '`':
						if (par[0]) par[0]--;
						gotoxy(par[0],y);
						break;
					case 'A':
						if (!par[0]) par[0]++;
						gotoxy(x,y-par[0]);
						break;
					case 'B': case 'e':
						if (!par[0]) par[0]++;
						gotoxy(x,y+par[0]);
						break;
					case 'C': case 'a':
						if (!par[0]) par[0]++;
						gotoxy(x+par[0],y);
						break;
					case 'D':
						if (!par[0]) par[0]++;
						gotoxy(x-par[0],y);
						break;
					case 'E':
						if (!par[0]) par[0]++;
						gotoxy(0,y+par[0]);
						break;
					case 'F':
						if (!par[0]) par[0]++;
						gotoxy(0,y-par[0]);
						break;
					case 'd':
						if (par[0]) par[0]--;
						gotoxy(x,par[0]);
						break;
					case 'H': case 'f':
						if (par[0]) par[0]--;
						if (par[1]) par[1]--;
						gotoxy(par[1],par[0]);
						break;
					case 'J':
						csi_J(par[0]);
						break;
					case 'K':
						csi_K(par[0]);
						break;
					case 'L':
						csi_L(par[0]);
						break;
					case 'M':
						csi_M(par[0]);
						break;
					case 'P':
						csi_P(par[0]);
						break;
					case '@':
						csi_at(par[0]);
						break;
					case 'm':
						csi_m();
						break;
					case 'r':
						if (par[0]) par[0]--;
						if (!par[1]) par[1]=lines;
						if (par[0] < par[1] &&
						    par[1] <= lines) {
							top=par[0];
							bottom=par[1];
						}
						break;
					case 's':
						save_cur();
						break;
					case 'u':
						restore_cur();
						break;
				}
		}
	}
	set_cursor();						//实际硬件处理光标
}

/*
 *  void con_init(void);
 *
 * This routine initalizes console interrupts, and does nothing
 * else. If you want the screen to clear, call tty_write with
 * the appropriate escape-sequece.
 */
void con_init(void)
{
	register unsigned char a;

	gotoxy(*(unsigned char *)(0x90000+510),*(unsigned char *)(0x90000+511));	//其中0X90000+510是在boot.s中已将光标X、Y保存在0X90000+510处
	set_trap_gate(0x21,&keyboard_interrupt);		//定义IDT[21]中断号是键盘中断处理程序，键盘中断处理程序在keyboard.s中
	outb_p(inb_p(0x21)&0xfd,0x21);			//屏蔽IRQ1
	a=inb_p(0x61);
	outb_p(a|0x80,0x61);					//禁止端口60H开关，开放键盘数据，允许键盘IRQ
	outb(a,0x61);						//恢复
}
