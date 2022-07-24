/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/unistd.h> 
#include <lib.h>
#include <proc.h>
#include <openfile.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <copyinout.h>
#include <spinlock.h>

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
runprogram(char *progname, char **args, unsigned long nargs)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	struct proc *proc = curproc;
	int argc = nargs;

	/* Local copying of args:
	char **kargs = (char **)kmalloc(argc*PATH_MAX);
	char ret = 0;
	size_t actual_len;

	for(int i=0;i<argc;i++){
		strcpy((char *)kargs,args[i]);
		//copyinstr((userptr_t)args[i],(char *)kargs,PATH_MAX,&actual_len);
		kargs = kargs + PATH_MAX;
	}*/

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when proc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when proc is destroyed */
		return result;
	}

	/* Open the console files: STDIN, STDOUT and STDERR */
	console_init(proc);
	if(result){
		return result;
	}
	
	/* Copy the arguments from kernel space to user stack */
	// TODO: manca il padding ma dovrebbe funzionare 
	// perchè le stringhe terminano per \0

	// Array with the arguments positions (+1 to consider the last NULL argument)
	vaddr_t arg_pointers[argc+1];
	int stackpos = 0; // Support variable to move inside the stack
	int len_i; // Length of i-th argument
	int padding; // N. of 0s to insert at the end of the argument

	for(int i=0;i<argc;i++){
		len_i = strlen(args[i])+1;
		padding = 4 - len_i%4;
		stackpos += len_i + padding;
		arg_pointers[i] = stackptr - stackpos;
		copyout(args[i], (userptr_t)arg_pointers[i], len_i);
	}
	arg_pointers[argc] = 0; // The last argument points to 0 (NULL)

	// Copy the pointers to arguments into the user stack
	// (the array to copy has been filled before)

	// Update the stackptr with the position in which copy the pointers
	stackptr = arg_pointers[argc-1] - 4*(argc+1);
	copyout(arg_pointers, (userptr_t)stackptr, 4*(argc+1));

	/* Release the waiting of the parent process*/
	proc->runprogram_finished = 1;
    //V(&proc->p_waitsem);

	/* Warp to user mode. */
	enter_new_process(argc /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

/*
 * Open the console files: STDIN, STDOUT and STDERR
 */
int
console_init(struct proc *proc)
{
	struct vnode *v_stdin, *v_stdout, *v_stderr;
	int result;
	char *kconsole = (char *)kmalloc(5);
	// Support openfile object
	struct openfile *of_tmp;
	of_tmp = kmalloc(sizeof(struct openfile));

	strcpy(kconsole,"con:");
	
	result = vfs_open(kconsole, O_RDONLY, 0664, &v_stdin);
	if (result) {
		return result;
	}
	of_tmp->of_vnode = v_stdin;
	of_tmp->of_flags = O_RDONLY;
	of_tmp->of_offset = 0; // dummy
    of_tmp->of_refcount = 0;
    spinlock_init(&of_tmp->of_lock);

	proc->p_filetable[STDIN_FILENO] = of_tmp;

	strcpy(kconsole,"con:");

	result = vfs_open(kconsole, O_WRONLY, 0664, &v_stdout);
	if (result) {
		return result;
	}
	of_tmp->of_vnode = v_stdout;
	of_tmp->of_flags = O_WRONLY;
	of_tmp->of_offset = 0; // dummy
    of_tmp->of_refcount = 0;
    spinlock_init(&of_tmp->of_lock);

	proc->p_filetable[STDOUT_FILENO] = of_tmp;

	strcpy(kconsole,"con:");

	result = vfs_open(kconsole, O_WRONLY, 0664, &v_stderr);
	if (result) {
		return result;
	}
	of_tmp->of_vnode = v_stderr;
	of_tmp->of_flags = O_WRONLY;
	of_tmp->of_offset = 0; // dummy
    of_tmp->of_refcount = 0;
    spinlock_init(&of_tmp->of_lock);

	proc->p_filetable[STDERR_FILENO] = of_tmp;

	return 0;
}

/*
 * Copies the arguments from the kernel space
 * into the user stack
 * (it is called by runprogram and execv)
 */
/*
int copyout_args(char **args, vaddr_t *stackptr){}
*/

/* 
 * wait for runprogram termination particularly
 * that runprogram finish to use the arguments
 * (just before enter_new_process)
 * 
 */
int wait_runprog(struct proc *proc){

	// this variable is set to 1 by runprogram() when
	// the arguments copying is terminated
	while(proc->runprogram_finished == 0);

	// Alternative: semaphores
    //P(&proc->p_waitsem);

	return 0;
}