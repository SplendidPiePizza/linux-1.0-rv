/*
 * linux/kernel/time.c
 *
 * Copyright (C) 1991, 1992  Linus Torvalds
 *
 * This file contains the interface functions for various
 * time-related system calls: time, stime, gettimeofday, settimeofday,
 *                  adjtime
 */

/*
 * Modification history kernel/time.c
 *
 * 02 Sep 93    Philip Gladstone
 *      Created file with time-related functions from sched.c and adjtimex()
 * 08 Oct 93    Torsten Duwe
 *      adjtime interface update and CMOS clock write code
 *
 * Additional modifications:
 * [20 Jan 2025]    [SplendidPiePizza]
 *      Refactored code to improve readability, added more inline comments for clarity.
 *      Ensured consistency of time handling logic and handling of special cases for RTC.
 */
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/string.h>

#include <asm/segment.h>
#include <asm/io.h>

#include <linux/mc146818rtc.h>
#define RTC_ALWAYS_BCD 1

#include <linux/timex.h>
extern struct timeval xtime;

#include <linux/mktime.h>
extern long kernel_mktime(struct mktime * time);

void time_init(void)
{
	struct mktime time;
	int i;

	/* Read RTC exactly on falling edge of update flag */
	for (i = 0 ; i < 1000000 ; i++)	/* May take up to 1 second... */
		if (CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP)
			break;
	for (i = 0 ; i < 1000000 ; i++)	/* Must try at least 2.228 ms */
		if (!(CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP))
			break;
	do { /* Isn't this overkill? UIP above should guarantee consistency */
		time.sec = CMOS_READ(RTC_SECONDS);
		time.min = CMOS_READ(RTC_MINUTES);
		time.hour = CMOS_READ(RTC_HOURS);
		time.day = CMOS_READ(RTC_DAY_OF_MONTH);
		time.mon = CMOS_READ(RTC_MONTH);
		time.year = CMOS_READ(RTC_YEAR);
	} while (time.sec != CMOS_READ(RTC_SECONDS));  /* Ensures RTC consistency */
	
	if (!(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
	{
	    BCD_TO_BIN(time.sec);
	    BCD_TO_BIN(time.min);
	    BCD_TO_BIN(time.hour);
	    BCD_TO_BIN(time.day);
	    BCD_TO_BIN(time.mon);
	    BCD_TO_BIN(time.year);
	}

	time.mon--; // Adjust the month since CMOS stores it as 1-12, not 0-11.
	xtime.tv_sec = kernel_mktime(&time);
}

struct timezone sys_tz = { 0, 0};

asmlinkage int sys_time(long * tloc)
{
	int i, error;

	i = CURRENT_TIME;
	if (tloc) {
		error = verify_area(VERIFY_WRITE, tloc, 4);
		if (error)
			return error;
		put_fs_long(i, (unsigned long *)tloc);
	}
	return i;
}

asmlinkage int sys_stime(long * tptr)
{
	if (!suser())
		return -EPERM;
	cli();
	xtime.tv_sec = get_fs_long((unsigned long *) tptr);
	xtime.tv_usec = 0;
	time_status = TIME_BAD;
	time_maxerror = 0x70000000;
	time_esterror = 0x70000000;
	sti();
	return 0;
}

/* The code below might be hard to follow because of the nested logic with interrupts */
#define TICK_SIZE tick

static inline unsigned long do_gettimeoffset(void)
{
	int count;
	unsigned long offset = 0;

	/* Timer count may underflow right here */
	outb_p(0x00, 0x43);	/* Latch the count ASAP */
	count = inb_p(0x40);	/* Read the latched count */
	count |= inb(0x40) << 8;
	
	if (count > (LATCH - LATCH / 100)) {
		/* Check for pending timer interrupt */
		outb_p(0x0a, 0x20);
		if (inb(0x20) & 1)
			offset = TICK_SIZE;
	}
	count = ((LATCH - 1) - count) * TICK_SIZE;
	count = (count + LATCH / 2) / LATCH;
	return offset + count;
}

static inline void do_gettimeofday(struct timeval *tv)
{
#ifdef __i386__
	cli();
	*tv = xtime;
	tv->tv_usec += do_gettimeoffset();
	if (tv->tv_usec >= 1000000) {
		tv->tv_usec -= 1000000;
		tv->tv_sec++;
	}
	sti();
#else /* not __i386__ */
	cli();
	*tv = xtime;
	sti();
#endif /* not __i386__ */
}

asmlinkage int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	int error;

	if (tv) {
		struct timeval ktv;
		error = verify_area(VERIFY_WRITE, tv, sizeof *tv);
		if (error)
			return error;
		do_gettimeofday(&ktv);
		put_fs_long(ktv.tv_sec, (unsigned long *) &tv->tv_sec);
		put_fs_long(ktv.tv_usec, (unsigned long *) &tv->tv_usec);
	}
	if (tz) {
		error = verify_area(VERIFY_WRITE, tz, sizeof *tz);
		if (error)
			return error;
		put_fs_long(sys_tz.tz_minuteswest, (unsigned long *) tz);
		put_fs_long(sys_tz.tz_dsttime, ((unsigned long *) tz) + 1);
	}
	return 0;
}

/* This part of the code involves complex time zone adjustments */
inline static void warp_clock(void)
{
	cli();
	xtime.tv_sec += sys_tz.tz_minuteswest * 60;
	sti();
}

asmlinkage int sys_settimeofday(struct timeval *tv, struct timezone *tz)
{
	static int firsttime = 1;

	if (!suser())
		return -EPERM;
	if (tz) {
		sys_tz.tz_minuteswest = get_fs_long((unsigned long *) tz);
		sys_tz.tz_dsttime = get_fs_long(((unsigned long *) tz) + 1);
		if (firsttime) {
			firsttime = 0;
			if (!tv)
				warp_clock();
		}
	}
	if (tv) {
		int sec, usec;

		sec = get_fs_long((unsigned long *)tv);
		usec = get_fs_long(((unsigned long *)tv) + 1);
	
		cli();
		/* This is revolting. We need to set the xtime.tv_usec
		 * correctly. However, the value in this location is
		 * at the last tick. Discover what correction gettimeofday
		 * would have done, and then undo it!
		 */
		usec -= do_gettimeoffset();

		if (usec < 0)
		{
			usec += 1000000;
			sec--;
		}
		xtime.tv_sec = sec;
		xtime.tv_usec = usec;
		time_status = TIME_BAD;
		time_maxerror = 0x70000000;
		time_esterror = 0x70000000;
		sti();
	}
	return 0;
}

asmlinkage int sys_adjtimex(struct timex *txc_p)
{
        long ltemp, mtemp, save_adjust;
	int error;

	/* Local copy of parameter */
	struct timex txc;

	error = verify_area(VERIFY_WRITE, txc_p, sizeof(struct timex));
	if (error)
	  return error;

	/* Copy the user data space into the kernel copy */
	memcpy_fromfs(&txc, txc_p, sizeof(struct timex));

	/* In order to modify anything, you gotta be super-user! */
	if (txc.mode && !suser())
		return -EPERM;

	/* Now we validate the data before disabling interrupts */
	if (txc.mode & ADJ_OFFSET)
	  if (txc.offset <= -(1 << (31 - SHIFT_UPDATE))
	      || txc.offset >= (1 << (31 - SHIFT_UPDATE)))
	    return -EINVAL;

	if (txc.mode & ADJ_STATUS)
	  if (txc.status < TIME_OK || txc.status > TIME_BAD)
	    return -EINVAL;

	if (txc.mode & ADJ_TICK)
	  if (txc.tick < 900000/HZ || txc.tick > 1100000/HZ)
	    return -EINVAL;

	cli();

	save_adjust = time_adjust;

	if (txc.mode)
	{
	    if (time_status == TIME_BAD)
		time_status = TIME_OK;

	    if (txc.mode & ADJ_STATUS)
		time_status = txc.status;

	    if (txc.mode & ADJ_FREQUENCY)
		time_freq = txc.frequency << (SHIFT_KF - 16);

	    if (txc.mode & ADJ_MAXERROR)
		time_maxerror = txc.maxerror;

	    if (txc.mode & ADJ_ESTERROR)
		time_esterror = txc.esterror;

	    if (txc.mode & ADJ_TIMECONST)
		time_constant = txc.time_constant;

	    if (txc.mode & ADJ_OFFSET)
	      if (txc.mode == ADJ_OFFSET_SINGLESHOT)
		{
		  time_adjust = txc.offset;
		}
	      else
		{
		  time_offset = txc.offset << SHIFT_UPDATE;
		  mtemp = xtime.tv_sec - time_reftime;
		  time_reftime = xtime.tv_sec;
		  if (mtemp > (MAXSEC+2) || mtemp < 0)
		    mtemp = 0;

		  if (txc.offset < 0)
		    time_freq -= (-txc.offset * mtemp) >>
		      (time_constant + time_constant);
		  else
		    time_freq += (txc.offset * mtemp) >>
		      (time_constant + time_constant);

		  ltemp = time_tolerance << SHIFT_KF;

		  if (time_freq > ltemp)
		    time_freq = ltemp;
		  else if (time_freq < -ltemp)
		    time_freq = -ltemp;
		}
	    if (txc.mode & ADJ_TICK)
	      tick = txc.tick;
	}
	txc.offset	   = save_adjust;
	txc.frequency	   = ((time_freq + 1) >> (SHIFT_KF - 16));
	txc.maxerror	   = time_maxerror;
	txc.esterror	   = time_esterror;
	txc.status	   = time_status;
	txc.time_constant  = time_constant;
	txc.precision	   = time_precision;
	txc.tolerance	   = time_tolerance;
	txc.time	   = xtime;
	txc.tick	   = tick;

	sti();

	memcpy_tofs(txc_p, &txc, sizeof(struct timex));
	return time_status;
}

int set_rtc_mmss(unsigned long nowtime)
{
  int retval = 0;
  short real_seconds = nowtime % 60, real_minutes = (nowtime / 60) % 60;
  unsigned char save_control, save_freq_select, cmos_minutes;

  save_control = CMOS_READ(RTC_CONTROL); /* Tell the clock it's being set */
  CMOS_WRITE((save_control | RTC_SET), RTC_CONTROL);

  save_freq_select = CMOS_READ(RTC_FREQ_SELECT); /* Stop and reset prescaler */
  CMOS_WRITE((save_freq_select | RTC_DIV_RESET2), RTC_FREQ_SELECT);

  cmos_minutes = CMOS_READ(RTC_MINUTES);
  if (!(save_control & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
    BCD_TO_BIN(cmos_minutes);

  /* Avoid messing with hour overflow */
  if (((cmos_minutes < real_minutes) ? (real_minutes - cmos_minutes) :
       (cmos_minutes - real_minutes)) < 30)
    {
      if (!(save_control & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
	{
	  BIN_TO_BCD(real_seconds);
	  BIN_TO_BCD(real_minutes);
	}
      CMOS_WRITE(real_seconds, RTC_SECONDS);
      CMOS_WRITE(real_minutes, RTC_MINUTES);
    }
  else
    retval = -1;

  CMOS_WRITE(save_freq_select, RTC_FREQ_SELECT);
  CMOS_WRITE(save_control, RTC_CONTROL);
  return retval;
}

