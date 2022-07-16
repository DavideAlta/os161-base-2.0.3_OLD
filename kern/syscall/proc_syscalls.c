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

    spinlock_acquire(&curproc->p_lock);

    // Check the presence of curproc in proctable
    for(int i=0;i<MAX_PROCESSES;i++){
		if(proctable[i] != NULL){
			if(proctable[i]->p_pid == curproc->p_pid)
			break;
		}
        // If there is no curproc inside proctable return
		if(i == MAX_PROCESSES - 1){
            return 0;
        }
	}

    curproc->exitcode = exitcode;
    curproc->is_exited = true;

    // Signal the semaphore for waitpid
    V(&curproc->p_waitsem);

    spinlock_release(&curproc->p_lock);

    // Cause the current thread to exit and remove curproc
    thread_exit(); // is zombie (pointer is NULL but proc is still in memory)

    return 0;
}

int sys_waitpid(pid_t pid, int *status, int options, pid_t *retval){

    int err;
    int *kstatus = (int *)kmalloc(sizeof(int));

    // The options argument requested invalid or unsupported options.
    if(options != 0){
        err = EINVAL;
        return err;
    }

    // The pid argument named a nonexistent process
    if(proctable[pid] == NULL){
        err = ESRCH;
        return err;
    }

    // The pid argument named a process that was not a child of the current process.
    // or if the pid is different by the curproc pid (Waiting for itself!)
    if((proctable[pid]->p_parentpid != curproc->p_pid) ||
        (curproc->p_pid == pid)){
        err = ECHILD;
        return err;
    }

    // wait
    P(&proctable[pid]->p_waitsem);

    *kstatus = proctable[pid]->exitcode;

    // Destroy the pid process
    proc_destroy(proctable[pid]);
    // pid is now available
    proctable[pid] = NULL; 

    err = copyout(kstatus, status, sizeof(int));
    if(err){
        kfree(kstatus);
        return err;
    }

    *retval = pid;

    return 0;
}

int sys_execv(const char *program, char **args){

    int err;
    int sizechar, maxargs, args_size=0, argc=0;
    int args_size_i;
    char kprogram[PATH_MAX] = (char *)kmalloc(PATH_MAX);
    struct vnode *vn;
    struct addrspace *as;
    
    if((progname == NULL) || (arg == NULL)) {
		return EFAULT;
	}

    /*
    ENOEXEC	program is not in a recognizable executable file format,
    was for the wrong platform, or contained invalid fields. ????
    */

    // Size of 1 character
    sizechar = sizeof(char);
    // Max number of args[] elements (if each one is a char)
    maxargs = ARG_MAX/sizechar;
    for(int i=0;i<maxargs;i++){
        if(args[i] != NULL){
            args_size =+ sizechar*(strlen(args[i])+1);
            argc++;
            // The total size of the argument strings exceeeds ARG_MAX.
            if(args_size > ARG_MAX){
                err = E2BIG;
                return err;
            }
        }else{
            // Last argument (is always NULL)
            break;
        }
    }

    // Move arguments to kernel space
    err = copyinstr((const_userptr_t)program, kprogram, PATH_MAX, NULL);
    if(err){
        return err;
    }

    char *kargs[argc];

    for(int i=0;i<argc;i++){
        args_size_i = sizechar*(strlen(args[i])+1);
        kargs[i] = (char *)kmalloc(args_size_i);
        err = copyinstr(args[i], kargs[i], args_size_i, NULL); //int because is a pointer
        if(err){
            return err;
        }
    }

    // Initilize the vnode struct associated to program
    err = vfs_open(program, O_RDONLY, 0, &vn);
    if(err){
        return err;
    }

    // Create an address space structure
    as = as_create();

    // ...

    return 0;
}