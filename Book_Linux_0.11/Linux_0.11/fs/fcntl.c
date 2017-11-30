#include <string.h>
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);

static int dupfd(unsigned int fd, unsigned int arg)	//拷贝本进程打开的fd文件到本进程空的filp[arg]中
{
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	if (arg >= NR_OPEN)
		return -EMFILE;
	current->close_on_exec &= ~(1<<arg);
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

int sys_dup2(unsigned int oldfd, unsigned int newfd)	//在本进程中用打开的oldfd文件替换newfd所指向的文件
{
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

int sys_dup(unsigned int fildes)		//在本进程内部拷贝一个打开的文件
{
	return dupfd(fildes,0);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))	//文件必须是打开的
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:
			return dupfd(fd,arg);		//拷贝一个打开的文件
		case F_GETFD:
			return (current->close_on_exec>>fd)&1;	//取这个文件在进程中的状态
		case F_SETFD:
			if (arg&1)
				current->close_on_exec |= (1<<fd);
			else
				current->close_on_exec &= ~(1<<fd);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:	case F_SETLK:	case F_SETLKW:
			return -1;
		default:
			return -1;
	}
}
