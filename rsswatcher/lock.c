#include <common.h>
#include "lock.h"

#define FILE_MODE        (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

int
lock_reg(int fd, int cmd, int type, off_t offset, int whence, off_t len)
{
        struct flock lock;
        
        lock.l_type = type;           /* F_RDLCK, F_WRLCK, F_UNLCK */
        lock.l_start = offset;        /* byte offset, relative to l_whence */
        lock.l_whence = whence;       /* SEEK_SET, SEEK_CUR, SEEK_END */
        lock.l_len = len;             /* #bytes (0 means to EOF) */
        
        return(fcntl(fd, cmd, &lock));
}

#define        read_lock(fd, offset, whence, len) \
lock_reg(fd, F_SETLK, F_RDLCK, offset, whence, len)
#define        readw_lock(fd, offset, whence, len) \
lock_reg(fd, F_SETLKW, F_RDLCK, offset, whence, len)
#define        write_lock(fd, offset, whence, len) \
lock_reg(fd, F_SETLK, F_WRLCK, offset, whence, len)
#define        writew_lock(fd, offset, whence, len) \
lock_reg(fd, F_SETLKW, F_WRLCK, offset, whence, len)
#define        un_lock(fd, offset, whence, len) \
lock_reg(fd, F_SETLK, F_UNLCK, offset, whence, len)


void err_sys(char * str)
{
        fprintf(stdout,"%s\n",str);
	return;
}

int check_running()
{
        int         fd, val;
        char        buf[10];
        
        if ((fd = open(PIDFILE, O_WRONLY | O_CREAT, FILE_MODE)) < 0)
                err_sys("open file error");
        
                        /* try and set a write lock on the entire file */
        if (write_lock(fd, 0, SEEK_SET, 0) < 0){
		err_sys("process already running ...");
		return -1; /* gracefully exit, daemon is already running */
        }
                        /* truncate to zero length, now that we have the lock */
        if (ftruncate(fd, 0) < 0)
                err_sys("ftruncate error");
        
                        /* and write our process ID */
        sprintf(buf, "%d\n", getpid());
        if (write(fd, buf, strlen(buf)) != strlen(buf))
                err_sys("write error");
        
                        /* set close-on-exec flag for descriptor */
        if ((val = fcntl(fd, F_GETFD, 0)) < 0)
                err_sys("fcntl F_GETFD error");

        val |= FD_CLOEXEC;

        if (fcntl(fd, F_SETFD, val) < 0)
                err_sys("fcntl F_SETFD error");

        return 0;
}
