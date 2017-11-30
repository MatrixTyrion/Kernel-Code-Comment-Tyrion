#define outb(value,port) \
//下面的内嵌汇编的意思是：
//	movl	value	%eax
//	movl	port	%edx
//	outb	%dx
__asm__ ("outb %%al,%%dx"::"a" (value),"d" (port))


#define inb(port) ({ \
unsigned char _v; \
//下面的内嵌汇编的意思是：
//	movl	port	%edx
//	inb	%dx	%al
//	movl	%eax	_v	
__asm__ volatile ("inb %%dx,%%al":"=a" (_v):"d" (port)); \
_v; \
})

#define outb_p(value,port) \
//下面的内嵌汇编的意思是：
//	movl	value	%eax
//	movl	port	%edx
//	outb	%al	%dx
//	jmp	1f
//	1:	jmp	1f
//	1:
__asm__ ("outb %%al,%%dx\n" \
		"\tjmp 1f\n" \
		"1:\tjmp 1f\n" \
		"1:"::"a" (value),"d" (port))

#define inb_p(port) ({ \
unsigned char _v; \
//下面的内嵌汇编的意思是：
//	movl	port	%edx
//	inb	%dx	%al
//	jmp	1f
//	1:	jmp	1f
//	1:	movl	%eax	_v
__asm__ volatile ("inb %%dx,%%al\n" \
	"\tjmp 1f\n" \
	"1:\tjmp 1f\n" \
	"1:":"=a" (_v):"d" (port)); \
_v; \
})
