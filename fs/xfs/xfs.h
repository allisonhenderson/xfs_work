// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_H__
#define __XFS_H__

#ifdef CONFIG_XFS_DEBUG
#define DEBUG 1
#endif

#ifdef CONFIG_XFS_ASSERT_FATAL
#define XFS_ASSERT_FATAL 1
#endif

#ifdef CONFIG_XFS_WARN
#define XFS_WARN 1
#endif

//#define my_debug //donnuthing
//#define my_debug(a...)          {printk("ach: [%05d]%s:%s:%d ", current->pid,__FILE__, __FUNCTION__,__LINE__);printk(KERN_CONT a);}

#include <linux/stacktrace.h>

#define STACK_DEPTH 25
#define MAX_FNAME_LEN 50


#define my_debug(a...)          {\
                                       static unsigned long entries[STACK_DEPTH];\
                                       static char fnames[STACK_DEPTH][MAX_FNAME_LEN+1];\
                                       static struct stack_trace trace;\
                                       int num_entries = 0;\
                                       int i = 0;\
                                       int j = 0;\
                                       int n = __LINE__;\
                                       int flen = strlen(__FILE__);\
                                       while(n>0){n = n/10; flen++;}\
                                       memset(&trace, 0, sizeof(trace));\
                                       memset(entries, 0, sizeof(entries));\
                                       memset(fnames, 0, sizeof(fnames));\
                                       trace.entries = entries;\
                                       trace.max_entries = STACK_DEPTH;\
                                       trace.skip = 0;\
                                       save_stack_trace(&trace);\
                                       num_entries = trace.nr_entries;\
                                       j = trace.nr_entries;\
                                       /* Get the back trace*/\
                                       for (i = 0; i < num_entries; i++)\
                                               snprintf(fnames[i],MAX_FNAME_LEN,"%ps",(char *)trace.entries[i]);\
                                       /*Scan through it and trim off the trailing [xfs] chars. Gets too verbose*/\
                                       for (i = 0; i < num_entries; i++){\
                                               int len = strlen(&fnames[i][0]);\
                                               if (len >= 6 && strncmp(&fnames[i][len-6], " [xfs]", 6) == 0){\
                                                       fnames[i][len-6] = 0;\
                                                       /*keep track of the last one we saw*/\
                                                       j = i;\
                                               }\
                                       }\
                                       /*get rid of preceeding non xfs function names.  Dont care about seeing those*/\
                                       if (j < num_entries) j++;\
                                       num_entries-=num_entries-j;\
                                       printk("[%05d] [%s:%d]%-*s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                               "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                                "%s%s"\
                                               "%s: ",\
                                               (current != NULL?current->pid:0x0),__FILE__,__LINE__,(flen < 40?40-flen:0),"",\
                                                (num_entries>24?fnames[24]:""), (num_entries>24?":":""),\
                                                (num_entries>23?fnames[23]:""), (num_entries>23?":":""),\
                                                (num_entries>22?fnames[22]:""), (num_entries>22?":":""),\
                                                (num_entries>21?fnames[21]:""), (num_entries>21?":":""),\
                                                (num_entries>20?fnames[20]:""), (num_entries>20?":":""),\
                                                (num_entries>19?fnames[19]:""), (num_entries>19?":":""),\
                                                (num_entries>18?fnames[18]:""), (num_entries>18?":":""),\
                                                (num_entries>17?fnames[17]:""), (num_entries>17?":":""),\
                                                (num_entries>16?fnames[16]:""), (num_entries>16?":":""),\
                                                (num_entries>15?fnames[15]:""), (num_entries>15?":":""),\
                                                (num_entries>14?fnames[14]:""), (num_entries>14?":":""),\
                                                (num_entries>13?fnames[13]:""), (num_entries>13?":":""),\
                                                (num_entries>12?fnames[12]:""), (num_entries>12?":":""),\
                                                (num_entries>11?fnames[11]:""), (num_entries>11?":":""),\
                                                (num_entries>10?fnames[10]:""), (num_entries>10?":":""),\
                                                (num_entries> 9?fnames[ 9]:""), (num_entries> 9?":":""),\
                                                (num_entries> 8?fnames[ 8]:""), (num_entries> 8?":":""),\
                                                (num_entries> 7?fnames[ 7]:""), (num_entries> 7?":":""),\
                                                (num_entries> 6?fnames[ 6]:""), (num_entries> 6?":":""),\
                                                (num_entries> 5?fnames[ 5]:""), (num_entries> 5?":":""),\
                                                (num_entries> 4?fnames[ 4]:""), (num_entries> 4?":":""),\
                                                (num_entries> 3?fnames[ 3]:""), (num_entries> 3?":":""),\
                                                (num_entries> 2?fnames[ 2]:""), (num_entries> 2?":":""),\
                                                (num_entries> 1?fnames[ 1]:""), (num_entries> 1?":":""),\
                                                (num_entries> 0?fnames[ 0]:""), (num_entries> 0?":":""),\
                                               (__FUNCTION__));\
                                               printk(KERN_CONT a);}


#include "xfs_linux.h"


#endif	/* __XFS_H__ */
