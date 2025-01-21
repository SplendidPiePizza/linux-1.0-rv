/*
 * linux/kernel/itimer.c
 *
 * Copyright (C) 1992 Darren Senn
 */

/* These are all the functions necessary to implement itimers */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/time.h>

#include <asm/segment.h>

static unsigned long tv_to_jiffies(struct timeval *value)
{
    return (unsigned long)value->tv_sec * HZ +
           (unsigned long)(value->tv_usec + (1000000 / HZ - 1)) /
               (1000000 / HZ);
}

static void jiffies_to_tv(unsigned long jiffies, struct timeval *value)
{
    value->tv_usec = (jiffies % HZ) * (1000000 / HZ); // Who thought mixing this bullshit timeval and jiffies was a good idea?! This is pathetic.
    value->tv_sec = jiffies / HZ;
}

int get_itimer(int which, struct itimerval *value)
{
    unsigned long val, interval;

    switch (which)
    {
    case ITIMER_REAL:
        val = current->it_real_value;
        interval = current->it_real_incr;
        break;
    case ITIMER_VIRTUAL:
        val = current->it_virt_value;
        interval = current->it_virt_incr;
        break;
    case ITIMER_PROF:
        val = current->it_prof_value;
        interval = current->it_prof_incr;
        break;
    default:
        return -EINVAL;
    }

    jiffies_to_tv(val, &value->it_value);
    jiffies_to_tv(interval, &value->it_interval);
    return 0;
}

asmlinkage int sys_getitimer(int which, struct itimerval *value)
{
    struct itimerval get_buffer;
    int error;

    if (!value)
        return -EFAULT;

    error = get_itimer(which, &get_buffer);
    if (error)
        return error;

    error = verify_area(VERIFY_WRITE, value, sizeof(struct itimerval));
    if (error)
        return error;

    memcpy_tofs(value, &get_buffer, sizeof(get_buffer));
    return 0;
}

int set_itimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
    unsigned long interval, time_val;
    int result;

    interval = tv_to_jiffies(&value->it_interval);
    time_val = tv_to_jiffies(&value->it_value);

    if (ovalue)
    {
        result = get_itimer(which, ovalue);
        if (result < 0)
            return result;
    }

    switch (which)
    {
    case ITIMER_REAL:
        if (time_val)
        {
            time_val += 1 + itimer_ticks;
            if (time_val < itimer_next)
                itimer_next = time_val;
        }
        current->it_real_value = time_val;
        current->it_real_incr = interval;
        break;

    case ITIMER_VIRTUAL:
        if (time_val)
            time_val++;
        current->it_virt_value = time_val;
        current->it_virt_incr = interval;
        break;

    case ITIMER_PROF:
        if (time_val)
            time_val++;
        current->it_prof_value = time_val;
        current->it_prof_incr = interval;
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

asmlinkage int sys_setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
    struct itimerval set_buffer, get_buffer;
    int error;

    if (!value)
        memset(&set_buffer, 0, sizeof(set_buffer));
    else
        memcpy_fromfs(&set_buffer, value, sizeof(set_buffer));

    error = set_itimer(which, &set_buffer, ovalue ? &get_buffer : NULL);
    if (error || !ovalue)
        return error;

    error = verify_area(VERIFY_WRITE, ovalue, sizeof(struct itimerval));
    if (!error)
        memcpy_tofs(ovalue, &get_buffer, sizeof(get_buffer));

    return error;
}
