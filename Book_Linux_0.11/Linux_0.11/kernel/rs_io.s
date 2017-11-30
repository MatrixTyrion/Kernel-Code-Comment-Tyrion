/*
 *	rs_io.s
 *
 * This module implements the rs232 io interrupts.
 */

.text
.globl _rs1_interrupt,_rs2_interrupt

size	= 1024				/* must be power of two !
					   and must match the value
					   in tty_io.c!!! */

/* these are the offsets into the read/write buffer structures */
rs_addr = 0
head = 4
tail = 8
proc_list = 12
buf = 16

startup	= 256		/* chars left in write queue when we restart it */

/*
 * These are the actual interrupt routines. They look where
 * the interrupt is coming from, and take appropriate action.
 */
.align 2
_rs1_interrupt:
	pushl $_table_list+8		//这里在tty_io.c中定义的table_list结构的基地址+8就是偏移量8个字节（就是table_list[1].read_q结构的首地址），这个结构的第一个成员是data（0x3f8），此行就是将它入栈
	jmp rs_int
.align 2
_rs2_interrupt:
	pushl $_table_list+16
rs_int:
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	push %es
	push %ds		/* as this is an interrupt, we cannot */
	pushl $0x10		/* know that bs is ok. Load it */
	pop %ds			//将ds设为0X10
	pushl $0x10
	pop %es			//将es设为0X10
	movl 24(%esp),%edx		//将指针指向$_table_list+8的地址0x3F8（esp+24）
	movl (%edx),%edx		//将地址上的值送入edx，值为0x3F8
	movl rs_addr(%edx),%edx
	addl $2,%edx		/* interrupt ident. reg */		//=0X3FA
rep_int:
	xorl %eax,%eax
	inb %dx,%al			//UART寄存器2（中断标识寄存器）
	testb $1,%al			//如果0位为1，那么无中断挂起，结束
	jne end
	cmpb $6,%al		/* this shouldn't happen, but ... */	//如果al>6，就是第3位设为1，就是字符超时，结束
	ja end
	movl 24(%esp),%ecx		//将$_table_list+8（table_list[1].read_q）的基地址送入ecx
	pushl %edx
	subl $2,%edx			//0X3F8
	call jmp_table(,%eax,2)		/* NOTE! not *4, bit0 is 0 already */
	popl %edx
	jmp rep_int			//循环，不停的检查是否有中断发生
end:	movb $0x20,%al		//向端口20发送命令20是结束中断
	outb %al,$0x20		/* EOI */
	pop %ds
	pop %es
	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	addl $4,%esp		# jump over _table_list entry
	iret

jmp_table:
	.long modem_status,write_char,read_char,line_status

.align 2
modem_status:				//读取寄存器状态使此0X3F8+6的寄存器的0—3位设置为0
	addl $6,%edx		/* clear intr by reading modem status reg */
	inb %dx,%al
	ret

.align 2
line_status:				//读取寄存器状态，使此0X3F8+5的寄存器设置为60H，表示没有错误，传送缓冲区为空
	addl $5,%edx		/* clear intr by reading line status reg. */
	inb %dx,%al
	ret

.align 2
read_char:
	inb %dx,%al			//0X3F8寄存器输入是获得接收缓冲区数据字节
	movl %ecx,%edx		//此时ecx中是table_list[1].read_q的地址
	subl $_table_list,%edx	//edx=8
	shrl $3,%edx			//edx=1
	movl (%ecx),%ecx		# read-queue	//将table_list[1].read_q的地址（ecx）中的值送入ecx，data值为3F8
	movl head(%ecx),%ebx		//将table_list[1].read_q结构中的第2个成员地址（head）中的值送入ebx，为0
	movb %al,buf(%ecx,%ebx)	//将字符数据放入结构中的char
	incl %ebx			//ebx=1
	andl $size-1,%ebx		//andl	3FF	%ebx
	cmpl tail(%ecx),%ebx		//第一次循环期间tail（%ecx）为0
	je 1f
	movl %ebx,head(%ecx)		//将结构成员head的值设为1
	pushl %edx
	call _do_tty_interrupt
	addl $4,%esp
1:	ret

.align 2
write_char:
	movl 4(%ecx),%ecx		# write-queue		//ecx=table_list[1].write_q
	movl head(%ecx),%ebx		//ebx=table_list[1].write_q结构中的head成员=0
	subl tail(%ecx),%ebx		//指向table_list[1].write_q结构中的tail成员=0
	andl $size-1,%ebx		# nr chars in queue
	je write_buffer_empty
	cmpl $startup,%ebx
	ja 1f
	movl proc_list(%ecx),%ebx	# wake up sleeping process
	testl %ebx,%ebx			# is there any?
	je 1f				//ebx=0时跳转
	movl $0,(%ebx)
1:	movl tail(%ecx),%ebx
	movb buf(%ecx,%ebx),%al	//将缓冲区的字节数据送入al，实际写数据的代码
	outb %al,%dx
	incl %ebx			//调整table_list[1].write_q中的状态
	andl $size-1,%ebx
	movl %ebx,tail(%ecx)
	cmpl head(%ecx),%ebx
	je write_buffer_empty
	ret
.align 2
write_buffer_empty:
	movl proc_list(%ecx),%ebx	# wake up sleeping process
	testl %ebx,%ebx			# is there any?
	je 1f
	movl $0,(%ebx)
1:	incl %edx			//0X3F9是中断开放寄存器，低4位有用
	inb %dx,%al
	jmp 1f
1:	jmp 1f
1:	andb $0xd,%al		/* disable transmit interrupt */
	outb %al,%dx
	ret
