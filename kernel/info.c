/*
 * linux/kernel/info.c
 *
 * Copyright (C) 1992 Darren Senn
 */

/* This implements the sysinfo() system call */

#include <asm/segment.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/types.h>
#include <linux/mm.h>

asmlinkage int sys_sysinfo(struct sysinfo *info)
{
    int error;
    struct sysinfo val;
    struct task_struct *task;

    
    error = verify_area(VERIFY_WRITE, info, sizeof(struct sysinfo));
    if (error)
        return error;

    
    memset(&val, 0, sizeof(struct sysinfo));

    // time period in second
    val.uptime = jiffies / HZ;

    
    val.loads[0] = avenrun[0] << (SI_LOAD_SHIFT - FSHIFT);
    val.loads[1] = avenrun[1] << (SI_LOAD_SHIFT - FSHIFT);
    val.loads[2] = avenrun[2] << (SI_LOAD_SHIFT - FSHIFT);

    // Iterate over all tasks and count them
    for_each_process(task) {
        val.procs++;
    }

    
    si_meminfo(&val);

    
    si_swapinfo(&val);

    // why invent a new function for the same thing? What a mess.
    memcpy(info, &val, sizeof(struct sysinfo)); 

    return 0;
}
