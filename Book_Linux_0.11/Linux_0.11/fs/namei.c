#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
static int permission(struct m_inode * inode,int mask)		//权限检查
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	if (!(current->uid && current->euid))			//uid和euid都为0，表示最高权限
		mode=0777;
	else if (current->uid==inode->i_uid || current->euid==inode->i_uid)		//用户权限检查
		mode >>= 6;
	else if (current->gid==inode->i_gid || current->egid==inode->i_gid)		//用户组权限检查
		mode >>= 3;
	return mode & mask & 0007;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same __asm__("ax");

	if (!de || !de->inode || len > NAME_LEN)		//如果目录指针为空或节点不存在或长度太长
		return 0;
	if (len < NAME_LEN && de->name[len])
		return 0;
//下面内嵌汇编的意思是：
//	movl	0	%eax
//	movl	name	%esi
//	movl	de->name	%edi
//	cld
//	repe				//ZF=1
//	cmpsb
//	setz	%al
//	movl	%eax	same
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"
		"setz %%al"
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		:"cx","di","si");
	return same;
}

/*
 *	find_entry()
 *
 * finds and entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
static struct buffer_head * find_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	entries = dir->i_size / (sizeof (struct dir_entry));	//该目录里包含几个入口
	*res_dir = NULL;
	if (!namelen)
		return NULL;
	if (!(block = dir->i_zone[0]))				//此目录里有没有元素
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))			//从硬盘读取该目录
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;			//de指向目录文件数据区的开始
	while (i < entries) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {	//如果该目录文件中一个块读完仍未找到，就读该目录文件的下一个块
			brelse(bh);					//释放bh
			bh = NULL;
			if (!(block = bmap(dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread(dir->i_dev,block))) {	//寻找下一块并将它读入内存
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (match(namelen,name,de)) {			//对比目录文件中的一个元素，返回1是找到
			*res_dir = de;				//指向所找数据的偏移量
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	if (!namelen)						//如果光是目录，没有文件名，返回空
		return NULL;
	if (!(block = dir->i_zone[0]))			//取得目录文件的第一个块号
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))		//读取目录文件的第一块
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;		//de指向第一块的数据区
	while (1) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {	//如果de指向了该块的末尾
			brelse(bh);
			bh = NULL;
			block = create_block(dir,i/DIR_ENTRIES_PER_BLOCK);	//取得目录文件的下一个块号，如果需要，就创建新块
			if (!block)
				return NULL;
			if (!(bh = bread(dir->i_dev,block))) {	//将此块读入buffer
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;	//de指向该块的数据区
		}
		if (i*sizeof(struct dir_entry) >= dir->i_size) {	//在目录文件的末尾创建新的节点，初始化为0，并修改该目录文件的尺寸
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}
		if (!de->inode) {
			dir->i_mtime = CURRENT_TIME;
			for (i=0; i < NAME_LEN ; i++)
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;	//将文件名写入de->name
			bh->b_dirt = 1;
			*res_dir = de;			//这时de是一个inode=0和name=文件名的结构
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
static struct m_inode * get_dir(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;

	if (!current->root || !current->root->i_count)
		panic("No root inode");
	if (!current->pwd || !current->pwd->i_count)
		panic("No cwd inode");
	if ((c=get_fs_byte(pathname))=='/') {		//取第一个字节
		inode = current->root;
		pathname++;
	} else if (c)
		inode = current->pwd;
	else
		return NULL;	/* empty name is bad */
	inode->i_count++;
	while (1) {
		thisname = pathname;
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {		//如果不是目录或没有权限
			iput(inode);				//释放该节点
			return NULL;
		}
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)	//取得目录长度
			/* nothing */ ;
		if (!c)
			return inode;				//取完全部目录返回，循环出口
		if (!(bh = find_entry(inode,thisname,namelen,&de))) {
			iput(inode);
			return NULL;
		}
		inr = de->inode;				//取得目录的inode
		idev = inode->i_dev;
		brelse(bh);
		iput(inode);
		if (!(inode = iget(idev,inr)))		//取得该目录的内存inode，为寻找下一个子目录做准备
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * dir;

	if (!(dir = get_dir(pathname)))			//取得该目录的最后子目录的内存inode
		return NULL;
	basename = pathname;					//两个指针指向同一地址，即文件全名的起始地址
	while (c=get_fs_byte(pathname++))
		if (c=='/')
			basename=pathname;			//basename最终将是要运行的程序名
	*namelen = pathname-basename-1;
	*name = basename;
	return dir;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(pathname,&namelen,&basename)))		//取得目录的内存inode
		return NULL;
	if (!namelen)			/* special case: '/usr/' etc */
		return dir;
	bh = find_entry(dir,basename,namelen,&de);			//取得需要运行或打开的inode
	if (!bh) {					//如果没有这个文件，释放该目录的buffer
		iput(dir);
		return NULL;
	}
	inr = de->inode;				//取得该文件的硬盘inode
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	dir=iget(dev,inr);				//取得该文件的内存inode
	if (dir) {
		dir->i_atime=CURRENT_TIME;
		dir->i_dirt=1;
	}
	return dir;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))		//如果flag是可修剪的或可访问的
		flag |= O_WRONLY;					//那么加上可写位
	mode &= 0777 & ~current->umask;				//mode必须接受当前进程掩码的约束
	mode |= I_REGULAR;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {			/* special case: '/usr/' etc */	//如果pathname只是目录，而没有文件名
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir;
			return 0;
		}
		iput(dir);
		return -EISDIR;
	}
	bh = find_entry(dir,basename,namelen,&de);
	if (!bh) {					//如果在目录中没有发现该文件的入口点
		if (!(flag & O_CREAT)) {		//如果flag中没有建立位，返回
			iput(dir);
			return -ENOENT;
		}
		if (!permission(dir,MAY_WRITE)) {		//如果此目录的权限是不可写的，返回
			iput(dir);
			return -EACCES;
		}
		inode = new_inode(dir->i_dev);	//为在这个目录中不存在的文件建立一个新的inode
		if (!inode) {
			iput(dir);
			return -ENOSPC;
		}
		inode->i_mode = mode;		//设置新的inode的mode
		inode->i_dirt = 1;
		bh = add_entry(dir,basename,namelen,&de);		//在目录文件中写入de->name
		if (!bh) {
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		de->inode = inode->i_num;		//将de->inode写入实际块号，它将在dir中与de->name对照
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	if (flag & O_EXCL)				//如果flag是检查该文件有没有
		return -EEXIST;
	if (!(inode=iget(dev,inr)))			//取该文件的inode
		return -EACCES;
	if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
	    permission(inode,ACC_MODE(flag))!=ACC_MODE(flag)) {
		iput(inode);
		return -EPERM;
	}
	inode->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode);
	*res_inode = inode;
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (current->euid && current->uid)
		return -EPERM;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))		//读取待建目录的那个目录
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {		//检查此目录的写权限
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(dir,basename,namelen,&de);	//在这个目录文件中是否有待建目录的名字
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);		//创建一个新的inode
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {	//为该目录节点创建第一个块
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {	//读该目录块
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	de = (struct dir_entry *) dir_block->b_data;		//de指向该块的数据区
	de->inode=inode->i_num;			//建立本目录的入口
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;			//建立父目录的入口
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);	//将本节点设为目录文件
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);		//在父目录中登记本目录文件
	if (!bh) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);	//本目录中有几个项目
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {	//检查目录是否为有效目录
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;			//de指向目录中的元素
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {	//检查元素中的第一项是否为“。”和第二项是否为“。。”
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {		//如果该目录文件很大，超过了本块
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);	//找本目录的下一个块
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))		//读取该块
				return 0;
			de = (struct dir_entry *) bh->b_data;		//重新定位指针de
		}
		if (de->inode) {			//如果目录文件中只有本目录和父目录项，那么就是null目录
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (current->euid && current->uid)
		return -EPERM;
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	bh = find_entry(dir,basename,namelen,&de);	//在目录文件找是否有要删除的那个目录项
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {			//检查写权限
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {	//取要删除的那个文件的inode
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */		//要删除的目录不能是父目录
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {			//此节点必须是目录
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {				//此节点是否为空
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;					//在父目录中清除该目录的节点
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	inode = iget(dir->i_dev, de->inode);
	if (!inode) {					//不能联结不存在的文件
		printk("iget failed in delete (%04x:%d)",dir->i_dev,de->inode);
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
	if (!S_ISREG(inode->i_mode)) {		//是否为正常的节点mode
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir);
	return 0;
}

int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);			//保存老文件名的inode
	if (!oldinode)
		return -ENOENT;
	if (!S_ISREG(oldinode->i_mode)) {		//必须是正常文件
		iput(oldinode);
		return -EPERM;
	}
	dir = dir_namei(newname,&namelen,&basename);	//找出newname的父目录节点
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	bh = find_entry(dir,basename,namelen,&de);	//看看父目录中是否有这个名字
	if (bh) {						//如果有，就不能建立
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	bh = add_entry(dir,basename,namelen,&de);		//在父目录中为这个名字建立一个入口点
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inode = oldinode->i_num;			//将此节点指向和oldinode一样的inode
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
