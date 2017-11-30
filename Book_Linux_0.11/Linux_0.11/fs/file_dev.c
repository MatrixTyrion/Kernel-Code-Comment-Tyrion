#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

	if ((left=count)<=0)
		return 0;
	while (left) {
		if (nr = bmap(inode,(filp->f_pos)/BLOCK_SIZE)) {	//取得要读取的文件块号
			if (!(bh=bread(inode->i_dev,nr)))
				break;
		} else
			bh = NULL;
		nr = filp->f_pos % BLOCK_SIZE;			//取得在该块中的位移
		chars = MIN( BLOCK_SIZE-nr , left );
		filp->f_pos += chars;		//重新设置2个参数，为读下一块做准备
		left -= chars;
		if (bh) {
			char * p = nr + bh->b_data;		//指针指向块中的数据区
			while (chars-->0)
				put_fs_byte(*(p++),buf++);	//读取数据到指定缓冲区buf
			brelse(bh);			//释放缓冲区
		} else {
			while (chars-->0)		//到文件尾
				put_fs_byte(0,buf++);
		}
	}
	inode->i_atime = CURRENT_TIME;
	return (count-left)?(count-left):-ERROR;		//返回读取的字符数
}

int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	if (filp->f_flags & O_APPEND)		//如果是在现有文件尾增加数据
		pos = inode->i_size;			//位移指向文件尾
	else
		pos = filp->f_pos;			//位移指向当前光标位置
	while (i<count) {
		if (!(block = create_block(inode,pos/BLOCK_SIZE)))	//如有必要，在盘中创建新块
			break;
		if (!(bh=bread(inode->i_dev,block)))		//读取该块
			break;
		c = pos % BLOCK_SIZE;		//取得在该块中的位移
		p = c + bh->b_data;			//指针指向该位置
		bh->b_dirt = 1;			//令bh将来写盘
		c = BLOCK_SIZE-c;			//在该块中还可以写多少字符
		if (c > count-i) c = count-i;
		pos += c;				//重新计算位移
		if (pos > inode->i_size) {		//如果文件增大
			inode->i_size = pos;		//重新设置文件尺寸
			inode->i_dirt = 1;
		}
		i += c;
		while (c-->0)
			*(p++) = get_fs_byte(buf++);	//将buf中的数据写入该块的数据区
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME;
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME;
	}
	return (i?i:-1);
}
