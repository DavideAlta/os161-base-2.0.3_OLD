/*
* Read system call
*/

#include <types.h> // userptr_t struct
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h> // kprintf()

int
sys_read(int fd, userptr_t buf, int nbyte)
{
	char *p = (char *)buf;
    kprintf("TEST MESSAGE DAVIDE: We are entered in read sys call with %d, %d, %s.\n",fd,nbyte,p);
    return 0;

}