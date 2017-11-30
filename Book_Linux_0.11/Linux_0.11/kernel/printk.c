/*
 * When in kernel-mode, we cannot use printf, as fs is liable to
 * point to 'interesting' things. Make a printf with fs-saving, and
 * all is well.
 */
#include <stdarg.h>
#include <stddef.h>

#include <linux/kernel.h>

static char buf[1024];

int printk(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);			//定位要打印的串在args中的开始位置
	i=vsprintf(buf,fmt,args);		//要打印的字符个数
	va_end(args);
	__asm__("push %%fs\n\t"
		"push %%ds\n\t"
		"pop %%fs\n\t"
		"pushl %0\n\t"		//要打印的字符串个数i，tty_write函数的第3个参数
		"pushl $_buf\n\t"		//要打印的字符串指针，tty_write函数的第2个参数
		"pushl $0\n\t"		//tty_write函数的第一个参数，为0
		"call _tty_write\n\t"
		"addl $8,%%esp\n\t"
		"popl %0\n\t"
		"pop %%fs"
		::"r" (i):"ax","cx","dx");
	return i;
}
