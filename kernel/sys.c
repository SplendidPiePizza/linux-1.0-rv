/*
 *  linux/kernel/sys.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/times.h>
#include <linux/utsname.h>
#include <linux/param.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/stat.h>
#include <linux/mman.h>

#include <asm/segment.h>
#include <asm/io.h>


static int C_A_D = 1;

extern void adjust_clock(void);

#define	PZERO	15


static int proc_sel(struct task_struct *p, int which, int who)
{
    switch (which) {
        case PRIO_PROCESS:
            
            if (!who && p == current)
                return 1; // Oh, look, it’s the current process. Big surprise. We match it.
            return p->pid == who; 
        case PRIO_PGRP:
            
            if (!who)
                who = current->pgrp; 
            return p->pgrp == who; 
        case PRIO_USER:
            
            if (!who)
                who = current->uid; 
            return p->uid == who; 
    }
    return 0; // No match, whatever.
}

// System call to set process priority
asmlinkage int sys_setpriority(int which, int who, int niceval)
{
    struct task_struct **p;
    int error = ESRCH; // Default error is ESRCH because why not? This means no process found. Shocking.
    int priority;

   
    if (which > 2 || which < 0)
        return -EINVAL; // Oh wow, this god damn invalid argument. Like we didn’t see that coming.

    // Calculate the priority: adjust based on 'niceval'
    if ((priority = PZERO - niceval) <= 0)
        priority = 1; // Wow, you messed up the nice value so badly, it had to be set to 1. Nice job.

    
    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if (!*p || !proc_sel(*p, which, who)) 
            continue; // fuck it, If it doesn't match, just skip. Whatever.

        // Check if the current process has permission to change the task's priority
        if ((*p)->uid != current->euid && 
            (*p)->uid != current->uid && !suser()) {
            error = EPERM; // Oh damn, you don't have permission. Shocker.
            continue; 
        }


        if (error == ESRCH)
            error = 0; 
        if (priority > (*p)->priority && !suser()) {
            error = EACCES; 
        } else {
            (*p)->priority = priority; // Finally, set the priority. After all those checks. Good job.
        }
    }
    
    return -error; 
}


asmlinkage int sys_getpriority(int which, int who)
{
	struct task_struct **p;
	int max_prio = 0;

	if (which > 2 || which < 0)
		return -EINVAL;

	for(p = &LAST_TASK; p > &FIRST_TASK; --p) {
		if (!*p || !proc_sel(*p, which, who))
			continue;
		if ((*p)->priority > max_prio)
			max_prio = (*p)->priority;
	}
	return(max_prio ? max_prio : -ESRCH);
}

asmlinkage int sys_profil(void)
{
	return -ENOSYS;
}

asmlinkage int sys_ftime(void)
{
	return -ENOSYS;
}

asmlinkage int sys_break(void)
{
	return -ENOSYS;
}

asmlinkage int sys_stty(void)
{
	return -ENOSYS;
}

asmlinkage int sys_gtty(void)
{
	return -ENOSYS;
}

asmlinkage int sys_prof(void)
{
	return -ENOSYS;
}

asmlinkage unsigned long save_v86_state(struct vm86_regs * regs)
{
	unsigned long stack;

	if (!current->vm86_info) {
		printk("no vm86_info: BAD\n");
		do_exit(SIGSEGV);
	}
	memcpy_tofs(&(current->vm86_info->regs),regs,sizeof(*regs));
	put_fs_long(current->screen_bitmap,&(current->vm86_info->screen_bitmap));
	stack = current->tss.esp0;
	current->tss.esp0 = current->saved_kernel_stack;
	current->saved_kernel_stack = 0;
	return stack;
}

static void mark_screen_rdonly(struct task_struct * tsk)
{
	unsigned long tmp;
	unsigned long *pg_table;

	if ((tmp = tsk->tss.cr3) != 0) {
		tmp = *(unsigned long *) tmp;
		if (tmp & PAGE_PRESENT) {
			tmp &= PAGE_MASK;
			pg_table = (0xA0000 >> PAGE_SHIFT) + (unsigned long *) tmp;
			tmp = 32;
			while (tmp--) {
				if (PAGE_PRESENT & *pg_table)
					*pg_table &= ~PAGE_RW;
				pg_table++;
			}
		}
	}
}

asmlinkage int sys_vm86(struct vm86_struct * v86)
{
	struct vm86_struct info;
	struct pt_regs * pt_regs = (struct pt_regs *) &v86;

	if (current->saved_kernel_stack)
		return -EPERM;
	memcpy_fromfs(&info,v86,sizeof(info));
/*
 * make sure the vm86() system call doesn't try to do anything silly
 */
	info.regs.__null_ds = 0;
	info.regs.__null_es = 0;
	info.regs.__null_fs = 0;
	info.regs.__null_gs = 0;
/*
 * The eflags register is also special: we cannot trust that the user
 * has set it up safely, so this makes sure interrupt etc flags are
 * inherited from protected mode.
 */
	info.regs.eflags &= 0x00000dd5;
	info.regs.eflags |= ~0x00000dd5 & pt_regs->eflags;
	info.regs.eflags |= VM_MASK;
	current->saved_kernel_stack = current->tss.esp0;
	current->tss.esp0 = (unsigned long) pt_regs;
	current->vm86_info = v86;
	current->screen_bitmap = info.screen_bitmap;
	if (info.flags & VM86_SCREEN_BITMAP)
		mark_screen_rdonly(current);
	__asm__ __volatile__("movl %0,%%esp\n\t"
		"pushl $ret_from_sys_call\n\t"
		"ret"
		: /* no outputs */
		:"g" ((long) &(info.regs)),"a" (info.regs.eax));
	return 0;
}

extern void hard_reset_now(void);

/*
 * Reboot system call: for obvious reasons only root may call it,
 * and even root needs to set up some magic numbers in the registers
 * so that some mistake won't make this reboot the whole machine.
 * You can also set the meaning of the ctrl-alt-del-key here.
 *
 * reboot doesn't sync: do that yourself before calling this.
 */
asmlinkage int sys_reboot(int magic, int magic_too, int flag)
{
	if (!suser())
		return -EPERM;
	if (magic != 0xfee1dead || magic_too != 672274793)
		return -EINVAL;
	if (flag == 0x01234567)
		hard_reset_now();
	else if (flag == 0x89ABCDEF)
		C_A_D = 1;
	else if (!flag)
		C_A_D = 0;
	else
		return -EINVAL;
	return (0);
}

/*
 * This function gets called by ctrl-alt-del - ie the keyboard interrupt.
 * As it's called within an interrupt, it may NOT sync: the only choice
 * is wether to reboot at once, or just ignore the ctrl-alt-del.
 */
void ctrl_alt_del(void)
{
	if (C_A_D)
		hard_reset_now();
	else
		send_sig(SIGINT,task[1],1);
}
	

/*
 * This is done BSD-style, with no consideration of the saved gid, except
 * that if you set the effective gid, it sets the saved gid too.  This 
 * makes it possible for a setgid program to completely drop its privileges,
 * which is often a useful assertion to make when you are doing a security
 * audit over a program.
 *
 * The general idea is that a program which uses just setregid() will be
 * 100% compatible with BSD.  A program which uses just setgid() will be
 * 100% compatible with POSIX w/ Saved ID's. 
 */
asmlinkage int sys_setregid(gid_t rgid, gid_t egid)
{
	int old_rgid = current->gid;

	if (rgid != (gid_t) -1) {
		if ((current->egid==rgid) ||
		    (old_rgid == rgid) || 
		    suser())
			current->gid = rgid;
		else
			return(-EPERM);
	}
	if (egid != (gid_t) -1) {
		if ((old_rgid == egid) ||
		    (current->egid == egid) ||
		    suser()) {
			current->egid = egid;
			current->sgid = egid;
		} else {
			current->gid = old_rgid;
			return(-EPERM);
		}
	}
	return 0;
}

/*
 * setgid() is implemeneted like SysV w/ SAVED_IDS 
 */
asmlinkage int sys_setgid(gid_t gid)
{
	if (suser())
		current->gid = current->egid = current->sgid = gid;
	else if ((gid == current->gid) || (gid == current->sgid))
		current->egid = gid;
	else
		return -EPERM;
	return 0;
}

asmlinkage int sys_acct(void)
{
	return -ENOSYS;
}

asmlinkage int sys_phys(void)
{
	return -ENOSYS;
}

asmlinkage int sys_lock(void)
{
	return -ENOSYS;
}

asmlinkage int sys_mpx(void)
{
	return -ENOSYS;
}

asmlinkage int sys_ulimit(void)
{
	return -ENOSYS;
}

asmlinkage int sys_old_syscall(void)
{
	return -ENOSYS;
}

/*
 * Unprivileged users may change the real user id to the effective uid
 * or vice versa.  (BSD-style)
 *
 * When you set the effective uid, it sets the saved uid too.  This 
 * makes it possible for a setuid program to completely drop its privileges,
 * which is often a useful assertion to make when you are doing a security
 * audit over a program.
 *
 * The general idea is that a program which uses just setreuid() will be
 * 100% compatible with BSD.  A program which uses just setuid() will be
 * 100% compatible with POSIX w/ Saved ID's. 
 */
asmlinkage int sys_setreuid(uid_t ruid, uid_t euid)
{
	int old_ruid = current->uid;
	
	if (ruid != (uid_t) -1) {
		if ((current->euid==ruid) ||
		    (old_ruid == ruid) ||
		    suser())
			current->uid = ruid;
		else
			return(-EPERM);
	}
	if (euid != (uid_t) -1) {
		if ((old_ruid == euid) ||
		    (current->euid == euid) ||
		    suser()) {
			current->euid = euid;
			current->suid = euid;
		} else {
			current->uid = old_ruid;
			return(-EPERM);
		}
	}
	return 0;
}

/*
 * setuid() is implemeneted like SysV w/ SAVED_IDS 
 * 
 * Note that SAVED_ID's is deficient in that a setuid root program
 * like sendmail, for example, cannot set its uid to be a normal 
 * user and then switch back, because if you're root, setuid() sets
 * the saved uid too.  If you don't like this, blame the bright people
 * in the POSIX commmittee and/or USG.  Note that the BSD-style setreuid()
 * will allow a root program to temporarily drop privileges and be able to
 * regain them by swapping the real and effective uid.  
 */
asmlinkage int sys_setuid(uid_t uid)
{
	if (suser())
		current->uid = current->euid = current->suid = uid;
	else if ((uid == current->uid) || (uid == current->suid))
		current->euid = uid;
	else
		return -EPERM;
	return(0);
}

asmlinkage int sys_times(struct tms * tbuf)
{
	if (tbuf) {
		int error = verify_area(VERIFY_WRITE,tbuf,sizeof *tbuf);
		if (error)
			return error;
		put_fs_long(current->utime,(unsigned long *)&tbuf->tms_utime);
		put_fs_long(current->stime,(unsigned long *)&tbuf->tms_stime);
		put_fs_long(current->cutime,(unsigned long *)&tbuf->tms_cutime);
		put_fs_long(current->cstime,(unsigned long *)&tbuf->tms_cstime);
	}
	return jiffies;
}

asmlinkage int sys_brk(unsigned long brk)
{
	int freepages;
	unsigned long rlim;
	unsigned long newbrk, oldbrk;

	if (brk < current->end_code)
		return current->brk;
	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(current->brk);
	if (oldbrk == newbrk)
		return current->brk = brk;

	/*
	 * Always allow shrinking brk
	 */
	if (brk <= current->brk) {
		current->brk = brk;
		do_munmap(newbrk, oldbrk-newbrk);
		return brk;
	}
	/*
	 * Check against rlimit and stack..
	 */
	rlim = current->rlim[RLIMIT_DATA].rlim_cur;
	if (rlim >= RLIM_INFINITY)
		rlim = ~0;
	if (brk - current->end_code > rlim || brk >= current->start_stack - 16384)
		return current->brk;
	/*
	 * stupid algorithm to decide if we have enough memory: while
	 * simple, it hopefully works in most obvious cases.. Easy to
	 * fool it, but this should catch most mistakes.
	 */
	freepages = buffermem >> 12;
	freepages += nr_free_pages;
	freepages += nr_swap_pages;
	freepages -= (high_memory - 0x100000) >> 16;
	freepages -= (newbrk-oldbrk) >> 12;
	if (freepages < 0)
		return current->brk;
#if 0
	freepages += current->rss;
	freepages -= oldbrk >> 12;
	if (freepages < 0)
		return current->brk;
#endif
	/*
	 * Ok, we have probably got enough memory - let it rip.
	 */
	current->brk = brk;
	do_mmap(NULL, oldbrk, newbrk-oldbrk,
		PROT_READ|PROT_WRITE|PROT_EXEC,
		MAP_FIXED|MAP_PRIVATE, 0);
	return brk;
}

/*
 * This needs some heave checking ...
 * I just haven't get the stomach for it. I also don't fully
 * understand sessions/pgrp etc. Let somebody who does explain it.
 *
 * OK, I think I have the protection semantics right.... this is really
 * only important on a multi-user system anyway, to make sure one user
 * can't send a signal to a process owned by another.  -TYT, 12/12/91
 *
 * Auch. Had to add the 'did_exec' flag to conform completely to POSIX.
 * LBT 04.03.94
 */
asmlinkage int sys_setpgid(pid_t pid, pid_t pgid)
{
	struct task_struct * p;

	if (!pid)
		pid = current->pid;
	if (!pgid)
		pgid = pid;
	if (pgid < 0)
		return -EINVAL;
	for_each_task(p) {
		if (p->pid == pid)
			goto found_task;
	}
	return -ESRCH;

found_task:
	if (p->p_pptr == current || p->p_opptr == current) {
		if (p->session != current->session)
			return -EPERM;
		if (p->did_exec)
			return -EACCES;
	} else if (p != current)
		return -ESRCH;
	if (p->leader)
		return -EPERM;
	if (pgid != pid) {
		struct task_struct * tmp;
		for_each_task (tmp) {
			if (tmp->pgrp == pgid &&
			 tmp->session == current->session)
				goto ok_pgid;
		}
		return -EPERM;
	}

ok_pgid:
	p->pgrp = pgid;
	return 0;
}

asmlinkage int sys_getpgid(pid_t pid)
{
	struct task_struct * p;

	if (!pid)
		return current->pgrp;
	for_each_task(p) {
		if (p->pid == pid)
			return p->pgrp;
	}
	return -ESRCH;
}

asmlinkage int sys_getpgrp(void)
{
	return current->pgrp;
}

asmlinkage int sys_setsid(void)
{
	if (current->leader)
		return -EPERM;
	current->leader = 1;
	current->session = current->pgrp = current->pid;
	current->tty = -1;
	return current->pgrp;
}

/*
 * Supplementary group ID's
 */
asmlinkage int sys_getgroups(int gidsetsize, gid_t *grouplist)
{
	int i;

	if (gidsetsize) {
		i = verify_area(VERIFY_WRITE, grouplist, sizeof(gid_t) * gidsetsize);
		if (i)
			return i;
	}
	for (i = 0 ; (i < NGROUPS) && (current->groups[i] != NOGROUP) ; i++) {
		if (!gidsetsize)
			continue;
		if (i >= gidsetsize)
			break;
		put_fs_word(current->groups[i], (short *) grouplist);
		grouplist++;
	}
	return(i);
}

asmlinkage int sys_setgroups(int gidsetsize, gid_t *grouplist)
{
	int	i;

	if (!suser())
		return -EPERM;
	if (gidsetsize > NGROUPS)
		return -EINVAL;
	for (i = 0; i < gidsetsize; i++, grouplist++) {
		current->groups[i] = get_fs_word((unsigned short *) grouplist);
	}
	if (i < NGROUPS)
		current->groups[i] = NOGROUP;
	return 0;
}

int in_group_p(gid_t grp)
{
	int	i;

	if (grp == current->egid)
		return 1;

	for (i = 0; i < NGROUPS; i++) {
		if (current->groups[i] == NOGROUP)
			break;
		if (current->groups[i] == grp)
			return 1;
	}
	return 0;
}

asmlinkage int sys_newuname(struct new_utsname * name)
{
	int error;

	if (!name)
		return -EFAULT;
	error = verify_area(VERIFY_WRITE, name, sizeof *name);
	if (!error)
		memcpy_tofs(name,&system_utsname,sizeof *name);
	return error;
}

asmlinkage int sys_uname(struct old_utsname * name)
{
	int error;
	if (!name)
		return -EFAULT;
	error = verify_area(VERIFY_WRITE, name,sizeof *name);
	if (error)
		return error;
	memcpy_tofs(&name->sysname,&system_utsname.sysname,
		sizeof (system_utsname.sysname));
	memcpy_tofs(&name->nodename,&system_utsname.nodename,
		sizeof (system_utsname.nodename));
	memcpy_tofs(&name->release,&system_utsname.release,
		sizeof (system_utsname.release));
	memcpy_tofs(&name->version,&system_utsname.version,
		sizeof (system_utsname.version));
	memcpy_tofs(&name->machine,&system_utsname.machine,
		sizeof (system_utsname.machine));
	return 0;
}

asmlinkage int sys_olduname(struct oldold_utsname * name)
{
	int error;
	if (!name)
		return -EFAULT;
	error = verify_area(VERIFY_WRITE, name,sizeof *name);
	if (error)
		return error;
	memcpy_tofs(&name->sysname,&system_utsname.sysname,__OLD_UTS_LEN);
	put_fs_byte(0,name->sysname+__OLD_UTS_LEN);
	memcpy_tofs(&name->nodename,&system_utsname.nodename,__OLD_UTS_LEN);
	put_fs_byte(0,name->nodename+__OLD_UTS_LEN);
	memcpy_tofs(&name->release,&system_utsname.release,__OLD_UTS_LEN);
	put_fs_byte(0,name->release+__OLD_UTS_LEN);
	memcpy_tofs(&name->version,&system_utsname.version,__OLD_UTS_LEN);
	put_fs_byte(0,name->version+__OLD_UTS_LEN);
	memcpy_tofs(&name->machine,&system_utsname.machine,__OLD_UTS_LEN);
	put_fs_byte(0,name->machine+__OLD_UTS_LEN);
	return 0;
}

/*
 * Only sethostname; gethostname can be implemented by calling uname()
 */
asmlinkage int sys_sethostname(char *name, int len)
{
	int	i;
	
	if (!suser())
		return -EPERM;
	if (len > __NEW_UTS_LEN)
		return -EINVAL;
	for (i=0; i < len; i++) {
		if ((system_utsname.nodename[i] = get_fs_byte(name+i)) == 0)
			return 0;
	}
	system_utsname.nodename[i] = 0;
	return 0;
}

/*
 * Only setdomainname; getdomainname can be implemented by calling
 * uname()
 */
asmlinkage int sys_setdomainname(char *name, int len)
{
	int	i;
	
	if (!suser())
		return -EPERM;
	if (len > __NEW_UTS_LEN)
		return -EINVAL;
	for (i=0; i < len; i++) {
		if ((system_utsname.domainname[i] = get_fs_byte(name+i)) == 0)
			return 0;
	}
	system_utsname.domainname[i] = 0;
	return 0;
}

asmlinkage int sys_getrlimit(unsigned int resource, struct rlimit *rlim)
{
	int error;

	if (resource >= RLIM_NLIMITS)
		return -EINVAL;
	error = verify_area(VERIFY_WRITE,rlim,sizeof *rlim);
	if (error)
		return error;
	put_fs_long(current->rlim[resource].rlim_cur, 
		    (unsigned long *) rlim);
	put_fs_long(current->rlim[resource].rlim_max, 
		    ((unsigned long *) rlim)+1);
	return 0;	
}

asmlinkage int sys_setrlimit(unsigned int resource, struct rlimit *rlim)
{
	struct rlimit new_rlim, *old_rlim;

	if (resource >= RLIM_NLIMITS)
		return -EINVAL;
	old_rlim = current->rlim + resource;
	new_rlim.rlim_cur = get_fs_long((unsigned long *) rlim);
	new_rlim.rlim_max = get_fs_long(((unsigned long *) rlim)+1);
	if (((new_rlim.rlim_cur > old_rlim->rlim_max) ||
	     (new_rlim.rlim_max > old_rlim->rlim_max)) &&
	    !suser())
		return -EPERM;
	*old_rlim = new_rlim;
	return 0;
}

/*
 * It would make sense to put struct rusuage in the task_struct,
 * except that would make the task_struct be *really big*.  After
 * task_struct gets moved into malloc'ed memory, it would
 * make sense to do this.  It will make moving the rest of the information
 * a lot simpler!  (Which we're not doing right now because we're not
 * measuring them yet).
 */
int getrusage(struct task_struct *p, int who, struct rusage *ru)
{
	int error;
	struct rusage r;
	unsigned long	*lp, *lpend, *dest;

	error = verify_area(VERIFY_WRITE, ru, sizeof *ru);
	if (error)
		return error;
	memset((char *) &r, 0, sizeof(r));
	switch (who) {
		case RUSAGE_SELF:
			r.ru_utime.tv_sec = CT_TO_SECS(p->utime);
			r.ru_utime.tv_usec = CT_TO_USECS(p->utime);
			r.ru_stime.tv_sec = CT_TO_SECS(p->stime);
			r.ru_stime.tv_usec = CT_TO_USECS(p->stime);
			r.ru_minflt = p->min_flt;
			r.ru_majflt = p->maj_flt;
			break;
		case RUSAGE_CHILDREN:
			r.ru_utime.tv_sec = CT_TO_SECS(p->cutime);
			r.ru_utime.tv_usec = CT_TO_USECS(p->cutime);
			r.ru_stime.tv_sec = CT_TO_SECS(p->cstime);
			r.ru_stime.tv_usec = CT_TO_USECS(p->cstime);
			r.ru_minflt = p->cmin_flt;
			r.ru_majflt = p->cmaj_flt;
			break;
		default:
			r.ru_utime.tv_sec = CT_TO_SECS(p->utime + p->cutime);
			r.ru_utime.tv_usec = CT_TO_USECS(p->utime + p->cutime);
			r.ru_stime.tv_sec = CT_TO_SECS(p->stime + p->cstime);
			r.ru_stime.tv_usec = CT_TO_USECS(p->stime + p->cstime);
			r.ru_minflt = p->min_flt + p->cmin_flt;
			r.ru_majflt = p->maj_flt + p->cmaj_flt;
			break;
	}
	lp = (unsigned long *) &r;
	lpend = (unsigned long *) (&r+1);
	dest = (unsigned long *) ru;
	for (; lp < lpend; lp++, dest++) 
		put_fs_long(*lp, dest);
	return 0;
}

asmlinkage int sys_getrusage(int who, struct rusage *ru)
{
	if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)
		return -EINVAL;
	return getrusage(current, who, ru);
}

asmlinkage int sys_umask(int mask)
{
	int old = current->umask;

	current->umask = mask & S_IRWXUGO;
	return (old);
}
