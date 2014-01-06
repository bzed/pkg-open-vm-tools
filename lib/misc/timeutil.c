/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * timeutil.c --
 *
 *   Miscellaneous time related utility functions.
 */


#include "safetime.h"

#if defined(N_PLAT_NLM)
#  include <sys/timeval.h>
#elif defined(_WIN32)
#  include <wtypes.h>
#else
#  include <sys/time.h>
#endif

#include "vmware.h"
/* For HARD_EXPIRE --hpreg */
#include "vm_version.h"
#include "vm_basic_asm.h"
#include "timeutil.h"
#include "str.h"
#include "util.h"

/*
 * NT time of the Unix epoch:
 * midnight January 1, 1970 UTC
 */
#define UNIX_EPOCH ((((uint64)369 * 365) + 89) * 24 * 3600 * 10000000)

/*
 * NT time of the Unix 32 bit signed time_t wraparound:
 * 03:14:07 January 19, 2038 UTC
 */
#define UNIX_S32_MAX (UNIX_EPOCH + (uint64)0x80000000 * 10000000)


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_DaysAdd --
 *
 *    Add 'nr' days to a date.
 *    This function can be optimized a lot if needed.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

void
TimeUtil_DaysAdd(TimeUtil_Date *d, // IN/OUT
                 unsigned int nr)  // IN
{
   static unsigned int monthdays[13] = { 0,
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   unsigned int i;

   /*
    * Initialize the table
    */

   if (   (d->year % 4) == 0
       && (   (d->year % 100) != 0
           || (d->year % 400) == 0)) {
      /* Leap year */
      monthdays[2] = 29;
   } else {
      monthdays[2] = 28;
   }

   for (i = 0; i < nr; i++) {
      /*
       * Add 1 day to the date
       */

      d->day++;
      if (d->day > monthdays[d->month]) {
         d->day = 1;
         d->month++;
         if (d->month > 12) {
            d->month = 1;
            d->year++;

            /*
             * Update the table
             */

            if (   (d->year % 4) == 0
                && (   (d->year % 100) != 0
                    || (d->year % 400) == 0)) {
               /* Leap year */
               monthdays[2] = 29;
            } else {
               monthdays[2] = 28;
            }
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_PopulateWithCurrent --
 *
 *    Populate the given date object with the current date and time.
 *
 *    If 'local' is TRUE, the time will be expressed in the local time
 *    zone. Otherwise, the time will be expressed in UTC.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

void
TimeUtil_PopulateWithCurrent(Bool local,       // IN
                             TimeUtil_Date *d) // OUT
{
#ifdef _WIN32
   SYSTEMTIME currentTime;

   ASSERT(d);

   if (local) {
      GetLocalTime(&currentTime);
   } else {
      GetSystemTime(&currentTime);
   }
   d->year   = currentTime.wYear;
   d->month  = currentTime.wMonth;
   d->day    = currentTime.wDay;
   d->hour   = currentTime.wHour;
   d->minute = currentTime.wMinute;
   d->second = currentTime.wSecond;
#else
   struct tm *currentTime;
   time_t utcTime;

   ASSERT(d);

   utcTime = time(NULL);
   if (local) {
      currentTime = localtime(&utcTime);
   } else {
      currentTime = gmtime(&utcTime);
   }
   ASSERT_NOT_IMPLEMENTED(currentTime);
   d->year   = 1900 + currentTime->tm_year;
   d->month  = currentTime->tm_mon + 1;
   d->day    = currentTime->tm_mday;
   d->hour   = currentTime->tm_hour;
   d->minute = currentTime->tm_min;
   d->second = currentTime->tm_sec;
#endif // _WIN32
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_DaysLeft --
 *
 *    Computes the number of days left before a given date
 *
 * Results:
 *    0: the given date is in the past
 *    1 to MAX_DAYSLEFT: if there are 1 to MAX_DAYSLEFT days left
 *    MAX_DAYSLEFT+1 if there are more than MAX_DAYSLEFT days left
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

unsigned int
TimeUtil_DaysLeft(TimeUtil_Date const *d) // IN
{
   TimeUtil_Date c;
   unsigned int i;

   /* Get the current local date. */
   TimeUtil_PopulateWithCurrent(TRUE, &c);

   /* Compute how many days we can add to the current date before reaching
      the given date */
   for (i = 0; i < MAX_DAYSLEFT + 1; i++) {
      if (    c.year > d->year
          || (c.year == d->year && c.month > d->month)
          || (c.year == d->year && c.month == d->month && c.day >= d->day)) {
         /* current date >= given date */
         return i;
      }

      TimeUtil_DaysAdd(&c, 1);
   }

   /* There are at least MAX_DAYSLEFT+1 days left */
   return MAX_DAYSLEFT + 1;
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_ExpirationLowerThan --
 *
 *    Determine if 'left' is lower than 'right'
 *
 * Results:
 *    TRUE if yes
 *    FALSE if no
 *      
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

Bool
TimeUtil_ExpirationLowerThan(TimeUtil_Expiration const *left,  // IN
                             TimeUtil_Expiration const *right) // IN
{
   if (left->expires == FALSE) {
      return FALSE;
   }

   if (right->expires == FALSE) {
      return TRUE;
   }

   if (left->when.year < right->when.year) {
      return TRUE;
   }

   if (left->when.year > right->when.year) {
      return FALSE;
   }

   if (left->when.month < right->when.month) {
      return TRUE;
   }

   if (left->when.month > right->when.month) {
      return FALSE;
   }

   if (left->when.day < right->when.day) {
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_DateLowerThan --
 *
 *    Determine if 'left' is lower than 'right'
 *
 * Results:
 *    TRUE if yes
 *    FALSE if no
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

Bool
TimeUtil_DateLowerThan(TimeUtil_Date const *left,  // IN
                       TimeUtil_Date const *right) // IN
{
   ASSERT(left);
   ASSERT(right);

   if (left->year < right->year) {
      return TRUE;
   }

   if (left->year > right->year) {
      return FALSE;
   }

   if (left->month < right->month) {
      return TRUE;
   }

   if (left->month > right->month) {
      return FALSE;
   }

   if (left->day < right->day) {
      return TRUE;
   }

   if (left->day > right->day) {
      return FALSE;
   }

   if (left->hour < right->hour) {
      return TRUE;
   }

   if (left->hour > right->hour) {
      return FALSE;
   }

   if (left->minute < right->minute) {
      return TRUE;
   }

   if (left->minute > right->minute) {
      return FALSE;
   }
 
   if (left->second < right->second) {
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_ProductExpiration --
 *
 *    Retrieve the expiration information associated to the product in 'e'
 *
 * Results:
 *    None
 *      
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

void
TimeUtil_ProductExpiration(TimeUtil_Expiration *e) // OUT
{

   /*
    * The hard_expire string is used by post-build processing scripts to
    * determine if a build is set to expire or not.
    */
#ifdef HARD_EXPIRE
   static char *hard_expire = "Expire";
   (void)hard_expire;

   ASSERT(e);

   e->expires = TRUE;

   /*
    * Decode the hard-coded product expiration date.
    */

   e->when.day = HARD_EXPIRE;
   e->when.year = e->when.day / ((DATE_MONTH_MAX + 1) * (DATE_DAY_MAX + 1));
   e->when.day -= e->when.year * ((DATE_MONTH_MAX + 1) * (DATE_DAY_MAX + 1));
   e->when.month = e->when.day / (DATE_DAY_MAX + 1);
   e->when.day -= e->when.month * (DATE_DAY_MAX + 1);

   e->daysLeft = TimeUtil_DaysLeft(&e->when);
#else
   static char *hard_expire = "No Expire";
   (void)hard_expire;

   ASSERT(e);

   e->expires = FALSE;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_GetTimeFormat --
 *
 *    Converts a UTC time value to a human-readable string.
 *
 * Results:
 *    Returns the a formatted string of the given UTC time.  It is the
 *    caller's responsibility to free this string.  May return NULL.
 *
 *    If Win32, the time will be formatted according to the current
 *    locale.
 *      
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

char *
TimeUtil_GetTimeFormat(int64 utcTime,  // IN
                       Bool showDate,  // IN
                       Bool showTime)  // IN
{
#ifdef _WIN32
   SYSTEMTIME systemTime = { 0 };
   TCHAR dateStr[100];
   TCHAR timeStr[100];
   
   if (!showDate && !showTime) {
      return NULL;
   }

   if (!TimeUtil_UTCTimeToSystemTime((const __time64_t) utcTime, &systemTime)) {
      return NULL;
   }
   
   GetDateFormat(LOCALE_USER_DEFAULT, DATE_SHORTDATE,
                 &systemTime, NULL, dateStr, ARRAYSIZE(dateStr));
   
   GetTimeFormat(LOCALE_USER_DEFAULT, 0, &systemTime, NULL,
                 timeStr, ARRAYSIZE(timeStr));
   
   if (showDate && showTime) {
      return Str_Asprintf(NULL, "%s %s", dateStr, timeStr);
   } else {
      return Str_Asprintf(NULL, "%s", showDate ? dateStr : timeStr);
   }

#else
   char *str;
   str = Util_SafeStrdup(ctime((const time_t *) &utcTime));
   str[strlen(str)-1] = '\0';
   return str;
#endif // _WIN32
}

#if !defined _WIN32 && !defined N_PLAT_NLM
/*
 *-----------------------------------------------------------------------------
 *
 * TimeUtil_NtTimeToUnixTime --
 *
 *    Convert from Windows NT time to Unix time. If NT time is outside of
 *    Unix time range (1970-2038), returned time is nearest time valid in
 *    Unix.
 *
 * Results:
 *    0        on success
 *    non-zero if NT time is outside of valid range for UNIX
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
TimeUtil_NtTimeToUnixTime(struct timespec *unixTime,   // OUT: Time in Unix format
                          VmTimeType ntTime)           // IN: Time in Windows NT format
{
#ifndef VM_X86_64
   uint32 sec;
   uint32 nsec;

   ASSERT(unixTime);
   /* We assume that time_t is 32bit */
   ASSERT(sizeof (unixTime->tv_sec) == 4);

   /* Cap NT time values that are outside of Unix time's range */

   if (ntTime >= UNIX_S32_MAX) {
      unixTime->tv_sec = 0x7FFFFFFF;
      unixTime->tv_nsec = 0;
      return 1;
   }
#else
   ASSERT(unixTime);
#endif // VM_X86_64

   if (ntTime < UNIX_EPOCH) {
      unixTime->tv_sec = 0;
      unixTime->tv_nsec = 0;
      return -1;
   }

#ifndef VM_X86_64
   Div643232(ntTime - UNIX_EPOCH, 10000000, &sec, &nsec);
   unixTime->tv_sec = sec;
   unixTime->tv_nsec = nsec * 100;
#else
   unixTime->tv_sec = (ntTime - UNIX_EPOCH) / 10000000;
   unixTime->tv_nsec = ((ntTime - UNIX_EPOCH) % 10000000) * 100;
#endif // VM_X86_64

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TimeUtil_UnixTimeToNtTime --
 *
 *    Convert from Unix time to Windows NT time.
 *
 * Results:
 *    The time in Windows NT format.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

VmTimeType
TimeUtil_UnixTimeToNtTime(struct timespec unixTime) // IN: Time in Unix format
{
   return (VmTimeType)unixTime.tv_sec * 10000000 +
      unixTime.tv_nsec / 100 + UNIX_EPOCH;
}
#endif // _WIN32 && N_PLAT_NLM

#ifdef _WIN32
/*
 *----------------------------------------------------------------------
 *
 * TimeUtil_UTCTimeToSystemTime --
 *
 *    Converts the time from UTC time to SYSTEMTIME
 *
 * Results:
 *    TRUE if the time was converted successfully, FALSE otherwise.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

Bool
TimeUtil_UTCTimeToSystemTime(const __time64_t utcTime,   // IN
                             SYSTEMTIME *systemTime)     // OUT
{
   int atmYear;
   int atmMonth;

   struct tm *atm;

   /*
    * _localtime64 support years up through 3000.  At least it says
    * so.  I'm getting garbage only after reaching year 4408.
    */
   if (utcTime < 0 || utcTime > (60LL * 60 * 24 * 365 * (3000 - 1970))) {
      return FALSE;
   }
   
   atm = _localtime64(&utcTime);
   if (atm == NULL) {
      return FALSE;
   }

   atmYear = atm->tm_year + 1900;
   atmMonth = atm->tm_mon + 1;

   /*
    * Windows's SYSTEMTIME documentation says that these are limits...
    * Main reason for this test is to cut out negative values _localtime64
    * likes to return for some inputs.
    */
   if (atmYear < 1601 || atmYear > 30827 ||
       atmMonth < 1 || atmMonth > 12 ||
       atm->tm_wday < 0 || atm->tm_wday > 6 ||
       atm->tm_mday < 1 || atm->tm_mday > 31 ||
       atm->tm_hour < 0 || atm->tm_hour > 23 ||
       atm->tm_min < 0 || atm->tm_min > 59 ||
       /* Allow leap second, just in case... */
       atm->tm_sec < 0 || atm->tm_sec > 60) {
      return FALSE;
   }

   systemTime->wYear         = (WORD) atmYear;
   systemTime->wMonth        = (WORD) atmMonth;
   systemTime->wDayOfWeek    = (WORD) atm->tm_wday;
   systemTime->wDay          = (WORD) atm->tm_mday;
   systemTime->wHour         = (WORD) atm->tm_hour;
   systemTime->wMinute       = (WORD) atm->tm_min;
   systemTime->wSecond       = (WORD) atm->tm_sec;
   systemTime->wMilliseconds = 0;

   return TRUE;
}
#endif // _WIN32
