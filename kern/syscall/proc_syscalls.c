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
            curproc->p_filetable[i]->p_refcount++;
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

/*
 * sys_execv() is very similar to runprogram() but with some modifications
 * Since runprogram() loads a program and start running it in usermode
*/
int sys_execv(const_userptr_t program, userptr_t args){

    int err;
    struct vnode *vn;
    struct addrspace *as;
    vaddr_t entrypoint, stackptr;
    size_t program_len;

    // Check arguments validity
    if((progname == NULL) || (args == NULL)) {
		return EFAULT;
	}

    /*
    ENOEXEC	program is not in a recognizable executable file format,
    was for the wrong platform, or contained invalid fields. ????
    */

    /* 1. Compute argc */

    // The n. of elements of args[] is unknown but the last argument should be NULL.
    // Compute argc = n. of valid arguments in args[]
    int argc = 0;
    int args_size = 0 // Total size of the argument strings
    while(args[argc] != NULL){
        // Accumulate the size of each args[] element
        args_size =+ sizeof(char)*(strlen(args[argc])+1);
        // The total size of the argument strings exceeeds ARG_MAX.
        if(args_size > ARG_MAX){
            err = E2BIG;
            return err;
        }
        argc ++;
    }
    // Now argc contains the number of valid arguments inside args[]

    /* 2. Copy arguments from user space into kernel space. */

    // - Program path
    char kprogram[PATH_MAX] = (char *)kmalloc(PATH_MAX);
    err = copyinstr(program, kprogram, PATH_MAX, &program_len);
    if(err){
        return err;
    }

    // - Single arguments
    char *kargs; // array of char (= 1 byte)
    int args_size_i; // Size of i-th argument string
    int actual_len = 0;
    int padding = 0;
    int cur_pos = 0;
    int arg_pointers[argc];
    // Suppose to have these arguments: "foo\0" "hello\0" "1\0"
    for(int i=0;i<argc;i++){
        // Allocate in kernel space a space for a string of size of args[i] 
        args_size_i = sizeof(char)*(strlen(args[i])+1);
        //kargs[i] = (char *)kmalloc(args_size_i); // dev'essere in multipli di 4????? o lo tolgo proprio????
        // Move args to kernel space
        err = copyinstr(args[i], &kargs[cur_pos], args_size_i, &actual_len); // actual_len = 4 (foo\0) 6 (hello\0) 2 (1\0)
        if(err){
            return err;
        }
        arg_pointers[i] = cur_pos;
        cur_pos += actual_len; // cur_pos = 4 > 10 > 14
        // if the argument string has no exactly 4 bytes '\0'-padding is needed
        if((actual_len%4) != 0){
            // n. of bytes to pad at '\0'
            padding = 4 - actual_len%4; // padding = 2 > 2
            for(int j=0;j<padding;j++){
                kargs[cur_pos + j] = '\0';
            }
            cur_pos += padding; // cur_pos = 4 > 12 > 16
        }
    }
    // Now kargs[] is the copy of args in kernel space with the proper padding
    // Now cur_pos has the size of kargs[] array

    /* 3. Create a new address space and load the executable into it
     * (same of runprogram) */

    // Open the "program" file
    err = vfs_open(kprogram, O_RDONLY, 0, &vn);
    if(err){
        return err;
    }

    // Create a new address space
	as = as_create();
	if(as == NULL) {
		vfs_close(vn);
		err = ENOMEM;
        return err;
	}

    // Change the current address space and activate it
	proc_setas(as);
	as_activate();

    // Load the executable "program"
	err = load_elf(vn, &entrypoint);
	if (err) {
		vfs_close(vn);
		return err;
	}

    // File is loaded and can be closed
	vfs_close(vn);

    // Define the user stack in the address space
	err = as_define_stack(as, &stackptr);
	if (err) {
		return err;
	}

    /* 4. Copy the arguments from kernel space to user stack*/

    // argc is updated with prgogram name
    argc++;

    // Start position of the stack
    stackptr -= cur_pos;

    // Space to add for program path = 
    // = program path length (with padding) + 4 bytes for pointer args[]
    stackptr -= program_len + (4 - program_len%4) + 4;

    for(int i=0;i<argc;i++){
        copyout(kargs[i],);
    }





    copyout(kargs, (userptr_t)stackptr, cur_pos);


    

    /* 5. Warp to user mode
     * (same of runprogram) */

	enter_new_process(argc /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

    // PASSARE ARGC E ARGV COPIATI NELLO USER STACK

    // enter_new_process does not return
	panic("enter_new_process in execv returned\n");

    err = EINVAL;

    return err;
}