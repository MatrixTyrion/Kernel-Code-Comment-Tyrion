#define __LIBRARY__
#include <unistd.h>

volatile void _exit(int exit_code)
{
//此内嵌汇编的意思是：
//	movl _NR_exit %eax
//	movl exit_code %ebx
//	int $0x80	//$0x80是LINUX的中断调用入口
	__asm__("int $0x80"::"a" (__NR_exit),"b" (exit_code));
}
