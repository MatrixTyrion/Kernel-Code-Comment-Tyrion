#include <linux/sched.h>

#include <sys/stat.h>

static void free_ind(int dev,int block)		//释放inode的一级间接映射
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_block(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

static void free_dind(int dev,int block)		//释放inode的二级间接映射
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_ind(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

void truncate(struct m_inode * inode)		//释放此inode的直接映射及一级和二级间接映射
{
	int i;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))	//不能是目录
		return;
	for (i=0;i<7;i++)				//释放直接映射，i_zone[0]~i_zone[6]是直接块映射
		if (inode->i_zone[i]) {
			free_block(inode->i_dev,inode->i_zone[i]);	//从s_zmap中清除设备块
			inode->i_zone[i]=0;
		}
	free_ind(inode->i_dev,inode->i_zone[7]);	//释放一级间接映射
	free_dind(inode->i_dev,inode->i_zone[8]);	//释放二级间接映射
	inode->i_zone[7] = inode->i_zone[8] = 0;
	inode->i_size = 0;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

