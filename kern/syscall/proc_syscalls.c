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

int sys_fork(struct trapframe *tf, pid_t *retval){

    int err;
    pid_t pid = 0;
    struct trapframe *childtf;

    struct proc *childproc = NULL;

    if(proc_counter >= MAX_PROCESSES){
        err = ENPROC;
        return err;
    }

    // Initilize the child process
    childproc = proc_create("child_proc");
    if(childproc == NULL) {
        err = ENOMEM;
		return err;
	}


    /* PUT IN PROC_CREATE() IN PROC.C
    // Search a free slot in process table and put the child process
    while(proctable[pid] != NULL){
        // Full proc table
        if(pid == MAX_PROCESSES - 1){
            err = ENPROC;
			return err;
		}
        pid++;
    }
    proctable[pid] = childproc;
    */

    // Synchronization for current process struct variables
    spinlock_acquire(&curproc->p_lock);

    // Copy all the proc struct variables

    // Parent is the calling process (curproc)
    childproc->p_parentpid = curproc->p_pid;

    // 1. Current address space to the child one
    err = as_copy(curproc->p_addrspace, &childproc->p_addrspace);
    if(err)
        return err;

    // 2. File table
    for(int i=0;i<OPEN_MAX;i++) {
		if(curproc->p_filetable[i] != NULL){
			lock_acquire(curproc->p_filetable[i]->p_lock);
			childproc->p_filetable[i] = curproc->p_filetable[i];
			lock_release(curproc->p_filetable[i]->p_lock);
		}
	}

    // 3. Current directory
    if(curproc->p_cwd != NULL){
        VOP_INCREF(curproc->p_cwd); // vnode refcount increment
        childproc->p_cwd = curproc->p_cwd;
    }
    spinlock_release(&curproc->p_lock);

    // 4. Number of threads (il figlio eredita anche i thread del padre ???)
    childproc->p_numthreads = curproc->p_numthreads;

    // Copy current trapframe inside child
    childtf = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if(childtf == NULL){
        err = ENOMEM;
        return err;
    }
    childtf = tf;

    // Thread fork function
    err = thread_fork("child_thread", childproc, enter_forked_process, (void *)childtf, (unsigned long)childproc->p_addrspace);
    if(err)
        return err;

    // Return the child pid
    *retval = childproc->p_pid;

    return 0;
}

int sys_getpid(pid_t *retval){

    *retval = curproc->p_pid;

    return 0;
}

int sys__exit(int exitcode){

    int err;

    curproc->exitcode = exitcode;
    curproc->is_exited = true;

    // signal
    V(&curproc->p_waitsem);

    return 0;
}

int sys_waitpid(pid_t pid, int *status, int options, pid_t *retval){


    // wait
    P(&proctable[pid]->p_waitsem);

    *status = proctable[pid]->exitcode;

    *retval = pid;

    return 0;
}