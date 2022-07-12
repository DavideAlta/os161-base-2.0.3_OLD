#include <types.h> // userptr_t struct
#include <kern/errno.h> // error codes
#include <kern/fcntl.h> // flags
#include <kern/seek.h> // flags for lseek
#include <vfs.h> // vfs_open()
#include <vnode.h> // vnode struct declaration
#include <current.h> // curthread (that is a thread struct)
#include <thread.h>
#include <proc.h>
#include <openfile.h> // openfile struct
#include <limits.h> // macros with maximum values
#include <kern/unistd.h> // STDIN, STDOUT, STDERR
#include <kern/stat.h> // stat struct
#include <kern/iovec.h>
#include <uio.h>
#include <clock.h>
#include <copyinout.h> // copyinstr()
#include <syscall.h>
#include <lib.h> // kprintf(), KASSERT()

/*
* File handling system calls
*/

int sys_open(userptr_t filename, int flags, int *retval){

    struct vnode *vn = NULL;
    int err = 0;
    int append_mode = 0;
    int fd = -1;
    char kfilename[PATH_MAX]; // filename string inside kernel space
    size_t actual_len;
    
    /*[1] Check arguments validity*/
    
    // Filename
    if(filename == NULL){
        err = EFAULT;
        return err;
    }

    // Copy the filename string from user to kernel space to protect it
    // (used at point 3)
    err = copyinstr(filename, kfilename, sizeof(kfilename), &actual_len);
    if(err)
        return err;

    // Flags
    switch(flags){
        // VERIFICARE TUTTE LE COMBINAZIONI DEI FLAG
        case O_RDONLY: break;
        case O_WRONLY: break;
        case O_RDWR: break;
        // Create the file if it doesn't exist (handled by vfs_open())
        case O_CREAT|O_WRONLY: break;
        case O_CREAT|O_RDWR: break;
        // Create the file if it doesn't exist, fails if doesn't exist (handled by vfs_open())
        case O_CREAT|O_EXCL|O_WRONLY: break;
        case O_CREAT|O_EXCL|O_RDWR: break;
        // Truncate the file to length 0 upon open (handled by vfs_open())
        case O_TRUNC|O_WRONLY: break;
        case O_TRUNC|O_RDWR: break;
        // Write at the end of the file
        case O_WRONLY|O_APPEND:
            append_mode = 1;
            break;
        case O_RDWR|O_APPEND:
            append_mode = 1;
            break;
        // EINVAL = flags contained invalid values
        default:
            err = EINVAL;
            return err;
    }

    /* [2] Check if the current thread and the associated process are no NULL */
    KASSERT(curthread != NULL);
    KASSERT(curproc != NULL); // curproc = curthread->t_proc

    /* [3] Obtain a vnode object associated to the passed file to open */
    err = vfs_open(kfilename, flags, 0, &vn);
    //mode = 0 (runprogram.c) or 0664 (ftest.c) ??
    if(err)
        return err;
    
    /* [4] Define a file table inside the process structure (curproc->p_filetable) */
    
    /* [5] Find the index of the first free slot of the file table */

    spinlock_acquire(&curproc->p_lock); // Giusto?

    // meglio for+break o while ?
    for(int i=STDERR_FILENO+1; i<OPEN_MAX; i++){
        if(curproc->p_filetable[i] == NULL){
            fd = i;
            break;
        }
    }
    if(fd == -1){ // If there are not free slots, return error
        err = EMFILE;
        return err;
    }

    // [6] Allocate and fill the found openfile struct
    curproc->p_filetable[fd] = (struct openfile *)kmalloc(sizeof(struct openfile));
	KASSERT(curproc->p_filetable[fd] != NULL); // Check if the filetable is no more NULL

    curproc->p_filetable[fd]->of_offset = 0;
    curproc->p_filetable[fd]->of_flags = flags;
    curproc->p_filetable[fd]->of_vnode = vn;
    curproc->p_filetable[fd]->of_refcount = 1;
    spinlock_init(&curproc->p_filetable[fd]->of_lock);
    

    // If append mode: the offset is equal to the file size
    if(append_mode){
        struct stat statbuf;
		err = VOP_STAT(curproc->p_filetable[fd]->of_vnode, &statbuf);
		if (err){
			kfree(curproc->p_filetable[fd]); //opposite of kmalloc()
			curproc->p_filetable[fd] = NULL;
			return err;
		}
		curproc->p_filetable[fd]->of_offset = statbuf.st_size;
    }

    spinlock_release(&curproc->p_lock); // Giusto?

    // [7] fd (i.e. retval) = Place of openfile inside the file table
    *retval = fd;

    return 0;
}

int sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
    int err;
    int offset; // temporary variable to store the openfile field
    struct iovec iov;
    struct uio u;
    void *kbuf = kmalloc(size); // buffer inside kernel space

    KASSERT(curthread != NULL);
    KASSERT(curproc != NULL );

    // Synchronization of reading operations (the file must be locked during reading)
    spinlock_acquire(&curproc->p_filetable[fd]->of_lock);
    
    /*[1] Check arguments validity*/

    // fd is not a valid file descriptor, or was not opened for reading
    if(fd < 0 || fd >= OPEN_MAX || curproc->p_filetable[fd] == NULL ||
       (curproc->p_filetable[fd]->of_flags&O_WRONLY) == O_WRONLY){
        err = EBADF;
        return err;
    }
    // Part or all of the address space pointed to by buf is invalid
    if(buf == NULL){
        err = EFAULT;
        return err;
    }

    offset = curproc->p_filetable[fd]->of_offset;
	
    /* [2] Setup the uio record (use a proper function to init all fields) */
	uio_kinit(&iov, &u, kbuf, size, offset, UIO_READ);
    u.uio_space = curproc->p_addrspace;
	u.uio_segflg = UIO_USERSPACE; // for user space address

    /*[3] VOP_READ - Read data from file to uio, at offset specified
                     in the uio, updating uio_resid to reflect the
                     amount read, and updating uio_offset to match.*/
	err = VOP_READ(curproc->p_filetable[fd]->of_vnode, &u);
	if (err) {
		return err;
	}

    /* [4] uio_resid is the remaining byte to read => retval (the read bytes) is size-resid */
    *retval = size - u.uio_resid; // + 1 ? To test !
    // offset update
    curproc->p_filetable[fd]->of_offset = u.uio_offset; // Giusto ?

    // Copy the buffer from kernel to user space to make it available for the user
    // (used at point 2)
    err = copyout(kbuf, buf, size);
    if(err){
        kfree(kbuf);
        return err;
    }

    // Synchronization of reading operations
    spinlock_release(&curproc->p_filetable[fd]->of_lock);

    return 0;
}

int sys_write(int fd, userptr_t buf, size_t buflen, int *retval){
    
    int err;
    void *kbuf = kmalloc(buflen); // buffer inside kernel space
    int offset; // temporary variable to store the openfile field
    struct iovec iov;
    struct uio u;

    KASSERT(curthread != NULL);
    KASSERT(curproc != NULL );

    // Copy the buffer from user to kernel space to protect it
    // (used at point 2)
    err = copyin(buf, kbuf, buflen);
    if(err){
        kfree(kbuf);
        return err;
    }

    // Synchronization of writing operations (the file must be locked during writing)
    spinlock_acquire(&curproc->p_filetable[fd]->of_lock);

    /*[1] Check arguments validity*/

    // fd is not a valid file descriptor, or was not opened for writing
    if(fd < 0 || fd >= OPEN_MAX || curproc->p_filetable[fd] == NULL ||
       (curproc->p_filetable[fd]->of_flags&O_RDONLY) == O_RDONLY){
        err = EBADF;
        return err;
    }
    // Part or all of the address space pointed to by buf is invalid
    if(buf == NULL){
        err = EFAULT;
        return err;
    }

    offset = curproc->p_filetable[fd]->of_offset;

    /* [2] Setup the uio record (use a proper function to init all fields) */
	uio_kinit(&iov, &u, kbuf, buflen, offset, UIO_WRITE);
    u.uio_space = curproc->p_addrspace;
	u.uio_segflg = UIO_USERSPACE; // for user space address

    /*[3] VOP_WRITE - Write data from uio to file at offset specified
                      in the uio, updating uio_resid to reflect the
                      amount written, and updating uio_offset to match.*/
	err = VOP_WRITE(curproc->p_filetable[fd]->of_vnode, &u);
	if (err)
		return err;

    /* [4] uio_resid is the amount written => retval*/
    *retval = u.uio_resid; // giusto? o metto buflen??
    // offset update
    curproc->p_filetable[fd]->of_offset += buflen; //giusto? o metto u.uio_offset

    // Synchronization of reading operations
    spinlock_release(&curproc->p_filetable[fd]->of_lock);

    return 0;
}

int sys_close(int fd){

    int err;

    KASSERT(curthread != NULL);
    KASSERT(curproc != NULL );
    
    spinlock_acquire(&curproc->p_filetable[fd]->of_lock);

    /* [1] Check arguments validity */

    // fd is not a valid file handle
    if(fd < 0 || fd >= OPEN_MAX || curproc->p_filetable[fd] == NULL){
        err = EBADF;
        return err;
    } 

    /* [2] Delete the openfile structure only if it is the last open*/
    
    curproc->p_filetable[fd]->of_refcount--;

    if(curproc->p_filetable[fd]->of_refcount == 0){ // Last open => openfile removal
        vfs_close(curproc->p_filetable[fd]->of_vnode);
        spinlock_release(&curproc->p_filetable[fd]->of_lock);
        spinlock_cleanup(&curproc->p_filetable[fd]->of_lock);
        kfree(curproc->p_filetable[fd]);
        curproc->p_filetable[fd] = NULL;

    }else{ // No last open (no removal)
        spinlock_release(&curproc->p_filetable[fd]->of_lock);
        curproc->p_filetable[fd] = NULL;
    }

    return 0;
}

int sys_lseek(int fd, off_t pos, int whence, int *retval){

    int err;
    int offset;
    int filesize;
    struct stat statbuf;

    spinlock_acquire(&curproc->p_filetable[fd]->of_lock);

    /* [1] Check fd validity */

    // fd is not a valid file handle
    if(fd < 0 || fd >= OPEN_MAX || curproc->p_filetable[fd] == NULL){
        err = EBADF;
        return err;
    }

    // Retrieve the file size to can use it then
	err = VOP_STAT(curproc->p_filetable[fd]->of_vnode, &statbuf);
	if (err){
        //kfree(curproc->p_filetable[fd]);
        //curproc->p_filetable[fd] = NULL;
        return err;
    }else{
        filesize = statbuf.st_size;
    }

    /* [2] Check how is the flag passed as argument */
    switch(whence){
        case SEEK_SET: // the new position is pos
            offset = pos;
        break;
        case SEEK_CUR: // the new position is the current position plus pos
            offset = curproc->p_filetable[fd]->of_offset + pos;
        break;
        case SEEK_END: // the new position is the position of end-of-file plus pos
            offset =  filesize + pos;
        break;

        default:
            // whence is invalid
            err = EINVAL;
            return err;
    }
    
    // The resulting seek position would be negative
    if((offset < 0) /*|| (offset > filesize)*/){
        err = EINVAL;
        return err;
    }

    /* [3] Update the file offset */
    curproc->p_filetable[fd]->of_offset = offset;

    spinlock_release(&curproc->p_filetable[fd]->of_lock);

    *retval = offset;

    return 0;
}

int sys_dup2(int oldfd, int newfd, int *retval){

    int err;

    spinlock_acquire(&curproc->p_filetable[oldfd]->of_lock);

    /* [1] Check arguments validity */

    // old/newfd is not a valid file handle
    if(oldfd < 0 || oldfd >= OPEN_MAX || curproc->p_filetable[oldfd] == NULL ||
       newfd < 0 || newfd >= OPEN_MAX ){
        err = EBADF;
        return err;
    }

    /* [2] Check if the new fd is already associated to an open file, if yes close it */
    if(curproc->p_filetable[newfd] != NULL){
        err = sys_close(newfd);
        if(err)
            return err;
    }

    /* [3] Copy the openfile pointer of the old fd inside the new fd and update the refcount*/
    curproc->p_filetable[newfd] = curproc->p_filetable[oldfd];
    curproc->p_filetable[oldfd]->of_refcount++;
    
    *retval = newfd;

    spinlock_release(&curproc->p_filetable[oldfd]->of_lock);

    return 0;
}

/*
Practical example of refcount behaviour:

sys_open(pippo)
fd = 3
filetable[3] = *pippo   |   sysfiletable[*pippo] = openfile of pippo (refcount = 1)
filetable[4] = NULL

sys_dup2(3,4)
filetable[3] = *pippo   |   sysfiletable[*pippo] = openfile of pippo (refcount = 2)
filetable[4] = *pippo

sys_close(3)
filetable[3] = NULL     |   sysfiletable[*pippo] = openfile of pippo (refcount = 1)
filetable[4] = *pippo

sys_close(4)
filetable[3] = NULL     |   sysfiletable[*pippo] = <empty>
filetable[4] = NULL

*/
