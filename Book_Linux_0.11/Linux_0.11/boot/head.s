在网上本程序已有详细注解，因本人时间有限，在这里只对未说明的部分进行注解。
/*
 *  head.s contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00000000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory.
 */
.text
.globl _idt,_gdt,_pg_dir
_pg_dir:
startup_32:
//进入保护模式后，段寄存器就变成了选择子，选择子的结构：
//		16		    2    1  0
//		---------------------------
//		|	索引	  | TI	| RDL |
//		---------------------------
//其中TI=0时是从GDT中找描述符
	movl $0x10,%eax		//选择子=10，即索引为2，是GDT中第三个描述符，是DATA描述符
	mov %ax,%ds			//ds es fs gs都指向DATA描述符
	mov %ax,%es
	mov %ax,%fs
	mov %ax,%gs
	lss _stack_start,%esp	//ds送SS，esp指向_stack_start（在sched.c中定义）
	call setup_idt
	call setup_gdt
	movl $0x10,%eax		# reload all the segment registers
	mov %ax,%ds		# after changing gdt. CS was already
	mov %ax,%es		# reloaded in 'setup_gdt'
	mov %ax,%fs
	mov %ax,%gs
	lss _stack_start,%esp
	xorl %eax,%eax
1:	incl %eax		# check that A20 really IS enabled
//0X100000是1兆处，如果A20打开，那么0X000000和0X100000的值不一样，如果未打开，两处一样。
	movl %eax,0x000000
	cmpl %eax,0x100000
	je 1b
	movl %cr0,%eax		# check math chip
	andl $0x80000011,%eax	# Save PG,ET,PE
	testl $0x10,%eax				//测试协处理器，ET=1是387或487浮点协处理器
	jne 1f			# ET is set - 387 is present
	orl $4,%eax		# else set emulate bit	//模拟协处理器
1:	movl %eax,%cr0
	jmp after_page_tables

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
//描述符的一般格式：
//	7			      0
//	  -------------------------
//	0 |	    7-0偏移	      |
//	  -------------------------
//	1 |	   15-8偏移	      |
//	  -------------------------
//	2 |	  选择子低8位	      |
//	  -------------------------
//	3 |	  选择子高8位	      |
//	  -------------------------
//	4 | 0 | 0 | 0 |   字计数   |
//	  -------------------------
//	5 | P |  DPL  | 0 |  类型  |
//	  -------------------------
//	6 |	   23-16偏移	      |
//	  -------------------------
//	7 |	   31-24偏移	      |
//	  -------------------------

	lea ignore_int,%edx
//	7			      0
//	  -------------------------
//	0 |	 ignore_int的　     |
//	  -------------------------
//	1 |	  偏移的低16位      |
//	  -------------------------
//	2 |　   ０     |     ０    |
//	  -------------------------
//	3 |	 ０ 	｜　　 ８    |
//	  -------------------------
//	4 | 　　０　　 |   　０　  |
//	  -------------------------
//	5 | １ ０００　|    Ｅ  　 |
//	  -------------------------
//	6 |	 ignore_int的　     |
//	  -------------------------
//	7 |	 偏移的高16位 	      |
//	  -------------------------
	movl $0x00080000,%eax
	movw %dx,%ax		/* selector = 0x0008 = cs */
	movw $0x8E00,%dx	/* interrupt gate - dpl=0, present */

	lea _idt,%edi
	mov $256,%ecx
rp_sidt:
	movl %eax,(%edi)
	movl %edx,4(%edi)
	addl $8,%edi
	dec %ecx
	jne rp_sidt
	lidt idt_descr
	ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will beoverwritten by the page tables.
 */
setup_gdt:
	lgdt gdt_descr
	ret

.org 0x1000
pg0:

.org 0x2000
pg1:

.org 0x3000
pg2:		# This is not used yet, but if you
		# want to expand past 8 Mb, you'll have
		# to use it.

.org 0x4000
after_page_tables:				//将内核开始函数main及其参数推入堆栈
	pushl $0		# These are the parameters to main :-)
	pushl $0
	pushl $0
	pushl $L6		# return address for main, if it decides to.
	pushl $_main
	jmp setup_paging
L6:
	jmp L6			# main should never return here, but
				# just in case, we know what happens.

/* This is the default interrupt "handler" :-) */
.align 2
ignore_int:
	incb 0xb8000+160		# put something on the screen
	movb $2,0xb8000+161		# so that we know something
	iret				# happened


/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 8MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 8 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 8Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "8Mb"), but I
 * won't guarantee that's all :-( )
 */
.align 2
setup_paging:
	movl $1024*3,%ecx			//3个4K
	xorl %eax,%eax
	xorl %edi,%edi			/* pg_dir is at 0x000 */
	cld;rep;stosl				//stosl将eax中的值传入edi中
//1024个页目录项，这里只申明了2个（pg0  pg1)页表，每个页表寻址4MB，并且将每个页表项的低4位设为7，是说明页表有效
	movl $pg0+7,_pg_dir		/* set present bit/user r/w */
	movl $pg1+7,_pg_dir+4		/*  --------- " " --------- */
	movl $pg1+4092,%edi
	movl $0x7ff007,%eax		/*  8Mb - 4096 + 7 (r/w user,p) */		
	std
//下面的代码是反向填充PG0、PG1
1:	stosl			/* fill pages backwards - more efficient :-) */
	subl $0x1000,%eax
	jge 1b
	xorl %eax,%eax		/* pg_dir is at 0x0000 */
	movl %eax,%cr3		/* cr3 - page directory start */
	movl %cr0,%eax
	orl $0x80000000,%eax
	movl %eax,%cr0		/* set paging (PG) bit */
	ret			/* this also flushes prefetch-queue */

.align 2
.word 0
idt_descr:
	.word 256*8-1		# idt contains 256 entries
	.long _idt
.align 2
.word 0
gdt_descr:
	.word 256*8-1		# so does gdt (not that that's any
	.long _gdt		# magic number, but it works for me :^)

	.align 3
_idt:	.fill 256,8,0		# idt is uninitialized

_gdt:	.quad 0x0000000000000000	/* NULL descriptor */
//下面一行描述符的意思是：
//	  7   		    0
//	  -----------------
//	0 |1|1|1|1|1|1|1|1|
//	  -----------------
//	1 |0|0|0|0|0|1|1|1|
//	  -----------------
//	2 |0|0|0|0|0|0|0|0|
//	  -----------------
//	3 |0|0|0|0|0|0|0|0|
//	  -----------------
//	4 |0|0|0|0|0|0|0|0|
//	  -----------------
//	5 |1|0|0|1|1|0|1|0|
//	  -----------------
//	6 |1|1|0|0|0|0|0|0|
//	  -----------------
//	7 |0|0|0|0|0|0|0|0|
//	  -----------------
//我们看第5字节：第0位置0，表示此段未被存取过、未用过。第1位置1，表示此段是可写的。第2位置0，表示是按地址增加方向的
//		第3位置1，表示此段是可执行的。第4位置1，表示此段是用户段。第5、6位都置0表示特权级为0。第7位置1表示此段在存储器中。
//我们看第6字节：第6位置1操作数是32位的。第7位置1，说明段界限表示的长度是以4K字节为1页的页的数目。
	.quad 0x00c09a00000007ff	/* 8Mb */
	.quad 0x00c09200000007ff	/* 8Mb */
	.quad 0x0000000000000000	/* TEMPORARY - don't use */
	.fill 252,8,0			/* space for LDT's and TSS's etc */
