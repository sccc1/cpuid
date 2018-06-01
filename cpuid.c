/*
** cpuid dumps CPUID information for each CPU.
** Copyright 2003,2004,2005,2006,2010,2011,2012,2013,2014,2015,2016,2017,2018 by 
** Todd Allen.
** 
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifdef __linux__
#define USE_CPUID_MODULE
#define USE_KERNEL_SCHED_SETAFFINITY
#endif

#if __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__ >= 40300
#define USE_CPUID_COUNT
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <getopt.h>

#ifdef USE_CPUID_MODULE
#include <linux/major.h>
#endif

#ifdef USE_CPUID_COUNT
#include <cpuid.h>
#endif

#ifdef USE_KERNEL_SCHED_SETAFFINITY
#include <sys/syscall.h>
#else
#include <sched.h>
#endif

typedef int   boolean;
#define TRUE  1
#define FALSE 0

typedef char*              string;
typedef const char*        cstring;
typedef const char* const  ccstring;
#define SAME  0

#define STR(x)   #x
#define XSTR(x)  STR(x)


#define MAX(l,r)            ((l) > (r) ? (l) : (r))

#define LENGTH(array, type) (sizeof(array) / sizeof(type))
#define STRLEN(s)           (LENGTH(s,char) - 1)

#define BPI  32
#define POWER2(power) \
   (1 << (power))
#define RIGHTMASK(width) \
   (((width) >= BPI) ? ~0 : POWER2(width)-1)
#define BIT_EXTRACT_LE(value, start, after) \
   (((value) & RIGHTMASK(after)) >> start)

#define WORD_EAX  0
#define WORD_EBX  1
#define WORD_ECX  2
#define WORD_EDX  3

#define WORD_NUM  4

const char*  program = NULL;

static boolean  strregexp(const char*  haystack,
                          const char*  needle)
{
   regex_t  re;
   int      status;
   status = regcomp(&re, needle, REG_NOSUB);
   if (status != 0) {
      size_t  size = regerror(status, &re, NULL, 0);
      char*   buffer = malloc(size + 1);
      if (buffer == NULL || size + 1 == 0) {
         fprintf(stderr, "%s: out of memory\n", program);
         exit(1);
      }
      regerror(status, &re, buffer, size);
      fprintf(stderr, "%s: cannot regcomp \"%s\"; error = %s\n",
              program, needle, buffer);
      exit(1);
   }
   status = regexec(&re, haystack, 0, NULL, 0);
   if (status != 0 && status != REG_NOMATCH) {
      size_t  size = regerror(status, &re, NULL, 0);
      char*   buffer = malloc(size + 1);
      if (buffer == NULL || size + 1 == 0) {
         fprintf(stderr, "%s: out of memory\n", program);
         exit(1);
      }
      regerror(status, &re, buffer, size);
      fprintf(stderr, "%s: cannot regexec string \"%s\" with regexp \"%s\";"
              " error = %s\n",
              program, haystack, needle, buffer);
      exit(1);
   }
   regfree(&re);

   return (status == 0);
}

typedef struct {
   ccstring      name;
   unsigned int  low_bit;
   unsigned int  high_bit;
   ccstring*     images;
} named_item;

#define NIL_IMAGES  (ccstring*)NULL

static unsigned int
get_max_len (named_item    names[],
             unsigned int  length)
{
   unsigned int  result = 0;
   unsigned int  i;

   for (i = 0; i < length; i++) {
      result = MAX(result, strlen(names[i].name));
   }

   return result;
}

static void
print_names(unsigned int  value,
            named_item    names[],
            unsigned int  length,
            unsigned int  max_len)
{
   unsigned int  i;

   if (max_len == 0) {
      max_len = get_max_len(names, length);
   }

   for (i = 0; i < length; i++) {
      unsigned int  field = BIT_EXTRACT_LE(value,
                                           names[i].low_bit,
                                           names[i].high_bit + 1);
      if (names[i].images != NIL_IMAGES
          && names[i].images[field] != NULL) {
         printf("      %-*s = %s\n",
                max_len,
                names[i].name,
                names[i].images[field]);
      } else {
         printf("      %-*s = 0x%0x (%u)\n",
                max_len,
                names[i].name,
                field,
                field);
      }
   }
}

static ccstring  bools[] = { "false",
                             "true" };

typedef enum {
   VENDOR_UNKNOWN,
   VENDOR_INTEL,
   VENDOR_AMD,
   VENDOR_CYRIX,
   VENDOR_VIA,
   VENDOR_TRANSMETA,
   VENDOR_UMC,
   VENDOR_NEXGEN,
   VENDOR_RISE,
   VENDOR_SIS,
   VENDOR_NSC,
   VENDOR_VORTEX,
   VENDOR_RDC
} vendor_t;

typedef enum {
   HYPERVISOR_UNKNOWN,
   HYPERVISOR_VMWARE,
   HYPERVISOR_XEN,
   HYPERVISOR_KVM,
   HYPERVISOR_MICROSOFT,
} hypervisor_t;

#define __F(v)     ((v) & 0x0ff00f00)
#define __M(v)     ((v) & 0x000f00f0)
#define __FM(v)    ((v) & 0x0fff0ff0)
#define __FMS(v)   ((v) & 0x0fff0fff)

#define __TF(v)    ((v) & 0x0ff03f00)
#define __TFM(v)   ((v) & 0x0fff3ff0)
#define __TFMS(v)  ((v) & 0x0fff3fff)

#define _T(v)      ((v) << 12)
#define _F(v)      ((v) << 8)
#define _M(v)      ((v) << 4)
#define _S(v)      (v)
#define _XF(v)     ((v) << 20)
#define _XM(v)     ((v) << 16)

#define __B(v)     ((v) & 0x000000ff)
#define _B(v)      (v)

#define _FM(xf,f,xm,m)     (_XF(xf) + _F(f) + _XM(xm) + _M(m))
#define _FMS(xf,f,xm,m,s)  (_XF(xf) + _F(f) + _XM(xm) + _M(m) + _S(s))

#define START \
   if (0)
#define F(xf,f,str) \
   else if (__F(val)    ==       _XF(xf)        +_F(f)                              ) printf(str)
#define FM(xf,f,xm,m,str) \
   else if (__FM(val)   ==       _XF(xf)+_XM(xm)+_F(f)+_M(m)                        ) printf(str)
#define FMS(xf,f,xm,m,s,str) \
   else if (__FMS(val)  ==       _XF(xf)+_XM(xm)+_F(f)+_M(m)+_S(s)                  ) printf(str)
#define TF(t,xf,f,str) \
   else if (__TF(val)   == _T(t)+_XF(xf)        +_F(f)                              ) printf(str)
#define TFM(t,xf,f,xm,m,str) \
   else if (__TFM(val)  == _T(t)+_XF(xf)+_XM(xm)+_F(f)+_M(m)                        ) printf(str)
#define TFMS(t,xf,f,xm,m,s,str) \
   else if (__TFMS(val) == _T(t)+_XF(xf)+_XM(xm)+_F(f)+_M(m)+_S(s)                  ) printf(str)
#define FQ(xf,f,q,str) \
   else if (__F(val)    ==       _XF(xf)        +_F(f)             && (stash) && (q)) printf(str)
#define FMQ(xf,f,xm,m,q,str) \
   else if (__FM(val)   ==       _XF(xf)+_XM(xm)+_F(f)+_M(m)       && (stash) && (q)) printf(str)
#define FMSQ(xf,f,xm,m,s,q,str) \
   else if (__FMS(val)  ==       _XF(xf)+_XM(xm)+_F(f)+_M(m)+_S(s) && (stash) && (q)) printf(str)
#define DEFAULT(str) \
   else                                                                               printf(str)
#define FALLBACK(code) \
   else code

typedef struct {
   vendor_t       vendor;
   boolean        saw_4;
   boolean        saw_b;
   unsigned int   val_0_eax;
   unsigned int   val_1_eax;
   unsigned int   val_1_ebx;
   unsigned int   val_1_ecx;
   unsigned int   val_1_edx;
   unsigned int   val_4_eax;
   unsigned int   val_b_eax[2];
   unsigned int   val_b_ebx[2];
   unsigned int   val_80000001_eax;
   unsigned int   val_80000001_ebx;
   unsigned int   val_80000001_ecx;
   unsigned int   val_80000001_edx;
   unsigned int   val_80000008_ecx;
   unsigned int   transmeta_proc_rev;
   char           brand[48];
   char           transmeta_info[48];
   char           override_brand[48];
   char           soc_brand[48];
   hypervisor_t   hypervisor;

   struct mp {
      const char*    method;
      unsigned int   cores;
      unsigned int   hyperthreads;
   } mp;

   struct br {
      boolean    mobile;

      struct {
         boolean    celeron;
         boolean    core;
         boolean    pentium;
         boolean    xeon_mp;
         boolean    xeon;
         boolean    pentium_m;
         boolean    pentium_d;
         boolean    extreme;
         boolean    generic;
      };
      struct {
         boolean    athlon_lv;
         boolean    athlon_xp;
         boolean    duron;
         boolean    athlon;
         boolean    sempron;
         boolean    phenom;
         boolean    series;
         boolean    geode;
         boolean    turion;
         boolean    neo;
         boolean    athlon_fx;
         boolean    athlon_mp;
         boolean    duron_mp;
         boolean    opteron;
         boolean    fx;

         boolean    embedded;
         int        cores;
      };
   } br;
   struct bri {
      boolean  desktop_pentium;
      boolean  desktop_celeron;
      boolean  mobile_pentium;
      boolean  mobile_pentium_m;
      boolean  mobile_celeron;
      boolean  xeon_mp;
      boolean  xeon;
   } bri;

                                /* ==============implications============== */
                                /* PII (F6, M5)            PIII (F6, M7)    */
                                /* ----------------------  ---------------  */
   boolean       L2_4w_1Mor2M;  /* Xeon                    Xeon             */
   boolean       L2_4w_512K;    /* normal, Mobile or Xeon  normal or Xeon   */
   boolean       L2_4w_256K;    /* Mobile                   -               */
   boolean       L2_8w_1Mor2M;  /*  -                      Xeon             */
   boolean       L2_8w_512K;    /*  -                      normal           */
   boolean       L2_8w_256K;    /*  -                      normal or Xeon   */
                 /* none */     /* Celeron                  -               */

   boolean       L2_2M;         /* Nocona lacks, Irwindale has */
                                /* Conroe has more, Allendale has this */
   boolean       L2_6M;         /* Yorkfield C1/E0 has this, M1/R0 has less */
   boolean       L3;            /* Cranford lacks, Potomac has */

   boolean       L2_256K;       /* Barton has more, Thorton has this */
   boolean       L2_512K;       /* Toledo has more, Manchester E6 has this */
} code_stash_t;

#define NIL_STASH { VENDOR_UNKNOWN, \
                    FALSE, FALSE, \
                    0, 0, 0, 0, 0, 0, \
                    { 0, 0 }, \
                    { 0, 0 }, \
                    0, 0, 0, 0, 0, 0, \
                    "", "", "", "", \
                    HYPERVISOR_UNKNOWN, \
                    { NULL, -1, -1 }, \
                    { FALSE, \
                      { FALSE, FALSE, FALSE, FALSE, FALSE, \
                        FALSE, FALSE, FALSE, FALSE }, \
                      { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, \
                        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, \
                        FALSE, \
                        FALSE, 0 } }, \
                    { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE }, \
                    FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, \
                    FALSE, FALSE, FALSE, \
                    FALSE, FALSE }

static void
decode_amd_model(const code_stash_t*  stash,
                 const char**         brand_pre,
                 const char**         brand_post,
                 char*                proc)
{
   *brand_pre  = NULL;
   *brand_post = NULL;
   *proc       = '\0';

   if (stash == NULL) return;

   if (__F(stash->val_1_eax) == _XF(0) + _F(15)
       && __M(stash->val_1_eax) < _XM(4) + _M(0)) {
      /*
      ** Algorithm from:
      **    Revision Guide for AMD Athlon 64 and AMD Opteron Processors 
      **    (25759 Rev 3.79), Constructing the Processor Name String.
      ** But using only the Processor numbers.
      */
      unsigned int  bti;
      unsigned int  NN;

      if (__B(stash->val_1_ebx) != 0) {
         bti = BIT_EXTRACT_LE(__B(stash->val_1_ebx), 5, 8) << 2;
         NN  = BIT_EXTRACT_LE(__B(stash->val_1_ebx), 0, 5);
      } else if (BIT_EXTRACT_LE(stash->val_80000001_ebx, 0, 12) != 0) {
         bti = BIT_EXTRACT_LE(stash->val_80000001_ebx, 6, 12);
         NN  = BIT_EXTRACT_LE(stash->val_80000001_ebx, 0,  6);
      } else {
         return;
      }

#define XX  (22 + NN)
#define YY  (38 + 2*NN)
#define ZZ  (24 + NN)
#define TT  (24 + NN)
#define RR  (45 + 5*NN)
#define EE  ( 9 + NN)

      switch (bti) {
      case 0x04:
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x05:
         *brand_pre = "AMD Athlon(tm) 64 X2 Dual Core";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x06:
         *brand_pre  = "AMD Athlon(tm) 64";
         sprintf(proc, "FX-%02d", ZZ);
         *brand_post = "Dual Core";
         break;
      case 0x08:
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x09:
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x0a:
         *brand_pre = "AMD Turion(tm) Mobile Technology";
         sprintf(proc, "ML-%02d", XX);
         break;
      case 0x0b:
         *brand_pre = "AMD Turion(tm) Mobile Technology";
         sprintf(proc, "MT-%02d", XX);
         break;
      case 0x0c:
      case 0x0d:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d", YY);
         break;
      case 0x0e:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d HE", YY);
         break;
      case 0x0f:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d EE", YY);
         break;
      case 0x10:
      case 0x11:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d", YY);
         break;
      case 0x12:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d HE", YY);
         break;
      case 0x13:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d EE", YY);
         break;
      case 0x14:
      case 0x15:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d", YY);
         break;
      case 0x16:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d HE", YY);
         break;
      case 0x17:
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d EE", YY);
         break;
      case 0x18:
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", EE);
         break;
      case 0x1d:
         *brand_pre = "Mobile AMD Athlon(tm) XP-M";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x1e:
         *brand_pre = "Mobile AMD Athlon(tm) XP-M";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x20:
         *brand_pre = "AMD Athlon(tm) XP";
         sprintf(proc, "Processor %02d00+", XX);
         break;
      case 0x21:
      case 0x23:
         *brand_pre = "Mobile AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case 0x22:
      case 0x26:
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case 0x24:
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "FX-%02d", ZZ);
         break;
      case 0x29:
      case 0x2c:
      case 0x2d:
      case 0x38:
      case 0x3b:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d", RR);
         break;
      case 0x2a:
      case 0x30:
      case 0x31:
      case 0x39:
      case 0x3c:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d", RR);
         break;
      case 0x2b:
      case 0x34:
      case 0x35:
      case 0x3a:
      case 0x3d:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d", RR);
         break;
      case 0x2e:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d HE", RR);
         break;
      case 0x2f:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 1%02d EE", RR);
         break;
      case 0x32:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d HE", RR);
         break;
      case 0x33:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 2%02d EE", RR);
         break;
      case 0x36:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d HE", RR);
         break;
      case 0x37:
         *brand_pre = "Dual Core AMD Opteron(tm)";
         sprintf(proc, "Processor 8%02d EE", RR);
         break;
      }

#undef XX
#undef YY
#undef ZZ
#undef TT
#undef RR
#undef EE
   } else if (__F(stash->val_1_eax) == _XF(0) + _F(15)
              && __M(stash->val_1_eax) >= _XM(4) + _M(0)) {
      /*
      ** Algorithm from:
      **    Revision Guide for AMD NPT Family 0Fh Processors (33610 Rev 3.46),
      **    Constructing the Processor Name String.
      ** But using only the Processor numbers.
      */
      unsigned int  bti;
      unsigned int  pwrlmt;
      unsigned int  NN;
      unsigned int  pkgtype;
      unsigned int  cmpcap;

      pwrlmt  = ((BIT_EXTRACT_LE(stash->val_80000001_ebx, 6, 9) << 1)
                 + BIT_EXTRACT_LE(stash->val_80000001_ebx, 14, 15));
      bti     = BIT_EXTRACT_LE(stash->val_80000001_ebx, 9, 14);
      NN      = ((BIT_EXTRACT_LE(stash->val_80000001_ebx, 15, 16) << 5)
                 + BIT_EXTRACT_LE(stash->val_80000001_ebx, 0, 5));
      pkgtype = BIT_EXTRACT_LE(stash->val_80000001_eax, 4, 6);
      cmpcap  = ((BIT_EXTRACT_LE(stash->val_80000008_ecx, 0, 8) > 0)
                 ? 0x1 : 0x0);

#define RR  (NN - 1)
#define PP  (26 + NN)
#define TT  (15 + cmpcap*10 + NN)
#define ZZ  (57 + NN)
#define YY  (29 + NN)

#define PKGTYPE(pkgtype)  ((pkgtype) << 11)
#define CMPCAP(cmpcap)    ((cmpcap)  <<  9)
#define BTI(bti)          ((bti)     <<  4)
#define PWRLMT(pwrlmt)    (pwrlmt)

      switch (PKGTYPE(pkgtype) + CMPCAP(cmpcap) + BTI(bti) + PWRLMT(pwrlmt)) {
      /* Table 7: Name String Table for F (1207) and Fr3 (1207) Processors */
      case PKGTYPE(1) + CMPCAP(0) + BTI(1) + PWRLMT(2):
         *brand_pre = "AMD Opteron(tm)";
         sprintf(proc, "Processor 22%02d EE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(1) + PWRLMT(2):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 22%02d EE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(0) + PWRLMT(2) :
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 12%02d EE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(0) + PWRLMT(6):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 12%02d HE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(1) + PWRLMT(6):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 22%02d HE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(1) + PWRLMT(10):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 22%02d", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(1) + PWRLMT(12):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 22%02d SE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(4) + PWRLMT(2):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 82%02d EE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(4) + PWRLMT(6):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 82%02d HE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(4) + PWRLMT(10):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 82%02d", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(4) + PWRLMT(12):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 82%02d SE", RR);
         break;
      case PKGTYPE(1) + CMPCAP(1) + BTI(6) + PWRLMT(14):
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "FX-%02d", ZZ);
         break;
      /* Table 8: Name String Table for AM2 and ASB1 Processors */
      case PKGTYPE(3) + CMPCAP(0) + BTI(1) + PWRLMT(5):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor LE-1%02d0", RR);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(2) + PWRLMT(6):
         *brand_pre = "AMD Athlon(tm)";
         sprintf(proc, "Processor LE-1%02d0", ZZ);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(3) + PWRLMT(6):
         *brand_pre = "AMD Athlon(tm)";
         sprintf(proc, "Processor 1%02d0B", ZZ);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(1):
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(2):
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(3):
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(4):
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(5):
      case PKGTYPE(3) + CMPCAP(0) + BTI(4) + PWRLMT(8):
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(5) + PWRLMT(2):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor %02d50p", RR);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(6) + PWRLMT(4):
      case PKGTYPE(3) + CMPCAP(0) + BTI(6) + PWRLMT(8):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(7) + PWRLMT(1):
      case PKGTYPE(3) + CMPCAP(0) + BTI(7) + PWRLMT(2):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor %02d0U", TT);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(8) + PWRLMT(2):
      case PKGTYPE(3) + CMPCAP(0) + BTI(8) + PWRLMT(3):
         *brand_pre = "AMD Athlon(tm)";
         sprintf(proc, "Processor %02d50e", TT);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(9) + PWRLMT(2):
         *brand_pre = "AMD Athlon(tm) Neo";
         sprintf(proc, "Processor MV-%02d", TT);
         break;
      case PKGTYPE(3) + CMPCAP(0) + BTI(12) + PWRLMT(2):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor 2%02dU", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(1) + PWRLMT(6):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 12%02d HE", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(1) + PWRLMT(10):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 12%02d", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(1) + PWRLMT(12):
         *brand_pre = "Dual-Core AMD Opteron(tm)";
         sprintf(proc, "Processor 12%02d SE", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(3) + PWRLMT(3):
         *brand_pre = "AMD Athlon(tm) X2 Dual Core";
         sprintf(proc, "Processor BE-2%02d0", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(4) + PWRLMT(1):
      case PKGTYPE(3) + CMPCAP(1) + BTI(4) + PWRLMT(2):
      case PKGTYPE(3) + CMPCAP(1) + BTI(4) + PWRLMT(6):
      case PKGTYPE(3) + CMPCAP(1) + BTI(4) + PWRLMT(8):
      case PKGTYPE(3) + CMPCAP(1) + BTI(4) + PWRLMT(12):
         *brand_pre = "AMD Athlon(tm) 64 X2 Dual Core";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(5) + PWRLMT(12):
         *brand_pre  = "AMD Athlon(tm) 64";
         sprintf(proc, "FX-%02d", ZZ);
         *brand_post = "Dual Core";
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(6) + PWRLMT(6):
         *brand_pre = "AMD Sempron(tm) Dual Core";
         sprintf(proc, "Processor %02d00", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(7) + PWRLMT(3):
         *brand_pre = "AMD Athlon(tm) Dual Core";
         sprintf(proc, "Processor %02d50e", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(7) + PWRLMT(6):
      case PKGTYPE(3) + CMPCAP(1) + BTI(7) + PWRLMT(7):
         *brand_pre = "AMD Athlon(tm) Dual Core";
         sprintf(proc, "Processor %02d00B", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(8) + PWRLMT(3):
         *brand_pre = "AMD Athlon(tm) Dual Core";
         sprintf(proc, "Processor %02d50B", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(9) + PWRLMT(1):
         *brand_pre = "AMD Athlon(tm) X2 Dual Core";
         sprintf(proc, "Processor %02d50e", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(10) + PWRLMT(1):
      case PKGTYPE(3) + CMPCAP(1) + BTI(10) + PWRLMT(2):
         *brand_pre = "AMD Athlon(tm) Neo X2 Dual Core";
         sprintf(proc, "Processor %02d50e", TT);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(11) + PWRLMT(0):
         *brand_pre = "AMD Turion(tm) Neo X2 Dual Core";
         sprintf(proc, "Processor L6%02d", RR);
         break;
      case PKGTYPE(3) + CMPCAP(1) + BTI(12) + PWRLMT(0):
         *brand_pre = "AMD Turion(tm) Neo X2 Dual Core";
         sprintf(proc, "Processor L3%02d", RR);
         break;
      /* Table 9: Name String Table for S1g1 Processors */
      case PKGTYPE(0) + CMPCAP(0) + BTI(1) + PWRLMT(2):
         *brand_pre = "AMD Athlon(tm) 64";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(2) + PWRLMT(12):
         *brand_pre = "AMD Turion(tm) 64 Mobile Technology";
         sprintf(proc, "MK-%02d", YY);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(3) + PWRLMT(1):
         *brand_pre = "Mobile AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(3) + PWRLMT(6):
      case PKGTYPE(0) + CMPCAP(0) + BTI(3) + PWRLMT(12):
         *brand_pre = "Mobile AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", PP);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(4) + PWRLMT(2):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(6) + PWRLMT(4):
      case PKGTYPE(0) + CMPCAP(0) + BTI(6) + PWRLMT(6):
      case PKGTYPE(0) + CMPCAP(0) + BTI(6) + PWRLMT(12):
         *brand_pre = "AMD Athlon(tm)";
         sprintf(proc, "Processor TF-%02d", TT);
         break;
      case PKGTYPE(0) + CMPCAP(0) + BTI(7) + PWRLMT(3):
         *brand_pre = "AMD Athlon(tm)";
         sprintf(proc, "Processor L1%02d", RR);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(1) + PWRLMT(12):
         *brand_pre = "AMD Sempron(tm)";
         sprintf(proc, "Processor TJ-%02d", YY);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(2) + PWRLMT(12):
         *brand_pre = "AMD Turion(tm) 64 X2 Mobile Technology";
         sprintf(proc, "Processor TL-%02d", YY);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(3) + PWRLMT(4):
      case PKGTYPE(0) + CMPCAP(1) + BTI(3) + PWRLMT(12):
         *brand_pre = "AMD Turion(tm) 64 X2 Dual-Core";
         sprintf(proc, "Processor TK-%02d", YY);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(5) + PWRLMT(4):
         *brand_pre = "AMD Turion(tm) 64 X2 Dual Core";
         sprintf(proc, "Processor %02d00+", TT);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(6) + PWRLMT(2):
         *brand_pre = "AMD Turion(tm) X2 Dual Core";
         sprintf(proc, "Processor L3%02d", RR);
         break;
      case PKGTYPE(0) + CMPCAP(1) + BTI(7) + PWRLMT(4):
         *brand_pre = "AMD Turion(tm) X2 Dual Core";
         sprintf(proc, "Processor L5%02d", RR);
         break;
      }
      
#undef RR
#undef PP
#undef TT
#undef ZZ
#undef YY
   } else if (__F(stash->val_1_eax) == _XF(1) + _F(15)
              || __F(stash->val_1_eax) == _XF(2) + _F(15)
              || __F(stash->val_1_eax) == _XF(3) + _F(15)
              || __F(stash->val_1_eax) == _XF(5) + _F(15)) {
      /*
      ** Algorithm from:
      **    AMD Revision Guide for AMD Family 10h Processors (41322 Rev 3.74)
      **    AMD Revision Guide for AMD Family 11h Processors (41788 Rev 3.08)
      **    AMD Revision Guide for AMD Family 12h Processors (44739 Rev 3.10)
      **    AMD Revision Guide for AMD Family 14h Models 00h-0Fh Processors
      **    (47534 Rev 3.00)
      ** But using only the Processor numbers.
      */
      unsigned int  str1;
      unsigned int  str2;
      unsigned int  pg;
      unsigned int  partialmodel;
      unsigned int  pkgtype;
      unsigned int  nc;
      const char*   s1;
      const char*   s2;

      str2         = BIT_EXTRACT_LE(stash->val_80000001_ebx,  0,  4);
      partialmodel = BIT_EXTRACT_LE(stash->val_80000001_ebx,  4, 11);
      str1         = BIT_EXTRACT_LE(stash->val_80000001_ebx, 11, 15);
      pg           = BIT_EXTRACT_LE(stash->val_80000001_ebx, 15, 16);
      pkgtype      = BIT_EXTRACT_LE(stash->val_80000001_ebx, 28, 32);
      nc           = BIT_EXTRACT_LE(stash->val_80000008_ecx,  0,  8);

#define NC(nc)            ((nc)   << 9)
#define PG(pg)            ((pg)   << 8)
#define STR1(str1)        ((str1) << 4)
#define STR2(str2)        (str2)

      /* 
      ** In every String2 Values table, there were special cases for
      ** pg == 0 && str2 == 15 which defined them as the empty string.
      ** But that produces the same result as an undefined string, so
      ** don't bother trying to handle them.
      */
      if (__F(stash->val_1_eax) == _XF(1) + _F(15)) {
         if (pkgtype >= 2) {
            partialmodel--;
         }

         /* Family 10h tables */
         switch (pkgtype) {
         case 0:
            /* 41322 3.74: table 14: String1 Values for Fr2, Fr5, and Fr6 (1207) Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(3) + STR1(0): *brand_pre = "Quad-Core AMD Opteron(tm)"; s1 = "Processor 83"; break;
            case PG(0) + NC(3) + STR1(1): *brand_pre = "Quad-Core AMD Opteron(tm)"; s1 = "Processor 23"; break;
            case PG(0) + NC(5) + STR1(0): *brand_pre = "Six-Core AMD Opteron(tm)";  s1 = "Processor 84"; break;
            case PG(0) + NC(5) + STR1(1): *brand_pre = "Six-Core AMD Opteron(tm)";  s1 = "Processor 24"; break;
            case PG(1) + NC(3) + STR1(1): *brand_pre = "Embedded AMD Opteron(tm)";  s1 = "Processor ";   break;
            case PG(1) + NC(5) + STR1(1): *brand_pre = "Embedded AMD Opteron(tm)";  s1 = "Processor ";   break;
            default:                                                                s1 = NULL;           break;
            }
            /* 41322 3.74: table 15: String2 Values for Fr2, Fr5, and Fr6 (1207) Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(3) + STR2(10): s2 = " SE";   break;
            case PG(0) + NC(3) + STR2(11): s2 = " HE";   break;
            case PG(0) + NC(3) + STR2(12): s2 = " EE";   break;
            case PG(0) + NC(5) + STR2(0):  s2 = " SE";   break;
            case PG(0) + NC(5) + STR2(1):  s2 = " HE";   break;
            case PG(0) + NC(5) + STR2(2):  s2 = " EE";   break;
            case PG(1) + NC(3) + STR2(1):  s2 = "GF HE"; break;
            case PG(1) + NC(3) + STR2(2):  s2 = "HF HE"; break;
            case PG(1) + NC(3) + STR2(3):  s2 = "VS";    break;
            case PG(1) + NC(3) + STR2(4):  s2 = "QS HE"; break;
            case PG(1) + NC(3) + STR2(5):  s2 = "NP HE"; break;
            case PG(1) + NC(3) + STR2(6):  s2 = "KH HE"; break;
            case PG(1) + NC(3) + STR2(7):  s2 = "KS HE"; break;
            case PG(1) + NC(5) + STR2(1):  s2 = "QS";    break;
            case PG(1) + NC(5) + STR2(2):  s2 = "KS HE"; break;
            default:                       s2 = NULL;    break;
            }
            break;
         case 1:
            /* 41322 3.74: table 16: String1 Values for AM2r2 and AM3 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(0) + STR1(2):  *brand_pre = "AMD Sempron(tm)";           s1 = "1";
            /* This case obviously collides with one later */
            /* case PG(0) + NC(0) + STR1(3): *brand_pre = "AMD Athlon(tm) II";         s1 = "AMD Athlon(tm) II 1"; */
            case PG(0) + NC(0) + STR1(1):  *brand_pre = "AMD Athlon(tm)";            s1 = "";             break;
            case PG(0) + NC(0) + STR1(3):  *brand_pre = "AMD Athlon(tm) II X2";      s1 = "2";            break;
            case PG(0) + NC(0) + STR1(4):  *brand_pre = "AMD Athlon(tm) II X2";      s1 = "B";            break;
            case PG(0) + NC(0) + STR1(5):  *brand_pre = "AMD Athlon(tm) II X2";      s1 = "";             break;
            case PG(0) + NC(0) + STR1(7):  *brand_pre = "AMD Phenom(tm) II X2";      s1 = "5";            break;
            case PG(0) + NC(0) + STR1(10): *brand_pre = "AMD Phenom(tm) II X2";      s1 = "";             break;
            case PG(0) + NC(0) + STR1(11): *brand_pre = "AMD Phenom(tm) II X2";      s1 = "B";            break;
            case PG(0) + NC(0) + STR1(12): *brand_pre = "AMD Sempron(tm) X2";        s1 = "1";            break;
            case PG(0) + NC(2) + STR1(0):  *brand_pre = "AMD Phenom(tm)";            s1 = "";             break;
            case PG(0) + NC(2) + STR1(3):  *brand_pre = "AMD Phenom(tm) II X3";      s1 = "B";            break;
            case PG(0) + NC(2) + STR1(4):  *brand_pre = "AMD Phenom(tm) II X3";      s1 = "";             break;
            case PG(0) + NC(2) + STR1(7):  *brand_pre = "AMD Phenom(tm) II X3";      s1 = "4";            break;
            case PG(0) + NC(2) + STR1(8):  *brand_pre = "AMD Phenom(tm) II X3";      s1 = "7";            break;
            case PG(0) + NC(2) + STR1(10): *brand_pre = "AMD Phenom(tm) II X3";      s1 = "";             break;
            case PG(0) + NC(3) + STR1(0):  *brand_pre = "Quad-Core AMD Opteron(tm)"; s1 = "Processor 13"; break;
            case PG(0) + NC(3) + STR1(2):  *brand_pre = "AMD Phenom(tm)";            s1 = "";             break;
            case PG(0) + NC(3) + STR1(3):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "9";            break;
            case PG(0) + NC(3) + STR1(4):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "8";            break;
            case PG(0) + NC(3) + STR1(7):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "B";            break;
            case PG(0) + NC(3) + STR1(8):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "";             break;
            case PG(0) + NC(3) + STR1(10): *brand_pre = "AMD Athlon(tm) II X4";      s1 = "6";            break;
            case PG(0) + NC(3) + STR1(15): *brand_pre = "AMD Athlon(tm) II X4";      s1 = "";             break;
            case PG(0) + NC(5) + STR1(0):  *brand_pre = "AMD Phenom(tm) II X6";      s1 = "1";            break;
            case PG(1) + NC(1) + STR1(1):  *brand_pre = "AMD Athlon(tm) II XLT V";   s1 = "";             break;
            case PG(1) + NC(1) + STR1(2):  *brand_pre = "AMD Athlon(tm) II XL V";    s1 = "";             break;
            case PG(1) + NC(3) + STR1(1):  *brand_pre = "AMD Phenom(tm) II XLT Q";   s1 = "";             break;
            case PG(1) + NC(3) + STR1(2):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "9";            break;
            case PG(1) + NC(3) + STR1(3):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "8";            break;
            case PG(1) + NC(3) + STR1(4):  *brand_pre = "AMD Phenom(tm) II X4";      s1 = "6";            break;
            default:                                                                 s1 = NULL;           break;
            }
            /* 41322 3.74: table 17: String2 Values for AM2r2 and AM3 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(0) + STR2(10): s2 = " Processor";                break;
            case PG(0) + NC(0) + STR2(11): s2 = "u Processor";               break;
            case PG(0) + NC(1) + STR2(3):  s2 = "50 Dual-Core Processor";    break;
            case PG(0) + NC(1) + STR2(6):  s2 = " Processor";                break;
            case PG(0) + NC(1) + STR2(7):  s2 = "e Processor";               break;
            case PG(0) + NC(1) + STR2(9):  s2 = "0 Processor";               break;
            case PG(0) + NC(1) + STR2(10): s2 = "0e Processor";              break;
            case PG(0) + NC(1) + STR2(11): s2 = "u Processor";               break;
            case PG(0) + NC(2) + STR2(0):  s2 = "00 Triple-Core Processor";  break;
            case PG(0) + NC(2) + STR2(1):  s2 = "00e Triple-Core Processor"; break;
            case PG(0) + NC(2) + STR2(2):  s2 = "00B Triple-Core Processor"; break;
            case PG(0) + NC(2) + STR2(3):  s2 = "50 Triple-Core Processor";  break;
            case PG(0) + NC(2) + STR2(4):  s2 = "50e Triple-Core Processor"; break;
            case PG(0) + NC(2) + STR2(5):  s2 = "50B Triple-Core Processor"; break;
            case PG(0) + NC(2) + STR2(6):  s2 = " Processor";                break;
            case PG(0) + NC(2) + STR2(7):  s2 = "e Processor";               break;
            case PG(0) + NC(2) + STR2(9):  s2 = "0e Processor";              break;
            case PG(0) + NC(2) + STR2(10): s2 = "0 Processor";               break;
            case PG(0) + NC(3) + STR2(0):  s2 = "00 Quad-Core Processor";    break;
            case PG(0) + NC(3) + STR2(1):  s2 = "00e Quad-Core Processor";   break;
            case PG(0) + NC(3) + STR2(2):  s2 = "00B Quad-Core Processor";   break;
            case PG(0) + NC(3) + STR2(3):  s2 = "50 Quad-Core Processor";    break;
            case PG(0) + NC(3) + STR2(4):  s2 = "50e Quad-Core Processor";   break;
            case PG(0) + NC(3) + STR2(5):  s2 = "50B Quad-Core Processor";   break;
            case PG(0) + NC(3) + STR2(6):  s2 = " Processor";                break;
            case PG(0) + NC(3) + STR2(7):  s2 = "e Processor";               break;
            case PG(0) + NC(3) + STR2(9):  s2 = "0e Processor";              break;
            case PG(0) + NC(3) + STR2(14): s2 = "0 Processor";               break;
            case PG(0) + NC(5) + STR2(0):  s2 = "5T Processor";              break;
            case PG(0) + NC(5) + STR2(1):  s2 = "0T Processor";              break;
            case PG(1) + NC(1) + STR2(1):  s2 = "L Processor";               break;
            case PG(1) + NC(1) + STR2(2):  s2 = "C Processor";               break;
            case PG(1) + NC(3) + STR2(1):  s2 = "L Processor";               break;
            case PG(1) + NC(3) + STR2(4):  s2 = "T Processor";               break;
            default:                       s2 = NULL;                        break;
            }
            break;
         case 2:
            /* 41322 3.74: table 18: String1 Values for S1g3 and S1g4 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(0) + STR1(0): *brand_pre = "AMD Sempron(tm)";                          s1 = "M1"; break;
            case PG(0) + NC(0) + STR1(1): *brand_pre = "AMD";                                      s1 = "V";  break;
            case PG(0) + NC(1) + STR1(0): *brand_pre = "AMD Turion(tm) II Ultra Dual-Core Mobile"; s1 = "M6"; break;
            case PG(0) + NC(1) + STR1(1): *brand_pre = "AMD Turion(tm) II Dual-Core Mobile";       s1 = "M5"; break;
            case PG(0) + NC(1) + STR1(2): *brand_pre = "AMD Athlon(tm) II Dual-Core";              s1 = "M3"; break;
            case PG(0) + NC(1) + STR1(3): *brand_pre = "AMD Turion(tm) II";                        s1 = "P";  break;
            case PG(0) + NC(1) + STR1(4): *brand_pre = "AMD Athlon(tm) II";                        s1 = "P";  break;
            case PG(0) + NC(1) + STR1(5): *brand_pre = "AMD Phenom(tm) II";                        s1 = "X";  break;
            case PG(0) + NC(1) + STR1(6): *brand_pre = "AMD Phenom(tm) II";                        s1 = "N";  break;
            case PG(0) + NC(1) + STR1(7): *brand_pre = "AMD Turion(tm) II";                        s1 = "N";  break;
            case PG(0) + NC(1) + STR1(8): *brand_pre = "AMD Athlon(tm) II";                        s1 = "N";  break;
            case PG(0) + NC(2) + STR1(2): *brand_pre = "AMD Phenom(tm) II";                        s1 = "P";  break;
            case PG(0) + NC(2) + STR1(3): *brand_pre = "AMD Phenom(tm) II";                        s1 = "N";  break;
            case PG(0) + NC(3) + STR1(1): *brand_pre = "AMD Phenom(tm) II";                        s1 = "P";  break;
            case PG(0) + NC(3) + STR1(2): *brand_pre = "AMD Phenom(tm) II";                        s1 = "X";  break;
            case PG(0) + NC(3) + STR1(3): *brand_pre = "AMD Phenom(tm) II";                        s1 = "N";  break;
            default:                                                                               s1 = NULL; break;
            }
            /* 41322 3.74: table 19: String1 Values for S1g3 and S1g4 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(0) + STR2(1): s2 = "0 Processor";             break;
            case PG(0) + NC(1) + STR2(2): s2 = "0 Dual-Core Processor";   break;
            case PG(0) + NC(2) + STR2(2): s2 = "0 Triple-Core Processor"; break;
            case PG(0) + NC(3) + STR2(1): s2 = "0 Quad-Core Processor";   break;
            default:                      s2 = NULL;                      break;
            }
            break;
         case 3:
            /* 41322 3.74: table 20: String1 Values for G34r1 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(7)  + STR1(0): *brand_pre = "AMD Opteron(tm)";          s1 = "Processor 61"; break;
            case PG(0) + NC(11) + STR1(0): *brand_pre = "AMD Opteron(tm)";          s1 = "Processor 61"; break;
            case PG(1) + NC(7)  + STR1(1): *brand_pre = "Embedded AMD Opteron(tm)"; s1 = "Processor ";   break;
            /* It sure is odd that there are no 0/7/1 or 0/11/1 cases here. */
            default:                                                                s1 = NULL;           break;
            }
            /* 41322 3.74: table 21: String2 Values for G34r1 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(7)  + STR1(0): s2 = " HE"; break;
            case PG(0) + NC(7)  + STR1(1): s2 = " SE"; break;
            case PG(0) + NC(11) + STR1(0): s2 = " HE"; break;
            case PG(0) + NC(11) + STR1(1): s2 = " SE"; break;
            case PG(1) + NC(7)  + STR1(1): s2 = "QS";  break;
            case PG(1) + NC(7)  + STR1(2): s2 = "KS";  break;
            default:                       s2 = NULL;  break;
            }
            break;
         case 4:
            /* 41322 3.74: table 22: String1 Values for ASB2 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(0) + STR1(1): *brand_pre = "AMD Athlon(tm) II Neo"; s1 = "K";  break;
            case PG(0) + NC(0) + STR1(2): *brand_pre = "AMD";                   s1 = "V";  break;
            case PG(0) + NC(0) + STR1(3): *brand_pre = "AMD Athlon(tm) II Neo"; s1 = "R";  break;
            case PG(0) + NC(1) + STR1(1): *brand_pre = "AMD Turion(tm) II Neo"; s1 = "K";  break;
            case PG(0) + NC(1) + STR1(2): *brand_pre = "AMD Athlon(tm) II Neo"; s1 = "K";  break;
            case PG(0) + NC(1) + STR1(3): *brand_pre = "AMD";                   s1 = "V";  break;
            case PG(0) + NC(1) + STR1(4): *brand_pre = "AMD Turion(tm) II Neo"; s1 = "N";  break;
            case PG(0) + NC(1) + STR1(5): *brand_pre = "AMD Athlon(tm) II Neo"; s1 = "N";  break;
            default:                                                            s1 = NULL; break;
            }
            /* 41322 3.74: table 23: String2 Values for ASB2 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(0)  + STR1(1): s2 = "5 Processor";           break;
            case PG(0) + NC(0)  + STR1(2): s2 = "L Processor";           break;
            case PG(0) + NC(1)  + STR1(1): s2 = "5 Dual-Core Processor"; break;
            case PG(0) + NC(1)  + STR1(2): s2 = "L Dual-Core Processor"; break;
            default:                       s2 = NULL;                    break;
            }
            break;
         case 5:
            /* 41322 3.74: table 24: String1 Values for C32r1 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(3) + STR1(0): *brand_pre = "AMD Opteron(tm)";          s1 = "41"; break;
            case PG(0) + NC(5) + STR1(0): *brand_pre = "AMD Opteron(tm)";          s1 = "41"; break;
            case PG(1) + NC(3) + STR1(1): *brand_pre = "Embedded AMD Opteron(tm)"; s1 = " ";  break;
            case PG(1) + NC(5) + STR1(1): *brand_pre = "Embedded AMD Opteron(tm)"; s1 = " ";  break;
            /* It sure is odd that there are no 0/3/1 or 0/5/1 cases here. */
            default:                                                               s1 = NULL; break;
            }
            /* 41322 3.74: table 25: String2 Values for C32r1 Processors */
            /* 41322 3.74: table 25 */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(3) + STR1(0): s2 = " HE";   break;
            case PG(0) + NC(3) + STR1(1): s2 = " EE";   break;
            case PG(0) + NC(5) + STR1(0): s2 = " HE";   break;
            case PG(0) + NC(5) + STR1(1): s2 = " EE";   break;
            case PG(1) + NC(3) + STR1(1): s2 = "QS HE"; break;
            case PG(1) + NC(3) + STR1(2): s2 = "LE HE"; break;
            case PG(1) + NC(3) + STR1(3): s2 = "CL EE"; break;
            case PG(1) + NC(5) + STR1(1): s2 = "KX HE"; break;
            case PG(1) + NC(5) + STR1(2): s2 = "GL EE"; break;
            default:                      s2 = NULL;    break;
            }
            break;
         default:
            s1 = NULL;
            s2 = NULL;
            break;
         }
      } else if (__F(stash->val_1_eax) == _XF(2) + _F(15)) {
         /* Family 11h tables */
         switch (pkgtype) {
         case 2:
            /* 41788 3.08: table 3: String1 Values for S1g2 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(0) + STR1(0): *brand_pre = "AMD Sempron(tm)";                          s1 = "SI-"; break;
            case PG(0) + NC(0) + STR1(1): *brand_pre = "AMD Athlon(tm)";                           s1 = "QI-"; break;
            case PG(0) + NC(1) + STR1(0): *brand_pre = "AMD Turion(tm) X2 Ultra Dual-Core Mobile"; s1 = "ZM-"; break;
            case PG(0) + NC(1) + STR1(1): *brand_pre = "AMD Turion(tm) X2 Dual-Core Mobile";       s1 = "RM-"; break;
            case PG(0) + NC(1) + STR1(2): *brand_pre = "AMD Athlon(tm) X2 Dual-Core";              s1 = "QL-"; break;
            case PG(0) + NC(1) + STR1(3): *brand_pre = "AMD Sempron(tm) X2 Dual-Core";             s1 = "NI-"; break;
            default:                                                                               s1 = NULL;  break;
            }
            /* 41788 3.08: table 4: String2 Values for S1g2 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(0) + STR2(0): s2 = "";   break;
            case PG(0) + NC(1) + STR2(0): s2 = "";   break;
            default:                      s2 = NULL; break;
            }
            break;
         default:
            s1 = NULL;
            s2 = NULL;
            break;
         }
      } else if (__F(stash->val_1_eax) == _XF(3) + _F(15)) {
         partialmodel--;

         /* Family 12h tables */
         switch (pkgtype) {
         case 1:
            /* 44739 3.10: table 6: String1 Values for FS1 Processors */ 
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(1) + STR1(3): *brand_pre = "AMD"; s1 = "A4-33"; break;
            case PG(0) + NC(1) + STR1(5): *brand_pre = "AMD"; s1 = "E2-30"; break;
            case PG(0) + NC(3) + STR1(1): *brand_pre = "AMD"; s1 = "A8-35"; break;
            case PG(0) + NC(3) + STR1(3): *brand_pre = "AMD"; s1 = "A6-34"; break;
            default:                                          s1 = NULL;    break;
            }
            /* 44739 3.10: table 7: String2 Values for FS1 Processors */ 
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(1) + STR2(1):  s2 = "M";  break;
            case PG(0) + NC(1) + STR2(2):  s2 = "MX"; break;
            case PG(0) + NC(3) + STR2(1):  s2 = "M";  break;
            case PG(0) + NC(3) + STR2(2):  s2 = "MX"; break;
            default:                       s2 = NULL; break;
            }
         case 2:
            /* 44739 3.10: table 8: String1 Values for FM1 Processors */ 
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(1) + STR1(1):  *brand_pre = "AMD";                   s1 = "A4-33"; break;
            case PG(0) + NC(1) + STR1(2):  *brand_pre = "AMD";                   s1 = "E2-32"; break;
            case PG(0) + NC(1) + STR1(4):  *brand_pre = "AMD Athlon(tm) II X2";  s1 = "2";     break;
            case PG(0) + NC(1) + STR1(5):  *brand_pre = "AMD";                   s1 = "A4-34"; break;
            case PG(0) + NC(1) + STR1(12): *brand_pre = "AMD Sempron(tm) X2";    s1 = "1";     break;
            case PG(0) + NC(2) + STR1(5):  *brand_pre = "AMD";                   s1 = "A6-35"; break;
            case PG(0) + NC(3) + STR1(5):  *brand_pre = "AMD";                   s1 = "A8-38"; break;
            case PG(0) + NC(3) + STR1(6):  *brand_pre = "AMD";                   s1 = "A6-36"; break;
            case PG(0) + NC(3) + STR1(13): *brand_pre = "AMD Athlon(tm) II X4";  s1 = "6";     break;
            default:                                                             s1 = NULL;    break;
            }
            /* 44739 3.10: table 9: String2 Values for FM1 Processors */ 
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(1) + STR2(1):  s2 = " APU with Radeon(tm) HD Graphics"; break;
            case PG(0) + NC(1) + STR2(2):  s2 = " Dual-Core Processor";             break;
            case PG(0) + NC(2) + STR2(1):  s2 = " APU with Radeon(tm) HD Graphics"; break;
            case PG(0) + NC(3) + STR2(1):  s2 = " APU with Radeon(tm) HD Graphics"; break;
            case PG(0) + NC(3) + STR2(3):  s2 = " Quad-Core Processor";             break;
            default:                       s2 = NULL;                               break;
            }
         default:
            s1 = NULL;
            s2 = NULL;
            break;
         }
      } else if (__F(stash->val_1_eax) == _XF(5) + _F(15)) {
         partialmodel--;

         /* Family 14h Models 00h-0Fh tables */
         switch (pkgtype) {
         case 0:
            /* 47534 3.00: table 4: String1 Values for FT1 Processors */
            switch (PG(pg) + NC(nc) + STR1(str1)) {
            case PG(0) + NC(0) + STR1(1): *brand_pre = "AMD"; s1 = "C-";   break;
            case PG(0) + NC(0) + STR1(2): *brand_pre = "AMD"; s1 = "E-";   break;
            case PG(0) + NC(0) + STR1(4): *brand_pre = "AMD"; s1 = "G-T";  break;
            case PG(0) + NC(1) + STR1(1): *brand_pre = "AMD"; s1 = "C-";   break;
            case PG(0) + NC(1) + STR1(2): *brand_pre = "AMD"; s1 = "E-";   break;
            case PG(0) + NC(1) + STR1(3): *brand_pre = "AMD"; s1 = "Z-";   break;
            case PG(0) + NC(1) + STR1(4): *brand_pre = "AMD"; s1 = "G-T";  break;
            case PG(0) + NC(1) + STR1(5): *brand_pre = "AMD"; s1 = "E1-1"; break;
            case PG(0) + NC(1) + STR1(6): *brand_pre = "AMD"; s1 = "E2-1"; break;
            case PG(0) + NC(1) + STR1(7): *brand_pre = "AMD"; s1 = "E2-2"; break;
            default:                                          s1 = NULL;   break;
            }
            /* 47534 3.00: table 5: String2 Values for FT1 Processors */
            switch (PG(pg) + NC(nc) + STR2(str2)) {
            case PG(0) + NC(0) + STR2(1):  s2 = "";   break;
            case PG(0) + NC(0) + STR2(2):  s2 = "0";  break;
            case PG(0) + NC(0) + STR2(3):  s2 = "5";  break;
            case PG(0) + NC(0) + STR2(4):  s2 = "0x"; break;
            case PG(0) + NC(0) + STR2(5):  s2 = "5x"; break;
            case PG(0) + NC(0) + STR2(6):  s2 = "x";  break;
            case PG(0) + NC(0) + STR2(7):  s2 = "L";  break;
            case PG(0) + NC(0) + STR2(8):  s2 = "N";  break;
            case PG(0) + NC(0) + STR2(9):  s2 = "R";  break;
            case PG(0) + NC(0) + STR2(10): s2 = "0";  break;
            case PG(0) + NC(0) + STR2(11): s2 = "5";  break;
            case PG(0) + NC(0) + STR2(12): s2 = "";   break;
            case PG(0) + NC(0) + STR2(13): s2 = "0D"; break;
            case PG(0) + NC(1) + STR2(1):  s2 = "";   break;
            case PG(0) + NC(1) + STR2(2):  s2 = "0";  break;
            case PG(0) + NC(1) + STR2(3):  s2 = "5";  break;
            case PG(0) + NC(1) + STR2(4):  s2 = "0x"; break;
            case PG(0) + NC(1) + STR2(5):  s2 = "5x"; break;
            case PG(0) + NC(1) + STR2(6):  s2 = "x";  break;
            case PG(0) + NC(1) + STR2(7):  s2 = "L";  break;
            case PG(0) + NC(1) + STR2(8):  s2 = "N";  break;
            case PG(0) + NC(1) + STR2(9):  s2 = "0";  break;
            case PG(0) + NC(1) + STR2(10): s2 = "5";  break;
            case PG(0) + NC(1) + STR2(11): s2 = "";   break;
            case PG(0) + NC(1) + STR2(12): s2 = "E";  break;
            case PG(0) + NC(1) + STR2(13): s2 = "0D"; break;
            default:                       s2 = NULL; break;
            }
            break;
         default:
            s1 = NULL;
            s2 = NULL;
            break;
         }
      } else {
         s1 = NULL;
         s2 = NULL;
      }

#undef NC
#undef PG
#undef STR1
#undef STR2

      if (s1 != NULL) {
         char*  p = proc;
         p += sprintf(p, "%s%02d", s1, partialmodel);
         if (s2) sprintf(p, "%s", s2);
      }
   }
}

static void
decode_override_brand(code_stash_t*  stash)
{
   if (stash->vendor == VENDOR_AMD
       && strstr(stash->brand, "model unknown") != NULL) {
      /*
      ** AMD has this exotic architecture where the BIOS decodes the brand
      ** string from tables and feeds it back into the CPU via MSR's.  If an old
      ** BIOS cannot understand a new CPU, it uses the string "model unknown".
      ** In this case, I use my own copies of tables to deduce the brand string
      ** and decode that.
      */
      const char*  brand_pre;
      const char*  brand_post;
      char         proc[96];
      decode_amd_model(stash, &brand_pre, &brand_post, proc);
      if (brand_pre != NULL) {
         char*  b = stash->override_brand;
         b += sprintf(b, "%s %s", brand_pre, proc);
         if (brand_post != NULL) sprintf(b, " %s", brand_post);
      }
   }
}

static void
print_override_brand(code_stash_t*  stash)
{
   if (stash->override_brand[0] != '\0') {
      printf("   (override brand synth) = %s\n", stash->override_brand);
   }
}

static void
stash_intel_cache(code_stash_t*  stash,
                  unsigned char  value)
{
   switch (value) {
   case 0x42: stash->L2_4w_256K   = TRUE; break;
   case 0x43: stash->L2_4w_512K   = TRUE; break;
   case 0x44: stash->L2_4w_1Mor2M = TRUE; break;
   case 0x45: stash->L2_4w_1Mor2M = TRUE; break;
   case 0x80: stash->L2_8w_512K   = TRUE; break;
   case 0x82: stash->L2_8w_256K   = TRUE; break;
   case 0x83: stash->L2_8w_512K   = TRUE; break;
   case 0x84: stash->L2_8w_1Mor2M = TRUE; break;
   case 0x85: stash->L2_8w_1Mor2M = TRUE; break;
   }

   switch (value) {
   case 0x45:
   case 0x7d:
   case 0x85:
      stash->L2_2M = TRUE; 
      break;
   }

   switch (value) {
   case 0x4e:
      stash->L2_6M = TRUE; 
      break;
   }

   switch (value) {
   case 0x22:
   case 0x23:
   case 0x25:
   case 0x29:
   case 0x46:
   case 0x47:
   case 0x49:
   case 0x4a:
   case 0x4b:
   case 0x4c:
   case 0x4d:
   case 0x88:
   case 0x89:
   case 0x8a:
   case 0x8d:
   case 0xd0:
   case 0xd1:
   case 0xd2:
   case 0xd6:
   case 0xd7:
   case 0xd8:
   case 0xdc:
   case 0xdd:
   case 0xde:
   case 0xe2:
   case 0xe3:
   case 0xe4:
   case 0xea:
   case 0xeb:
   case 0xec:
      stash->L3 = TRUE;
      break;
   }

   switch (value) {
   case 0x21:
   case 0x3c:
   case 0x42:
   case 0x7a:
   case 0x7e:
   case 0x82:
      stash->L2_256K = TRUE;
      break;
   }

   switch (value) {
   case 0x3e:
   case 0x43:
   case 0x7b:
   case 0x7f:
   case 0x83:
   case 0x86:
      stash->L2_512K = TRUE;
      break;
   }
}

static void
decode_brand_id_stash(code_stash_t*  stash)
{
   unsigned int  val_1_eax = stash->val_1_eax;
   unsigned int  val_1_ebx = stash->val_1_ebx;

   switch (__B(val_1_ebx)) {
   case _B(0):  break;
   case _B(1):  stash->bri.desktop_celeron = TRUE; break;
   case _B(2):  stash->bri.desktop_pentium = TRUE; break;
   case _B(3):  if ( __FMS(val_1_eax) == _FMS(0,6, 0,11, 1)) {
                   stash->bri.desktop_celeron = TRUE;
                } else {
                   stash->bri.xeon = TRUE;
                }
                break;
   case _B(4):  stash->bri.desktop_pentium = TRUE; break;
   case _B(6):  stash->bri.desktop_pentium = TRUE; break;
   case _B(7):  stash->bri.desktop_celeron = TRUE; break;
   case _B(8):  stash->bri.desktop_pentium = TRUE; break;
   case _B(9):  stash->bri.desktop_pentium = TRUE; break;
   case _B(10): stash->bri.desktop_celeron = TRUE; break;
   case _B(11): if (__FMS(val_1_eax) <= _FMS(0,15, 0,1, 2)) {
                   stash->bri.xeon_mp = TRUE;
                } else {
                   stash->bri.xeon = TRUE;
                }
                break;
   case _B(12): stash->bri.xeon_mp         = TRUE; break;
   case _B(14): if (__FMS(val_1_eax) <= _FMS(0,15, 0,1, 3)) {
                   stash->bri.xeon = TRUE;
                } else {
                   stash->bri.mobile_pentium_m = TRUE;
                }
                break;
   case _B(15): if (__FM(val_1_eax) == _FM (0,15, 0,2)) {
                   stash->bri.mobile_pentium_m = TRUE;
                } else {
                   stash->bri.mobile_celeron = TRUE;
                }
                break;
   case _B(16): stash->bri.desktop_celeron = TRUE; break;
   case _B(17): stash->bri.mobile_pentium  = TRUE; break;
   case _B(18): stash->bri.desktop_celeron = TRUE; break;
   case _B(19): stash->bri.mobile_celeron  = TRUE; break;
   case _B(20): stash->bri.desktop_celeron = TRUE; break;
   case _B(21): stash->bri.mobile_pentium  = TRUE; break;
   case _B(22): stash->bri.desktop_pentium = TRUE; break;
   case _B(23): stash->bri.mobile_celeron  = TRUE; break;
   default:     break;
   }
}

static void
decode_brand_string(const char*    brand,
                    code_stash_t*  stash)
{
   stash->br.mobile      = (strstr(brand, "Mobile") != NULL
                            || strstr(brand, "mobile") != NULL);

   stash->br.celeron     = strstr(brand, "Celeron") != NULL;
   stash->br.core        = strstr(brand, "Core(TM)") != NULL;
   stash->br.pentium     = strstr(brand, "Pentium") != NULL;
   stash->br.xeon_mp     = (strstr(brand, "Xeon MP") != NULL
                            || strstr(brand, "Xeon(TM) MP") != NULL
                            || strstr(brand, "Xeon(R)") != NULL);
   stash->br.xeon        = strstr(brand, "Xeon") != NULL;
   stash->br.pentium_m   = strstr(brand, "Pentium(R) M") != NULL;
   stash->br.pentium_d   = strstr(brand, "Pentium(R) D") != NULL;
   stash->br.extreme     = strregexp(brand, " ?X[0-9][0-9][0-9][0-9]");
   stash->br.generic     = strstr(brand, "Genuine Intel(R) CPU") != NULL;

   stash->br.athlon_lv   = strstr(brand, "Athlon(tm) XP-M (LV)") != NULL;
   stash->br.athlon_xp   = (strstr(brand, "Athlon(tm) XP") != NULL
                            || strstr(brand, "Athlon(TM) XP") != NULL);
   stash->br.duron       = strstr(brand, "Duron") != NULL;
   stash->br.athlon      = strstr(brand, "Athlon") != NULL;
   stash->br.sempron     = strstr(brand, "Sempron") != NULL;
   stash->br.phenom      = strstr(brand, "Phenom") != NULL;
   stash->br.series      = strstr(brand, "Series") != NULL;
   stash->br.geode       = strstr(brand, "Geode") != NULL;
   stash->br.turion      = strstr(brand, "Turion") != NULL;
   stash->br.neo         = strstr(brand, "Neo") != NULL;
   stash->br.athlon_fx   = strstr(brand, "Athlon(tm) 64 FX") != NULL;
   stash->br.athlon_mp   = strstr(brand, "Athlon(tm) MP") != NULL;
   stash->br.duron_mp    = strstr(brand, "Duron(tm) MP") != NULL;
   stash->br.opteron     = strstr(brand, "Opteron") != NULL;
   stash->br.fx          = strstr(brand, "AMD FX") != NULL;

   stash->br.embedded    = strstr(brand, "Embedded") != NULL;
   if (strstr(brand, "Dual Core") != NULL
       || strstr(brand, " X2 ") != NULL) {
      stash->br.cores = 2;
   } else if (strstr(brand, "Triple-Core") != NULL
              || strstr(brand, " X3 ") != NULL) {
      stash->br.cores = 3;
   } else if (strstr(brand, "Quad-Core") != NULL
              || strstr(brand, " X4 ") != NULL) {
      stash->br.cores = 4;
   } else if (strstr(brand, "Six-Core") != NULL
              || strstr(brand, " X6 ") != NULL) {
      stash->br.cores = 6;
   } else {
      stash->br.cores = 0; // means unspecified by the brand string
   }
}

static void
decode_brand_stash(code_stash_t*  stash)
{
   if (stash->override_brand[0] != '\0') {
      decode_brand_string(stash->override_brand, stash);
   } else {
      decode_brand_string(stash->brand, stash);
   }
}

/*
** Query macros are used in the synth tables to disambiguate multiple chips
** with the same family, model, and/or stepping.
*/

#define is_intel      (stash->vendor == VENDOR_INTEL)
#define is_amd        (stash->vendor == VENDOR_AMD)
#define is_transmeta  (stash->vendor == VENDOR_TRANSMETA)
#define is_mobile     (stash->br.mobile)

/*
** Intel major queries:
**
** d? = think "desktop"
** s? = think "server" (multiprocessor)
** M? = think "mobile"
** X? = think "Extreme Edition"
**
** ?P = think Pentium
** ?C = think Celeron
** ?X = think Xeon
** ?M = think Xeon MP / Pentium M
** ?c = think Core (or generic CPU)
** ?d = think Pentium D
*/
#define dP ((is_intel && stash->br.pentium) \
            || stash->bri.desktop_pentium)
#define dC ((is_intel && !is_mobile && stash->br.celeron) \
            || stash->bri.desktop_celeron)
#define dd (is_intel && stash->br.pentium_d)
#define dc (is_intel && !is_mobile && (stash->br.core || stash->br.generic))
#define sX ((is_intel && stash->br.xeon) || stash->bri.xeon)
#define sM ((is_intel && stash->br.xeon_mp) \
            || stash->bri.xeon_mp)
#define MP ((is_intel && is_mobile && stash->br.pentium) \
            || stash->bri.mobile_pentium)
#define MC ((is_intel && is_mobile && stash->br.celeron) \
            || stash->bri.mobile_celeron)
#define MM ((is_intel && stash->br.pentium_m)\
            || stash->bri.mobile_pentium_m)
#define Mc (is_intel && is_mobile && stash->br.core)
#define Xc (is_intel && stash->br.extreme)

/* 
** Intel special cases 
*/
/* Pentium II Xeon (Deschutes), distinguished from Pentium II (Deschutes) */
#define xD (stash->L2_4w_1Mor2M)
/* Mobile Pentium II (Deschutes), distinguished from Pentium II (Deschutes) */
#define mD (stash->L2_4w_256K)
/* Intel Celeron (Deschutes), distinguished from  Pentium II (Deschutes) */
#define cD (!stash->L2_4w_1Mor2M && !stash->L2_4w_512K && !stash->L2_4w_256K)
/* Pentium III Xeon (Katmai), distinguished from Pentium III (Katmai) */
#define xK (stash->L2_4w_1Mor2M || stash->L2_8w_1Mor2M)
/* Pentium II (Katmai), verified, so distinguished from fallback case */
#define pK ((stash->L2_4w_512K || stash->L2_8w_256K || stash->L2_8w_512K) \
            && !stash->L2_4w_1Mor2M && !stash->L2_8w_1Mor2M)
/* Irwindale, distinguished from Nocona */
#define sI (sX && stash->L2_2M)
/* Potomac, distinguished from Cranford */
#define sP (sM && stash->L3)
/* Allendale, distinguished from Conroe */
#define da (dc && stash->L2_2M)
/* Dual-Core Xeon Processor 5100 (Woodcrest B1) pre-production,
   distinguished from Core 2 Duo (Conroe B1) */
#define QW (dc && stash->br.generic \
            && (stash->mp.cores == 4 \
                || (stash->mp.cores == 2 && stash->mp.hyperthreads == 2)))
/* Core Duo (Yonah), distinguished from Core Solo (Yonah) */
#define Dc (dc && stash->mp.cores == 2)
/* Core 2 Quad, distinguished from Core 2 Duo */
#define Qc (dc && stash->mp.cores == 4)
/* Core 2 Extreme (Conroe B1), distinguished from Core 2 Duo (Conroe B1) */
#define XE (dc && strstr(stash->brand, " E6800") != NULL)
/* Quad-Core Xeon, distinguished from Xeon; and
   Xeon Processor 3300, distinguished from Xeon Processor 3100 */
#define sQ (sX && stash->mp.cores == 4)
/* Xeon Processor 7000, distinguished from Xeon */
#define IS_VMX(val_1_ecx)  (BIT_EXTRACT_LE((val_1_ecx), 5, 6))
#define s7 (sX && IS_VMX(stash->val_1_ecx))
/* Wolfdale C0/E0, distinguished from Wolfdale M0/R0 */
#define de (dc && stash->L2_6M)
/* Penryn C0/E0, distinguished from Penryn M0/R0 */
#define Me (Mc && stash->L2_6M)
/* Yorkfield C1/E0, distinguished from Yorkfield M1/E0 */
#define Qe (Qc && stash->L2_6M)
/* Yorkfield E0, distinguished from Yorkfield R0 */
#define se (sQ && stash->L2_6M)


/*
** AMD major queries:
**
** d? = think "desktop"
** s? = think "server" (MP)
** M? = think "mobile"
**
** ?A = think Athlon
** ?X = think Athlon XP
** ?L = think Athlon XP (LV)
** ?f = think FX
** ?F = think Athlon FX
** ?D = think Duron
** ?S = think Sempron
** ?O = think Opteron
** ?T = think Turion
** ?p = think Phenom
** ?s = think ?-Series
** ?n = think Turion Neo
** ?N = think Neo
*/
#define dA (is_amd && !is_mobile && stash->br.athlon)
#define dX (is_amd && !is_mobile && stash->br.athlon_xp)
#define dF (is_amd && !is_mobile && stash->br.athlon_fx)
#define df (is_amd && !is_mobile && stash->br.fx)
#define dD (is_amd && !is_mobile && stash->br.duron)
#define dS (is_amd && !is_mobile && stash->br.sempron)
#define dO (is_amd && !is_mobile && stash->br.opteron)
#define dp (is_amd && !is_mobile && stash->br.phenom)
#define sA (is_amd && !is_mobile && stash->br.athlon_mp)
#define sD (is_amd && !is_mobile && stash->br.duron_mp)
#define MA (is_amd && is_mobile && stash->br.athlon)
#define MX (is_amd && is_mobile && stash->br.athlon_xp)
#define ML (is_amd && is_mobile && stash->br.athlon_lv)
#define MD (is_amd && is_mobile && stash->br.duron)
#define MS (is_amd && is_mobile && stash->br.sempron)
#define Mp (is_amd && is_mobile && stash->br.phenom)
#define Ms (is_amd && is_mobile && stash->br.series)
#define MG (is_amd && stash->br.geode)
#define MT (is_amd && stash->br.turion)
#define Mn (is_amd && stash->br.turion && stash->br.neo)
#define MN (is_amd && stash->br.neo)

/*
** AMD special cases
*/
static boolean is_amd_egypt_athens_8xx(const code_stash_t*  stash)
{
   /*
   ** This makes its determination based on the Processor model
   ** logic from:
   **    Revision Guide for AMD Athlon 64 and AMD Opteron Processors 
   **    (25759 Rev 3.79), Constructing the Processor Name String.
   ** See also decode_amd_model().
   */

   if (stash->vendor == VENDOR_AMD && stash->br.opteron) {
      switch (__FM(stash->val_1_eax)) {
      case _FM(0,15, 2,1): /* Italy/Egypt */
      case _FM(0,15, 2,5): /* Troy/Athens */
         {
            unsigned int  bti;

            if (__B(stash->val_1_ebx) != 0) {
               bti = BIT_EXTRACT_LE(__B(stash->val_1_ebx), 5, 8) << 2;
            } else if (BIT_EXTRACT_LE(stash->val_80000001_ebx, 0, 12) != 0) {
               bti = BIT_EXTRACT_LE(stash->val_80000001_ebx, 6, 12);
            } else {
               return FALSE;
            }

            switch (bti) {
            case 0x10:
            case 0x11:
            case 0x12:
            case 0x13:
            case 0x2a:
            case 0x30:
            case 0x31:
            case 0x39:
            case 0x3c:
            case 0x32:
            case 0x33:
               /* It's a 2xx */
               return FALSE;
            case 0x14:
            case 0x15:
            case 0x16:
            case 0x17:
            case 0x2b:
            case 0x34:
            case 0x35:
            case 0x3a:
            case 0x3d:
            case 0x36:
            case 0x37:
               /* It's an 8xx */
               return TRUE;
            }
         }
      }
   }

   return FALSE;
}

/* Embedded Opteron, distinguished from Opteron (Barcelona & Shanghai) */
#define EO (dO && stash->br.embedded)
/* Opterons, distinguished by number of processors */
#define DO (dO && stash->br.cores == 2)
#define SO (dO && stash->br.cores == 6)
/* Athlons, distinguished by number of processors */
#define DA (dA && stash->br.cores == 2)
#define TA (dA && stash->br.cores == 3)
#define QA (dA && stash->br.cores == 4)
/* Phenoms distinguished by number of processors */
#define Dp (dp && stash->br.cores == 2)
#define Tp (dp && stash->br.cores == 3)
#define Qp (dp && stash->br.cores == 4)
#define Sp (dp && stash->br.cores == 6)
/* Semprons, distinguished by number of processors */
#define DS (dS  && stash->br.cores == 2)
/* Egypt, distinguished from Italy; and
   Athens, distingushed from Troy */
#define d8 (dO && is_amd_egypt_athens_8xx(stash))
/* Thorton A2, distinguished from Barton A2 */
#define dt (dX && stash->L2_256K)
/* Manchester E6, distinguished from from Toledo E6 */
#define dm (dA && stash->L2_512K)
/* Propus, distinguished from Regor */
#define dR (dA && stash->L2_512K)
/* Trinidad, distinguished from Taylor */
#define Mt (MT && stash->L2_512K)

/*
** Transmeta major queries
**
** t2 = TMx200
** t4 = TMx400
** t5 = TMx500
** t6 = TMx600
** t8 = TMx800
*/
#define tm_rev(rev) (is_transmeta \
                     && (stash->transmeta_proc_rev & 0xffff0000) == rev)
/* TODO: Add cases for Transmeta Crusoe TM5700/TM5900 */
/* TODO: Add cases for Transmeta Efficeon */
#define t2 (tm_rev(0x01010000))
#define t4 (tm_rev(0x01020000) || (tm_rev(0x01030000) && stash->L2_4w_256K))
#define t5 ((tm_rev(0x01040000) || tm_rev(0x01040000)) && stash->L2_4w_256K)
#define t6 (tm_rev(0x01030000) && stash->L2_4w_512K)
#define t8 ((tm_rev(0x01040000) || tm_rev(0x01040000)) && stash->L2_4w_512K)

static void
debug_queries(const code_stash_t*  stash)
{
#define DEBUGQ(q) printf("%s = %s\n", #q, (q)      ? "TRUE" : "FALSE")
#define DEBUGF(f) printf("%s = %s\n", #f, f(stash) ? "TRUE" : "FALSE")

   DEBUGQ(is_intel);
   DEBUGQ(is_amd);
   DEBUGQ(is_transmeta);
   DEBUGQ(is_mobile);
   DEBUGQ(MC);
   DEBUGQ(Mc);
   DEBUGQ(MP);
   DEBUGQ(sM);
   DEBUGQ(sX);
   DEBUGQ(dC);
   DEBUGQ(MM);
   DEBUGQ(dd);
   DEBUGQ(dP);
   DEBUGQ(Xc);
   DEBUGQ(dc);

   DEBUGQ(xD);
   DEBUGQ(mD);
   DEBUGQ(cD);
   DEBUGQ(xK);
   DEBUGQ(pK);
   DEBUGQ(sI);
   DEBUGQ(sP);
   DEBUGQ(da);
   DEBUGQ(QW);
   DEBUGQ(Dc);
   DEBUGQ(Qc);
   DEBUGQ(XE);
   DEBUGQ(sQ);
   DEBUGQ(s7);
   DEBUGQ(de);
   DEBUGQ(Me);
   DEBUGQ(Qe);
   DEBUGQ(se);

   DEBUGQ(ML);
   DEBUGQ(MX);
   DEBUGQ(MD);
   DEBUGQ(MA);
   DEBUGQ(MS);
   DEBUGQ(Mp);
   DEBUGQ(Ms);
   DEBUGQ(Mn);
   DEBUGQ(MN);
   DEBUGQ(MT);
   DEBUGQ(dO);
   DEBUGQ(dp);
   DEBUGQ(dX);
   DEBUGQ(dF);
   DEBUGQ(sA);
   DEBUGQ(sD);
   DEBUGQ(dD);
   DEBUGQ(dA);
   DEBUGQ(dS);

   DEBUGF(is_amd_egypt_athens_8xx);
   DEBUGQ(EO);
   DEBUGQ(DO);
   DEBUGQ(SO);
   DEBUGQ(DA);
   DEBUGQ(TA);
   DEBUGQ(QA);
   DEBUGQ(Dp);
   DEBUGQ(Tp);
   DEBUGQ(Qp);
   DEBUGQ(Sp);
   DEBUGQ(DS);
   DEBUGQ(d8);
   DEBUGQ(dt);
   DEBUGQ(dm);
   DEBUGQ(dR);
   DEBUGQ(Mt);

   DEBUGQ(t2);
   DEBUGQ(t4);
   DEBUGQ(t5);
   DEBUGQ(t6);
   DEBUGQ(t8);

#undef DEBUGQ
#undef DEBUGF
}

static void
print_synth_intel(const char*          name,
                  unsigned int         val,  /* val_1_eax */
                  const code_stash_t*  stash)
{
   printf("%s", name);
   START;
   FM  (    0, 4,  0, 0,         "Intel i80486DX-25/33");
   FM  (    0, 4,  0, 1,         "Intel i80486DX-50");
   FM  (    0, 4,  0, 2,         "Intel i80486SX");
   FM  (    0, 4,  0, 3,         "Intel i80486DX/2");
   FM  (    0, 4,  0, 4,         "Intel i80486SL");
   FM  (    0, 4,  0, 5,         "Intel i80486SX/2");
   FM  (    0, 4,  0, 7,         "Intel i80486DX/2-WB");
   FM  (    0, 4,  0, 8,         "Intel i80486DX/4");
   FM  (    0, 4,  0, 9,         "Intel i80486DX/4-WB");
   F   (    0, 4,                "Intel i80486 (unknown model)");
   FM  (    0, 5,  0, 0,         "Intel Pentium 60/66 A-step");
   TFM (1,  0, 5,  0, 1,         "Intel Pentium 60/66 OverDrive for P5");
   FMS (    0, 5,  0, 1,  3,     "Intel Pentium 60/66 (B1)");
   FMS (    0, 5,  0, 1,  5,     "Intel Pentium 60/66 (C1)");
   FMS (    0, 5,  0, 1,  7,     "Intel Pentium 60/66 (D1)");
   FM  (    0, 5,  0, 1,         "Intel Pentium 60/66");
   TFM (1,  0, 5,  0, 2,         "Intel Pentium 75 - 200 OverDrive for P54C");
   FMS (    0, 5,  0, 2,  1,     "Intel Pentium P54C 75 - 200 (B1)");
   FMS (    0, 5,  0, 2,  2,     "Intel Pentium P54C 75 - 200 (B3)");
   FMS (    0, 5,  0, 2,  4,     "Intel Pentium P54C 75 - 200 (B5)");
   FMS (    0, 5,  0, 2,  5,     "Intel Pentium P54C 75 - 200 (C2/mA1)");
   FMS (    0, 5,  0, 2,  6,     "Intel Pentium P54C 75 - 200 (E0)");
   FMS (    0, 5,  0, 2, 11,     "Intel Pentium P54C 75 - 200 (cB1)");
   FMS (    0, 5,  0, 2, 12,     "Intel Pentium P54C 75 - 200 (cC0)");
   FM  (    0, 5,  0, 2,         "Intel Pentium P54C 75 - 200");
   TFM (1,  0, 5,  0, 3,         "Intel Pentium OverDrive for i486 (P24T)");
   TFM (1,  0, 5,  0, 4,         "Intel Pentium OverDrive for P54C");
   FMS (    0, 5,  0, 4,  3,     "Intel Pentium MMX P55C (B1)");
   FMS (    0, 5,  0, 4,  4,     "Intel Pentium MMX P55C (A3)");
   FM  (    0, 5,  0, 4,         "Intel Pentium MMX P55C");
   FMS (    0, 5,  0, 7,  0,     "Intel Pentium MMX P54C 75 - 200 (A4)");
   FM  (    0, 5,  0, 7,         "Intel Pentium MMX P54C 75 - 200");
   FMS (    0, 5,  0, 8,  1,     "Intel Pentium MMX P55C (A0), .25um");
   FMS (    0, 5,  0, 8,  2,     "Intel Pentium MMX P55C (B2), .25um");
   FM  (    0, 5,  0, 8,         "Intel Pentium MMX P55C, .25um");
   FM  (    0, 5,  0, 9,         "Intel Quark X1000 / D1000 / D2000 / C1000 (Lakemont), 32nm");
   F   (    0, 5,                "Intel Pentium (unknown model)");
   FM  (    0, 6,  0, 0,         "Intel Pentium Pro A-step");
   FMS (    0, 6,  0, 1,  1,     "Intel Pentium Pro (B0)");
   FMS (    0, 6,  0, 1,  2,     "Intel Pentium Pro (C0)");
   FMS (    0, 6,  0, 1,  6,     "Intel Pentium Pro (sA0)");
   FMS (    0, 6,  0, 1,  7,     "Intel Pentium Pro (sA1)");
   FMS (    0, 6,  0, 1,  9,     "Intel Pentium Pro (sB1)");
   FM  (    0, 6,  0, 1,         "Intel Pentium Pro");
   TFM (1,  0, 6,  0, 3,         "Intel Pentium II OverDrive");
   FMS (    0, 6,  0, 3,  3,     "Intel Pentium II (Klamath C0), .28um");
   FMS (    0, 6,  0, 3,  4,     "Intel Pentium II (Klamath C1), .28um");
   FM  (    0, 6,  0, 3,         "Intel Pentium II (Klamath), .28um");
   FM  (    0, 6,  0, 4,         "Intel Pentium P55CT OverDrive (Deschutes)");
   FMSQ(    0, 6,  0, 5,  0, xD, "Intel Pentium II Xeon (Deschutes A0), .25um");
   FMSQ(    0, 6,  0, 5,  0, mD, "Intel Mobile Pentium II (Deschutes A0), .25um");
   FMSQ(    0, 6,  0, 5,  0, cD, "Intel Celeron (Deschutes A0), .25um");
   FMS (    0, 6,  0, 5,  0,     "Intel Pentium II / Pentium II Xeon / Mobile Pentium II (Deschutes A0), .25um");
   FMSQ(    0, 6,  0, 5,  1, xD, "Intel Pentium II Xeon (Deschutes A1), .25um");
   FMSQ(    0, 6,  0, 5,  1, cD, "Intel Celeron (Deschutes A1), .25um");
   FMS (    0, 6,  0, 5,  1,     "Intel Pentium II / Pentium II Xeon (Deschutes A1), .25um");
   FMSQ(    0, 6,  0, 5,  2, xD, "Intel Pentium II Xeon (Deschutes B0), .25um");
   FMSQ(    0, 6,  0, 5,  2, mD, "Intel Mobile Pentium II (Deschutes B0), .25um");
   FMSQ(    0, 6,  0, 5,  2, cD, "Intel Celeron (Deschutes B0), .25um");
   FMS (    0, 6,  0, 5,  2,     "Intel Pentium II / Pentium II Xeon / Mobile Pentium II (Deschutes B0), .25um");
   FMSQ(    0, 6,  0, 5,  3, xD, "Intel Pentium II Xeon (Deschutes B1), .25um");
   FMSQ(    0, 6,  0, 5,  3, cD, "Intel Celeron (Deschutes B1), .25um");
   FMS (    0, 6,  0, 5,  3,     "Intel Pentium II / Pentium II Xeon (Deschutes B1), .25um");
   FMQ (    0, 6,  0, 5,     xD, "Intel Pentium II Xeon (Deschutes), .25um");
   FMQ (    0, 6,  0, 5,     mD, "Intel Mobile Pentium II (Deschutes), .25um");
   FMQ (    0, 6,  0, 5,     cD, "Intel Celeron (Deschutes), .25um");
   FM  (    0, 6,  0, 5,         "Intel Pentium II / Pentium II Xeon / Mobile Pentium II (Deschutes), .25um");
   FMSQ(    0, 6,  0, 6,  0, dP, "Intel Pentium II (Mendocino A0), L2 cache");
   FMSQ(    0, 6,  0, 6,  0, dC, "Intel Celeron (Mendocino A0), L2 cache");
   FMS (    0, 6,  0, 6,  0,     "Intel Pentium II / Celeron (Mendocino A0), L2 cache");
   FMSQ(    0, 6,  0, 6,  5, dC, "Intel Celeron (Mendocino B0), L2 cache");
   FMSQ(    0, 6,  0, 6,  5, dP, "Intel Pentium II (Mendocino B0), L2 cache");
   FMS (    0, 6,  0, 6,  5,     "Intel Pentium II / Celeron (Mendocino B0), L2 cache");
   FMS (    0, 6,  0, 6, 10,     "Intel Mobile Pentium II (Mendocino A0), L2 cache");
   FM  (    0, 6,  0, 6,         "Intel Pentium II (Mendocino), L2 cache");
   FMSQ(    0, 6,  0, 7,  2, pK, "Intel Pentium III (Katmai B0), .25um");
   FMSQ(    0, 6,  0, 7,  2, xK, "Intel Pentium III Xeon (Katmai B0), .25um");
   FMS (    0, 6,  0, 7,  2,     "Intel Pentium III / Pentium III Xeon (Katmai B0), .25um");
   FMSQ(    0, 6,  0, 7,  3, pK, "Intel Pentium III (Katmai C0), .25um");
   FMSQ(    0, 6,  0, 7,  3, xK, "Intel Pentium III Xeon (Katmai C0), .25um");
   FMS (    0, 6,  0, 7,  3,     "Intel Pentium III / Pentium III Xeon (Katmai C0), .25um");
   FMQ (    0, 6,  0, 7,     pK, "Intel Pentium III (Katmai), .25um");
   FMQ (    0, 6,  0, 7,     xK, "Intel Pentium III Xeon (Katmai), .25um");
   FM  (    0, 6,  0, 7,         "Intel Pentium III / Pentium III Xeon (Katmai), .25um");
   FMSQ(    0, 6,  0, 8,  1, sX, "Intel Pentium III Xeon (Coppermine A2), .18um");
   FMSQ(    0, 6,  0, 8,  1, MC, "Intel Mobile Celeron (Coppermine A2), .18um");
   FMSQ(    0, 6,  0, 8,  1, dC, "Intel Celeron (Coppermine A2), .18um");
   FMSQ(    0, 6,  0, 8,  1, MP, "Intel Mobile Pentium III (Coppermine A2), .18um");
   FMSQ(    0, 6,  0, 8,  1, dP, "Intel Pentium III (Coppermine A2), .18um");
   FMS (    0, 6,  0, 8,  1,     "Intel Pentium III / Pentium III Xeon / Celeron / Mobile Pentium III / Mobile Celeron (Coppermine A2), .18um");
   FMSQ(    0, 6,  0, 8,  3, sX, "Intel Pentium III Xeon (Coppermine B0), .18um");
   FMSQ(    0, 6,  0, 8,  3, MC, "Intel Mobile Celeron (Coppermine B0), .18um");
   FMSQ(    0, 6,  0, 8,  3, dC, "Intel Celeron (Coppermine B0), .18um");
   FMSQ(    0, 6,  0, 8,  3, MP, "Intel Mobile Pentium III (Coppermine B0), .18um");
   FMSQ(    0, 6,  0, 8,  3, dP, "Intel Pentium III (Coppermine B0), .18um");
   FMS (    0, 6,  0, 8,  3,     "Intel Pentium III / Pentium III Xeon / Celeron / Mobile Pentium III / Mobile Celeron (Coppermine B0), .18um");
   FMSQ(    0, 6,  0, 8,  6, sX, "Intel Pentium III Xeon (Coppermine C0), .18um");
   FMSQ(    0, 6,  0, 8,  6, MC, "Intel Mobile Celeron (Coppermine C0), .18um");
   FMSQ(    0, 6,  0, 8,  6, dC, "Intel Celeron (Coppermine C0), .18um");
   FMSQ(    0, 6,  0, 8,  6, MP, "Intel Mobile Pentium III (Coppermine C0), .18um");
   FMSQ(    0, 6,  0, 8,  6, dP, "Intel Pentium III (Coppermine C0), .18um");
   FMS (    0, 6,  0, 8,  6,     "Intel Pentium III / Pentium III Xeon / Celeron / Mobile Pentium III / Mobile Celeron (Coppermine C0), .18um");
   FMSQ(    0, 6,  0, 8, 10, sX, "Intel Pentium III Xeon (Coppermine D0), .18um");
   FMSQ(    0, 6,  0, 8, 10, MC, "Intel Mobile Celeron (Coppermine D0), .18um");
   FMSQ(    0, 6,  0, 8, 10, dC, "Intel Celeron (Coppermine D0), .18um");
   FMSQ(    0, 6,  0, 8, 10, MP, "Intel Mobile Pentium III (Coppermine D0), .18um");
   FMSQ(    0, 6,  0, 8, 10, dP, "Intel Pentium III (Coppermine D0), .18um");
   FMS (    0, 6,  0, 8, 10,     "Intel Pentium III / Pentium III Xeon / Celeron / Mobile Pentium III / Mobile Celeron (Coppermine D0), .18um");
   FMQ (    0, 6,  0, 8,     sX, "Intel Pentium III Xeon (Coppermine), .18um");
   FMQ (    0, 6,  0, 8,     MC, "Intel Mobile Celeron (Coppermine), .18um");
   FMQ (    0, 6,  0, 8,     dC, "Intel Celeron (Coppermine), .18um");
   FMQ (    0, 6,  0, 8,     MP, "Intel Mobile Pentium III (Coppermine), .18um");
   FMQ (    0, 6,  0, 8,     dP, "Intel Pentium III (Coppermine), .18um");
   FM  (    0, 6,  0, 8,         "Intel Pentium III / Pentium III Xeon / Celeron / Mobile Pentium III / Mobile Celeron (Coppermine), .18um");
   FMSQ(    0, 6,  0, 9,  5, dC, "Intel Celeron M (Banias B1), .13um");
   FMSQ(    0, 6,  0, 9,  5, dP, "Intel Pentium M (Banias B1), .13um");
   FMS (    0, 6,  0, 9,  5,     "Intel Pentium M / Celeron M (Banias B1), .13um");
   FMQ (    0, 6,  0, 9,     dC, "Intel Celeron M (Banias), .13um");
   FMQ (    0, 6,  0, 9,     dP, "Intel Pentium M (Banias), .13um");
   FM  (    0, 6,  0, 9,         "Intel Pentium M / Celeron M (Banias), .13um");
   FMS (    0, 6,  0,10,  0,     "Intel Pentium III Xeon (Cascades A0), .18um");
   FMS (    0, 6,  0,10,  1,     "Intel Pentium III Xeon (Cascades A1), .18um");
   FMS (    0, 6,  0,10,  4,     "Intel Pentium III Xeon (Cascades B0), .18um");
   FM  (    0, 6,  0,10,         "Intel Pentium III Xeon (Cascades), .18um");
   FMSQ(    0, 6,  0,11,  1, dC, "Intel Celeron (Tualatin A1), .13um");
   FMSQ(    0, 6,  0,11,  1, MC, "Intel Mobile Celeron (Tualatin A1), .13um");
   FMSQ(    0, 6,  0,11,  1, dP, "Intel Pentium III (Tualatin A1), .13um");
   FMS (    0, 6,  0,11,  1,     "Intel Pentium III / Celeron / Mobile Celeron (Tualatin A1), .13um");
   FMSQ(    0, 6,  0,11,  4, dC, "Intel Celeron (Tualatin B1), .13um");
   FMSQ(    0, 6,  0,11,  4, MC, "Intel Mobile Celeron (Tualatin B1), .13um");
   FMSQ(    0, 6,  0,11,  4, dP, "Intel Pentium III (Tualatin B1), .13um");
   FMS (    0, 6,  0,11,  4,     "Intel Pentium III / Celeron / Mobile Celeron (Tualatin B1), .13um");
   FMQ (    0, 6,  0,11,     dC, "Intel Celeron (Tualatin), .13um");
   FMQ (    0, 6,  0,11,     MC, "Intel Mobile Celeron (Tualatin), .13um");
   FMQ (    0, 6,  0,11,     dP, "Intel Pentium III (Tualatin), .13um");
   FM  (    0, 6,  0,11,         "Intel Pentium III / Celeron / Mobile Celeron (Tualatin), .13um");
   FMSQ(    0, 6,  0,13,  6, dC, "Intel Celeron M (Dothan B1), 90nm");
   FMSQ(    0, 6,  0,13,  6, dP, "Intel Pentium M (Dothan B1), 90nm");
   FMS (    0, 6,  0,13,  6,     "Intel Pentium M (Dothan B1) / Celeron M (Dothan B1), 90nm");
   FMSQ(    0, 6,  0,13,  8, dC, "Intel Celeron M (Dothan C0), 90nm/65nm");
   FMSQ(    0, 6,  0,13,  8, MP, "Intel Processor A100/A110 (Stealey C0) / Pentium M (Crofton C0), 90nm");
   FMSQ(    0, 6,  0,13,  8, dP, "Intel Pentium M (Dothan C0), 90nm");
   FMS (    0, 6,  0,13,  8,     "Intel Pentium M (Dothan C0) / Celeron M (Dothan C0) / A100/A110 (Stealey C0) / Pentium M (Crofton C0), 90nm/65nm");
   FMQ (    0, 6,  0,13,     dC, "Intel Celeron M (Dothan), 90nm");
   FMQ (    0, 6,  0,13,     MP, "Intel Processor A100/A110 (Stealey), 90nm");
   FMQ (    0, 6,  0,13,     dP, "Intel Pentium M (Dothan), 90nm");
   FM  (    0, 6,  0,13,         "Intel Pentium M (Dothan) / Celeron M (Dothan) / Pentium M (Crofton), 90nm");
   FMSQ(    0, 6,  0,14,  8, sX, "Intel Xeon Processor LV (Sossaman C0), 65nm");
   FMSQ(    0, 6,  0,14,  8, dC, "Intel Celeron (Yonah C0), 65nm");
   FMSQ(    0, 6,  0,14,  8, Dc, "Intel Core Duo (Yonah C0), 65nm");
   FMSQ(    0, 6,  0,14,  8, dc, "Intel Core Solo (Yonah C0), 65nm");
   FMS (    0, 6,  0,14,  8,     "Intel Core Solo (Yonah C0) / Core Duo (Yonah C0) / Xeon Processor LV (Sossaman C0) / Celeron (Yonah C0), 65nm");
   FMSQ(    0, 6,  0,14, 12, sX, "Intel Xeon Processor LV (Sossaman D0), 65nm");
   FMSQ(    0, 6,  0,14, 12, dC, "Intel Celeron M (Yonah D0), 65nm");
   FMSQ(    0, 6,  0,14, 12, MP, "Intel Pentium Dual-Core Mobile T2000 (Yonah D0), 65nm");
   FMSQ(    0, 6,  0,14, 12, Dc, "Intel Core Duo (Yonah D0), 65nm");
   FMSQ(    0, 6,  0,14, 12, dc, "Intel Core Solo (Yonah D0), 65nm");
   FMS (    0, 6,  0,14, 12,     "Intel Core Solo (Yonah D0) / Core Duo (Yonah D0) / Xeon Processor LV (Sossaman D0) / Pentium Dual-Core Mobile T2000 (Yonah D0) / Celeron M (Yonah D0), 65nm");
   FMS (    0, 6,  0,14, 13,     "Intel Pentium Dual-Core Mobile T2000 (Yonah M0), 65nm");
   FMQ (    0, 6,  0,14,     sX, "Intel Xeon Processor LV (Sossaman), 65nm");
   FMQ (    0, 6,  0,14,     dC, "Intel Celeron (Yonah), 65nm");
   FMQ (    0, 6,  0,14,     MP, "Intel Pentium Dual-Core Mobile (Yonah), 65nm");
   FMQ (    0, 6,  0,14,     Dc, "Intel Core Duo (Yonah), 65nm");
   FMQ (    0, 6,  0,14,     dc, "Intel Core Solo (Yonah), 65nm");
   FM  (    0, 6,  0,14,         "Intel Core Solo (Yonah) / Core Duo (Yonah) / Xeon Processor LV (Sossaman) / Celeron (Yonah) / Pentium Dual-Core Mobile (Yonah), 65nm");
   FMSQ(    0, 6,  0,15,  2, sX, "Intel Dual-Core Xeon Processor 3000 (Conroe L2), 65nm");
   FMSQ(    0, 6,  0,15,  2, Mc, "Intel Core Duo Mobile (Merom L2), 65nm");
   FMSQ(    0, 6,  0,15,  2, dc, "Intel Core Duo (Conroe L2), 65nm");
   FMSQ(    0, 6,  0,15,  2, dP, "Intel Pentium Dual-Core Desktop Processor E2000 (Allendale L2), 65nm");
   FMS (    0, 6,  0,15,  2,     "Intel Core Duo (Conroe L2) / Core Duo Mobile (Merom L2) / Pentium Dual-Core Desktop Processor E2000 (Allendale L2) / Dual-Core Xeon Processor 3000 (Conroe L2), 65nm");
   FMS (    0, 6,  0,15,  4,     "Intel Core 2 Duo (Conroe B0) / Xeon Processor 5100 (Woodcrest B0) (pre-production), 65nm");
   FMSQ(    0, 6,  0,15,  5, QW, "Intel Dual-Core Xeon Processor 5100 (Woodcrest B1) (pre-production), 65nm");
   FMSQ(    0, 6,  0,15,  5, XE, "Intel Core 2 Extreme Processor (Conroe B1), 65nm");
   FMSQ(    0, 6,  0,15,  5, da, "Intel Core 2 Duo (Allendale B1), 65nm");
   FMSQ(    0, 6,  0,15,  5, dc, "Intel Core 2 Duo (Conroe B1), 65nm");
   FMS (    0, 6,  0,15,  5,     "Intel Core 2 Duo (Conroe/Allendale B1) / Core 2 Extreme Processor (Conroe B1), 65nm");
   FMSQ(    0, 6,  0,15,  6, Xc, "Intel Core 2 Extreme Processor (Conroe B2), 65nm");
   FMSQ(    0, 6,  0,15,  6, Mc, "Intel Core 2 Duo Mobile (Merom B2), 65nm");
   FMSQ(    0, 6,  0,15,  6, da, "Intel Core 2 Duo (Allendale B2), 65nm");
   FMSQ(    0, 6,  0,15,  6, dc, "Intel Core 2 Duo (Conroe B2), 65nm");
   FMSQ(    0, 6,  0,15,  6, dC, "Intel Celeron M (Conroe B2), 65nm");
   FMSQ(    0, 6,  0,15,  6, sX, "Intel Dual-Core Xeon Processor 3000 (Conroe B2) / Dual-Core Xeon Processor 5100 (Woodcrest B2), 65nm");
   FMS (    0, 6,  0,15,  6,     "Intel Core 2 Duo (Conroe/Allendale B2) / Core 2 Extreme Processor (Conroe B2) / Dual-Core Xeon Processor 3000 (Conroe B2) / Dual-Core Xeon Processor 5100 (Woodcrest B2) / Core 2 Duo Mobile (Conroe B2), 65nm");
   FMSQ(    0, 6,  0,15,  7, sX, "Intel Quad-Core Xeon Processor 3200 (Kentsfield B3) / Quad-Core Xeon Processor 5300 (Clovertown B3), 65nm");
   FMSQ(    0, 6,  0,15,  7, Xc, "Intel Core 2 Extreme Quad-Core Processor QX6xx0 (Kentsfield B3), 65nm");
   FMS (    0, 6,  0,15,  7,     "Intel Quad-Core Xeon Processor 3200 (Kentsfield B3) / Quad-Core Xeon Processor 5300 (Clovertown B3) / Core 2 Extreme Quad-Core Processor QX6700 (Clovertown B3)a, 65nm");
   FMSQ(    0, 6,  0,15, 10, Mc, "Intel Core 2 Duo Mobile (Merom E1), 65nm");
   FMSQ(    0, 6,  0,15, 10, dC, "Intel Celeron Processor 500 (Merom E1), 65nm");
   FMS (    0, 6,  0,15, 10,     "Intel Core 2 Duo Mobile (Merom E1) / Celeron Processor 500 (Merom E1), 65nm");
   FMSQ(    0, 6,  0,15, 11, sQ, "Intel Quad-Core Xeon Processor 5300 (Clovertown G0), 65nm");
   FMSQ(    0, 6,  0,15, 11, sX, "Intel Xeon Processor 3000 (Conroe G0) / Xeon Processor 3200 (Kentsfield G0) / Xeon Processor 7200/7300 (Tigerton G0), 65nm");
   FMSQ(    0, 6,  0,15, 11, Xc, "Intel Core 2 Extreme Quad-Core Processor QX6xx0 (Kentsfield G0), 65nm");
   FMSQ(    0, 6,  0,15, 11, Mc, "Intel Core 2 Duo Mobile (Merom G2), 65nm");
   FMSQ(    0, 6,  0,15, 11, Qc, "Intel Core 2 Quad (Conroe G0), 65nm");
   FMSQ(    0, 6,  0,15, 11, dc, "Intel Core 2 Duo (Conroe G0), 65nm");
   FMS (    0, 6,  0,15, 11,     "Intel Core 2 Duo (Conroe G0) / Xeon Processor 3000 (Conroe G0) / Xeon Processor 3200 (Kentsfield G0) / Xeon Processor 7200/7300 (Tigerton G0) / Quad-Core Xeon Processor 5300 (Clovertown G0) / Core 2 Extreme Quad-Core Processor QX6xx0 (Kentsfield G0) / Core 2 Duo Mobile (Merom G2), 65nm");
   FMSQ(    0, 6,  0,15, 13, Mc, "Intel Core 2 Duo Mobile (Merom M1) / Celeron Processor 500 (Merom E1), 65nm");
   FMSQ(    0, 6,  0,15, 13, Qc, "Intel Core 2 Quad (Conroe M0), 65nm");
   FMSQ(    0, 6,  0,15, 13, dc, "Intel Core 2 Duo (Conroe M0), 65nm");
   FMSQ(    0, 6,  0,15, 13, dP, "Intel Pentium Dual-Core Desktop Processor E2000 (Allendale M0), 65nm");
   FMSQ(    0, 6,  0,15, 13, dC, "Intel Celeron Dual-Core E1000 (Allendale M0) / Celeron Dual-Core T1000 (Merom M0)");
   FMS (    0, 6,  0,15, 13,     "Intel Core 2 Duo (Conroe M0) / Core 2 Duo Mobile (Merom M1) / Celeron Processor 500 (Merom E1) / Pentium Dual-Core Desktop Processor E2000 (Allendale M0) / Celeron Dual-Core E1000 (Allendale M0) / Celeron Dual-Core T1000 (Merom M0)");
   FMQ (    0, 6,  0,15,     sQ, "Intel Quad-Core Xeon (Woodcrest), 65nm");
   FMQ (    0, 6,  0,15,     sX, "Intel Dual-Core Xeon (Conroe / Woodcrest) / Quad-Core Xeon (Kentsfield / Clovertown) / Core 2 Extreme Quad-Core (Clovertown) / Xeon (Tigerton G0), 65nm");
   FMQ (    0, 6,  0,15,     Xc, "Intel Core 2 Extreme Processor (Conroe) / Core 2 Extreme Quad-Core (Kentsfield), 65nm");
   FMQ (    0, 6,  0,15,     Mc, "Intel Core Duo Mobile / Core 2 Duo Mobile (Merom) / Celeron (Merom), 65nm");
   FMQ (    0, 6,  0,15,     Qc, "Intel Core 2 Quad (Conroe), 65nm");
   FMQ (    0, 6,  0,15,     dc, "Intel Core Duo / Core 2 Duo (Conroe), 65nm");
   FMQ (    0, 6,  0,15,     dP, "Intel Pentium Dual-Core (Allendale), 65nm");
   FMQ (    0, 6,  0,15,     dC, "Intel Celeron M (Conroe) / Celeron (Merom) / Celeron Dual-Core (Allendale), 65nm");
   FM  (    0, 6,  0,15,         "Intel Core Duo / Core 2 Duo (Conroe / Allendale) / Core Duo Mobile (Merom) / Core 2 Duo Mobile (Merom) / Celeron (Merom) / Core 2 Extreme (Conroe) / Core 2 Extreme Quad-Core (Kentsfield) / Pentium Dual-Core (Allendale) / Celeron M (Conroe) / Celeron (Merom) / Celeron Dual-Core (Allendale) / Quad-Core Xeon (Kentsfield / Clovertown / Woodcrest) / Core 2 Extreme Quad-Core (Clovertown) / Xeon (Tigerton) / Dual-Core Xeon (Conroe / Woodcrest), 65nm");
   FMS (    0, 6,  1, 5,  0,     "Intel EP80579 (Tolapai B0), 65nm");
   FMSQ(    0, 6,  1, 6,  1, MC, "Intel Celeron Processor 200/400/500 (Conroe-L/Merom-L A1), 65nm");
   FMSQ(    0, 6,  1, 6,  1, dC, "Intel Celeron M (Merom-L A1), 65nm");
   FMSQ(    0, 6,  1, 6,  1, Mc, "Intel Core 2 Duo Mobile (Merom A1), 65nm");
   FMS (    0, 6,  1, 6,  1,     "Intel Core 2 Duo Mobile (Merom A1) / Celeron 200/400/500 (Conroe-L/Merom-L A1) / Celeron M (Merom-L A1), 65nm");
   FMQ (    0, 6,  1, 6,     MC, "Intel Celeron Processor 200/400/500 (Conroe-L/Merom-L), 65nm");
   FMQ (    0, 6,  1, 6,     dC, "Intel Celeron M (Merom-L), 65nm");
   FMQ (    0, 6,  1, 6,     Mc, "Intel Core 2 Duo Mobile (Merom), 65nm");
   FM  (    0, 6,  1, 6,         "Intel Core 2 Duo Mobile (Merom) / Celeron (Conroe-L/Merom-L) / Celeron M (Merom-L), 65nm");
   FMSQ(    0, 6,  1, 7,  6, sQ, "Intel Xeon Processor 3300 (Yorkfield C0) / Xeon Processor 5200 (Wolfdale C0) / Xeon Processor 5400 (Harpertown C0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, sX, "Intel Xeon Processor 3100 (Wolfdale C0) / Xeon Processor 5200 (Wolfdale C0) / Xeon Processor 5400 (Harpertown C0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, Xc, "Intel Core 2 Extreme QX9000 (Yorkfield C0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, Me, "Intel Mobile Core 2 Duo (Penryn C0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, Mc, "Intel Mobile Core 2 Duo (Penryn M0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, de, "Intel Core 2 Duo (Wolfdale C0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, dc, "Intel Core 2 Duo (Wolfdale M0), 45nm");
   FMSQ(    0, 6,  1, 7,  6, dP, "Intel Pentium Dual-Core Processor E5000 (Wolfdale M0), 45nm");
   FMS (    0, 6,  1, 7,  6,     "Intel Core 2 Duo (Wolfdale C0/M0) / Mobile Core 2 Duo (Penryn C0/M0) / Core 2 Extreme QX9000 (Yorkfield C0) / Pentium Dual-Core Processor E5000 (Wolfdale M0) / Xeon Processor 3100 (Wolfdale C0) / Xeon Processor 3300 (Yorkfield C0) / Xeon Processor 5200 (Wolfdale C0) / Xeon Processor 5400 (Harpertown C0), 45nm");
   FMSQ(    0, 6,  1, 7,  7, sQ, "Intel Xeon Processor 3300 (Yorkfield C1), 45nm");
   FMSQ(    0, 6,  1, 7,  7, Xc, "Intel Core 2 Extreme QX9000 (Yorkfield C1), 45nm");
   FMSQ(    0, 6,  1, 7,  7, Qe, "Intel Core 2 Quad-Core Q9000 (Yorkfield C1), 45nm");
   FMSQ(    0, 6,  1, 7,  7, Qc, "Intel Core 2 Quad-Core Q9000 (Yorkfield M1), 45nm");
   FMS (    0, 6,  1, 7,  7,     "Intel Core 2 Quad-Core Q9000 (Yorkfield C1/M1) / Core 2 Extreme QX9000 (Yorkfield C1) / Xeon Processor 3300 (Yorkfield C1), 45nm");
   FMSQ(    0, 6,  1, 7, 10, Me, "Intel Mobile Core 2 (Penryn E0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, Mc, "Intel Mobile Core 2 (Penryn R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, Qe, "Intel Core 2 Quad-Core Q9000 (Yorkfield E0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, Qc, "Intel Core 2 Quad-Core Q9000 (Yorkfield R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, de, "Intel Core 2 Duo (Wolfdale E0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, dc, "Intel Core 2 Duo (Wolfdale R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, dP, "Intel Pentium Dual-Core Processor E5000/E6000 (Wolfdale R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, dC, "Intel Celeron E3000 (Wolfdale R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, se, "Intel Xeon Processor 3300 (Yorkfield E0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, sQ, "Intel Xeon Processor 3300 (Yorkfield R0), 45nm");
   FMSQ(    0, 6,  1, 7, 10, sX, "Intel Xeon Processor 3100 (Wolfdale E0) / Xeon Processor 3300 (Yorkfield R0) / Xeon Processor 5200 (Wolfdale E0) / Xeon Processor 5400 (Harpertown E0), 45nm");
   FMS (    0, 6,  1, 7, 10,     "Intel Core 2 Duo (Wolfdale E0/R0) / Core 2 Quad-Core Q9000 (Yorkfield E0/R0) / Mobile Core 2 (Penryn E0/R0) / Pentium Dual-Core Processor E5000/E600 (Wolfdale R0) / Celeron E3000 (Wolfdale R0) / Xeon Processor 3100 (Wolfdale E0) / Xeon Processor 3300 (Yorkfield E0/R0) / Xeon Processor 5200 (Wolfdale E0) / Xeon Processor 5400 (Harpertown E0), 45nm");
   FMQ (    0, 6,  1, 7,     se, "Intel Xeon (Wolfdale / Yorkfield / Harpertown), 45nm");
   FMQ (    0, 6,  1, 7,     sQ, "Intel Xeon (Wolfdale / Yorkfield / Harpertown), 45nm");
   FMQ (    0, 6,  1, 7,     sX, "Intel Xeon (Wolfdale / Yorkfield / Harpertown), 45nm");
   FMQ (    0, 6,  1, 7,     Mc, "Intel Mobile Core 2 (Penryn), 45nm");
   FMQ (    0, 6,  1, 7,     Xc, "Intel Core 2 Extreme (Yorkfield), 45nm");
   FMQ (    0, 6,  1, 7,     Qc, "Intel Core 2 Quad-Core (Yorkfield), 45nm");
   FMQ (    0, 6,  1, 7,     dc, "Intel Core 2 Duo (Wolfdale), 45nm");
   FMQ (    0, 6,  1, 7,     dC, "Intel Celeron (Wolfdale), 45nm");
   FMQ (    0, 6,  1, 7,     dP, "Intel Pentium Dual-Core (Wolfdale), 45nm");
   FM  (    0, 6,  1, 7,         "Intel Core 2 Duo (Wolfdale) / Mobile Core 2 (Penryn) / Core 2 Quad-Core (Yorkfield) / Core 2 Extreme (Yorkfield) / Celeron (Wolfdale) / Pentium Dual-Core (Wolfdale) / Xeon (Wolfdale / Yorkfield / Harpertown), 45nm");
   FMS (    0, 6,  1,10,  4,     "Intel Core i7-900 (Bloomfield C0), 45nm");
   FMSQ(    0, 6,  1,10,  5, dc, "Intel Core i7-900 (Bloomfield D0), 45nm");
   FMSQ(    0, 6,  1,10,  5, sX, "Intel Xeon Processor 3500 (Bloomfield D0) / Xeon Processor 5500 (Gainestown D0), 45nm");
   FMS (    0, 6,  1,10,  5,     "Intel Core i7-900 (Bloomfield D0) / Xeon Processor 3500 (Bloomfield D0) / Xeon Processor 5500 (Gainestown D0), 45nm");
   FMQ (    0, 6,  1,10,     dc, "Intel Core (Bloomfield), 45nm");
   FMQ (    0, 6,  1,10,     sX, "Intel Xeon (Bloomfield / Gainestown), 45nm");
   FM  (    0, 6,  1,10,         "Intel Core (Bloomfield) / Xeon (Bloomfield / Gainestown), 45nm");
   FMS (    0, 6,  1,12,  1,     "Intel Atom N270 (Diamondville B0), 45nm");
   FMS (    0, 6,  1,12,  2,     "Intel Atom 200/N200/300 (Diamondville C0) / Atom Z500 (Silverthorne C0), 45nm");
   FMS (    0, 6,  1,12, 10,     "Intel Atom D400/N400 (Pineview A0) / Atom D500/N500 (Pineview B0), 45nm");
   FM  (    0, 6,  1,12,         "Intel Atom (Diamondville / Silverthorne / Pineview), 45nm");
   FMS (    0, 6,  1,13,  1,     "Intel Xeon Processor 7400 (Dunnington A1), 45nm");
   FM  (    0, 6,  1,13,         "Intel Xeon (Dunnington), 45nm");
   FMSQ(    0, 6,  1,14,  4, sX, "Intel Xeon Processor C3500/C5500 (Jasper Forest B0), 45nm");
   FMSQ(    0, 6,  1,14,  4, dC, "Intel Celeron P1053 (Jasper Forest B0), 45nm");
   FMQ (    0, 6,  1,14,     sX, "Intel Xeon Processor C3500/C5500 (Jasper Forest B0) / Celeron P1053 (Jasper Forest B0), 45nm");
   FMSQ(    0, 6,  1,14,  5, sX, "Intel Xeon Processor 3400 (Lynnfield B1), 45nm");
   FMSQ(    0, 6,  1,14,  5, Mc, "Intel Core i7-700/800/900 Mobile (Clarksfield B1), 45nm");
   FMSQ(    0, 6,  1,14,  5, dc, "Intel Core i5-700 / i7-800 (Lynnfield B1), 45nm");
   FMS (    0, 6,  1,14,  5,     "Intel Intel Core i5-700 / i7-800 (Lynnfield B1) / Core i7-700/800/900 Mobile (Clarksfield B1) / Xeon Processor 3400 (Lynnfield B1), 45nm");
   FMQ (    0, 6,  1,14,     sX, "Intel Xeon (Lynnfield) / Xeon (Jasper Forest), 45nm");
   FMQ (    0, 6,  1,14,     dC, "Intel Celeron (Jasper Forest), 45nm");
   FMQ (    0, 6,  1,14,     Mc, "Intel Core Mobile (Clarksfield), 45nm");
   FMQ (    0, 6,  1,14,     dc, "Intel Core (Lynnfield), 45nm");
   FM  (    0, 6,  1,14,         "Intel Intel Core (Lynnfield) / Core Mobile (Clarksfield) / Xeon (Lynnfield) / Xeon (Jasper Forest), 45nm");
   FMSQ(    0, 6,  2, 5,  2, sX, "Intel Xeon Processor L3406 (Clarkdale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  2, MC, "Intel Celeron Mobile P4500 (Arrandale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  2, MP, "Intel Pentium P6000 Mobile (Arrandale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  2, dP, "Intel Pentium G6900 / P4505 (Clarkdale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  2, Mc, "Intel Core i3-300 Mobile / Core i5-400 Mobile / Core i5-500 Mobile / Core i7-600 Mobile (Arrandale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  2, dc, "Intel Core i3-300 / i3-500 / i5-500 / i5-600 / i7-600 (Clarkdale C2), 32nm");
   FMS (    0, 6,  2, 5,  2,     "Intel Core i3 / i5 / i7 (Clarkdale C2) / Core i3 Mobile / Core i5 Mobile / Core i7 Mobile (Arrandale C2) / Pentium P6000 Mobile (Arrandale C2) / Celeron Mobile P4500 (Arrandale C2) / Xeon Processor L3406 (Clarkdale C2), 32nm");
   FMSQ(    0, 6,  2, 5,  5, MC, "Intel Celeron Celeron Mobile U3400 (Arrandale K0) / Celeron Mobile P4600 (Arrandale K0), 32nm");
   FMSQ(    0, 6,  2, 5,  5, MP, "Intel Pentium U5000 Mobile (Arrandale K0), 32nm");
   FMSQ(    0, 6,  2, 5,  5, dP, "Intel Pentium P4505 / U3405 (Clarkdale K0), 32nm");
   FMSQ(    0, 6,  2, 5,  5, dc, "Intel Core i3-300 / i3-500 / i5-400 / i5-500 / i5-600 / i7-600 (Clarkdale K0), 32nm");
   FMS (    0, 6,  2, 5,  5,     "Intel Core i3 / i5 / i7  (Clarkdale K0) / Pentium U5000 Mobile / Pentium P4505 / U3405 / Celeron Mobile P4000 / U3000 (Arrandale K0), 32nm");
   FMQ (    0, 6,  2, 5,     sX, "Intel Xeon Processor L3406 (Clarkdale), 32nm");
   FMQ (    0, 6,  2, 5,     MC, "Intel Celeron Mobile (Arrandale), 32nm");
   FMQ (    0, 6,  2, 5,     MP, "Intel Pentium Mobile (Arrandale), 32nm");
   FMQ (    0, 6,  2, 5,     dP, "Intel Pentium (Clarkdale), 32nm");
   FMQ (    0, 6,  2, 5,     Mc, "Intel Core Mobile (Arrandale), 32nm");
   FMQ (    0, 6,  2, 5,     dc, "Intel Core (Clarkdale), 32nm");
   FM  (    0, 6,  2, 5,         "Intel Core (Clarkdale) / Core (Arrandale) / Pentium (Clarkdale) / Pentium Mobile (Arrandale) / Celeron Mobile (Arrandale) / Xeon (Clarkdale), 32nm");
   FMS (    0, 6,  2, 6,  1,     "Intel Atom Z600 (Lincroft C0) / Atom E600 (Tunnel Creek B0/B1), 45nm");
   FM  (    0, 6,  2, 6,         "Intel Atom Z600 (Lincroft) / Atom E600 (Tunnel Creek B0/B1), 45nm");
   FMSQ(    0, 6,  2,10,  7, Xc, "Intel Mobile Core i7 Extreme (Sandy Bridge D2/J1/Q0), 32nm");
   FMSQ(    0, 6,  2,10,  7, Mc, "Intel Mobile Core i3-2000 / Mobile Core i5-2000 / Mobile Core i7-2000 (Sandy Bridge D2/J1/Q0), 32nm");
   FMSQ(    0, 6,  2,10,  7, dc, "Intel Core i3-2000 / Core i5-2000 / Core i7-2000 (Sandy Bridge D2/J1/Q0), 32nm");
   FMSQ(    0, 6,  2,10,  7, MC, "Intel Celeron G400/G500/700/800/B800 (Sandy Bridge J1/Q0), 32nm");
   FMSQ(    0, 6,  2,10,  7, sX, "Intel Xeon E3-1100 / E3-1200 v1 (Sandy Bridge D2/J1/Q0), 32nm");
   FMSQ(    0, 6,  2,10,  7, dP, "Intel Pentium G500/G600/G800 / Pentium B915C (Sandy Bridge Q0), 32nm");
   FMS (    0, 6,  2,10,  7,     "Intel Core i3-2000 / Core i5-2000 / Core i7-2000 / Mobile Core i7-2000 (Sandy Bridge D2/J1/Q0) / Pentium G500/G600/G800 / Pentium B915C (Sandy Bridge Q0) / Celeron G400/G500/700/800/B800 (Sandy Bridge J1/Q0) / Xeon E1-1100 / E3-1200 (Sandy Bridge D2/J1/Q0), 32nm");
   FMQ (    0, 6,  2,10,     Xc, "Intel Mobile Core i7 Extreme (Sandy Bridge), 32nm");
   FMQ (    0, 6,  2,10,     Mc, "Intel Mobile Core i3-2000 / Mobile Core i5-2000 / Mobile Core i7-2000 (Sandy Bridge), 32nm");
   FMQ (    0, 6,  2,10,     dc, "Intel Core i5-2000 / Core i7-2000 (Sandy Bridge), 32nm");
   FMQ (    0, 6,  2,10,     MC, "Intel Celeron G400/G500/700/800/B800 (Sandy Bridge), 32nm");
   FMQ (    0, 6,  2,10,     sX, "Intel Xeon E3-1100 / E3-1200 v1 (Sandy Bridge), 32nm");
   FMQ (    0, 6,  2,10,     dP, "Intel Pentium G500/G600/G800 / Pentium B915C (Sandy Bridge), 32nm");
   FM  (    0, 6,  2,10,         "Intel Core i5-2000 / Core i7-2000 / Mobile Core i3-2000 / Mobile Core i5-2000 / Mobile Core i7-2000 / Pentium G500/G600/G800 / Pentium B915C / Celeron G400/G500/700/800/B800 / Xeon E1-1100 / E3-1200 (Sandy Bridge), 32nm");
   FMSQ(    0, 6,  2,12,  2, dc, "Intel Core i7-900 / Core i7-980X (Gulftown B1), 32nm");
   FMSQ(    0, 6,  2,12,  2, sX, "Intel Xeon Processor 3600 (Westmere-EP B1) / Xeon Processor 5600 (Westmere-EP B1), 32nm");
   FMS (    0, 6,  2,12,  2,     "Intel Core i7-900 (Gulftown B1) / Core i7-980X (Gulftown B1) / Xeon Processor 3600 (Westmere-EP B1) / Xeon Processor 5600 (Westmere-EP B1), 32nm");
   FM  (    0, 6,  2,12,         "Intel Core (Gulftown) / Xeon (Westmere-EP), 32nm");
   FMSQ(    0, 6,  2,13,  6, sX, "Intel Xeon E5-1600/2600 (Sandy Bridge-E C1), 32nm");
   FMSQ(    0, 6,  2,13,  6, dP, "Intel Core i7-3800/3900 (Sandy Bridge-E C1), 32nm");
   FMS (    0, 6,  2,13,  6,     "Intel Core i7-3800/3900 (Sandy Bridge-E C1) / Xeon E5-1600/2600 (Sandy Bridge-E C1), 32nm");
   FMSQ(    0, 6,  2,13,  7, sX, "Intel Xeon E5-1600/2600/4600 (Sandy Bridge-E C2/M1), 32nm");
   FMSQ(    0, 6,  2,13,  7, dP, "Intel Core i7-3800/3900 (Sandy Bridge-E C2), 32nm");
   FMS (    0, 6,  2,13,  7,     "Intel Core i7-3800/3900 (Sandy Bridge-E C2) / Xeon E5-1600/2600/4600 (Sandy Bridge-E C2/M1), 32nm");
   FMQ (    0, 6,  2,13,     sX, "Intel Xeon E5-1600/2600 (Sandy Bridge-E), 32nm");
   FMQ (    0, 6,  2,13,     dP, "Intel Core i7-3800/3900 (Sandy Bridge-E), 32nm");
   FM  (    0, 6,  2,13,         "Intel Core i7-3800/3900 (Sandy Bridge-E) / Xeon E5-1600/2600/4600 (Sandy Bridge-E), 32nm");
   FMS (    0, 6,  2,14,  6,     "Intel Xeon Processor 7500 (Beckton D0), 45nm");
   FM  (    0, 6,  2,14,         "Intel Xeon (Beckton), 45nm");
   FMS (    0, 6,  2,15,  2,     "Intel Xeon E7-8800 / Xeon E7-4800 / Xeon E7-2800 (Westmere-EX A2), 32nm");
   FM  (    0, 6,  2,15,         "Intel Xeon E7-8800 / Xeon E7-4800 / Xeon E7-2800 (Westmere-EX), 32nm");
   FMS (    0, 6,  3, 5,  1,     "Intel Atom Z2760 (Clover Trail C0) / Z8000 (Cherry Trail C0), 14nm");
   FM  (    0, 6,  3, 5,         "Intel Atom Z2760 (Clover Trail) / Z8000 (Cherry Trail), 14nm");
   // Intel docs (328198) do not provide any FMS for Centerton, but an example
   // from jhladky@redhat.com does.
   FMS (    0, 6,  3, 6,  1,     "Intel Atom D2000/N2000 (Cedarview B1/B2/B3) / S1200 (Centerton B1), 32nm");
   FM  (    0, 6,  3, 6,         "Intel Atom D2000/N2000 (Cedarview) / S1200 (Centerton B1), 32nm");
   FMS (    0, 6,  3, 7,  1,     "Intel Atom Z3000 (Bay Trail-T A0), 22nm");
   FMS (    0, 6,  3, 7,  2,     "Intel Pentium / Celeron (Bay Trail-M B0/B1), 22nm");
   FMS (    0, 6,  3, 7,  3,     "Intel Pentium N3500 / J2850 / Celeron N1700 / N1800 / N2800 / N2900 (Bay Trail-M B2/B3) / Atom E3800 (Bay Trail-I B3), 22nm");
   FMSQ(    0, 6,  3, 7,  4, dC, "Intel Celeron N2800 / N2900 (Bay Trail-M C0), 22nm");
   FMSQ(    0, 6,  3, 7,  4, dP, "Intel Pentium N3500 (Bay Trail-M C0), 22nm");
   FMS (    0, 6,  3, 7,  4,     "Intel Pentium N3500 / Celeron N2800 / N2900 (Bay Trail-M C0) / Atom Z3000 (Bay Trail-T B2/B3), 22nm");
   FMS (    0, 6,  3, 7,  9,     "Intel Atom E3800 (Bay Trail-I D0), 22nm");
   FM  (    0, 6,  3, 7,         "Intel Pentium N3500 / J2850 / Celeron N1700 / N1800 / N2800 / N2900 (Bay Trail-M) / Atom Z3000 (Bay Trail-T) / Atom E3800 (Bay Trail-I), 22nm");
   FMSQ(    0, 6,  3,10,  9, Mc, "Intel Mobile Core i3-3000 (Ivy Bridge L1) / i5-3000 (Ivy Bridge L1) / i7-3000 (Ivy Bridge E1/L1) / Pentium 900/1000/2000/2100 (P0), 22nm");
   FMSQ(    0, 6,  3,10,  9, dc, "Intel Core i3-3000 (Ivy Bridge L1) / i5-3000 (Ivy Bridge E1/N0/L1) / i7-3000 (Ivy Bridge E1), 22nm");
   FMSQ(    0, 6,  3,10,  9, sX, "Intel Xeon E3-1100 v2 / E3-1200 v2 (Ivy Bridge E1/N0/L1), 22nm");
   FMSQ(    0, 6,  3,10,  9, dP, "Intel Pentium G1600/G2000/G2100 / Pentium B925C (Ivy Bridge P0), 22nm");
   FMS (    0, 6,  3,10,  9,     "Intel Core i3-3000 (Ivy Bridge L1) / i5-3000 (Ivy Bridge E1/N0/L1) / i7-3000 (Ivy Bridge E1) / Mobile Core i3-3000 (Ivy Bridge L1) / i5-3000 (Ivy Bridge L1) / Mobile Core i7-3000 (Ivy Bridge E1/L1) / Xeon E3-1100 v2 / E3-1200 v2 (Ivy Bridge E1/N0/L1) / Pentium G1600/G2000/G2100 / Pentium B925C (Ivy Bridge P0) / Pentium 900/1000/2000/2100 (P0), 22nm");
   FMQ (    0, 6,  3,10,     Mc, "Intel Mobile Core i3-3000 (Ivy Bridge) / Mobile Core i5-3000 (Ivy Bridge) / Mobile Core i7-3000 (Ivy Bridge) / Pentium 900/1000/2000/2100, 22nm");
   FMQ (    0, 6,  3,10,     dc, "Intel Core i3-3000 (Ivy Bridge) / i5-3000 (Ivy Bridge) / i7-3000 (Ivy Bridge), 22nm");
   FMQ (    0, 6,  3,10,     sX, "Intel Xeon E3-1100 v2 / E3-1200 v2 (Ivy Bridge), 22nm");
   FMQ (    0, 6,  3,10,     dP, "Intel Pentium G1600/G2000/G2100 / Pentium B925C (Ivy Bridge), 22nm");
   FM  (    0, 6,  3,10,         "Intel Core i3-3000 (Ivy Bridge) / i5-3000 (Ivy Bridge) / i7-3000 (Ivy Bridge) / Mobile Core i3-3000 (Ivy Bridge) / Mobile Core i5-3000 (Ivy Bridge) / Mobile Core i7-3000 (Ivy Bridge) / Xeon E3-1100 v2 / E3-1200 v2 (Ivy Bridge) / Pentium G1600/G2000/G2100 (Ivy Bridge) / Pentium 900/1000/2000/2100 / Pentium B925C, 22nm");
   // Intel docs (328899, 328903, 328908) omit the stepping numbers for (0,6),(3,12) C0 & D0.
   FMQ (    0, 6,  3,12,     sX, "Intel Xeon E3-1200 v3 (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     Mc, "Intel Mobile Core i3-4000U / Mobile Core i5-4000U / Mobile Core i7-4000U (Mobile M) (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     dc, "Intel Core i3-4000 / i5-4000 / i7-4000 / Mobile Core i3-4000 / i5-4000 / i7-4000 (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     MC, "Intel Mobile Celeron 2900U (Mobile M) (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     dC, "Intel Celeron G1800 (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     MP, "Intel Mobile Pentium 3500U / 3600U / 3500Y (Mobile M) (Haswell), 22nm");
   FMQ (    0, 6,  3,12,     dP, "Intel Pentium G3000 (Haswell), 22nm");
   FM  (    0, 6,  3,12,         "Intel Core i5-4000 / i7-4000 / Mobile Core i3-4000 / i5-4000 / i7-4000 / Mobile Core i3-4000 / Mobile Core i5-4000 / Mobile Core i7-4000 / Pentium G3000 / Celeron G1800 / Mobile Pentium 3500U / Mobile Celeron 2900U / Xeon E3-1200 v3 (Mobile M) (Haswell), 22nm");
   // Intel docs (330836) omit the stepping numbers for (0,6),(3,13) E0 & F0.
   FMQ (    0, 6,  3,13,     dc, "Intel Core i3-5000 / i5-5000 / i7-5000 / Core M (Broadwell), 14nm");
   FMQ (    0, 6,  3,13,     MC, "Intel Mobile Celeron 3000 (Broadwell), 14nm");
   FMQ (    0, 6,  3,13,     dC, "Intel Celeron 3000 (Broadwell), 14nm");
   FM  (    0, 6,  3,13,         "Intel Core i3-5000 / i5-5000 / i7-5000 / Core M / Celeron 3000 / Mobile Celeron 3000 (Broadwell), 14nm");
   FMSQ(    0, 6,  3,14,  4, sX, "Intel Xeon E5-1600/E5-2600 v2 (Ivy Bridge-EP C1/M1/S1), 22nm");
   FMSQ(    0, 6,  3,14,  4, dc, "Intel Core i7-4000 / i9-4000 (Ivy Bridge-EP S1), 22nm");
   FMS (    0, 6,  3,14,  4,     "Intel Core i7-4000 / i9-4000 / Xeon E5-1600/E5-2600 v2 (Ivy Bridge-EP C1/M1/S1), 22nm");
   FMSQ(    0, 6,  3,14,  7, sX, "Intel Xeon E7 v2 (Ivy Bridge-EX D1), 22nm");
   FMS (    0, 6,  3,14,  7,     "Intel Xeon E7 v2 (Ivy Bridge-EX D1), 22nm");
   FMQ (    0, 6,  3,14,     sX, "Intel Xeon E5-1600/E5-2600 v2 (Ivy Bridge-EP) / Xeon E7 (Ivy Bridge-EX), 22nm");
   FMQ (    0, 6,  3,14,     dc, "Intel Core i9-4000 (Ivy Bridge-EP), 22nm");
   FM  (    0, 6,  3,14,         "Intel Core i9-4000 / Xeon E5-1600/E5-2600 v2 (Ivy Bridge-EP) / Xeon E7 (Ivy Bridge-EX), 22nm");
   FMS (    0, 6,  3,15,  2,     "Intel Core i7-5000 Extreme Edition (Haswell R2) / Xeon E5-x600 v3 (Haswell-EP C1/M1/R2), 22nm");
   FMS (    0, 6,  3,15,  4,     "Intel Xeon E7-4800 / E7-8800 v3 (Haswell-EP E0), 22nm");
   FM  (    0, 6,  3,15,         "Intel Core i7-5000 Extreme Edition (Haswell R2) / Xeon E5-x600 / Xeon E5-4800 / Xeon E5-8800 v3 (Haswell-EP), 22nm");
   // Intel docs (328903) omit the stepping numbers for (0,6),(4,5) C0 & D0.
   FMQ (    0, 6,  4, 5,     Mc, "Intel Mobile Core i3-4000Y / Mobile Core i5-4000Y / Mobile Core i7-4000Y (Mobile U/Y) (Haswell), 22nm");
   FMQ (    0, 6,  4, 5,     MP, "Intel Mobile Pentium 3500U / 3600U / 3500Y (Mobile U/Y) (Haswell), 22nm");
   FMQ (    0, 6,  4, 5,     MC, "Intel Mobile Celeron 2900U (Mobile U/Y) (Haswell), 22nm");
   FM  (    0, 6,  4, 5,         "Intel Mobile Core i3-4000Y / Mobile Core i5-4000Y / Mobile Core i7-4000Y / Mobile Pentium 3500U/3600U/3500Y / Mobile Celeron 2900U (Mobile U/Y) (Haswell), 22nm");
   // Intel docs (328899,328903) omit the stepping numbers for (0,6),(4,6) C0 & D0.
   FMQ (    0, 6,  4, 6,     Mc, "Intel Mobile Core i3-4000Y / Mobile Core i5-4000Y / Mobile Core i7-4000Y (Mobile H) (Haswell), 22nm");
   FMQ (    0, 6,  4, 6,     dc, "Intel Core i3-4000 / i5-4000 / i7-4000 / Mobile Core i3-4000 / i5-4000 / i7-4000 (Desktop R) (Haswell), 22nm");
   FMQ (    0, 6,  4, 6,     MP, "Intel Mobile Pentium 3500U / 3600U / 3500Y (Mobile H) (Haswell), 22nm");
   FMQ (    0, 6,  4, 6,     dC, "Intel Celeron G1800 (Desktop R) (Haswell), 22nm");
   FMQ (    0, 6,  4, 6,     MC, "Intel Mobile Celeron 2900U (Mobile H) (Haswell), 22nm");
   FMQ (    0, 6,  4, 6,     dP, "Intel Pentium G3000 (Desktop R) (Haswell), 22nm");
   FM  (    0, 6,  4, 6,         "Intel Core i5-4000 / i7-4000 / Mobile Core i3-4000 / i5-4000 / i7-4000 / Mobile Core i5-4000 / Mobile Core i5-4000 / Mobile Core i7-4000 / Pentium G3000 / Celeron G1800 / Mobile Pentium 3500U/3600U/3500Y / Mobile Celeron 2900U / Xeon E3-1200 v3 (Desktop R/Mobile H) (Haswell), 22nm");
   // So far, all these (0,6),(4,7) processors are stepping G0, but the
   // Intel docs (332381, 332382) omit the stepping number for G0.
   FMQ (    0, 6,  4, 7,     dc, "Intel Core i7-5000 (Broadwell), 14nm");
   FMQ (    0, 6,  4, 7,     Mc, "Intel Mobile Core i7-5000 (Broadwell), 14nm");
   FMQ (    0, 6,  4, 7,     sX, "Intel Xeon E3-1200 v4 (Broadwell), 14nm");
   FM  (    0, 6,  4, 7,         "Intel Core i7-5000 / Mobile Core i7-5000 / Xeon E3-1200 v4 (Broadwell), 14nm");
   FM  (    0, 6,  4,10,         "Intel Atom Z3400 (Merrifield), 22nm"); // no spec update; only 325462 Volume 3 Table 35-1 so far
   // The (0,6),(4,12) processors also have a D1 stepping, but the
   // Intel docs (332095) omit the stepping number.
   FMS (    0, 6,  4,12,  0,     "Intel Pentium N3000 / Celeron N3000 (Braswell C0), 14nm");
   FM  (    0, 6,  4,12,         "Intel Pentium N3000 / Celeron N3000 (Braswell), 14nm");
   FMS (    0, 6,  4,13,  0,     "Intel Atom C2000 (Avoton A0/A1), 22nm");
   FMS (    0, 6,  4,13,  8,     "Intel Atom C2000 (Avoton B0/C0), 22nm");
   FM  (    0, 6,  4,13,         "Intel Atom C2000 (Avoton), 22nm");
   // Intel docs (332689) omit the stepping numbers for (0,6),(4,14) D1 & K1.
   FMQ (    0, 6,  4,14,     dc, "Intel Core i3-6000U / i5-6000U / i7-6000U / m3-6Y00 / m5-6Y00 / m7-6Y00 (Skylake), 14nm");
   FMQ (    0, 6,  4,14,     dP, "Intel Pentium 4405U / Pentium 4405Y (Skylake), 14nm");
   FMQ (    0, 6,  4,14,     dC, "Intel Celeron 3800U / 39000U (Skylake), 14nm");
   FMQ (    0, 6,  4,14,     sX, "Intel Xeon E3-1500m (Skylake), 14nm"); // no spec update; only 325462 Volume 3 Table 35-1 so far
   FM  (    0, 6,  4,14,         "Intel Core i3-6000U / i5-6000U / i7-6000U / m3-6Y00 / m5-6Y00 / m7-6Y00 / Pentium 4405U / Pentium 4405Y / Celeron 3800U / 39000U / Xeon E3-1500m (Skylake), 14nm");
   // Intel docs (334208,333811) omit the stepping numbers for (0,6),(4,15)
   // B0, M0 & R0.
   FMQ (    0, 6,  4,15,     dc, "Intel Core i7-6800K / i7-6900K / i7-6900X (Broadwell-E), 14nm");
   FMSQ(    0, 6,  4,15,  1, sX, "Intel Xeon E5-1600 / E5-2600 / E5-4600 v4 (Broadwell) / E7-4800 / E7-8800 v4 (Broadwell-EX B0), 14nm");
   FMQ (    0, 6,  4,15,     sX, "Intel Xeon E5-1600 / E5-2600 / E5-4600 v4 (Broadwell) / E7-4800 / E7-8800 v4 (Broadwell-EX), 14nm");
   FM  (    0, 6,  4,15,         "Intel Core i7-6800K / i7-6900K / i7-6900X (Broadwell-E) / Xeon E5-1600 / E5-2500 / E5-4600 (Broadwell) / E7-4800 / E7-8800 v4 (Broadwell-EX), 14nm");
   FMSQ(    0, 6,  5, 5,  2, sX, "Intel Xeon W 2000 / Scalable Bronze 3000 / Silver 4000 / Gold 5000 / 6000 / Platinum 8000 (Skylake B0/L0), 14nm");
   FMSQ(    0, 6,  5, 5,  4, sX, "Intel Xeon W 2000 / Scalable Bronze 3000 / Silver 4000 / Gold 5000 / 6000 / Platinum 8000 (Skylake H0/M0/U0), 14nm");
   // Intel docs (335901) omit almost all details for the Core versions of
   // (0,6),(5,5).
   FMQ (    0, 6,  5, 5,     dc, "Intel Core i7-6000X / i7-7000X / i9-7000X (Skylake-X), 14nm");
   FMQ (    0, 6,  5, 5,     sX, "Intel Xeon W 2000 / Scalable Bronze 3000 / Silver 4000 / Gold 5000 / 6000 / Platinum 8000 (Skylake), 14nm");
   FM  (    0, 6,  5, 5,         "Intel Core i7-6000X / i7-7000X / i9-7000X (Skylake-X) / Xeon W 2000 / Scalable Bronze 3000 / Silver 4000 / Gold 5000 / 6000 / Platinum 8000 (Skylake), 14nm");
   FMS (    0, 6,  5, 6,  1,     "Intel Xeon D-1500 (Broadwell-DE U0), 14nm");
   FMS (    0, 6,  5, 6,  2,     "Intel Xeon D-1500 (Broadwell-DE V1), 14nm");
   FMS (    0, 6,  5, 6,  3,     "Intel Xeon D-1500 (Broadwell-DE V2), 14nm");
   FMS (    0, 6,  5, 6,  4,     "Intel Xeon D-1500 (Broadwell-DE Y0), 14nm");
   FMS (    0, 6,  5, 6,  5,     "Intel Xeon D-1500N (Broadwell-DE A1), 14nm");
   FM  (    0, 6,  5, 6,         "Intel Xeon D-1500 / D-1500N (Broadwell-DE), 14nm");
   // Intel docs (334646) omit the stepping number for B0.  But as of Jan 2017,
   // it is the only stepping, and all examples seen have stepping number 1.
   FMS (    0, 6,  5, 7,  1,     "Intel Xeon Phi x200 (Knights Landing B0), 14nm");
   FM  (    0, 6,  5, 7,         "Intel Xeon Phi x200 (Knights Landing), 14nm");
   FM  (    0, 6,  5,10,         "Intel Atom Z3400 (Moorefield), 22nm"); // no spec update; only 325462 Volume 3 Table 35-1 so far
   // Intel docs (334820) omit the stepping numbers for B0 & B1.
   FMSQ(    0, 6,  5,12,  9, dP, "Intel Pentium N4000 / J4000 (Apollo Lake), 14nm");
   FMSQ(    0, 6,  5,12,  9, dC, "Intel Celeron N3000 / J3000 (Apollo Lake), 14nm");
   FMS (    0, 6,  5,12,  9,     "Intel Pentium N4000 / J4000 / Celeron N3000 / J3000 (Apollo Lake), 14nm");
   FM  (    0, 6,  5,12,         "Intel Atom (Goldmont) / Pentium N4000 / J4000 / Celeron N3000 / J3000 (Apollo Lake), 14nm"); // no spec update for Goldmont; only 325462 Volume 3 Table 35-1 so far
   FM  (    0, 6,  5,13,         "Intel Atom X3-C3000 (SoFIA), 22nm"); // no spec update; only 325462 Volume 3 Table 35-1 so far
   // Intel docs (332689,333133) omit the stepping numbers for (0,6),(5,14)
   // R0 & S0.
   FMQ (    0, 6,  5,14,     dc, "Intel Core i3-6000 / i5-6000 / i7-6000 (Skylake), 14nm");
   FMQ (    0, 6,  5,14,     dP, "Intel Pentium G4000 (Skylake), 14nm");
   FMQ (    0, 6,  5,14,     dC, "Intel Celeron G3900 (Skylake), 14nm");
   FMQ (    0, 6,  5,14,     sX, "Intel Xeon E3-1200 v5 (Skylake), 14nm");
   FM  (    0, 6,  5,14,         "Intel Core i3-6000 / i5-6000 / i7-6000 / Pentium G4000 / Celeron G3900 / Xeon E3-1200 (Skylake), 14nm");
   FM  (    0, 6,  5,15,         "Intel Atom (Goldmont), 14nm"); // no spec update; only 325462 Volume 3 Table 35-1 so far
   FMS (    0, 6,  7,10,  1,     "Intel Pentium Silver N5000 / J5000 / Celeron N4000 / J4000 (Gemini Lake B0), 14nm");
   FM  (    0, 6,  7,10,         "Intel Pentium Silver N5000 / J5000 / Celeron N4000 / J4000 (Gemini Lake), 14nm");
   FM  (    0, 6,  8, 5,         "Intel Xeon Phi (Knights Mill), 14nm"); // no spec update; 325462 Volume 3 Table 35-1 is vague; Piotr Luc said it would be Knights Mill
   // Intel docs (334663) omit the stepping numbers for (0,6),(8,14)
   // H0, J1 & Y0.
   FMQ (    0, 6,  8,14,     dc, "Intel m3-7Y00 / i5-7Y00 / i7-7Y00 / i3-7000U / i5-7000U / i7-7000U (Kaby Lake), 14nm");
   FMQ (    0, 6,  8,14,     dP, "Intel Pentium 4410Y / 4415U (Kaby Lake), 14nm");
   FMQ (    0, 6,  8,14,     dC, "Intel Celeron 3965Y / 3865U / 3965U (Kaby Lake), 14nm");
   FM  (    0, 6,  8,14,         "Intel m3-7Y00 / i5-7Y00 / i7-7Y00 / i3-7000U / i5-7000U / i7-7000U / Pentium 4410Y / 4415U / Celeron 3965Y / 3865U / 3965U (Kaby Lake), 14nm");
   // Intel docs (334663) omit the stepping numbers for (0,6),(9,14) B0.
   FMQ (    0, 6,  9,14,     dc, "Intel Core i5-7000 / i5-7000K / i5-7000T / i7-7000 / i3-7100H / i5-7000HQ / i7-7000HQ / i7-7000X / i5-7000X / i7-8000 / i5-8000 / i3-8000 (Kaby Lake), 14nm");
   FMQ (    0, 6,  9,14,     dC, "Intel Celeron G3930 (Kaby Lake), 14nm");
   FMQ (    0, 6,  9,14,     sX, "Intel Xeon E3-1200v6 / E3-1285v5 / E3-15x5Mv6 (Kaby Lake), 14nm");
   FM  (    0, 6,  9,14,         "Intel Core i5-7000 / i5-7000K / i5-7000T / i7-7000 / i3-7100H / i5-7000HQ / i7-7000HQ / i7-7000X / i5-7000X / i7-8000 / i5-8000 / i3-8000 / Xeon E3-1200v6 / E3-1285v5 / E3-15x5Mv6 / Celeron G3930 (Kaby Lake), 14nm");
   FQ  (    0, 6,            sX, "Intel Xeon (unknown model)");
   FQ  (    0, 6,            se, "Intel Xeon (unknown model)");
   FQ  (    0, 6,            MC, "Intel Mobile Celeron (unknown model)");
   FQ  (    0, 6,            dC, "Intel Celeron (unknown model)");
   FQ  (    0, 6,            Xc, "Intel Core Extreme (unknown model)");
   FQ  (    0, 6,            Mc, "Intel Mobile Core (unknown model)");
   FQ  (    0, 6,            dc, "Intel Core (unknown model)");
   FQ  (    0, 6,            MP, "Intel Mobile Pentium (unknown model)");
   FQ  (    0, 6,            dP, "Intel Pentium (unknown model)");
   F   (    0, 6,                "Intel Pentium II / Pentium III / Pentium M / Celeron / Celeron M / Core / Core 2 / Core i / Xeon / Atom (unknown model)");
   FMS (    0, 7,  0, 6,  4,     "Intel Itanium (Merced C0)");
   FMS (    0, 7,  0, 7,  4,     "Intel Itanium (Merced C1)");
   FMS (    0, 7,  0, 8,  4,     "Intel Itanium (Merced C2)");
   F   (    0, 7,                "Intel Itanium (unknown model)");
   FMS (    0,11,  0, 1,  1,     "Intel Xeon Phi x100 Coprocessor (Knights Corner B0), 22nm");
   FMS (    0,11,  0, 1,  3,     "Intel Xeon Phi x100 Coprocessor (Knights Corner B1), 22nm");
   FMS (    0,11,  0, 1,  4,     "Intel Xeon Phi x100 Coprocessor (Knights Corner C0), 22nm");
   FM  (    0,11,  0, 1,         "Intel Xeon Phi x100 Coprocessor (Knights Corner), 22nm");
   FMS (    0,15,  0, 0,  7,     "Intel Pentium 4 (Willamette B2), .18um");
   FMSQ(    0,15,  0, 0, 10, dP, "Intel Pentium 4 (Willamette C1), .18um");
   FMSQ(    0,15,  0, 0, 10, sX, "Intel Xeon (Foster C1), .18um");
   FMS (    0,15,  0, 0, 10,     "Intel Pentium 4 (Willamette C1) / Xeon (Foster C1), .18um");
   FMQ (    0,15,  0, 0,     dP, "Intel Pentium 4 (Willamette), .18um");
   FMQ (    0,15,  0, 0,     sX, "Intel Xeon (Foster), .18um");
   FM  (    0,15,  0, 0,         "Intel Pentium 4 (Willamette) / Xeon (Foster), .18um");
   FMS (    0,15,  0, 1,  1,     "Intel Xeon MP (Foster C0), .18um");
   FMSQ(    0,15,  0, 1,  2, dP, "Intel Pentium 4 (Willamette D0), .18um");
   FMSQ(    0,15,  0, 1,  2, sX, "Intel Xeon (Foster D0), .18um");
   FMS (    0,15,  0, 1,  2,     "Intel Pentium 4 (Willamette D0) / Xeon (Foster D0), .18um");
   FMSQ(    0,15,  0, 1,  3, dP, "Intel Pentium 4(Willamette E0), .18um");
   FMSQ(    0,15,  0, 1,  3, dC, "Intel Celeron 478-pin (Willamette E0), .18um");
   FMS (    0,15,  0, 1,  3,     "Intel Pentium 4 / Celeron (Willamette E0), .18um");
   FMQ (    0,15,  0, 1,     dP, "Intel Pentium 4 (Willamette), .18um");
   FMQ (    0,15,  0, 1,     sX, "Intel Xeon (Foster), .18um");
   FM  (    0,15,  0, 1,         "Intel Pentium 4 (Willamette) / Xeon (Foster), .18um");
   FMS (    0,15,  0, 2,  2,     "Intel Xeon MP (Gallatin A0), .13um");
   FMSQ(    0,15,  0, 2,  4, sX, "Intel Xeon (Prestonia B0), .13um");
   FMSQ(    0,15,  0, 2,  4, MM, "Intel Mobile Pentium 4 Processor-M (Northwood B0), .13um");
   FMSQ(    0,15,  0, 2,  4, MC, "Intel Mobile Celeron (Northwood B0), .13um");
   FMSQ(    0,15,  0, 2,  4, dP, "Intel Pentium 4 (Northwood B0), .13um");
   FMS (    0,15,  0, 2,  4,     "Intel Pentium 4 (Northwood B0) / Xeon (Prestonia B0) / Mobile Pentium 4 Processor-M (Northwood B0) / Mobile Celeron (Northwood B0), .13um");
   FMSQ(    0,15,  0, 2,  5, dP, "Intel Pentium 4 (Northwood B1/M0), .13um");
   FMSQ(    0,15,  0, 2,  5, sM, "Intel Xeon MP (Gallatin B1), .13um");
   FMSQ(    0,15,  0, 2,  5, sX, "Intel Xeon (Prestonia B1), .13um");
   FMS (    0,15,  0, 2,  5,     "Intel Pentium 4 (Northwood B1/M0) / Xeon (Prestonia B1) / Xeon MP (Gallatin B1), .13um");
   FMS (    0,15,  0, 2,  6,     "Intel Xeon MP (Gallatin C0), .13um");
   FMSQ(    0,15,  0, 2,  7, sX, "Intel Xeon (Prestonia C1), .13um");
   FMSQ(    0,15,  0, 2,  7, dC, "Intel Celeron 478-pin (Northwood C1), .13um");
   FMSQ(    0,15,  0, 2,  7, MC, "Intel Mobile Celeron (Northwood C1), .13um");
   FMSQ(    0,15,  0, 2,  7, MM, "Intel Mobile Pentium 4 Processor-M (Northwood C1), .13um");
   FMSQ(    0,15,  0, 2,  7, dP, "Intel Pentium 4 (Northwood C1), .13um");
   FMS (    0,15,  0, 2,  7,     "Intel Pentium 4 (Northwood C1) / Xeon (Prestonia C1) / Mobile Pentium 4 Processor-M (Northwood C1) / Celeron 478-Pin (Northwood C1) / Mobile Celeron (Northwood C1), .13um");
   FMSQ(    0,15,  0, 2,  9, sX, "Intel Xeon (Prestonia D1), .13um");
   FMSQ(    0,15,  0, 2,  9, dC, "Intel Celeron 478-pin (Northwood D1), .13um");
   FMSQ(    0,15,  0, 2,  9, MC, "Intel Mobile Celeron (Northwood D1), .13um");
   FMSQ(    0,15,  0, 2,  9, MM, "Intel Mobile Pentium 4 Processor-M (Northwood D1), .13um");
   FMSQ(    0,15,  0, 2,  9, MP, "Intel Mobile Pentium 4 (Northwood D1), .13um");
   FMSQ(    0,15,  0, 2,  9, dP, "Intel Pentium 4 (Northwood D1), .13um");
   FMS (    0,15,  0, 2,  9,     "Intel Pentium 4 (Northwood D1) / Xeon (Prestonia D1) / Mobile Pentium 4 (Northwood D1) / Mobile Pentium 4 Processor-M (Northwood D1) / Celeron 478-pin (Northwood D1), .13um");
   FMQ (    0,15,  0, 2,     dP, "Intel Pentium 4 (Northwood), .13um");
   FMQ (    0,15,  0, 2,     sM, "Intel Xeon MP (Gallatin), .13um");
   FMQ (    0,15,  0, 2,     sX, "Intel Xeon (Prestonia), .13um");
   FM  (    0,15,  0, 2,         "Intel Pentium 4 (Northwood) / Xeon (Prestonia) / Xeon MP (Gallatin) / Mobile Pentium 4 / Mobile Pentium 4 Processor-M (Northwood) / Celeron 478-pin (Northwood), .13um");
   FMSQ(    0,15,  0, 3,  3, dP, "Intel Pentium 4 (Prescott C0), 90nm");
   FMSQ(    0,15,  0, 3,  3, dC, "Intel Celeron D (Prescott C0), 90nm");
   FMS (    0,15,  0, 3,  3,     "Intel Pentium 4 (Prescott C0) / Celeron D (Prescott C0), 90nm");
   FMSQ(    0,15,  0, 3,  4, sX, "Intel Xeon (Nocona D0), 90nm");
   FMSQ(    0,15,  0, 3,  4, dC, "Intel Celeron D (Prescott D0), 90nm");
   FMSQ(    0,15,  0, 3,  4, MP, "Intel Mobile Pentium 4 (Prescott D0), 90nm");
   FMSQ(    0,15,  0, 3,  4, dP, "Intel Pentium 4 (Prescott D0), 90nm");
   FMS (    0,15,  0, 3,  4,     "Intel Pentium 4 (Prescott D0) / Xeon (Nocona D0) / Mobile Pentium 4 (Prescott D0), 90nm");
   FMQ (    0,15,  0, 3,     sX, "Intel Xeon (Nocona), 90nm");
   FMQ(     0,15,  0, 3,     dC, "Intel Celeron D (Prescott), 90nm");
   FMQ (    0,15,  0, 3,     MP, "Intel Mobile Pentium 4 (Prescott), 90nm");
   FMQ (    0,15,  0, 3,     dP, "Intel Pentium 4 (Prescott), 90nm");
   FM  (    0,15,  0, 3,         "Intel Pentium 4 (Prescott) / Xeon (Nocona) / Mobile Pentium 4 (Prescott), 90nm");
   FMSQ(    0,15,  0, 4,  1, sP, "Intel Xeon MP (Potomac C0), 90nm");
   FMSQ(    0,15,  0, 4,  1, sM, "Intel Xeon MP (Cranford A0), 90nm");
   FMSQ(    0,15,  0, 4,  1, sX, "Intel Xeon (Nocona E0), 90nm");
   FMSQ(    0,15,  0, 4,  1, dC, "Intel Celeron D (Prescott E0), 90nm");
   FMSQ(    0,15,  0, 4,  1, MP, "Intel Mobile Pentium 4 (Prescott E0), 90nm");
   FMSQ(    0,15,  0, 4,  1, dP, "Intel Pentium 4 (Prescott E0), 90nm");
   FMS (    0,15,  0, 4,  1,     "Intel Pentium 4 (Prescott E0) / Xeon (Nocona E0) / Xeon MP (Cranford A0 / Potomac C0) / Celeron D (Prescott E0 ) / Mobile Pentium 4 (Prescott E0), 90nm");
   FMSQ(    0,15,  0, 4,  3, sI, "Intel Xeon (Irwindale N0), 90nm");
   FMSQ(    0,15,  0, 4,  3, sX, "Intel Xeon (Nocona N0), 90nm");
   FMSQ(    0,15,  0, 4,  3, dP, "Intel Pentium 4 (Prescott N0), 90nm");
   FMS (    0,15,  0, 4,  3,     "Intel Pentium 4 (Prescott N0) / Xeon (Nocona N0 / Irwindale N0), 90nm");
   FMSQ(    0,15,  0, 4,  4, dc, "Intel Pentium Extreme Edition Processor 840 (Smithfield A0), 90nm");
   FMSQ(    0,15,  0, 4,  4, dd, "Intel Pentium D Processor 8x0 (Smithfield A0), 90nm");
   FMS (    0,15,  0, 4,  4,     "Intel Pentium D Processor 8x0 (Smithfield A0) / Pentium Extreme Edition Processor 840 (Smithfield A0), 90nm");
   FMSQ(    0,15,  0, 4,  7, dc, "Pentium Extreme Edition Processor 840 (Smithfield B0), 90nm");
   FMSQ(    0,15,  0, 4,  7, dd, "Intel Pentium D Processor 8x0 (Smithfield B0), 90nm");
   FMS (    0,15,  0, 4,  7,     "Intel Pentium D Processor 8x0 (Smithfield B0) / Pentium Extreme Edition Processor 840 (Smithfield B0), 90nm");
   FMSQ(    0,15,  0, 4,  8, s7, "Intel Dual-Core Xeon Processor 7000 (Paxville A0), 90nm");
   FMSQ(    0,15,  0, 4,  8, sX, "Intel Dual-Core Xeon (Paxville A0), 90nm");
   FMS (    0,15,  0, 4,  8,     "Intel Dual-Core Xeon (Paxville A0) / Dual-Core Xeon Processor 7000 (Paxville A0), 90nm");
   FMSQ(    0,15,  0, 4,  9, sM, "Intel Xeon MP (Cranford B0), 90nm");
   FMSQ(    0,15,  0, 4,  9, dC, "Intel Celeron D (Prescott G1), 90nm");
   FMSQ(    0,15,  0, 4,  9, dP, "Intel Pentium 4 (Prescott G1), 90nm");
   FMS (    0,15,  0, 4,  9,     "Intel Pentium 4 (Prescott G1) / Xeon MP (Cranford B0) / Celeron D (Prescott G1), 90nm");
   FMSQ(    0,15,  0, 4, 10, sI, "Intel Xeon (Irwindale R0), 90nm");
   FMSQ(    0,15,  0, 4, 10, sX, "Intel Xeon (Nocona R0), 90nm");
   FMSQ(    0,15,  0, 4, 10, dP, "Intel Pentium 4 (Prescott R0), 90nm");
   FMS (    0,15,  0, 4, 10,     "Intel Pentium 4 (Prescott R0) / Xeon (Nocona R0 / Irwindale R0), 90nm");
   FMQ (    0,15,  0, 4,     sM, "Intel Xeon MP (Nocona/Potomac), 90nm");
   FMQ (    0,15,  0, 4,     sX, "Intel Xeon (Nocona/Irwindale), 90nm");
   FMQ (    0,15,  0, 4,     dC, "Intel Celeron D (Prescott), 90nm");
   FMQ (    0,15,  0, 4,     MP, "Intel Mobile Pentium 4 (Prescott), 90nm");
   FMQ (    0,15,  0, 4,     dd, "Intel Pentium D (Smithfield A0), 90nm");
   FMQ (    0,15,  0, 4,     dP, "Intel Pentium 4 (Prescott) / Pentium Extreme Edition (Smithfield A0), 90nm");
   FM  (    0,15,  0, 4,         "Intel Pentium 4 (Prescott) / Xeon (Nocona / Irwindale) / Pentium D (Smithfield A0) / Pentium Extreme Edition (Smithfield A0) / Mobile Pentium 4 (Prescott) / Xeon MP (Nocona) / Xeon MP (Cranford / Potomac) / Celeron D (Prescott) / Dual-Core Xeon (Paxville A0) / Dual-Core Xeon Processor 7000 (Paxville A0), 90nm");
   FMSQ(    0,15,  0, 6,  2, dd, "Intel Pentium D Processor 9xx (Presler B1), 65nm");
   FMSQ(    0,15,  0, 6,  2, dP, "Intel Pentium 4 Processor 6x1 (Cedar Mill B1) / Pentium Extreme Edition Processor 955 (Presler B1)");
   FMS (    0,15,  0, 6,  2,     "Intel Pentium 4 Processor 6x1 (Cedar Mill B1) / Pentium Extreme Edition Processor 955 (Presler B1) / Pentium D Processor 900 (Presler B1), 65nm");
   FMSQ(    0,15,  0, 6,  4, dd, "Intel Pentium D Processor 9xx (Presler C1), 65nm");
   FMSQ(    0,15,  0, 6,  4, dP, "Intel Pentium 4 Processor 6x1 (Cedar Mill C1) / Pentium Extreme Edition Processor 955 (Presler C1)");
   FMSQ(    0,15,  0, 6,  4, dC, "Intel Celeron D Processor 34x/35x (Cedar Mill C1), 65nm");
   FMSQ(    0,15,  0, 6,  4, sX, "Intel Xeon Processor 5000 (Dempsey C1), 65nm");
   FMS (    0,15,  0, 6,  4,     "Intel Pentium 4 Processor 6x1 (Cedar Mill C1) / Pentium Extreme Edition Processor 955 (Presler C1) / Pentium D Processor 9xx (Presler C1) / Xeon Processor 5000 (Dempsey C1) / Celeron D Processor 3xx (Cedar Mill C1), 65nm");
   FMSQ(    0,15,  0, 6,  5, dC, "Intel Celeron D Processor 36x (Cedar Mill D0), 65nm");
   FMSQ(    0,15,  0, 6,  5, dd, "Intel Pentium D Processor 9xx (Presler D0), 65nm");
   FMSQ(    0,15,  0, 6,  5, dP, "Intel Pentium 4 Processor 6x1 (Cedar Mill D0) / Pentium Extreme Edition Processor 955 (Presler D0), 65nm");
   FMS (    0,15,  0, 6,  5,     "Intel Pentium 4 Processor 6x1 (Cedar Mill D0) / Pentium D Processor 9xx (Presler D0) / Pentium Extreme Edition Processor 955 (Presler D0) / Celeron D Processor 36x (Cedar Mill D0), 65nm");
   FMS (    0,15,  0, 6,  8,     "Intel Xeon Processor 71x0 (Tulsa B0), 65nm");
   FMQ (    0,15,  0, 6,     dd, "Intel Pentium D (Presler), 65nm");
   FMQ (    0,15,  0, 6,     dP, "Intel Pentium 4 (Cedar Mill) / Pentium Extreme Edition (Presler)");
   FMQ (    0,15,  0, 6,     dC, "Intel Celeron D (Cedar Mill), 65nm");
   FMQ (    0,15,  0, 6,     sX, "Intel Xeon (Dempsey / Tulsa), 65nm");
   FM  (    0,15,  0, 6,         "Intel Pentium 4 (Cedar Mill) / Pentium Extreme Edition (Presler) / Pentium D (Presler) / Xeon (Dempsey) / Xeon (Tulsa) / Celeron D (Cedar Mill), 65nm");
   FQ  (    0,15,            sM, "Intel Xeon MP (unknown model)");
   FQ  (    0,15,            sX, "Intel Xeon (unknown model)");
   FQ  (    0,15,            MC, "Intel Mobile Celeron (unknown model)");
   FQ  (    0,15,            MC, "Intel Mobile Pentium 4 (unknown model)");
   FQ  (    0,15,            MM, "Intel Mobile Pentium 4 Processor-M (unknown model)");
   FQ  (    0,15,            dC, "Intel Celeron (unknown model)");
   FQ  (    0,15,            dd, "Intel Pentium D (unknown model)");
   FQ  (    0,15,            dP, "Intel Pentium 4 (unknown model)");
   FQ  (    0,15,            dc, "Intel Pentium (unknown model)");
   F   (    0,15,                "Intel Pentium 4 / Pentium D / Xeon / Xeon MP / Celeron / Celeron D (unknown model)");
   FMS (    1,15,  0, 0,  7,     "Intel Itanium2 (McKinley B3), .18um");
   FM  (    1,15,  0, 0,         "Intel Itanium2 (McKinley), .18um");
   FMS (    1,15,  0, 1,  5,     "Intel Itanium2 (Madison/Deerfield/Hondo B1), .13um");
   FM  (    1,15,  0, 1,         "Intel Itanium2 (Madison/Deerfield/Hondo), .13um");
   FMS (    1,15,  0, 2,  1,     "Intel Itanium2 (Madison 9M/Fanwood A1), .13um");
   FMS (    1,15,  0, 2,  2,     "Intel Itanium2 (Madison 9M/Fanwood A2), .13um");
   FM  (    1,15,  0, 2,         "Intel Itanium2 (Madison), .13um");
   F   (    1,15,                "Intel Itanium2 (unknown model)");
   FMS (    2,15,  0, 0,  5,     "Intel Itanium2 Dual-Core Processor 9000 (Montecito/Millington C1), 90nm");
   FMS (    2,15,  0, 0,  7,     "Intel Itanium2 Dual-Core Processor 9000 (Montecito/Millington C2), 90nm");
   FM  (    2,15,  0, 0,         "Intel Itanium2 Dual-Core Processor 9000 (Montecito/Millington), 90nm");
   FMS (    2,15,  0, 1,  1,     "Intel Itanium2 Dual-Core Processor 9100 (Montvale A1), 90nm");
   FM  (    2,15,  0, 1,         "Intel Itanium2 Dual-Core Processor 9100 (Montvale), 90nm");
   FMS (    2,15,  0, 2,  4,     "Intel Itanium2 Dual-Core Processor 9300 (Tukwila E0), 90nm");
   FM  (    2,15,  0, 2,         "Intel Itanium2 Dual-Core Processor 9300 (Tukwila), 90nm");
   F   (    2,15,                "Intel Itanium2 (unknown model)");
   DEFAULT                      ("unknown");
   printf("\n");
}

static void
print_synth_amd(const char*          name,
                unsigned int         val,
                const code_stash_t*  stash)
{
   printf("%s", name);
   START;
   FM  (0, 4,  0, 3,         "AMD 80486DX2");
   FM  (0, 4,  0, 7,         "AMD 80486DX2WB");
   FM  (0, 4,  0, 8,         "AMD 80486DX4");
   FM  (0, 4,  0, 9,         "AMD 80486DX4WB");
   FM  (0, 4,  0,14,         "AMD 5x86");
   FM  (0, 4,  0,15,         "AMD 5xWB");
   F   (0, 4,                "AMD 80486 / 5x (unknown model)");
   FM  (0, 5,  0, 0,         "AMD SSA5 (PR75, PR90, PR100)");
   FM  (0, 5,  0, 1,         "AMD 5k86 (PR120, PR133)");
   FM  (0, 5,  0, 2,         "AMD 5k86 (PR166)");
   FM  (0, 5,  0, 3,         "AMD 5k86 (PR200)");
   FM  (0, 5,  0, 5,         "AMD Geode GX");
   FM  (0, 5,  0, 6,         "AMD K6, .30um");
   FM  (0, 5,  0, 7,         "AMD K6 (Little Foot), .25um");
   FMS (0, 5,  0, 8,  0,     "AMD K6-2 (Chomper A)");
   FMS (0, 5,  0, 8, 12,     "AMD K6-2 (Chomper A)");
   FM  (0, 5,  0, 8,         "AMD K6-2 (Chomper)");
   FMS (0, 5,  0, 9,  1,     "AMD K6-III (Sharptooth B)");
   FM  (0, 5,  0, 9,         "AMD K6-III (Sharptooth)");
   FM  (0, 5,  0,10,         "AMD Geode LX");
   FM  (0, 5,  0,13,         "AMD K6-2+, K6-III+");
   F   (0, 5,                "AMD 5k86 / K6 / Geode (unknown model)");
   FM  (0, 6,  0, 1,         "AMD Athlon (Argon), .25um");
   FM  (0, 6,  0, 2,         "AMD Athlon (K75 / Pluto / Orion), .18um");
   FMS (0, 6,  0, 3,  0,     "AMD Duron / mobile Duron (Spitfire A0)");
   FMS (0, 6,  0, 3,  1,     "AMD Duron / mobile Duron (Spitfire A2)");
   FM  (0, 6,  0, 3,         "AMD Duron / mobile Duron (Spitfire)");
   FMS (0, 6,  0, 4,  2,     "AMD Athlon (Thunderbird A4-A7)");
   FMS (0, 6,  0, 4,  4,     "AMD Athlon (Thunderbird A9)");
   FM  (0, 6,  0, 4,         "AMD Athlon (Thunderbird)");
   FMSQ(0, 6,  0, 6,  0, sA, "AMD Athlon MP (Palomino A0)");
   FMSQ(0, 6,  0, 6,  0, dA, "AMD Athlon (Palomino A0)");
   FMSQ(0, 6,  0, 6,  0, MA, "AMD mobile Athlon 4 (Palomino A0)");
   FMSQ(0, 6,  0, 6,  0, sD, "AMD Duron MP (Palomino A0)");
   FMSQ(0, 6,  0, 6,  0, MD, "AMD mobile Duron (Palomino A0)");
   FMS (0, 6,  0, 6,  0,     "AMD Athlon / Athlon MP mobile Athlon 4 / mobile Duron (Palomino A0)");
   FMSQ(0, 6,  0, 6,  1, sA, "AMD Athlon MP (Palomino A2)");
   FMSQ(0, 6,  0, 6,  1, dA, "AMD Athlon (Palomino A2)");
   FMSQ(0, 6,  0, 6,  1, MA, "AMD mobile Athlon 4 (Palomino A2)");
   FMSQ(0, 6,  0, 6,  1, sD, "AMD Duron MP (Palomino A2)");
   FMSQ(0, 6,  0, 6,  1, MD, "AMD mobile Duron (Palomino A2)");
   FMSQ(0, 6,  0, 6,  1, dD, "AMD Duron (Palomino A2)");
   FMS (0, 6,  0, 6,  1,     "AMD Athlon / Athlon MP / Duron / mobile Athlon / mobile Duron (Palomino A2)");
   FMSQ(0, 6,  0, 6,  2, sA, "AMD Athlon MP (Palomino A5)");
   FMSQ(0, 6,  0, 6,  2, dX, "AMD Athlon XP (Palomino A5)");
   FMSQ(0, 6,  0, 6,  2, MA, "AMD mobile Athlon 4 (Palomino A5)");
   FMSQ(0, 6,  0, 6,  2, sD, "AMD Duron MP (Palomino A5)");
   FMSQ(0, 6,  0, 6,  2, MD, "AMD mobile Duron (Palomino A5)");
   FMSQ(0, 6,  0, 6,  2, dD, "AMD Duron (Palomino A5)");
   FMS (0, 6,  0, 6,  2,     "AMD Athlon MP / Athlon XP / Duron / Duron MP / mobile Athlon / mobile Duron (Palomino A5)");
   FMQ (0, 6,  0, 6,     MD, "AMD mobile Duron (Palomino)");
   FMQ (0, 6,  0, 6,     dD, "AMD Duron (Palomino)");
   FMQ (0, 6,  0, 6,     MA, "AMD mobile Athlon (Palomino)");
   FMQ (0, 6,  0, 6,     dX, "AMD Athlon XP (Palomino)");
   FMQ (0, 6,  0, 6,     dA, "AMD Athlon (Palomino)");
   FM  (0, 6,  0, 6,         "AMD Athlon / Athlon MP / Athlon XP / Duron / Duron MP / mobile Athlon / mobile Duron (Palomino)");
   FMSQ(0, 6,  0, 7,  0, sD, "AMD Duron MP (Morgan A0)");
   FMSQ(0, 6,  0, 7,  0, MD, "AMD mobile Duron (Morgan A0)");
   FMSQ(0, 6,  0, 7,  0, dD, "AMD Duron (Morgan A0)");
   FMS (0, 6,  0, 7,  0,     "AMD Duron / Duron MP / mobile Duron (Morgan A0)");
   FMSQ(0, 6,  0, 7,  1, sD, "AMD Duron MP (Morgan A1)");
   FMSQ(0, 6,  0, 7,  1, MD, "AMD mobile Duron (Morgan A1)");
   FMSQ(0, 6,  0, 7,  1, dD, "AMD Duron (Morgan A1)");
   FMS (0, 6,  0, 7,  1,     "AMD Duron / Duron MP / mobile Duron (Morgan A1)");
   FMQ (0, 6,  0, 7,     sD, "AMD Duron MP (Morgan)");
   FMQ (0, 6,  0, 7,     MD, "AMD mobile Duron (Morgan)");
   FMQ (0, 6,  0, 7,     dD, "AMD Duron (Morgan)");
   FM  (0, 6,  0, 7,         "AMD Duron / Duron MP / mobile Duron (Morgan)");
   FMSQ(0, 6,  0, 8,  0, dS, "AMD Sempron (Thoroughbred A0)");
   FMSQ(0, 6,  0, 8,  0, sD, "AMD Duron MP (Applebred A0)");
   FMSQ(0, 6,  0, 8,  0, dD, "AMD Duron (Applebred A0)");
   FMSQ(0, 6,  0, 8,  0, MX, "AMD mobile Athlon XP (Thoroughbred A0)");
   FMSQ(0, 6,  0, 8,  0, sA, "AMD Athlon MP (Thoroughbred A0)");
   FMSQ(0, 6,  0, 8,  0, dX, "AMD Athlon XP (Thoroughbred A0)");
   FMSQ(0, 6,  0, 8,  0, dA, "AMD Athlon (Thoroughbred A0)");
   FMS (0, 6,  0, 8,  0,     "AMD Athlon / Athlon XP / Athlon MP / Sempron / Duron / Duron MP (Thoroughbred A0)");
   FMSQ(0, 6,  0, 8,  1, MG, "AMD Geode NX (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, dS, "AMD Sempron (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, sD, "AMD Duron MP (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, dD, "AMD Duron (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, sA, "AMD Athlon MP (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, dX, "AMD Athlon XP (Thoroughbred B0)");
   FMSQ(0, 6,  0, 8,  1, dA, "AMD Athlon (Thoroughbred B0)");
   FMS (0, 6,  0, 8,  1,     "AMD Athlon / Athlon XP / Athlon MP / Sempron / Duron / Duron MP / Geode (Thoroughbred B0)");
   FMQ (0, 6,  0, 8,     MG, "AMD Geode NX (Thoroughbred)");
   FMQ (0, 6,  0, 8,     dS, "AMD Sempron (Thoroughbred)");
   FMQ (0, 6,  0, 8,     sD, "AMD Duron MP (Thoroughbred)");
   FMQ (0, 6,  0, 8,     dD, "AMD Duron (Thoroughbred)");
   FMQ (0, 6,  0, 8,     MX, "AMD mobile Athlon XP (Thoroughbred)");
   FMQ (0, 6,  0, 8,     sA, "AMD Athlon MP (Thoroughbred)");
   FMQ (0, 6,  0, 8,     dX, "AMD Athlon XP (Thoroughbred)");
   FMQ (0, 6,  0, 8,     dA, "AMD Athlon XP (Thoroughbred)");
   FM  (0, 6,  0, 8,         "AMD Athlon / Athlon XP / Athlon MP / Sempron / Duron / Duron MP / Geode (Thoroughbred)");
   FMSQ(0, 6,  0,10,  0, dS, "AMD Sempron (Barton A2)");
   FMSQ(0, 6,  0,10,  0, ML, "AMD mobile Athlon XP-M (LV) (Barton A2)");
   FMSQ(0, 6,  0,10,  0, MX, "AMD mobile Athlon XP-M (Barton A2)");
   FMSQ(0, 6,  0,10,  0, dt, "AMD Athlon XP (Thorton A2)");
   FMSQ(0, 6,  0,10,  0, sA, "AMD Athlon MP (Barton A2)");
   FMSQ(0, 6,  0,10,  0, dX, "AMD Athlon XP (Barton A2)");
   FMS (0, 6,  0,10,  0,     "AMD Athlon XP / Athlon MP / Sempron / mobile Athlon XP-M / mobile Athlon XP-M (LV) (Barton A2)");
   FMQ (0, 6,  0,10,     dS, "AMD Sempron (Barton)");
   FMQ (0, 6,  0,10,     ML, "AMD mobile Athlon XP-M (LV) (Barton)");
   FMQ (0, 6,  0,10,     MX, "AMD mobile Athlon XP-M (Barton)");
   FMQ (0, 6,  0,10,     sA, "AMD Athlon MP (Barton)");
   FMQ (0, 6,  0,10,     dX, "AMD Athlon XP (Barton)");
   FM  (0, 6,  0,10,         "AMD Athlon XP / Athlon MP / Sempron / mobile Athlon XP-M / mobile Athlon XP-M (LV) (Barton)");
   F   (0, 6,                "AMD Athlon / Athlon XP / Athlon MP / Duron / Duron MP / Sempron / mobile Athlon / mobile Athlon XP-M / mobile Athlon XP-M (LV) / mobile Duron (unknown model)");
   F   (0, 7,                "AMD Opteron (unknown model)");
   FMS (0,15,  0, 4,  0,     "AMD Athlon 64 (SledgeHammer SH7-B0), .13um");
   FMSQ(0,15,  0, 4,  8, MX, "AMD mobile Athlon XP-M (SledgeHammer SH7-C0), 754-pin, .13um");
   FMSQ(0,15,  0, 4,  8, MA, "AMD mobile Athlon 64 (SledgeHammer SH7-C0), 754-pin, .13um");
   FMSQ(0,15,  0, 4,  8, dA, "AMD Athlon 64 (SledgeHammer SH7-C0), 754-pin, .13um");
   FMS (0,15,  0, 4,  8,     "AMD Athlon 64 (SledgeHammer SH7-C0) / mobile Athlon 64 (SledgeHammer SH7-C0) / mobile Athlon XP-M (SledgeHammer SH7-C0), 754-pin, .13um");
   FMSQ(0,15,  0, 4, 10, MX, "AMD mobile Athlon XP-M (SledgeHammer SH7-CG), 940-pin, .13um");
   FMSQ(0,15,  0, 4, 10, MA, "AMD mobile Athlon 64 (SledgeHammer SH7-CG), 940-pin, .13um");
   FMSQ(0,15,  0, 4, 10, dA, "AMD Athlon 64 (SledgeHammer SH7-CG), 940-pin, .13um");
   FMS (0,15,  0, 4, 10,     "AMD Athlon 64 (SledgeHammer SH7-CG) / mobile Athlon 64 (SledgeHammer SH7-CG) / mobile Athlon XP-M (SledgeHammer SH7-CG), 940-pin, .13um");
   FMQ (0,15,  0, 4,     MX, "AMD mobile Athlon XP-M (SledgeHammer SH7), .13um");
   FMQ (0,15,  0, 4,     MA, "AMD mobile Athlon 64 (SledgeHammer SH7), .13um");
   FMQ (0,15,  0, 4,     dA, "AMD Athlon 64 (SledgeHammer SH7), .13um");
   FM  (0,15,  0, 4,         "AMD Athlon 64 (SledgeHammer SH7) / mobile Athlon 64 (SledgeHammer SH7) / mobile Athlon XP-M (SledgeHammer SH7), .13um");
   FMS (0,15,  0, 5,  0,     "AMD Opteron (DP SledgeHammer SH7-B0), 940-pin, .13um");
   FMS (0,15,  0, 5,  1,     "AMD Opteron (DP SledgeHammer SH7-B3), 940-pin, .13um");
   FMSQ(0,15,  0, 5,  8, dO, "AMD Opteron (DP SledgeHammer SH7-C0), 940-pin, .13um");
   FMSQ(0,15,  0, 5,  8, dF, "AMD Athlon 64 FX (DP SledgeHammer SH7-C0), 940-pin, .13um");
   FMS (0,15,  0, 5,  8,     "AMD Opteron (DP SledgeHammer SH7-C0) / Athlon 64 FX (DP SledgeHammer SH7-C0), 940-pin, .13um");
   FMSQ(0,15,  0, 5, 10, dO, "AMD Opteron (DP SledgeHammer SH7-CG), 940-pin, .13um");
   FMSQ(0,15,  0, 5, 10, dF, "AMD Athlon 64 FX (DP SledgeHammer SH7-CG), 940-pin, .13um");
   FMS (0,15,  0, 5, 10,     "AMD Opteron (DP SledgeHammer SH7-CG) / Athlon 64 FX (DP SledgeHammer SH7-CG), 940-pin, .13um");
   FMQ (0,15,  0, 5,     dO, "AMD Opteron (SledgeHammer SH7), 940-pin, .13um");
   FMQ (0,15,  0, 5,     dF, "AMD Athlon 64 FX (SledgeHammer SH7), 940-pin, .13um");
   FM  (0,15,  0, 5,         "AMD Opteron (SledgeHammer SH7) / Athlon 64 (SledgeHammer SH7) FX, 940-pin, .13um");
   FMSQ(0,15,  0, 7, 10, dF, "AMD Athlon 64 FX (DP SledgeHammer SH7-CG), 939-pin, .13um");
   FMSQ(0,15,  0, 7, 10, dA, "AMD Athlon 64 (DP SledgeHammer SH7-CG), 939-pin, .13um");
   FMS (0,15,  0, 7, 10,     "AMD Athlon 64 (DP SledgeHammer SH7-CG) / Athlon 64 FX (DP SledgeHammer SH7-CG), 939-pin, .13um");
   FMQ (0,15,  0, 7,     dF, "AMD Athlon 64 FX (DP SledgeHammer SH7), 939-pin, .13um");
   FMQ (0,15,  0, 7,     dA, "AMD Athlon 64 (DP SledgeHammer SH7), 939-pin, .13um");
   FM  (0,15,  0, 7,         "AMD Athlon 64 (DP SledgeHammer SH7) / Athlon 64 FX (DP SledgeHammer SH7), 939-pin, .13um");
   FMSQ(0,15,  0, 8,  2, MS, "AMD mobile Sempron (ClawHammer CH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0, 8,  2, MX, "AMD mobile Athlon XP-M (ClawHammer CH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0, 8,  2, MA, "AMD mobile Athlon 64 (Odessa CH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0, 8,  2, dA, "AMD Athlon 64 (ClawHammer CH7-CG), 754-pin, .13um");
   FMS (0,15,  0, 8,  2,     "AMD Athlon 64 (ClawHammer CH7-CG) / mobile Athlon 64 (Odessa CH7-CG) / mobile Sempron (ClawHammer CH7-CG) / mobile Athlon XP-M (ClawHammer CH7-CG), 754-pin, .13um");
   FMQ (0,15,  0, 8,     MS, "AMD mobile Sempron (Odessa CH7), 754-pin, .13um");
   FMQ (0,15,  0, 8,     MX, "AMD mobile Athlon XP-M (Odessa CH7), 754-pin, .13um");
   FMQ (0,15,  0, 8,     MA, "AMD mobile Athlon 64 (Odessa CH7), 754-pin, .13um");
   FMQ (0,15,  0, 8,     dA, "AMD Athlon 64 (ClawHammer CH7), 754-pin, .13um");
   FM  (0,15,  0, 8,         "AMD Athlon 64 (ClawHammer CH7) / mobile Athlon 64 (Odessa CH7) / mobile Sempron (Odessa CH7) / mobile Athlon XP-M (Odessa CH7), 754-pin, .13um");
   FMS (0,15,  0,11,  2,     "AMD Athlon 64 (ClawHammer CH7-CG), 939-pin, .13um");
   FM  (0,15,  0,11,         "AMD Athlon 64 (ClawHammer CH7), 939-pin, .13um");
   FMSQ(0,15,  0,12,  0, MS, "AMD mobile Sempron (Dublin DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,12,  0, dS, "AMD Sempron (Paris DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,12,  0, MX, "AMD mobile Athlon XP-M (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,12,  0, MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,12,  0, dA, "AMD Athlon 64 (NewCastle DH7-CG), 754-pin, .13um");
   FMS (0,15,  0,12,  0,     "AMD Athlon 64 (NewCastle DH7-CG) / Sempron (Paris DH7-CG) / mobile Athlon 64 (ClawHammer/Odessa DH7-CG) / mobile Sempron (Dublin DH7-CG) / mobile Athlon XP-M (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMQ (0,15,  0,12,     MS, "AMD mobile Sempron (Dublin DH7), 754-pin, .13um");
   FMQ (0,15,  0,12,     dS, "AMD Sempron (Paris DH7), 754-pin, .13um");
   FMQ (0,15,  0,12,     MX, "AMD mobile Athlon XP-M (NewCastle DH7), 754-pin, .13um");
   FMQ (0,15,  0,12,     MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7), 754-pin, .13um");
   FMQ (0,15,  0,12,     dA, "AMD Athlon 64 (NewCastle DH7), 754-pin, .13um");
   FM  (0,15,  0,12,         "AMD Athlon 64 (NewCastle DH7) / Sempron (Paris DH7) / mobile Athlon 64 (ClawHammer/Odessa DH7) / mobile Sempron (Dublin DH7) / mobile Athlon XP-M (ClawHammer/Odessa DH7), 754-pin, .13um");
   FMSQ(0,15,  0,14,  0, MS, "AMD mobile Sempron (Dublin DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,14,  0, dS, "AMD Sempron (Paris DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,14,  0, MX, "AMD mobile Athlon XP-M (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,14,  0, MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMSQ(0,15,  0,14,  0, dA, "AMD Athlon 64 (NewCastle DH7-CG), 754-pin, .13um");
   FMS (0,15,  0,14,  0,     "AMD Athlon 64 (NewCastle DH7-CG) / Sempron (Paris DH7-CG) / mobile Athlon 64 (ClawHammer/Odessa DH7-CG) / mobile Sempron (Dublin DH7-CG) / mobile Athlon XP-M (ClawHammer/Odessa DH7-CG), 754-pin, .13um");
   FMQ (0,15,  0,14,     dS, "AMD Sempron (Paris DH7), 754-pin, .13um");
   FMQ (0,15,  0,14,     MS, "AMD mobile Sempron (Dublin DH7), 754-pin, .13um");
   FMQ (0,15,  0,14,     MX, "AMD mobile Athlon XP-M (ClawHammer/Odessa DH7), 754-pin, .13um");
   FMQ (0,15,  0,14,     MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7), 754-pin, .13um");
   FMQ (0,15,  0,14,     dA, "AMD Athlon 64 (NewCastle DH7), 754-pin, .13um");
   FM  (0,15,  0,14,         "AMD Athlon 64 (NewCastle DH7) / Sempron (Paris DH7) / mobile Athlon 64 (ClawHammer/Odessa DH7) / mobile Sempron (Dublin DH7) / mobile Athlon XP-M (ClawHammer/Odessa DH7), 754-pin, .13um");
   FMSQ(0,15,  0,15,  0, dS, "AMD Sempron (Paris DH7-CG), 939-pin, .13um");
   FMSQ(0,15,  0,15,  0, MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7-CG), 939-pin, .13um");
   FMSQ(0,15,  0,15,  0, dA, "AMD Athlon 64 (NewCastle DH7-CG), 939-pin, .13um");
   FMS (0,15,  0,15,  0,     "AMD Athlon 64 (NewCastle DH7-CG) / Sempron (Paris DH7-CG) / mobile Athlon 64 (ClawHammer/Odessa DH7-CG), 939-pin, .13um");
   FMQ (0,15,  0,15,     dS, "AMD Sempron (Paris DH7), 939-pin, .13um");
   FMQ (0,15,  0,15,     MA, "AMD mobile Athlon 64 (ClawHammer/Odessa DH7), 939-pin, .13um");
   FMQ (0,15,  0,15,     dA, "AMD Athlon 64 (NewCastle DH7), 939-pin, .13um");
   FM  (0,15,  0,15,         "AMD Athlon 64 (NewCastle DH7) / Sempron (Paris DH7) / mobile Athlon 64 (ClawHammer/Odessa DH7), 939-pin, .13um");
   FMSQ(0,15,  1, 4,  0, MX, "AMD mobile Athlon XP-M (Oakville SH7-D0), 754-pin, 90nm");
   FMSQ(0,15,  1, 4,  0, MA, "AMD mobile Athlon 64 (Oakville SH7-D0), 754-pin, 90nm");
   FMSQ(0,15,  1, 4,  0, dA, "AMD Athlon 64 (Winchester SH7-D0), 754-pin, 90nm");
   FMS (0,15,  1, 4,  0,     "AMD Athlon 64 (Winchester SH7-D0) / mobile Athlon 64 (Oakville SH7-D0) / mobile Athlon XP-M (Oakville SH7-D0), 754-pin, 90nm");
   FMQ (0,15,  1, 4,     MX, "AMD mobile Athlon XP-M (Winchester SH7), 754-pin, 90nm");
   FMQ (0,15,  1, 4,     MA, "AMD mobile Athlon 64 (Winchester SH7), 754-pin, 90nm");
   FMQ (0,15,  1, 4,     dA, "AMD Athlon 64 (Winchester SH7), 754-pin, 90nm");
   FM  (0,15,  1, 4,         "AMD Athlon 64 (Winchester SH7) / mobile Athlon 64 (Winchester SH7) / mobile Athlon XP-M (Winchester SH7), 754-pin, 90nm");
   FMSQ(0,15,  1, 5,  0, dO, "AMD Opteron (Winchester SH7-D0), 940-pin, 90nm");
   FMSQ(0,15,  1, 5,  0, dF, "AMD Athlon 64 FX (Winchester SH7-D0), 940-pin, 90nm");
   FMS (0,15,  1, 5,  0,     "AMD Opteron (Winchester SH7-D0) / Athlon 64 FX (Winchester SH7-D0), 940-pin, 90nm");
   FMQ (0,15,  1, 5,     dO, "AMD Opteron (Winchester SH7), 940-pin, 90nm");
   FMQ (0,15,  1, 5,     dF, "AMD Athlon 64 FX (Winchester SH7), 940-pin, 90nm");
   FM  (0,15,  1, 5,         "AMD Opteron (Winchester SH7) / Athlon 64 FX (Winchester SH7), 940-pin, 90nm");
   FMSQ(0,15,  1, 7,  0, dF, "AMD Athlon 64 FX (Winchester SH7-D0), 939-pin, 90nm");
   FMSQ(0,15,  1, 7,  0, dA, "AMD Athlon 64 (Winchester SH7-D0), 939-pin, 90nm");
   FMS (0,15,  1, 7,  0,     "AMD Athlon 64 (Winchester SH7-D0) / Athlon 64 FX (Winchester SH7-D0), 939-pin, 90nm");
   FMQ (0,15,  1, 7,     dF, "AMD Athlon 64 FX (Winchester SH7), 939-pin, 90nm");
   FMQ (0,15,  1, 7,     dA, "AMD Athlon 64 (Winchester SH7), 939-pin, 90nm");
   FM  (0,15,  1, 7,         "AMD Athlon 64 (Winchester SH7) / Athlon 64 FX (Winchester SH7), 939-pin, 90nm");
   FMSQ(0,15,  1, 8,  0, MS, "AMD mobile Sempron (Georgetown/Sonora CH-D0), 754-pin, 90nm");
   FMSQ(0,15,  1, 8,  0, MX, "AMD mobile Athlon XP-M (Oakville CH-D0), 754-pin, 90nm");
   FMSQ(0,15,  1, 8,  0, MA, "AMD mobile Athlon 64 (Oakville CH-D0), 754-pin, 90nm");
   FMSQ(0,15,  1, 8,  0, dA, "AMD Athlon 64 (Winchester CH-D0), 754-pin, 90nm");
   FMS (0,15,  1, 8,  0,     "AMD Athlon 64 (Winchester CH-D0) / mobile Athlon 64 (Oakville CH-D0) / mobile Sempron (Georgetown/Sonora CH-D0) / mobile Athlon XP-M (Oakville CH-D0), 754-pin, 90nm");
   FMQ (0,15,  1, 8,     MS, "AMD mobile Sempron (Georgetown/Sonora CH), 754-pin, 90nm");
   FMQ (0,15,  1, 8,     MX, "AMD mobile Athlon XP-M (Winchester CH), 754-pin, 90nm");
   FMQ (0,15,  1, 8,     MA, "AMD mobile Athlon 64 (Winchester CH), 754-pin, 90nm");
   FMQ (0,15,  1, 8,     dA, "AMD Athlon 64 (Winchester CH), 754-pin, 90nm");
   FM  (0,15,  1, 8,         "AMD Athlon 64 (Winchester CH) / mobile Athlon 64 (Winchester CH) / mobile Sempron (Georgetown/Sonora CH) / mobile Athlon XP-M (Winchester CH), 754-pin, 90nm");
   FMS (0,15,  1,11,  0,     "AMD Athlon 64 (Winchester CH-D0), 939-pin, 90nm");
   FM  (0,15,  1,11,         "AMD Athlon 64 (Winchester CH), 939-pin, 90nm");
   FMSQ(0,15,  1,12,  0, MS, "AMD mobile Sempron (Georgetown/Sonora DH8-D0), 754-pin, 90nm");
   FMSQ(0,15,  1,12,  0, dS, "AMD Sempron (Palermo DH8-D0), 754-pin, 90nm");
   FMSQ(0,15,  1,12,  0, MX, "AMD Athlon XP-M (Winchester DH8-D0), 754-pin, 90nm");
   FMSQ(0,15,  1,12,  0, MA, "AMD mobile Athlon 64 (Oakville DH8-D0), 754-pin, 90nm");
   FMSQ(0,15,  1,12,  0, dA, "AMD Athlon 64 (Winchester DH8-D0), 754-pin, 90nm");
   FMS (0,15,  1,12,  0,     "AMD Athlon 64 (Winchester DH8-D0) / Sempron (Palermo DH8-D0) / mobile Athlon 64 (Oakville DH8-D0) / mobile Sempron (Georgetown/Sonora DH8-D0) / mobile Athlon XP-M (Winchester DH8-D0), 754-pin, 90nm");
   FMQ (0,15,  1,12,     MS, "AMD mobile Sempron (Georgetown/Sonora DH8), 754-pin, 90nm");
   FMQ (0,15,  1,12,     dS, "AMD Sempron (Palermo DH8), 754-pin, 90nm");
   FMQ (0,15,  1,12,     MX, "AMD Athlon XP-M (Winchester DH8), 754-pin, 90nm");
   FMQ (0,15,  1,12,     MA, "AMD mobile Athlon 64 (Winchester DH8), 754-pin, 90nm");
   FMQ (0,15,  1,12,     dA, "AMD Athlon 64 (Winchester DH8), 754-pin, 90nm");
   FM  (0,15,  1,12,         "AMD Athlon 64 (Winchester DH8) / Sempron (Palermo DH8) / mobile Athlon 64 (Winchester DH8) / mobile Sempron (Georgetown/Sonora DH8) / mobile Athlon XP-M (Winchester DH8), 754-pin, 90nm");
   FMSQ(0,15,  1,15,  0, dS, "AMD Sempron (Palermo DH8-D0), 939-pin, 90nm");
   FMSQ(0,15,  1,15,  0, dA, "AMD Athlon 64 (Winchester DH8-D0), 939-pin, 90nm");
   FMS (0,15,  1,15,  0,     "AMD Athlon 64 (Winchester DH8-D0) / Sempron (Palermo DH8-D0), 939-pin, 90nm");
   FMQ (0,15,  1,15,     dS, "AMD Sempron (Palermo DH8), 939-pin, 90nm");
   FMQ (0,15,  1,15,     dA, "AMD Athlon 64 (Winchester DH8), 939-pin, 90nm");
   FM  (0,15,  1,15,         "AMD Athlon 64 (Winchester DH8) / Sempron (Palermo DH8), 939-pin, 90nm");
   FMSQ(0,15,  2, 1,  0, d8, "AMD Dual Core Opteron (Egypt JH-E1), 940-pin, 90nm");
   FMSQ(0,15,  2, 1,  0, dO, "AMD Dual Core Opteron (Italy JH-E1), 940-pin, 90nm");
   FMS (0,15,  2, 1,  0,     "AMD Dual Core Opteron (Italy/Egypt JH-E1), 940-pin, 90nm");
   FMSQ(0,15,  2, 1,  2, d8, "AMD Dual Core Opteron (Egypt JH-E6), 940-pin, 90nm");
   FMSQ(0,15,  2, 1,  2, dO, "AMD Dual Core Opteron (Italy JH-E6), 940-pin, 90nm");
   FMS (0,15,  2, 1,  2,     "AMD Dual Core Opteron (Italy/Egypt JH-E6), 940-pin, 90nm");
   FMQ (0,15,  2, 1,     d8, "AMD Dual Core Opteron (Egypt JH), 940-pin, 90nm");
   FMQ (0,15,  2, 1,     dO, "AMD Dual Core Opteron (Italy JH), 940-pin, 90nm");
   FM  (0,15,  2, 1,         "AMD Dual Core Opteron (Italy/Egypt JH), 940-pin, 90nm");
   FMSQ(0,15,  2, 3,  2, DO, "AMD Dual Core Opteron (Denmark JH-E6), 939-pin, 90nm");
   FMSQ(0,15,  2, 3,  2, dF, "AMD Athlon 64 FX (Toledo JH-E6), 939-pin, 90nm");
   FMSQ(0,15,  2, 3,  2, dm, "AMD Athlon 64 X2 (Manchester JH-E6), 939-pin, 90nm");
   FMSQ(0,15,  2, 3,  2, dA, "AMD Athlon 64 X2 (Toledo JH-E6), 939-pin, 90nm");
   FMS (0,15,  2, 3,  2,     "AMD Dual Core Opteron (Denmark JH-E6) / Athlon 64 X2 (Toledo JH-E6) / Athlon 64 FX (Toledo JH-E6), 939-pin, 90nm");
   FMQ (0,15,  2, 3,     dO, "AMD Dual Core Opteron (Denmark JH), 939-pin, 90nm");
   FMQ (0,15,  2, 3,     dF, "AMD Athlon 64 FX (Toledo JH), 939-pin, 90nm");
   FMQ (0,15,  2, 3,     dm, "AMD Athlon 64 X2 (Manchester JH), 939-pin, 90nm");
   FMQ (0,15,  2, 3,     dA, "AMD Athlon 64 X2 (Toledo JH), 939-pin, 90nm");
   FM  (0,15,  2, 3,         "AMD Dual Core Opteron (Denmark JH) / Athlon 64 X2 (Toledo JH / Manchester JH) / Athlon 64 FX (Toledo JH), 939-pin, 90nm");
   FMSQ(0,15,  2, 4,  2, MA, "AMD mobile Athlon 64 (Newark SH-E5), 754-pin, 90nm");
   FMSQ(0,15,  2, 4,  2, MT, "AMD mobile Turion (Lancaster/Richmond SH-E5), 754-pin, 90nm");
   FMS (0,15,  2, 4,  2,     "AMD mobile Athlon 64 (Newark SH-E5) / mobile Turion (Lancaster/Richmond SH-E5), 754-pin, 90nm");
   FMQ (0,15,  2, 4,     MA, "AMD mobile Athlon 64 (Newark SH), 754-pin, 90nm");
   FMQ (0,15,  2, 4,     MT, "AMD mobile Turion (Lancaster/Richmond SH), 754-pin, 90nm");
   FM  (0,15,  2, 4,         "AMD mobile Athlon 64 (Newark SH) / mobile Turion (Lancaster/Richmond SH), 754-pin, 90nm");
   FMQ (0,15,  2, 5,     d8, "AMD Opteron (Athens SH-E4), 940-pin, 90nm");
   FMQ (0,15,  2, 5,     dO, "AMD Opteron (Troy SH-E4), 940-pin, 90nm");
   FM  (0,15,  2, 5,         "AMD Opteron (Troy/Athens SH-E4), 940-pin, 90nm");
   FMSQ(0,15,  2, 7,  1, dO, "AMD Opteron (Venus SH-E4), 939-pin, 90nm");
   FMSQ(0,15,  2, 7,  1, dF, "AMD Athlon 64 FX (San Diego SH-E4), 939-pin, 90nm");
   FMSQ(0,15,  2, 7,  1, dA, "AMD Athlon 64 (San Diego SH-E4), 939-pin, 90nm");
   FMS (0,15,  2, 7,  1,     "AMD Opteron (Venus SH-E4) / Athlon 64 (San Diego SH-E4) / Athlon 64 FX (San Diego SH-E4), 939-pin, 90nm");
   FMQ (0,15,  2, 7,     dO, "AMD Opteron (San Diego SH), 939-pin, 90nm");
   FMQ (0,15,  2, 7,     dF, "AMD Athlon 64 FX (San Diego SH), 939-pin, 90nm");
   FMQ (0,15,  2, 7,     dA, "AMD Athlon 64 (San Diego SH), 939-pin, 90nm");
   FM  (0,15,  2, 7,         "AMD Opteron (San Diego SH) / Athlon 64 (San Diego SH) / Athlon 64 FX (San Diego SH), 939-pin, 90nm");
   FM  (0,15,  2,11,         "AMD Athlon 64 X2 (Manchester BH-E4), 939-pin, 90nm");
   FMS (0,15,  2,12,  0,     "AMD Sempron (Palermo DH-E3), 754-pin, 90nm");
   FMSQ(0,15,  2,12,  2, MS, "AMD mobile Sempron (Albany/Roma DH-E6), 754-pin, 90nm");
   FMSQ(0,15,  2,12,  2, dS, "AMD Sempron (Palermo DH-E6), 754-pin, 90nm");
   FMSQ(0,15,  2,12,  2, dA, "AMD Athlon 64 (Venice DH-E6), 754-pin, 90nm");
   FMS (0,15,  2,12,  2,     "AMD Athlon 64 (Venice DH-E6) / Sempron (Palermo DH-E6) / mobile Sempron (Albany/Roma DH-E6), 754-pin, 90nm");
   FMQ (0,15,  2,12,     MS, "AMD mobile Sempron (Albany/Roma DH), 754-pin, 90nm");
   FMQ (0,15,  2,12,     dS, "AMD Sempron (Palermo DH), 754-pin, 90nm");
   FMQ (0,15,  2,12,     dA, "AMD Athlon 64 (Venice DH), 754-pin, 90nm");
   FM  (0,15,  2,12,         "AMD Athlon 64 (Venice DH) / Sempron (Palermo DH) / mobile Sempron (Albany/Roma DH), 754-pin, 90nm");
   FMSQ(0,15,  2,15,  0, dS, "AMD Sempron (Palermo DH-E3), 939-pin, 90nm");
   FMSQ(0,15,  2,15,  0, dA, "AMD Athlon 64 (Venice DH-E3), 939-pin, 90nm");
   FMS (0,15,  2,15,  0,     "AMD Athlon 64 (Venice DH-E3) / Sempron (Palermo DH-E3), 939-pin, 90nm");
   FMSQ(0,15,  2,15,  2, dS, "AMD Sempron (Palermo DH-E6), 939-pin, 90nm");
   FMSQ(0,15,  2,15,  2, dA, "AMD Athlon 64 (Venice DH-E6), 939-pin, 90nm");
   FMS (0,15,  2,15,  2,     "AMD Athlon 64 (Venice DH-E6) / Sempron (Palermo DH-E6), 939-pin, 90nm");
   FMQ (0,15,  2,15,     dS, "AMD Sempron (Palermo DH), 939-pin, 90nm");
   FMQ (0,15,  2,15,     dA, "AMD Athlon 64 (Venice DH), 939-pin, 90nm");
   FM  (0,15,  2,15,         "AMD Athlon 64 (Venice DH) / Sempron (Palermo DH), 939-pin, 90nm");
   FMS (0,15,  4, 1,  2,     "AMD Dual-Core Opteron (Santa Rosa JH-F2), 90nm");
   FMS (0,15,  4, 1,  3,     "AMD Dual-Core Opteron (Santa Rosa JH-F3), 90nm");
   FM  (0,15,  4, 1,         "AMD Dual-Core Opteron (Santa Rosa), 90nm");
   FMSQ(0,15,  4, 3,  2, DO, "AMD Dual-Core Opteron (Santa Rosa JH-F2), 90nm");
   FMSQ(0,15,  4, 3,  2, dO, "AMD Opteron (Santa Rosa JH-F2), 90nm");
   FMSQ(0,15,  4, 3,  2, dF, "AMD Athlon 64 FX Dual-Core (Windsor JH-F2), 90nm");
   FMSQ(0,15,  4, 3,  2, dA, "AMD Athlon 64 X2 Dual-Core (Windsor JH-F2), 90nm");
   FMS (0,15,  4, 3,  2,     "AMD Opteron (Santa Rosa JH-F2) / Athlon 64 X2 Dual-Core (Windsor JH-F2) / Athlon 64 FX Dual-Core (Windsor JH-F2), 90nm");
   FMSQ(0,15,  4, 3,  3, DO, "AMD Dual-Core Opteron (Santa Rosa JH-F3), 90nm");
   FMSQ(0,15,  4, 3,  3, dO, "AMD Opteron (Santa Rosa JH-F3), 90nm");
   FMSQ(0,15,  4, 3,  3, dF, "AMD Athlon 64 FX Dual-Core (Windsor JH-F3), 90nm");
   FMSQ(0,15,  4, 3,  3, dA, "AMD Athlon 64 X2 Dual-Core (Windsor JH-F3), 90nm");
   FMS (0,15,  4, 3,  3,     "AMD Opteron (Santa Rosa JH-F3) / Athlon 64 X2 Dual-Core (Windsor JH-F3) / Athlon 64 FX Dual-Core (Windsor JH-F3), 90nm");
   FMQ (0,15,  4, 3,     DO, "AMD Dual-Core Opteron (Santa Rosa), 90nm");
   FMQ (0,15,  4, 3,     dO, "AMD Opteron (Santa Rosa), 90nm");
   FMQ (0,15,  4, 3,     dF, "AMD Athlon 64 FX Dual-Core (Windsor), 90nm");
   FMQ (0,15,  4, 3,     dA, "AMD Athlon 64 X2 Dual-Core (Windsor), 90nm");
   FM  (0,15,  4, 3,         "AMD Opteron (Santa Rosa) / Athlon 64 X2 Dual-Core (Windsor) / Athlon 64 FX Dual-Core (Windsor), 90nm");
   FMSQ(0,15,  4, 8,  2, dA, "AMD Athlon 64 X2 Dual-Core (Windsor BH-F2), 90nm");
   FMSQ(0,15,  4, 8,  2, Mt, "AMD Turion 64 X2 (Trinidad BH-F2), 90nm");
   FMSQ(0,15,  4, 8,  2, MT, "AMD Turion 64 X2 (Taylor BH-F2), 90nm");
   FMS (0,15,  4, 8,  2,     "AMD Athlon 64 X2 Dual-Core (Windsor BH-F2) / Turion 64 X2 (Taylor / Trinidad BH-F2), 90nm");
   FMQ (0,15,  4, 8,     dA, "AMD Athlon 64 X2 Dual-Core (Windsor), 90nm");
   FMQ (0,15,  4, 8,     Mt, "AMD Turion 64 X2 (Trinidad), 90nm");
   FMQ (0,15,  4, 8,     MT, "AMD Turion 64 X2 (Taylor), 90nm");
   FM  (0,15,  4, 8,         "AMD Athlon 64 X2 Dual-Core (Windsor) / Turion 64 X2 (Taylor / Trinidad), 90nm");
   FMS (0,15,  4,11,  2,     "AMD Athlon 64 X2 Dual-Core (Windsor BH-F2), 90nm");
   FM  (0,15,  4,11,         "AMD Athlon 64 X2 Dual-Core (Windsor), 90nm");
   FMSQ(0,15,  4,12,  2, MS, "AMD mobile Sempron (Keene BH-F2), 90nm");
   FMSQ(0,15,  4,12,  2, dS, "AMD Sempron (Manila BH-F2), 90nm");
   FMSQ(0,15,  4,12,  2, Mt, "AMD Turion (Trinidad BH-F2), 90nm");
   FMSQ(0,15,  4,12,  2, MT, "AMD Turion (Taylor BH-F2), 90nm");
   FMSQ(0,15,  4,12,  2, dA, "AMD Athlon 64 (Orleans BH-F2), 90nm"); 
   FMS (0,15,  4,12,  2,     "AMD Athlon 64 (Orleans BH-F2) / Sempron (Manila BH-F2) / mobile Sempron (Keene BH-F2) / Turion (Taylor/Trinidad BH-F2), 90nm");
   FMQ (0,15,  4,12,     MS, "AMD mobile Sempron (Keene), 90nm");
   FMQ (0,15,  4,12,     dS, "AMD Sempron (Manila), 90nm");
   FMQ (0,15,  4,12,     Mt, "AMD Turion (Trinidad), 90nm");
   FMQ (0,15,  4,12,     MT, "AMD Turion (Taylor), 90nm");
   FMQ (0,15,  4,12,     dA, "AMD Athlon 64 (Orleans), 90nm"); 
   FM  (0,15,  4,12,         "AMD Athlon 64 (Orleans) / Sempron (Manila) / mobile Sempron (Keene) / Turion (Taylor/Trinidad), 90nm");
   FMSQ(0,15,  4,15,  2, MS, "AMD mobile Sempron (Keene DH-F2), 90nm");
   FMSQ(0,15,  4,15,  2, dS, "AMD Sempron (Manila DH-F2), 90nm");
   FMSQ(0,15,  4,15,  2, dA, "AMD Athlon 64 (Orleans DH-F2), 90nm");
   FMS (0,15,  4,15,  2,     "AMD Athlon 64 (Orleans DH-F2) / Sempron (Manila DH-F2) / mobile Sempron (Keene DH-F2), 90nm");
   FMQ (0,15,  4,15,     MS, "AMD mobile Sempron (Keene), 90nm");
   FMQ (0,15,  4,15,     dS, "AMD Sempron (Manila), 90nm");
   FMQ (0,15,  4,15,     dA, "AMD Athlon 64 (Orleans), 90nm");
   FM  (0,15,  4,15,         "AMD Athlon 64 (Orleans) / Sempron (Manila) / mobile Sempron (Keene), 90nm");
   FMS (0,15,  5,13,  3,     "AMD Opteron (Santa Rosa JH-F3), 90nm");
   FM  (0,15,  5,13,         "AMD Opteron (Santa Rosa), 90nm");
   FMSQ(0,15,  5,15,  2, MS, "AMD mobile Sempron (Keene DH-F2), 90nm");
   FMSQ(0,15,  5,15,  2, dS, "AMD Sempron (Manila DH-F2), 90nm");
   FMSQ(0,15,  5,15,  2, dA, "AMD Athlon 64 (Orleans DH-F2), 90nm");
   FMS (0,15,  5,15,  2,     "AMD Athlon 64 (Orleans DH-F2) / Sempron (Manila DH-F2) / mobile Sempron (Keene DH-F2), 90nm");
   FMS (0,15,  5,15,  3,     "AMD Athlon 64 (Orleans DH-F3), 90nm");
   FMQ (0,15,  5,15,     MS, "AMD mobile Sempron (Keene), 90nm");
   FMQ (0,15,  5,15,     dS, "AMD Sempron (Manila), 90nm");
   FMQ (0,15,  5,15,     dA, "AMD Athlon 64 (Orleans), 90nm");
   FM  (0,15,  5,15,         "AMD Athlon 64 (Orleans) / Sempron (Manila) / mobile Sempron (Keene), 90nm");
   FM  (0,15,  5,15,         "AMD Athlon 64 (Orleans), 90nm");
   FMS (0,15,  6, 8,  1,     "AMD Turion 64 X2 (Tyler BH-G1), 65nm");
   FMSQ(0,15,  6, 8,  2, MT, "AMD Turion 64 X2 (Tyler BH-G2), 65nm");
   FMSQ(0,15,  6, 8,  2, dS, "AMD Sempron Dual-Core (Tyler BH-G2), 65nm");
   FMS (0,15,  6, 8,  2,     "AMD AMD Turion 64 X2 (Tyler BH-G2) / Sempron Dual-Core (Tyler BH-G2), 65nm");
   FMQ (0,15,  6, 8,     MT, "AMD Turion 64 X2 (Tyler), 65nm");
   FMQ (0,15,  6, 8,     dS, "AMD Sempron Dual-Core (Tyler), 65nm");
   FM  (0,15,  6, 8,         "AMD AMD Turion 64 X2 (Tyler) / Sempron Dual-Core (Tyler), 65nm");
   FMSQ(0,15,  6,11,  1, dS, "AMD Sempron Dual-Core (Sparta BH-G1), 65nm");
   FMSQ(0,15,  6,11,  1, dA, "AMD Athlon 64 X2 Dual-Core (Brisbane BH-G1), 65nm");
   FMS (0,15,  6,11,  1,     "AMD Athlon 64 X2 Dual-Core (Brisbane BH-G1) / Sempron Dual-Core (Sparta BH-G1), 65nm");
   FMSQ(0,15,  6,11,  2, dA, "AMD Athlon 64 X2 Dual-Core (Brisbane BH-G2), 65nm");
   FMSQ(0,15,  6,11,  2, Mn, "AMD Turion Neo X2 Dual-Core (Huron BH-G2), 65nm");
   FMSQ(0,15,  6,11,  2, MN, "AMD Athlon Neo X2 (Huron BH-G2), 65nm");
   FMS (0,15,  6,11,  2,     "AMD Athlon 64 X2 Dual-Core (Brisbane BH-G2) / Athlon Neo X2 (Huron BH-G2), 65nm");
   FMQ (0,15,  6,11,     dS, "AMD Sempron Dual-Core (Sparta), 65nm");
   FMQ (0,15,  6,11,     Mn, "AMD Turion Neo X2 Dual-Core (Huron), 65nm");
   FMQ (0,15,  6,11,     MN, "AMD Athlon Neo X2 (Huron), 65nm");
   FMQ (0,15,  6,11,     dA, "AMD Athlon 64 X2 Dual-Core (Brisbane), 65nm");
   FM  (0,15,  6,11,         "AMD Athlon 64 X2 Dual-Core (Brisbane) / Sempron Dual-Core (Sparta) / Athlon Neo X2 (Huron), 65nm");
   FMSQ(0,15,  6,12,  2, MS, "AMD mobile Sempron (Sherman DH-G2), 65nm");
   FMSQ(0,15,  6,12,  2, dS, "AMD Sempron (Sparta DH-G2), 65nm");
   FMSQ(0,15,  6,12,  2, dA, "AMD Athlon 64 (Lima DH-G2), 65nm");
   FMS (0,15,  6,12,  2,     "AMD Athlon 64 (Lima DH-G2) / Sempron (Sparta DH-G2) / mobile Sempron (Sherman DH-G2), 65nm");
   FMQ (0,15,  6,12,     MS, "AMD mobile Sempron (Sherman), 65nm");
   FMQ (0,15,  6,12,     dS, "AMD Sempron (Sparta), 65nm");
   FMQ (0,15,  6,12,     dA, "AMD Athlon 64 (Lima), 65nm");
   FM  (0,15,  6,12,         "AMD Athlon 64 (Lima) / Sempron (Sparta) / mobile Sempron (Sherman), 65nm");
   FMSQ(0,15,  6,15,  2, MS, "AMD mobile Sempron (Sherman DH-G2), 65nm");
   FMSQ(0,15,  6,15,  2, dS, "AMD Sempron (Sparta DH-G2), 65nm");
   FMSQ(0,15,  6,15,  2, MN, "AMD Athlon Neo (Huron DH-G2), 65nm");
   FMS (0,15,  6,15,  2,     "AMD Athlon Neo (Huron DH-G2) / Sempron (Sparta DH-G2) / mobile Sempron (Sherman DH-G2), 65nm");
   FMQ (0,15,  6,15,     MS, "AMD mobile Sempron (Sherman), 65nm");
   FMQ (0,15,  6,15,     dS, "AMD Sempron (Sparta), 65nm");
   FMQ (0,15,  6,15,     MN, "AMD Athlon Neo (Huron), 65nm");
   FM  (0,15,  6,15,         "AMD Athlon Neo (Huron) / Sempron (Sparta) / mobile Sempron (Sherman), 65nm");
   FMSQ(0,15,  7,12,  2, MS, "AMD mobile Sempron (Sherman DH-G2), 65nm");
   FMSQ(0,15,  7,12,  2, dS, "AMD Sempron (Sparta DH-G2), 65nm");
   FMSQ(0,15,  7,12,  2, dA, "AMD Athlon (Lima DH-G2), 65nm");
   FMS (0,15,  7,12,  2,     "AMD Athlon (Lima DH-G2) / Sempron (Sparta DH-G2) / mobile Sempron (Sherman DH-G2), 65nm");
   FMQ (0,15,  7,12,     MS, "AMD mobile Sempron (Sherman), 65nm");
   FMQ (0,15,  7,12,     dS, "AMD Sempron (Sparta), 65nm");
   FMQ (0,15,  7,12,     dA, "AMD Athlon (Lima), 65nm");
   FM  (0,15,  7,12,         "AMD Athlon (Lima) / Sempron (Sparta) / mobile Sempron (Sherman), 65nm");
   FMSQ(0,15,  7,15,  1, MS, "AMD mobile Sempron (Sherman DH-G1), 65nm");
   FMSQ(0,15,  7,15,  1, dS, "AMD Sempron (Sparta DH-G1), 65nm");
   FMSQ(0,15,  7,15,  1, dA, "AMD Athlon 64 (Lima DH-G1), 65nm");
   FMS (0,15,  7,15,  1,     "AMD Athlon 64 (Lima DH-G1) / Sempron (Sparta DH-G1) / mobile Sempron (Sherman DH-G1), 65nm");
   FMSQ(0,15,  7,15,  2, MS, "AMD mobile Sempron (Sherman DH-G2), 65nm");
   FMSQ(0,15,  7,15,  2, dS, "AMD Sempron (Sparta DH-G2), 65nm");
   FMSQ(0,15,  7,15,  2, MN, "AMD Athlon Neo (Huron DH-G2), 65nm");
   FMSQ(0,15,  7,15,  2, dA, "AMD Athlon 64 (Lima DH-G2), 65nm");
   FMS (0,15,  7,15,  2,     "AMD Athlon 64 (Lima DH-G2) / Sempron (Sparta DH-G2) / mobile Sempron (Sherman DH-G2) / Athlon Neo (Huron DH-G2), 65nm");
   FMQ (0,15,  7,15,     MS, "AMD mobile Sempron (Sherman), 65nm");
   FMQ (0,15,  7,15,     dS, "AMD Sempron (Sparta), 65nm");
   FMQ (0,15,  7,15,     MN, "AMD Athlon Neo (Huron), 65nm");
   FMQ (0,15,  7,15,     dA, "AMD Athlon 64 (Lima), 65nm");
   FM  (0,15,  7,15,         "AMD Athlon 64 (Lima) / Sempron (Sparta) / mobile Sempron (Sherman) / Athlon Neo (Huron), 65nm");
   FMS (0,15, 12, 1,  3,     "AMD Athlon 64 FX Dual-Core (Windsor JH-F3), 90nm");
   FM  (0,15, 12, 1,         "AMD Athlon 64 FX Dual-Core (Windsor), 90nm");
   F   (0,15,                "AMD Opteron / Athlon 64 / Athlon 64 FX / Sempron / Turion / Athlon Neo / Dual Core Opteron / Athlon 64 X2 / Athlon 64 FX / mobile Athlon 64 / mobile Sempron / mobile Athlon XP-M (DP) (unknown model)");
   FMSQ(1,15,  0, 2,  1, dO, "AMD Quad-Core Opteron (Barcelona DR-B1) [K10], 65nm");
   FMS (1,15,  0, 2,  1,     "AMD Quad-Core Opteron (Barcelona DR-B1) [K10], 65nm");
   FMSQ(1,15,  0, 2,  2, EO, "AMD Embedded Opteron (Barcelona DR-B2) [K10], 65nm");
   FMSQ(1,15,  0, 2,  2, dO, "AMD Quad-Core Opteron (Barcelona DR-B2) [K10], 65nm");
   FMSQ(1,15,  0, 2,  2, Tp, "AMD Phenom Triple-Core (Toliman DR-B2) [K10], 65nm");
   FMSQ(1,15,  0, 2,  2, Qp, "AMD Phenom Quad-Core (Agena DR-B2) [K10], 65nm");
   FMS (1,15,  0, 2,  2,     "AMD Quad-Core Opteron (Barcelona DR-B2) / Embedded Opteron (Barcelona DR-B2) / Phenom Triple-Core (Toliman DR-B2) / Phenom Quad-Core (Agena DR-B2) [K10], 65nm");
   FMQ (1,15,  0, 2,     EO, "AMD Embedded Opteron (Barcelona) [K10], 65nm");
   FMQ (1,15,  0, 2,     dO, "AMD Quad-Core Opteron (Barcelona) [K10], 65nm");
   FMQ (1,15,  0, 2,     Tp, "AMD Phenom Triple-Core (Toliman) [K10], 65nm");
   FMQ (1,15,  0, 2,     Qp, "AMD Phenom Quad-Core (Agena) [K10], 65nm");
   FM  (1,15,  0, 2,         "AMD Opteron (Barcelona) / Phenom Triple-Core (Toliman) / Phenom Quad-Core (Agena) [K10], 65nm");
   FMSQ(1,15,  0, 2,  3, EO, "AMD Embedded Opteron (Barcelona DR-B3) [K10], 65nm");
   FMSQ(1,15,  0, 2,  3, dO, "AMD Quad-Core Opteron (Barcelona DR-B3) [K10], 65nm");
   FMSQ(1,15,  0, 2,  3, Tp, "AMD Phenom Triple-Core (Toliman DR-B3) [K10], 65nm");
   FMSQ(1,15,  0, 2,  3, Qp, "AMD Phenom Quad-Core (Agena DR-B3) [K10], 65nm");
   FMSQ(1,15,  0, 2,  3, dA, "AMD Athlon Dual-Core (Kuma DR-B3) [K10], 65nm");
   FMS (1,15,  0, 2,  3,     "AMD Quad-Core Opteron (Barcelona DR-B3) / Embedded Opteron (Barcelona DR-B2) / Phenom Triple-Core (Toliman DR-B3) / Phenom Quad-Core (Agena DR-B3) / Athlon Dual-Core (Kuma DR-B3) [K10], 65nm");
   FMS (1,15,  0, 2, 10,     "AMD Quad-Core Opteron (Barcelona DR-BA) [K10], 65nm");
   FMQ (1,15,  0, 2,     EO, "AMD Embedded Opteron (Barcelona) [K10], 65nm");
   FMQ (1,15,  0, 2,     dO, "AMD Quad-Core Opteron (Barcelona) [K10], 65nm");
   FMQ (1,15,  0, 2,     Tp, "AMD Phenom Triple-Core (Toliman) [K10], 65nm");
   FMQ (1,15,  0, 2,     Qp, "AMD Phenom Quad-Core (Agena) [K10], 65nm");
   FMQ (1,15,  0, 2,     dA, "AMD Athlon Dual-Core (Kuma) [K10], 65nm");
   FM  (1,15,  0, 2,         "AMD Quad-Core Opteron (Barcelona) / Phenom Triple-Core (Toliman) / Phenom Quad-Core (Agena) / Athlon Dual-Core (Kuma) [K10], 65nm");
   FMSQ(1,15,  0, 4,  2, EO, "AMD Embedded Opteron (Shanghai RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, dO, "AMD Quad-Core Opteron (Shanghai RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, dR, "AMD Athlon Dual-Core (Propus RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, dA, "AMD Athlon Dual-Core (Regor RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, Dp, "AMD Phenom II X2 (Callisto RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, Tp, "AMD Phenom II X3 (Heka RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  2, Qp, "AMD Phenom II X4 (Deneb RB-C2) [K10], 45nm");
   FMS (1,15,  0, 4,  2,     "AMD Quad-Core Opteron (Shanghai RB-C2) / Embedded Opteron (Shanghai RB-C2) / Athlon Dual-Core (Regor / Propus RB-C2) / Phenom II (Callisto / Heka / Deneb RB-C2) [K10], 45nm");
   FMSQ(1,15,  0, 4,  3, Dp, "AMD Phenom II X2 (Callisto RB-C3) [K10], 45nm");
   FMSQ(1,15,  0, 4,  3, Tp, "AMD Phenom II X3 (Heka RB-C3) [K10], 45nm");
   FMSQ(1,15,  0, 4,  3, Qp, "AMD Phenom II X4 (Deneb RB-C3) [K10], 45nm");
   FMS (1,15,  0, 4,  3,     "AMD Phenom II (Callisto / Heka / Deneb RB-C3) [K10], 45nm");
   FMQ (1,15,  0, 4,     EO, "AMD Embedded Opteron (Shanghai) [K10], 45nm");
   FMQ (1,15,  0, 4,     dO, "AMD Quad-Core Opteron (Shanghai) [K10], 45nm");
   FMQ (1,15,  0, 4,     dR, "AMD Athlon Dual-Core (Propus) [K10], 45nm");
   FMQ (1,15,  0, 4,     dA, "AMD Athlon Dual-Core (Regor) [K10], 45nm");
   FMQ (1,15,  0, 4,     Dp, "AMD Phenom II X2 (Callisto) [K10], 45nm");
   FMQ (1,15,  0, 4,     Tp, "AMD Phenom II X3 (Heka) [K10], 45nm");
   FMQ (1,15,  0, 4,     Qp, "AMD Phenom II X4 (Deneb) [K10], 45nm");
   FM  (1,15,  0, 4,         "AMD Quad-Core Opteron (Shanghai) / Athlon Dual-Core (Regor / Propus) / Phenom II (Callisto / Heka / Deneb) [K10], 45nm");
   FMSQ(1,15,  0, 5,  2, DA, "AMD Athlon II X2 (Regor BL-C2) [K10], 45nm");
   FMSQ(1,15,  0, 5,  2, TA, "AMD Athlon II X3 (Rana BL-C2) [K10], 45nm");
   FMSQ(1,15,  0, 5,  2, QA, "AMD Athlon II X4 (Propus BL-C2) [K10], 45nm");
   FMS (1,15,  0, 5,  2,     "AMD Athlon II X2 / X3 / X4 (Regor / Rana / Propus BL-C2) [K10], 45nm");
   FMSQ(1,15,  0, 5,  3, TA, "AMD Athlon II X3 (Rana BL-C3) [K10], 45nm");
   FMSQ(1,15,  0, 5,  3, QA, "AMD Athlon II X4 (Propus BL-C3) [K10], 45nm");
   FMSQ(1,15,  0, 5,  3, Tp, "AMD Phenom II Triple-Core (Heka BL-C3) [K10], 45nm");
   FMSQ(1,15,  0, 5,  3, Qp, "AMD Phenom II Quad-Core (Deneb BL-C3) [K10], 45nm");
   FMS (1,15,  0, 5,  3,     "AMD Athlon II X3 / X4 (Rana / Propus BL-C3) / Phenom II Triple-Core (Heka BL-C3) / Quad-Core (Deneb BL-C3) [K10], 45nm");
   FMQ (1,15,  0, 5,     DA, "AMD Athlon II X2 (Regor) [K10], 45nm");
   FMQ (1,15,  0, 5,     TA, "AMD Athlon II X3 (Rana) [K10], 45nm");
   FMQ (1,15,  0, 5,     QA, "AMD Athlon II X4 (Propus) [K10], 45nm");
   FMQ (1,15,  0, 5,     Tp, "AMD Phenom II Triple-Core (Heka) [K10], 45nm");
   FMQ (1,15,  0, 5,     Qp, "AMD Phenom II Quad-Core (Deneb) [K10], 45nm");
   FM  (1,15,  0, 5,         "AMD Athlon II X2 / X3 / X4 (Regor / Rana / Propus) / Phenom II Triple-Core (Heka) / Quad-Core (Deneb) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, MS, "AMD Sempron Mobile (Sargas DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, dS, "AMD Sempron II (Sargas DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, MT, "AMD Turion II Dual-Core Mobile (Caspian DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, MA, "AMD Athlon II Dual-Core Mobile (Regor DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, DA, "AMD Athlon II X2 (Regor DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  2, dA, "AMD Athlon II (Sargas DA-C2) [K10], 45nm");
   FMS (1,15,  0, 6,  2,     "AMD Athlon II (Sargas DA-C2) / Athlon II X2 (Regor DA-C2) / Sempron II (Sargas DA-C2) / Athlon II Dual-Core Mobile (Regor DA-C2) / Sempron Mobile (Sargas DA-C2) / Turion II Dual-Core Mobile (Caspian DA-C2) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, Ms, "AMD V-Series Mobile (Champlain DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, DS, "AMD Sempron II X2 (Regor DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, dS, "AMD Sempron II (Sargas DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, MT, "AMD Turion II Dual-Core Mobile (Champlain DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, Mp, "AMD Phenom II Dual-Core Mobile (Champlain DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, MA, "AMD Athlon II Dual-Core Mobile (Champlain DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, DA, "AMD Athlon II X2 (Regor DA-C3) [K10], 45nm");
   FMSQ(1,15,  0, 6,  3, dA, "AMD Athlon II (Sargas DA-C3) [K10], 45nm");
   FMS (1,15,  0, 6,  3,     "AMD Athlon II (Sargas DA-C3) / Athlon II X2 (Regor DA-C2) / Sempron II (Sargas DA-C2) / Sempron II X2 (Regor DA-C3) / V-Series Mobile (Champlain DA-C3) / Athlon II Dual-Core Mobile (Champlain DA-C3) / Turion II Dual-Core Mobile (Champlain DA-C3) / Phenom II Dual-Core Mobile (Champlain DA-C3) [K10], 45nm");
   FMQ (1,15,  0, 6,     Ms, "AMD V-Series Mobile (Champlain) [K10], 45nm");
   FMQ (1,15,  0, 6,     MS, "AMD Sempron Mobile (Sargas) [K10], 45nm");
   FMQ (1,15,  0, 6,     DS, "AMD Sempron II X2 (Regor) [K10], 45nm");
   FMQ (1,15,  0, 6,     dS, "AMD Sempron II (Sargas) [K10], 45nm");
   FMQ (1,15,  0, 6,     MT, "AMD Turion II Dual-Core Mobile (Caspian / Champlain) [K10], 45nm");
   FMQ (1,15,  0, 6,     Mp, "AMD Phenom II Dual-Core Mobile (Champlain) [K10], 45nm");
   FMQ (1,15,  0, 6,     MA, "AMD Athlon II Dual-Core Mobile (Regor / Champlain) [K10], 45nm");
   FMQ (1,15,  0, 6,     DA, "AMD Athlon II X2 (Regor) [K10], 45nm");
   FMQ (1,15,  0, 6,     dA, "AMD Athlon II (Sargas) [K10], 45nm");
   FM  (1,15,  0, 6,         "AMD Athlon II (Sargas) / Athlon II X2 (Regor) / Sempron II (Sargas) / Sempron II X2 (Regor) / Sempron Mobile (Sargas) / V-Series Mobile (Champlain) / Athlon II Dual-Core Mobile (Regor / Champlain) / Turion II Dual-Core Mobile (Caspian / Champlain) / Phenom II Dual-Core Mobile (Champlain) [K10], 45nm");
   FMSQ(1,15,  0, 8,  0, SO, "AMD Six-Core Opteron (Istanbul HY-D0) [K10], 45nm");
   FMSQ(1,15,  0, 8,  0, dO, "AMD Opteron 4100 (Lisbon HY-D0) [K10], 45nm");
   FMS (1,15,  0, 8,  0,     "AMD Opteron 4100 (Lisbon HY-D0) / Six-Core Opteron (Istanbul HY-D0) [K10], 45nm");
   FMS (1,15,  0, 8,  1,     "AMD Opteron 4100 (Lisbon HY-D1) [K10], 45nm");
   FMQ (1,15,  0, 8,     SO, "AMD Six-Core Opteron (Istanbul) [K10], 45nm");
   FMQ (1,15,  0, 8,     dO, "AMD Opteron 4100 (Lisbon) [K10], 45nm");
   FM  (1,15,  0, 8,         "AMD Opteron 4100 (Lisbon) / Six-Core Opteron (Istanbul) [K10], 45nm");
   FMS (1,15,  0, 9,  1,     "AMD Opteron 6100 (Magny-Cours HY-D1) [K10], 45nm");
   FM  (1,15,  0, 9,         "AMD Opteron 6100 (Magny-Cours) [K10], 45nm");
   FMSQ(1,15,  0,10,  0, Qp, "AMD Phenom II X4 (Zosma PH-E0) [K10], 45nm");
   FMSQ(1,15,  0,10,  0, Sp, "AMD Phenom II X6 (Thuban PH-E0) [K10], 45nm");
   FMS (1,15,  0,10,  0,     "AMD Phenom II X4 / X6 (Zosma / Thuban PH-E0) [K10], 45nm");
   FMQ (1,15,  0,10,     Qp, "AMD Phenom II X4 (Zosma) [K10], 45nm");
   FMQ (1,15,  0,10,     Sp, "AMD Phenom II X6 (Thuban) [K10], 45nm");
   FM  (1,15,  0,10,         "AMD Phenom II X4 / X6 (Zosma / Thuban) [K10], 45nm");
   F   (1,15,                "AMD Athlon / Athlon II / Athlon II Xn / Opteron / Opteron 4100 / Opteron 6100 / Embedded Opteron / Phenom / Phenom II / Phenom II Xn / Sempron II / Sempron II Xn / Sempron Mobile / Turion II / V-Series Mobile [K10]");
   FMSQ(2,15,  0, 3,  1, MT, "AMD Turion X2 Dual-Core Mobile (Lion LG-B1) [Puma 2008], 65nm");
   FMSQ(2,15,  0, 3,  1, DS, "AMD Sempron X2 Dual-Core (Sable LG-B1) [Puma 2008], 65nm");
   FMSQ(2,15,  0, 3,  1, dS, "AMD Sempron (Sable LG-B1) [Puma 2008], 65nm");
   FMSQ(2,15,  0, 3,  1, DA, "AMD Athlon X2 Dual-Core (Lion LG-B1) [Puma 2008], 65nm");
   FMSQ(2,15,  0, 3,  1, dA, "AMD Athlon (Lion LG-B1) [Puma 2008], 65nm");
   FMS (2,15,  0, 3,  1,     "AMD Turion X2 Dual-Core Mobile (Lion LG-B1) / Athlon (Lion LG-B1) / Athlon X2 Dual-Core (Lion LG-B1) / Sempron (Sable LG-B1) / Sempron X2 Dual-Core (Sable LG-B1) [Puma 2008], 65nm");
   FMQ (2,15,  0, 3,     MT, "AMD Turion X2 (Lion) [Puma 2008], 65nm");
   FMQ (2,15,  0, 3,     DS, "AMD Sempron X2 Dual-Core (Sable) [Puma 2008], 65nm");
   FMQ (2,15,  0, 3,     dS, "AMD Sempron (Sable) [Puma 2008], 65nm");
   FMQ (2,15,  0, 3,     DA, "AMD Athlon X2 Dual-Core (Lion) [Puma 2008], 65nm");
   FMQ (2,15,  0, 3,     dA, "AMD Athlon (Lion) [Puma 2008], 65nm");
   FM  (2,15,  0, 3,         "AMD Turion X2 (Lion) / Athlon (Lion) / Sempron (Sable) [Puma 2008], 65nm");
   F   (2,15,                "AMD Turion X2 Mobile / Athlon / Athlon X2 / Sempron / Sempron X2 [Puma 2008], 65nm");
   FMSQ(3,15,  0, 1,  0, dS, "AMD Sempron Dual-Core (Llano LN-B0) [K10], 32nm");
   FMSQ(3,15,  0, 1,  0, dA, "AMD Athlon II Dual-Core (Llano LN-B0) [K10], 32nm");
   FMSQ(3,15,  0, 1,  0, Ms, "AMD A-Series (Llano LN-B0) / E2-Series (Llano LN-B0) [K10], 32nm");
   FMS (3,15,  0, 1,  0,     "AMD Sempron Dual-Core (Llano LN-B0) / Athlon II Dual-Core (Llano LN-B0) / A-Series (Llano LN-B0) / E2-Series (Llano LN-B0) [K10], 32nm");
   FMQ (3,15,  0, 1,     dS, "AMD Sempron Dual-Core (Llano) [K10], 32nm");
   FMQ (3,15,  0, 1,     dA, "AMD Athlon II Dual-Core (Llano) [K10], 32nm");
   FMQ (3,15,  0, 1,     Ms, "AMD A-Series (Llano) / E2-Series (Llano) [K10], 32nm");
   FM  (3,15,  0, 1,         "AMD Sempron Dual-Core (Llano) / Athlon II Dual-Core (Llano) / A-Series (Llano) / E2-Series (Llano) [K10], 32nm");
   FMS (5,15,  0, 1,  0,     "AMD C-Series (Ontario ON-B0) / E-Series (Zacate ON-B0) / G-Series (Ontario/Zacate ON-B0) / Z-Series (Desna ON-B0) [Bobcat], 40nm");
   FM  (5,15,  0, 1,         "AMD C-Series (Ontario) / E-Series (Zacate) / G-Series (Ontario/Zacat) / Z-Series (Desna) [Bobcat], 40nm");
   FMS (5,15,  0, 2,  0,     "AMD C-Series (Ontario ON-C0) / E-Series (Zacate ON-C0) / G-Series (Ontario/Zacate ON-C0) / Z-Series (Desna ON-C0) [Bobcat], 40nm");
   FM  (5,15,  0, 2,         "AMD C-Series (Ontario) / E-Series (Zacate) / G-Series (Ontario/Zacat) / Z-Series (Desna) [Bobcat], 40nm");
   F   (5,15,                "AMD C-Series / E-Series / G-Series / Z-Series [Bobcat], 40nm");
   FMSQ(6,15,  0, 1,  2, dO, "AMD Opteron 6200 (Interlagos OR-B2) / Opteron 4200 (Valencia OR-B2) / Opteron 3200 (Zurich OR-B2) [Bulldozer], 32nm");
   FMSQ(6,15,  0, 1,  2, df, "AMD FX-Series (Zambezi OR-B2) [Bulldozer], 32nm");
   FMS (6,15,  0, 1,  2,     "AMD Opteron 6200 (Interlagos OR-B2) / Opteron 4200 (Valencia OR-B2) / Opteron 3200 (Zurich OR-B2) / AMD FX-Series (Zambezi OR-B2) [Bulldozer], 32nm");
   FMQ (6,15,  0, 1,     dO, "AMD Opteron 6200 (Interlagos) / Opteron 4200 (Valencia) / Opteron 3200 (Zurich) [Bulldozer], 32nm");
   FM  (6,15,  0, 1,         "AMD Opteron 6200 (Interlagos) / Opteron 4200 (Valencia) / Opteron 3200 (Zurich) / AMD FX-Series (Zambezi) [Bulldozer], 32nm");
   FMSQ(6,15,  0, 2,  0, dO, "AMD Opteron 6300 (Abu Dhabi OR-C0) / Opteron 4300 (Seoul OR-C0) / Opteron 3300 (Delhi OR-C0) [Piledriver], 32nm");
   FMSQ(6,15,  0, 2,  0, df, "AMD FX-Series (Vishera OR-C0) [Piledriver], 32nm");
   FMS (6,15,  0, 2,  0,     "AMD Opteron 6300 (Abu Dhabi OR-C0) / Opteron 4300 (Seoul OR-C0) / Opteron 3300 (Delhi OR-C0) / FX-Series (Vishera OR-C0) [Piledriver], 32nm");
   FMS (6,15,  1, 0,  1,     "AMD A-Series / AMD R-Series / Athlon Dual-Core / Athlon Quad-Core / Sempron Dual-Core / FirePro (Trinity TN-A1) [Piledriver], 32nm");
   FM  (6,15,  1, 0,         "AMD A-Series / AMD R-Series / Athlon Dual-Core / Athlon Quad-Core / Sempron Dual-Core / FirePro (Trinity) [Piledriver], 32nm");
   FMS (6,15,  1, 3,  1,     "AMD A-Series / AMD R-Series / Athlon Dual-Core / Athlon Quad-Core / Sempron Dual-Core / FirePro (Richland RL-A1) [Piledriver], 32nm");
   FM  (6,15,  1, 3,         "AMD A-Series / AMD R-Series / Athlon Dual-Core / Athlon Quad-Core / Sempron Dual-Core / FirePro (Richland) [Piledriver], 32nm");
   FMS (6,15,  3, 0,  1,     "AMD Elite Performance A-Series / AMD Mobile R-Series / Opteron X1200 / X2200 (Kaveri KV-A1) [Steamroller], 28nm");
   FM  (6,15,  3, 0,         "AMD Elite Performance A-Series / AMD Mobile R-Series / Opteron X1200 / X2200 (Kaveri) [Steamroller], 28nm");
   FMS (6,15,  7, 0,  0,     "AMD A-Series / E-Series / G-Series (Stoney Ridge ST-A0) [Excavator], 28nm");
   FM  (6,15,  7, 0,         "AMD A-Series / E-Series / G-Series (Stoney Ridge) [Excavator], 28nm");
   F   (6,15,                "AMD Opteron 6x00 / Opteron 4x00 / Opteron 3x00 / AMD FX-Series / A-Series / E-Series / G-Series / R-Series / Opteron X1200 / X2200 / Athlon Dual-Core / Athlon Quad-Core / Sempron Dual-Core / FirePro");
   FMS (7,15,  0, 0,  1,     "AMD A-Series / E-Series / G-Series / Opteron X1100 Series / Opteron X2100 Series (Kabini KB-A1) [Jaguar], 28nm");
   FM  (7,15,  0, 0,         "AMD A-Series / E-Series / G-Series / Opteron X1100 Series / Opteron X2100 Series [Jaguar], 28nm");
   // The AMD docs (53072) omit the CPUID entirely.  But if this sticks to the
   // recent AMD pattern, these must be (7,15),(3,0).
   FMS (7,15,  3, 0,  1,     "AMD A-Series / E-Series Series (Mullins ML-A1) [Puma 2014], 28nm");
   FM  (7,15,  3, 0,         "AMD A-Series / E-Series Series (Mullins) [Puma 2014], 28nm");
   FMS (8,15,  0, 1,  1,     "AMD Ryzen (Summit Ridge B1) [Zen], 14nm");
   FM  (8,15,  0, 1,         "AMD Ryzen (Summit Ridge) [Zen], 14nm");
   DEFAULT                  ("unknown");

   const char*  brand_pre;
   const char*  brand_post;
   char         proc[96];
   decode_amd_model(stash, &brand_pre, &brand_post, proc);
   if (proc[0] != '\0') {
      printf(" %s", proc);
   }
   
   printf("\n");
}

static void
print_synth_cyrix(const char*   name,
                  unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,4,  0,4,     "Cyrix Media GX / GXm");
   FM (0,4,  0,9,     "Cyrix 5x86");
   F  (0,4,           "Cyrix 5x86 (unknown model)");
   FM (0,5,  0,2,     "Cyrix M1 6x86");
   FM (0,5,  0,4,     "Cyrix M1 WinChip (C6)");
   FM (0,5,  0,8,     "Cyrix M1 WinChip 2 (C6-2)");
   FM (0,5,  0,9,     "Cyrix M1 WinChip 3 (C6-2)");
   F  (0,5,           "Cyrix M1 (unknown model)");
   FM (0,6,  0,0,     "Cyrix M2 6x86MX");
   FM (0,6,  0,5,     "Cyrix M2");
   F  (0,6,           "Cyrix M2 (unknown model)");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_via(const char*   name,
                unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0, 5,  0, 4,     "VIA WinChip (C6)");
   FM (0, 5,  0, 8,     "VIA WinChip 2 (C6-2)");
   FM (0, 6,  0, 6,     "VIA C3 (Samuel WinChip C5A core)");
   FM (0, 6,  0, 6,     "VIA C3 (Samuel WinChip C5A core)");
   FMS(0, 6,  0, 7,  0, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  1, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  2, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  3, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  4, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  5, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  6, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FMS(0, 6,  0, 7,  7, "VIA C3 (Samuel 2 WinChip C5B core) / Eden ESP 4000/5000/6000");
   FM (0, 6,  0, 7,     "VIA C3 (Ezra WinChip C5C core)");
   FM (0, 6,  0, 8,     "VIA C3 (Ezra-T WinChip C5N core)");
   FMS(0, 6,  0, 9,  0, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  1, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  2, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  3, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  4, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  5, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  6, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FMS(0, 6,  0, 9,  7, "VIA C3 / Eden ESP 7000/8000/10000 (Nehemiah WinChip C5XL core)");
   FM (0, 6,  0, 9,     "VIA C3 / C3-M / Eden-N (Antaur WinChip C5P core)");
   FM (0, 6,  0,10,     "VIA C7 / C7-M (Esther WinChip C5J core)");
   FM (0, 6,  0,13,     "VIA C7 / C7-M / C7-D / Eden (Esther unknown core)");
   FM (0, 6,  0,15,     "VIA Nano (Isaiah)");
   F  (0, 6,            "VIA C3 / C3-M / C7 / C7-M / Eden / Eden ESP 7000/8000/10000 / Nano (unknown model)");
   DEFAULT             ("unknown");
   printf("\n");
}

static void
print_synth_umc(const char*   name,
                unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,4,  0,1,     "UMC U5D (486DX)");
   FMS(0,4,  0,2,  3, "UMC U5S (486SX)");
   FM (0,4,  0,2,     "UMC U5S (486SX) (unknown stepping)");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_nexgen(const char*   name,
                   unsigned int  val)
{
   printf("%s", name);
   START;
   FMS(0,5,  0,0,  4, "NexGen P100");
   FMS(0,5,  0,0,  6, "NexGen P120 (E2/C0)");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_rise(const char*   name,
                 unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,5,  0,0,     "Rise mP6 iDragon, .25u");
   FM (0,5,  0,2,     "Rise mP6 iDragon, .18u");
   FM (0,5,  0,8,     "Rise mP6 iDragon II, .25u");
   FM (0,5,  0,9,     "Rise mP6 iDragon II, .18u");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_transmeta(const char*          name,
                      unsigned int         val,
                      const code_stash_t*  stash)
{
   /* TODO: Add code-based detail for Transmeta Crusoe TM5700/TM5900 */
   /* TODO: Add code-based detail for Transmeta Efficeon */
   printf("%s", name);
   START;
   FMSQ(0, 5,  0,4,  2, t2, "Transmeta Crusoe TM3200");
   FMS (0, 5,  0,4,  2,     "Transmeta Crusoe TM3x00 (unknown model)");
   FMSQ(0, 5,  0,4,  3, t4, "Transmeta Crusoe TM5400");
   FMSQ(0, 5,  0,4,  3, t5, "Transmeta Crusoe TM5500 / Crusoe SE TM55E");
   FMSQ(0, 5,  0,4,  3, t6, "Transmeta Crusoe TM5600");
   FMSQ(0, 5,  0,4,  3, t8, "Transmeta Crusoe TM5800 / Crusoe SE TM58E");
   FMS (0, 5,  0,4,  3,     "Transmeta Crusoe TM5x00 (unknown model)");
   FM  (0, 5,  0,4,         "Transmeta Crusoe");
   F   (0, 5,               "Transmeta Crusoe (unknown model)");
   FM  (0,15,  0,2,         "Transmeta Efficeon TM8x00");
   FM  (0,15,  0,3,         "Transmeta Efficeon TM8x00");
   F   (0,15,               "Transmeta Efficeon (unknown model)");
   DEFAULT                ("unknown");
   printf("\n");
}

static void
print_synth_sis(const char*   name,
                unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,5,  0,0,     "SiS 55x");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_nsc(const char*   name,
                unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,5,  0,4,     "NSC Geode GX1/GXLV/GXm");
   FM (0,5,  0,5,     "NSC Geode GX2");
   F  (0,5,           "NSC Geode (unknown model)");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_vortex(const char*   name,
                   unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,5,  0,2,     "Vortex86DX");
   FM (0,5,  0,8,     "Vortex86MX");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_rdc(const char*   name,
                unsigned int  val)
{
   printf("%s", name);
   START;
   FM (0,5,  0,8,     "RDC IAD 100");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_x_synth_amd(unsigned int  val)
{
   printf("      (simple synth) = ");
   START;
   FM  (0, 5,  0, 0,     "AMD SSA5 (PR75, PR90, PR100)");
   FM  (0, 5,  0, 1,     "AMD 5k86 (PR120, PR133)");
   FM  (0, 5,  0, 2,     "AMD 5k86 (PR166)");
   FM  (0, 5,  0, 3,     "AMD 5k86 (PR200)");
   F   (0, 5,            "AMD 5k86 (unknown model)");
   FM  (0, 6,  0, 6,     "AMD K6, .30um");
   FM  (0, 6,  0, 7,     "AMD K6 (Little Foot), .25um");
   FMS (0, 6,  0, 8,  0, "AMD K6-2 (Chomper A)");
   FMS (0, 6,  0, 8, 12, "AMD K6-2 (Chomper A)");
   FM  (0, 6,  0, 8,     "AMD K6-2 (Chomper)");
   FMS (0, 6,  0, 9,  1, "AMD K6-III (Sharptooth B)");
   FM  (0, 6,  0, 9,     "AMD K6-III (Sharptooth)");
   FM  (0, 6,  0,13,     "AMD K6-2+, K6-III+");
   F   (0, 6,            "AMD K6 (unknown model)");
   FM  (0, 7,  0, 1,     "AMD Athlon, .25um");
   FM  (0, 7,  0, 2,     "AMD Athlon (K75 / Pluto / Orion), .18um");
   FMS (0, 7,  0, 3,  0, "AMD Duron / mobile Duron (Spitfire A0)");
   FMS (0, 7,  0, 3,  1, "AMD Duron / mobile Duron (Spitfire A2)");
   FM  (0, 7,  0, 3,     "AMD Duron / mobile Duron (Spitfire)");
   FMS (0, 7,  0, 4,  2, "AMD Athlon (Thunderbird A4-A7)");
   FMS (0, 7,  0, 4,  4, "AMD Athlon (Thunderbird A9)");
   FM  (0, 7,  0, 4,     "AMD Athlon (Thunderbird)");
   FMS (0, 7,  0, 6,  0, "AMD Athlon / Athlon MP mobile Athlon 4 / mobile Duron (Palomino A0)");
   FMS (0, 7,  0, 6,  1, "AMD Athlon / Athlon MP / Duron / mobile Athlon / mobile Duron (Palomino A2)");
   FMS (0, 7,  0, 6,  2, "AMD Athlon MP / Athlon XP / Duron / Duron MP / mobile Athlon / mobile Duron (Palomino A5)");
   FM  (0, 7,  0, 6,     "AMD Athlon / Athlon MP / Athlon XP / Duron / Duron MP / mobile Athlon / mobile Duron (Palomino)");
   FMS (0, 7,  0, 7,  0, "AMD Duron / Duron MP / mobile Duron (Morgan A0)");
   FMS (0, 7,  0, 7,  1, "AMD Duron / Duron MP / mobile Duron (Morgan A1)");
   FM  (0, 7,  0, 7,     "AMD Duron / Duron MP / mobile Duron (Morgan)");
   FMS (0, 7,  0, 8,  0, "AMD Athlon XP / Athlon MP / Sempron / Duron / Duron MP (Thoroughbred A0)");
   FMS (0, 7,  0, 8,  1, "AMD Athlon XP / Athlon MP / Sempron / Duron / Duron MP (Thoroughbred B0)");
   FM  (0, 7,  0, 8,     "AMD Athlon XP / Athlon MP / Sempron / Duron / Duron MP (Thoroughbred)");
   FMS (0, 7,  0,10,  0, "AMD Athlon XP / Athlon MP / Sempron / mobile Athlon XP-M / mobile Athlon XP-M (LV) (Barton A2)");
   FM  (0, 7,  0,10,     "AMD Athlon XP / Athlon MP / Sempron / mobile Athlon XP-M / mobile Athlon XP-M (LV) (Barton)");
   F   (0, 7,            "AMD Athlon XP / Athlon MP / Sempron / Duron / Duron MP / mobile Athlon / mobile Athlon XP-M / mobile Athlon XP-M (LV) / mobile Duron (unknown model)");
   FALLBACK({ print_synth_amd("", val, NULL); return; })
   printf("\n");
}

static void
print_x_synth_via(unsigned int  val)
{
   printf("      (simple synth) = ");
   START;
   FM (0,6,  0,6,     "VIA C3 (WinChip C5A)");
   FM (0,6,  0,6,     "VIA C3 (WinChip C5A)");
   FMS(0,6,  0,7,  0, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  1, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  2, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  3, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  4, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  5, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  6, "VIA C3 (WinChip C5B)");
   FMS(0,6,  0,7,  7, "VIA C3 (WinChip C5B)");
   FM (0,6,  0,7,     "VIA C3 (WinChip C5C)");
   FM (0,6,  0,8,     "VIA C3 (WinChip C5N)");
   FM (0,6,  0,9,     "VIA C3 (WinChip C5XL)");
   F  (0,6,           "VIA C3 (unknown model)");
   DEFAULT           ("unknown");
   printf("\n");
}

static void
print_synth_simple(unsigned int  val_eax,
                   vendor_t      vendor)
{
   switch (vendor) {
   case VENDOR_INTEL:
      print_synth_intel("      (simple synth)  = ", val_eax, NULL);
      break;
   case VENDOR_AMD:
      print_synth_amd("      (simple synth)  = ", val_eax, NULL);
      break;
   case VENDOR_CYRIX:
      print_synth_cyrix("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_VIA:
      print_synth_via("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_TRANSMETA:
      print_synth_transmeta("      (simple synth)  = ", val_eax, NULL);
      break;
   case VENDOR_UMC:
      print_synth_umc("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_NEXGEN:
      print_synth_nexgen("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_RISE:
      print_synth_rise("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_SIS:
      print_synth_sis("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_NSC:
      print_synth_nsc("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_VORTEX:
      print_synth_vortex("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_RDC:
      print_synth_rdc("      (simple synth)  = ", val_eax);
      break;
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

static void
print_synth(const code_stash_t*  stash)
{
   switch (stash->vendor) {
   case VENDOR_INTEL:
      print_synth_intel("   (synth) = ", stash->val_1_eax, stash);
      break;
   case VENDOR_AMD:
      print_synth_amd("   (synth) = ", stash->val_1_eax, stash);
      break;
   case VENDOR_CYRIX:
      print_synth_cyrix("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_VIA:
      print_synth_via("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_TRANSMETA:
      print_synth_transmeta("   (synth) = ", stash->val_1_eax, stash);
      break;
   case VENDOR_UMC:
      print_synth_umc("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_NEXGEN:
      print_synth_nexgen("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_RISE:
      print_synth_rise("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_SIS:
      print_synth_sis("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_NSC:
      print_synth_nsc("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_VORTEX:
      print_synth_vortex("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_RDC:
      print_synth_rdc("   (synth) = ", stash->val_1_eax);
      break;
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

#define GET_ApicIdCoreIdSize(val_80000008_ecx) \
   (BIT_EXTRACT_LE((val_80000008_ecx), 0, 4))
#define GET_LogicalProcessorCount(val_1_ebx) \
   (BIT_EXTRACT_LE((val_1_ebx), 16, 24))
#define IS_HTT(val_1_edx) \
   (BIT_EXTRACT_LE((val_1_edx), 28, 29))
#define IS_CmpLegacy(val_80000001_ecx) \
   (BIT_EXTRACT_LE((val_80000001_ecx), 1, 2))
#define GET_NC_INTEL(val_4_eax) \
   (BIT_EXTRACT_LE((val_4_eax), 26, 32))
#define GET_NC_AMD(val_80000008_ecx) \
   (BIT_EXTRACT_LE((val_80000008_ecx), 0, 8))
#define GET_X2APIC_PROCESSORS(val_b_ebx) \
   (BIT_EXTRACT_LE((val_b_ebx), 0, 16))

static void decode_mp_synth(code_stash_t*  stash)
{
   switch (stash->vendor) {
   case VENDOR_INTEL:
      /*
      ** Logic derived from information in:
      **    Detecting Multi-Core Processor Topology in an IA-32 Platform
      **    by Khang Nguyen and Shihjong Kuo
      ** and:
      **    Intel 64 Architecture Processor Topology Enumeration (Whitepaper)
      **    by Shih Kuo
      */
      if (stash->saw_b) {
         unsigned int  ht = GET_X2APIC_PROCESSORS(stash->val_b_ebx[0]);
         unsigned int  tc = GET_X2APIC_PROCESSORS(stash->val_b_ebx[1]);
         stash->mp.method = "Intel leaf 0xb";
         if (ht == 0) {
            ht = 1;
         }
         stash->mp.cores        = tc / ht;
         stash->mp.hyperthreads = ht;
      } else if (stash->saw_4) {
         unsigned int  tc = GET_LogicalProcessorCount(stash->val_1_ebx);
         unsigned int  c;
         if ((stash->val_4_eax & 0x1f) != 0) {
            c = GET_NC_INTEL(stash->val_4_eax) + 1;
            stash->mp.method = "Intel leaf 1/4";
         } else {
            /* Workaround for older 'cpuid -r' dumps with incomplete 4 data */
            c = tc / 2;
            stash->mp.method = "Intel leaf 1/4 (zero fallback)";
         }
         stash->mp.cores        = c;
         stash->mp.hyperthreads = tc / c;
      } else {
         stash->mp.method = "Intel leaf 1";
         stash->mp.cores  = 1;
         if (IS_HTT(stash->val_1_edx)) {
            unsigned int  tc = GET_LogicalProcessorCount(stash->val_1_ebx);
            stash->mp.hyperthreads = (tc >= 2 ? tc : 2);
         } else {
            stash->mp.hyperthreads = 1;
         }
      }
      break;
   case VENDOR_AMD:
      /*
      ** Logic from:
      **    AMD CPUID Specification (25481 Rev. 2.16),
      **    3. LogicalProcessorCount, CmpLegacy, HTT, and NC
      **    AMD CPUID Specification (25481 Rev. 2.28),
      **    3. Multiple Core Calculation
      */
      if (IS_HTT(stash->val_1_edx)) {
         unsigned int  tc = GET_LogicalProcessorCount(stash->val_1_ebx);
         unsigned int  c;
         if (GET_ApicIdCoreIdSize(stash->val_80000008_ecx) != 0) {
            unsigned int  size = GET_ApicIdCoreIdSize(stash->val_80000008_ecx);
            unsigned int  mask = (1 << size) - 1;
            c = (GET_NC_AMD(stash->val_80000008_ecx) & mask) + 1;
         } else {
            c = GET_NC_AMD(stash->val_80000008_ecx) + 1;
         }
         if ((tc == c) == IS_CmpLegacy(stash->val_80000001_ecx)) {
            stash->mp.method = "AMD";
            if (c > 1) {
               stash->mp.cores        = c;
               stash->mp.hyperthreads = tc / c;
            } else {
               stash->mp.cores        = 1;
               stash->mp.hyperthreads = (tc >= 2 ? tc : 2);
            }
         } else {
            /* 
            ** Rev 2.28 leaves out mention that this case is nonsensical, but
            ** I'm leaving it in here as an "unknown" case.
            */
         }
      } else {
         stash->mp.method       = "AMD";
         stash->mp.cores        = 1;
         stash->mp.hyperthreads = 1;
      }
      break;
   default:
      if (!IS_HTT(stash->val_1_edx)) {
         stash->mp.method       = "Generic leaf 1 no multi-threading";
         stash->mp.cores        = 1;
         stash->mp.hyperthreads = 1;
      }
      break;
   }
}

static void print_mp_synth(const struct mp*  mp)
{
   printf("   (multi-processing synth): ");
   if (mp->method == NULL) {
      printf("?");
   } else if (mp->cores > 1) {
      if (mp->hyperthreads > 1) {
         printf("multi-core (c=%u), hyper-threaded (t=%u)", 
                mp->cores, mp->hyperthreads);
      } else {
         printf("multi-core (c=%u)", mp->cores);
      }
   } else if (mp->hyperthreads > 1) {
      printf("hyper-threaded (t=%u)", mp->hyperthreads);
   } else {
      printf("none");
   }
   printf("\n");

   printf("   (multi-processing method): %s\n", mp->method);
}

static int bits_needed(unsigned long  v)
{
   int  result;
#if defined(__x86_64) && !defined(__ILP32__)
   asm("movq %[v],%%rax;"
       "movq $0,%%rcx;"
       "movl $0,%[result];"
       "decq %%rax;"
       "bsr %%ax,%%cx;"
       "jz 1f;"
       "incq %%rcx;"
       "movl %%ecx,%[result];"
       "1:"
       : [result] "=rm" (result)
       : [v] "irm" (v) 
       : "eax", "ecx");
#else
   asm("movl %[v],%%eax;"
       "movl $0,%%ecx;"
       "movl $0,%[result];"
       "decl %%eax;"
       "bsr %%ax,%%cx;"
       "jz 1f;"
       "incl %%ecx;"
       "movl %%ecx,%[result];"
       "1:"
       : [result] "=rm" (result)
       : [v] "irm" (v) 
       : "eax", "ecx");
#endif
   return result;
}

#define GET_X2APIC_WIDTH(val_b_eax) \
   (BIT_EXTRACT_LE((val_b_eax), 0, 5))

static void print_apic_synth (code_stash_t*  stash)
{
   unsigned int  smt_width;
   unsigned int  core_width;

   switch (stash->vendor) {
   case VENDOR_INTEL:
      /*
      ** Logic derived from information in:
      **    Detecting Multi-Core Processor Topology in an IA-32 Platform
      **    by Khang Nguyen and Shihjong Kuo
      ** and:
      **    Intel 64 Architecture Processor Topology Enumeration (Whitepaper)
      **    by Shih Kuo
      */
      if (stash->saw_b) {
         smt_width  = GET_X2APIC_WIDTH(stash->val_b_eax[0]);
         core_width = GET_X2APIC_WIDTH(stash->val_b_eax[1]);
      } else if (stash->saw_4 && (stash->val_4_eax & 0x1f) != 0) {
         unsigned int  core_count = GET_NC_INTEL(stash->val_4_eax) + 1;
         unsigned int  smt_count  = (GET_LogicalProcessorCount(stash->val_1_ebx)
                                     / core_count);
         smt_width   = bits_needed(smt_count);
         core_width  = bits_needed(core_count);
      } else {
         return;
      }
      break;
   case VENDOR_AMD:
      /*
      ** Logic deduced by analogy: As Intel's decode_mp_synth code is to AMD's
      ** decode_mp_synth code, so is Intel's APIC synth code to this.
      */
      if (IS_HTT(stash->val_1_edx)
          && GET_ApicIdCoreIdSize(stash->val_80000008_ecx) != 0) {
         unsigned int  size = GET_ApicIdCoreIdSize(stash->val_80000008_ecx);
         unsigned int  mask = (1 << size) - 1;
         unsigned int  core_count = ((GET_NC_AMD(stash->val_80000008_ecx) & mask)
                                     + 1);
         unsigned int  smt_count  = (GET_LogicalProcessorCount(stash->val_1_ebx)
                                     / core_count);
         smt_width   = bits_needed(smt_count);
         core_width  = bits_needed(core_count);
      } else {
         return;
      }
      break;
   default:
      return;
   }

   unsigned int  smt_off  = 24;
   unsigned int  core_off = smt_off + smt_width;
   unsigned int  pkg_off  = core_off + core_width;
   
   printf("   (APIC widths synth): CORE_width=%d SMT_width=%d\n",
          core_width, smt_width);
   printf("   (APIC synth):");
   printf(" PKG_ID=%d",  (pkg_off < 32
                          ? BIT_EXTRACT_LE(stash->val_1_ebx, pkg_off,  32)
                          : 0));
   printf(" CORE_ID=%d", BIT_EXTRACT_LE(stash->val_1_ebx, core_off, pkg_off));
   printf(" SMT_ID=%d",  BIT_EXTRACT_LE(stash->val_1_ebx, smt_off,  core_off));
   printf("\n");
}

static void print_instr_synth_amd (code_stash_t*  stash)
{
   boolean  cmpxchg8b = (BIT_EXTRACT_LE(stash->val_80000001_edx, 8, 9)
                         || BIT_EXTRACT_LE(stash->val_1_edx, 8, 9));
   boolean  cond      = (BIT_EXTRACT_LE(stash->val_80000001_edx, 15, 16)
                         || BIT_EXTRACT_LE(stash->val_1_edx, 15, 16));
   boolean  prefetch  = (BIT_EXTRACT_LE(stash->val_80000001_ecx, 8, 9)
                         || BIT_EXTRACT_LE(stash->val_80000001_edx, 29, 30)
                         || BIT_EXTRACT_LE(stash->val_80000001_edx, 31, 32));

   printf("   (instruction supported synth):\n");
   printf("      CMPXCHG8B                = %s\n", bools[cmpxchg8b]);
   printf("      conditional move/compare = %s\n", bools[cond]);
   printf("      PREFETCH/PREFETCHW       = %s\n", bools[prefetch]);
}

static void print_instr_synth (code_stash_t*  stash)
{
   switch (stash->vendor) {
   case VENDOR_AMD:
      print_instr_synth_amd(stash);
      break;
   default:
      break;
   }
}

static void do_final (boolean        raw,
                      boolean        debug,
                      code_stash_t*  stash)
{
   if (!raw) {
      print_instr_synth(stash);
      decode_mp_synth(stash);
      print_mp_synth(&stash->mp);
      print_apic_synth(stash);
      decode_override_brand(stash);
      print_override_brand(stash);
      decode_brand_id_stash(stash);
      decode_brand_stash(stash);
      if (debug) {
         debug_queries(stash);
      }
      print_synth(stash);
   }
}

static void
print_1_eax(unsigned int  value,
            vendor_t      vendor)
{
   static ccstring  processor[] = { "primary processor (0)",
                                    "Intel OverDrive (1)",
                                    "secondary processor (2)",
                                    NULL };
   static ccstring  family[]    = { NULL,
                                    NULL,
                                    NULL,
                                    "Intel 80386 (3)",
                                    "Intel 80486, AMD Am486, UMC 486 U5 (4)",
                                    "Intel Pentium, AMD K5/K6,"
                                       " Cyrix M1, NexGen Nx586,"
                                       " Centaur C6, Rise mP6,"
                                       " Transmeta Crusoe (5)", 
                                    "Intel Pentium Pro/II/III/Celeron/Core/Core 2/Atom,"
                                       " AMD Athlon/Duron, Cyrix M2,"
                                       " VIA C3 (6)",
                                    "Intel Itanium,"
                                       " AMD Athlon 64/Opteron/Sempron/Turion"
                                       " (7)",
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    "Intel Pentium 4/Pentium D/"
                                    "Pentium Extreme Edition/Celeron/Xeon/"
                                    "Xeon MP/Itanium2,"
                                    " AMD Athlon 64/Athlon XP-M/Opteron/"
                                    "Sempron/Turion (15)" };
   static named_item  names[]
      = { { "processor type"                          , 12, 13, processor },
          { "family"                                  ,  8, 11, family },
          { "model"                                   ,  4,  7, NIL_IMAGES },
          { "stepping id"                             ,  0,  3, NIL_IMAGES },
          { "extended family"                         , 20, 27, NIL_IMAGES },
          { "extended model"                          , 16, 19, NIL_IMAGES },
        };

   printf("   version information (1/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   print_synth_simple(value, vendor);
}

#define B(b,str)                                   \
   else if (   __B(val_ebx)   == _B(b))            \
      printf(str)
#define FMB(xf,f,xm,m,b,str)                       \
   else if (   __FM(val_eax)  == _FM(xf,f,xm,m)    \
            && __B(val_ebx)   == _B(b))            \
      printf(str)
#define FMSB(xf,f,xm,m,s,b,str)                    \
   else if (   __FMS(val_eax) == _FMS(xf,f,xm,m,s) \
            && __B(val_ebx)   == _B(b))            \
      printf(str)

static void
print_brand(unsigned int  val_eax,
            unsigned int  val_ebx)
{
   printf("   brand id = 0x%02x (%u): ", __B(val_ebx), __B(val_ebx));
   START;
   B   (                  1, "Intel Celeron, .18um");
   B   (                  2, "Intel Pentium III, .18um");
   FMSB(0, 6,  0,11,  1,  3, "Intel Celeron, .13um");
   B   (                  3, "Intel Pentium III Xeon, .18um");
   B   (                  4, "Intel Pentium III, .13um");
   B   (                  6, "Mobile Intel Pentium III, .13um");
   B   (                  7, "Mobile Intel Celeron, .13um");
   FMB (0,15,  0, 0,      8, "Intel Pentium 4, .18um");
   FMSB(0,15,  0, 1,  0,  8, "Intel Pentium 4, .18um");
   FMSB(0,15,  0, 1,  1,  8, "Intel Pentium 4, .18um");
   FMSB(0,15,  0, 1,  2,  8, "Intel Pentium 4, .18um");
   B   (                  8, "Mobile Intel Celeron 4, .13um");
   B   (                  9, "Intel Pentium 4, .13um");
   B   (                 10, "Intel Celeron 4, .18um");
   FMB (0,15,  0, 0,     11, "Intel Xeon MP, .18um");
   FMSB(0,15,  0, 1,  0, 11, "Intel Xeon MP, .18um");
   FMSB(0,15,  0, 1,  1, 11, "Intel Xeon MP, .18um");
   FMSB(0,15,  0, 1,  2, 11, "Intel Xeon MP, .18um");
   B   (                 11, "Intel Xeon, .13um");
   B   (                 12, "Intel Xeon MP, .13um");
   FMB (0,15,  0, 0,     14, "Intel Xeon, .18um");
   FMSB(0,15,  0, 1,  0, 14, "Intel Xeon, .18um");
   FMSB(0,15,  0, 1,  1, 14, "Intel Xeon, .18um");
   FMSB(0,15,  0, 1,  2, 14, "Intel Xeon, .18um");
   FMB (0,15,  0, 2,     14, "Mobile Intel Pentium 4 Processor-M");
   B   (                 14, "Mobile Intel Xeon, .13um");
   FMB (0,15,  0, 2,     15, "Mobile Intel Pentium 4 Processor-M");
   B   (                 15, "Mobile Intel Celeron 4");
   B   (                 17, "Mobile Genuine Intel");
   B   (                 18, "Intel Celeron M");
   B   (                 19, "Mobile Intel Celeron");
   B   (                 20, "Intel Celeron");
   B   (                 21, "Mobile Genuine Intel");
   B   (                 22, "Intel Pentium M, .13um");
   B   (                 23, "Mobile Intel Celeron");
   DEFAULT                  ("unknown");
   printf("\n");
}

static void
print_1_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "process local APIC physical ID"          , 24, 31, NIL_IMAGES },
          { "cpu count"                               , 16, 23, NIL_IMAGES },
          { "CLFLUSH line size"                       ,  8, 15, NIL_IMAGES },
          { "brand index"                             ,  0,  7, NIL_IMAGES },
        };

   printf("   miscellaneous (1/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_1_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "PNI/SSE3: Prescott New Instructions"     ,  0,  0, bools },
          { "PCLMULDQ instruction"                    ,  1,  1, bools },
          { "DTES64: 64-bit debug store"              ,  2,  2, bools },
          { "MONITOR/MWAIT"                           ,  3,  3, bools },
          { "CPL-qualified debug store"               ,  4,  4, bools },
          { "VMX: virtual machine extensions"         ,  5,  5, bools },
          { "SMX: safer mode extensions"              ,  6,  6, bools },
          { "Enhanced Intel SpeedStep Technology"     ,  7,  7, bools },
          { "TM2: thermal monitor 2"                  ,  8,  8, bools },
          { "SSSE3 extensions"                        ,  9,  9, bools },
          { "context ID: adaptive or shared L1 data"  , 10, 10, bools },
          { "SDBG: IA32_DEBUG_INTERFACE"              , 11, 11, bools },
          { "FMA instruction"                         , 12, 12, bools },
          { "CMPXCHG16B instruction"                  , 13, 13, bools },
          { "xTPR disable"                            , 14, 14, bools },
          { "PDCM: perfmon and debug"                 , 15, 15, bools },
          { "PCID: process context identifiers"       , 17, 17, bools },
          { "DCA: direct cache access"                , 18, 18, bools },
          { "SSE4.1 extensions"                       , 19, 19, bools },
          { "SSE4.2 extensions"                       , 20, 20, bools },
          { "x2APIC: extended xAPIC support"          , 21, 21, bools },
          { "MOVBE instruction"                       , 22, 22, bools },
          { "POPCNT instruction"                      , 23, 23, bools },
          { "time stamp counter deadline"             , 24, 24, bools },
          { "AES instruction"                         , 25, 25, bools },
          { "XSAVE/XSTOR states"                      , 26, 26, bools },
          { "OS-enabled XSAVE/XSTOR"                  , 27, 27, bools },
          { "AVX: advanced vector extensions"         , 28, 28, bools },
          { "F16C half-precision convert instruction" , 29, 29, bools },
          { "RDRAND instruction"                      , 30, 30, bools },
          { "hypervisor guest status"                 , 31, 31, bools },
        };

   printf("   feature information (1/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_1_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "x87 FPU on chip"                         ,  0,  0, bools },
          { "VME: virtual-8086 mode enhancement"      ,  1,  1, bools },
          { "DE: debugging extensions"                ,  2,  2, bools },
          { "PSE: page size extensions"               ,  3,  3, bools },
          { "TSC: time stamp counter"                 ,  4,  4, bools },
          { "RDMSR and WRMSR support"                 ,  5,  5, bools },
          { "PAE: physical address extensions"        ,  6,  6, bools },
          { "MCE: machine check exception"            ,  7,  7, bools },
          { "CMPXCHG8B inst."                         ,  8,  8, bools },
          { "APIC on chip"                            ,  9,  9, bools },
          { "SYSENTER and SYSEXIT"                    , 11, 11, bools },
          { "MTRR: memory type range registers"       , 12, 12, bools },
          { "PTE global bit"                          , 13, 13, bools },
          { "MCA: machine check architecture"         , 14, 14, bools },
          { "CMOV: conditional move/compare instr"    , 15, 15, bools },
          { "PAT: page attribute table"               , 16, 16, bools },
          { "PSE-36: page size extension"             , 17, 17, bools },
          { "PSN: processor serial number"            , 18, 18, bools },
          { "CLFLUSH instruction"                     , 19, 19, bools },
          { "DS: debug store"                         , 21, 21, bools },
          { "ACPI: thermal monitor and clock ctrl"    , 22, 22, bools },
          { "MMX Technology"                          , 23, 23, bools },
          { "FXSAVE/FXRSTOR"                          , 24, 24, bools },
          { "SSE extensions"                          , 25, 25, bools },
          { "SSE2 extensions"                         , 26, 26, bools },
          { "SS: self snoop"                          , 27, 27, bools },
          { "hyper-threading / multi-core supported"  , 28, 28, bools },
          { "TM: therm. monitor"                      , 29, 29, bools },
          { "IA64"                                    , 30, 30, bools },
          { "PBE: pending break event"                , 31, 31, bools },
        };

   printf("   feature information (1/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void print_2_byte(unsigned char  value,
                         vendor_t       vendor,
                         unsigned int   val_1_eax)
{
   if (value == 0x00) return;

   printf("      0x%02x: ", value);
#define CONT "\n            "

   if (vendor == VENDOR_CYRIX || vendor == VENDOR_VIA) {
      switch (value) {
      case 0x70: printf("TLB: 4K pages, 4-way, 32 entries");    return;
      case 0x74: printf("Cyrix-specific: ?");                   return;
      case 0x77: printf("Cyrix-specific: ?");                   return;
      case 0x80: printf("L1 cache: 16K, 4-way, 16 byte lines"); return;
      case 0x82: printf("Cyrix-specific: ?");                   return;
      case 0x84: printf("L2 cache: 1M, 8-way, 32 byte lines");  return;
      }
   }

   switch (value) {
   case 0x01: printf("instruction TLB: 4K pages, 4-way, 32 entries");    break;
   case 0x02: printf("instruction TLB: 4M pages, 4-way, 2 entries");     break;
   case 0x03: printf("data TLB: 4K pages, 4-way, 64 entries");           break;
   case 0x04: printf("data TLB: 4M pages, 4-way, 8 entries");            break;
   case 0x05: printf("data TLB: 4M pages, 4-way, 32 entries");           break;
   case 0x06: printf("L1 instruction cache: 8K, 4-way, 32 byte lines");  break;
   case 0x08: printf("L1 instruction cache: 16K, 4-way, 32 byte lines"); break;
   case 0x09: printf("L1 instruction cache: 32K, 4-way, 64-byte lines"); break;
   case 0x0a: printf("L1 data cache: 8K, 2-way, 32 byte lines");         break;
   case 0x0b: printf("instruction TLB: 4M pages, 4-way, 4 entries");     break;
   case 0x0c: printf("L1 data cache: 16K, 4-way, 32 byte lines");        break;
   case 0x0d: printf("L1 data cache: 16K, 4-way, 64-byte lines");        break;
   case 0x0e: printf("L1 data cache: 24K, 6-way, 64 byte lines");        break;
   case 0x10: printf("L1 data cache: 16K, 4-way, 32 byte lines");        break;
   case 0x15: printf("L1 instruction cache: 16K, 4-way, 32 byte lines"); break;
   case 0x1d: printf("L2 cache: 128K, 2-way, 64 byte lines");            break;
   case 0x1a: printf("L2 cache: 96K, 6-way, 64 byte lines");             break;
   case 0x21: printf("L2 cache: 256K MLC, 8-way, 64 byte lines");        break;
   case 0x22: printf("L3 cache: 512K, 4-way, 64 byte lines");            break;
   case 0x23: printf("L3 cache: 1M, 8-way, 64 byte lines");              break;
   case 0x24: printf("L2 cache: 1M, 16-way, 64 byte lines");             break;
   case 0x25: printf("L3 cache: 2M, 8-way, 64 byte lines");              break;
   case 0x29: printf("L3 cache: 4M, 8-way, 64 byte lines");              break;
   case 0x2c: printf("L1 data cache: 32K, 8-way, 64 byte lines");        break;
   case 0x30: printf("L1 cache: 32K, 8-way, 64 byte lines");             break;
   case 0x39: printf("L2 cache: 128K, 4-way, sectored, 64 byte lines");  break;
   case 0x3a: printf("L2 cache: 192K, 6-way, sectored, 64 byte lines");  break;
   case 0x3b: printf("L2 cache: 128K, 2-way, sectored, 64 byte lines");  break;
   case 0x3c: printf("L2 cache: 256K, 4-way, sectored, 64 byte lines");  break;
   case 0x3d: printf("L2 cache: 384K, 6-way, sectored, 64 byte lines");  break;
   case 0x3e: printf("L2 cache: 512K, 4-way, sectored, 64 byte lines");  break;
   case 0x40: if (__F(val_1_eax) <= _XF(0) + _F(6)) {
                 printf("No L2 cache");
              } else {
                 printf("No L3 cache");
              }
              break;
   case 0x41: printf("L2 cache: 128K, 4-way, 32 byte lines");            break;
   case 0x42: printf("L2 cache: 256K, 4-way, 32 byte lines");            break;
   case 0x43: printf("L2 cache: 512K, 4-way, 32 byte lines");            break;
   case 0x44: printf("L2 cache: 1M, 4-way, 32 byte lines");              break;
   case 0x45: printf("L2 cache: 2M, 4-way, 32 byte lines");              break;
   case 0x46: printf("L3 cache: 4M, 4-way, 64 byte lines");              break;
   case 0x47: printf("L3 cache: 8M, 8-way, 64 byte lines");              break;
   case 0x48: printf("L2 cache: 3M, 12-way, 64 byte lines");             break;
   case 0x49: if (__FM(val_1_eax) == _XF(0) + _F(15) + _XM(0) + _M(6)) {
                 printf("L3 cache: 4M, 16-way, 64 byte lines");
              } else {
                 printf("L2 cache: 4M, 16-way, 64 byte lines");
              }
              break;
   case 0x4a: printf("L3 cache: 6M, 12-way, 64 byte lines");             break;
   case 0x4b: printf("L3 cache: 8M, 16-way, 64 byte lines");             break;
   case 0x4c: printf("L3 cache: 12M, 12-way, 64 byte lines");            break;
   case 0x4d: printf("L3 cache: 16M, 16-way, 64 byte lines");            break;
   case 0x4e: printf("L2 cache: 6M, 24-way, 64 byte lines");             break;
   case 0x4f: printf("instruction TLB: 4K pages, 32 entries");           break;
   case 0x50: printf("instruction TLB: 4K & 2M/4M pages, 64 entries");   break;
   case 0x51: printf("instruction TLB: 4K & 2M/4M pages, 128 entries");  break;
   case 0x52: printf("instruction TLB: 4K & 2M/4M pages, 256 entries");  break;
   case 0x55: printf("instruction TLB: 2M/4M pages, fully, 7 entries");  break;
   case 0x56: printf("L1 data TLB: 4M pages, 4-way, 16 entries");        break;
   case 0x57: printf("L1 data TLB: 4K pages, 4-way, 16 entries");        break;
   case 0x59: printf("data TLB: 4K pages, 16 entries");                  break;
   case 0x5a: printf("data TLB: 2M/4M pages, 4-way, 32 entries");        break;
   case 0x5b: printf("data TLB: 4K & 4M pages, 64 entries");             break;
   case 0x5c: printf("data TLB: 4K & 4M pages, 128 entries");            break;
   case 0x5d: printf("data TLB: 4K & 4M pages, 256 entries");            break;
   case 0x60: printf("L1 data cache: 16K, 8-way, 64 byte lines");        break;
   case 0x61: printf("instruction TLB: 4K pages, 48 entries");           break;
   case 0x63: printf("data TLB: 2M/4M pages, 4-way, 32 entries");
              printf(CONT "data TLB: 1G pages, 4-way, 4 entries");       break;
   case 0x64: printf("data TLB: 4K pages, 4-way, 512 entries");          break;
   case 0x66: printf("L1 data cache: 8K, 4-way, 64 byte lines");         break;
   case 0x67: printf("L1 data cache: 16K, 4-way, 64 byte lines");        break;
   case 0x68: printf("L1 data cache: 32K, 4-way, 64 byte lines");        break;
   case 0x6a: printf("micro-data TLB: 4K pages, 8-way, 64 entries");     break;
   case 0x6b: printf("data TLB: 4K pages, 8-way, 256 entries");          break;
   case 0x6c: printf("data TLB: 2M/4M pages, 8-way, 128 entries");       break;
   case 0x6d: printf("data TLB: 1G pages, fully, 16 entries");           break;
   case 0x70: printf("Trace cache: 12K-uop, 8-way");                     break;
   case 0x71: printf("Trace cache: 16K-uop, 8-way");                     break;
   case 0x72: printf("Trace cache: 32K-uop, 8-way");                     break;
   case 0x73: printf("Trace cache: 64K-uop, 8-way");                     break;
   case 0x76: printf("instruction TLB: 2M/4M pages, fully, 8 entries");  break;
   case 0x77: printf("L1 instruction cache: 16K, 4-way, sectored,"
                     " 64 byte lines");                                  break;
   case 0x78: printf("L2 cache: 1M, 4-way, 64 byte lines");              break;
   case 0x79: printf("L2 cache: 128K, 8-way, sectored, 64 byte lines");  break;
   case 0x7a: printf("L2 cache: 256K, 8-way, sectored, 64 byte lines");  break;
   case 0x7b: printf("L2 cache: 512K, 8-way, sectored, 64 byte lines");  break;
   case 0x7c: printf("L2 cache: 1M, 8-way, sectored, 64 byte lines");    break;
   case 0x7d: printf("L2 cache: 2M, 8-way, 64 byte lines");              break;
   case 0x7e: printf("L2 cache: 256K, 8-way, sectored, 128 byte lines"); break;
   case 0x7f: printf("L2 cache: 512K, 2-way, 64 byte lines");            break;
   case 0x80: printf("L2 cache: 512K, 8-way, 64 byte lines");            break;
   case 0x81: printf("L2 cache: 128K, 8-way, 32 byte lines");            break;
   case 0x82: printf("L2 cache: 256K, 8-way, 32 byte lines");            break;
   case 0x83: printf("L2 cache: 512K, 8-way, 32 byte lines");            break;
   case 0x84: printf("L2 cache: 1M, 8-way, 32 byte lines");              break;
   case 0x85: printf("L2 cache: 2M, 8-way, 32 byte lines");              break;
   case 0x86: printf("L2 cache: 512K, 4-way, 64 byte lines");            break;
   case 0x87: printf("L2 cache: 1M, 8-way, 64 byte lines");              break;
   case 0x88: printf("L3 cache: 2M, 4-way, 64 byte lines");              break;
   case 0x89: printf("L3 cache: 4M, 4-way, 64 byte lines");              break;
   case 0x8a: printf("L3 cache: 8M, 4-way, 64 byte lines");              break;
   case 0x8d: printf("L3 cache: 3M, 12-way, 128 byte lines");            break;
   case 0x90: printf("instruction TLB: 4K-256M, fully, 64 entries");     break;
   case 0x96: printf("instruction TLB: 4K-256M, fully, 32 entries");     break;
   case 0x9b: printf("instruction TLB: 4K-256M, fully, 96 entries");     break;
   case 0xa0: printf("data TLB: 4K pages, fully, 32 entries");           break;
   case 0xb0: printf("instruction TLB: 4K, 4-way, 128 entries");         break;
   case 0xb1: printf("instruction TLB: 2M/4M, 4-way, 4/8 entries");      break;
   case 0xb2: printf("instruction TLB: 4K, 4-way, 64 entries");          break;
   case 0xb3: printf("data TLB: 4K pages, 4-way, 128 entries");          break;
   case 0xb4: printf("data TLB: 4K pages, 4-way, 256 entries");          break;
   case 0xb5: printf("instruction TLB: 4K, 8-way, 64 entries");          break;
   case 0xb6: printf("instruction TLB: 4K, 8-way, 128 entries");         break;
   case 0xba: printf("data TLB: 4K pages, 4-way, 64 entries");           break;
   case 0xc0: printf("data TLB: 4K & 4M pages, 4-way, 8 entries");       break;
   case 0xc1: printf("L2 TLB: 4K/2M pages, 8-way, 1024 entries");        break;
   case 0xc2: printf("data TLB: 4K & 2M pages, 4-way, 16 entries");      break;
   case 0xc3: printf("L2 TLB: 4K/2M pages, 6-way, 1536 entries");        break;
   case 0xc4: printf("data TLB: 2M/4M pages, 4-way, 32 entries");        break;
   case 0xca: printf("L2 TLB: 4K pages, 4-way, 512 entries");            break;
   case 0xd0: printf("L3 cache: 512K, 4-way, 64 byte lines");            break;
   case 0xd1: printf("L3 cache: 1M, 4-way, 64 byte lines");              break;
   case 0xd2: printf("L3 cache: 2M, 4-way, 64 byte lines");              break;
   case 0xd6: printf("L3 cache: 1M, 8-way, 64 byte lines");              break;
   case 0xd7: printf("L3 cache: 2M, 8-way, 64 byte lines");              break;
   case 0xd8: printf("L3 cache: 4M, 8-way, 64 byte lines");              break;
   case 0xdc: printf("L3 cache: 1.5M, 12-way, 64 byte lines");           break;
   case 0xdd: printf("L3 cache: 3M, 12-way, 64 byte lines");             break;
   case 0xde: printf("L3 cache: 6M, 12-way, 64 byte lines");             break;
   case 0xe2: printf("L3 cache: 2M, 16-way, 64 byte lines");             break;
   case 0xe3: printf("L3 cache: 4M, 16-way, 64 byte lines");             break;
   case 0xe4: printf("L3 cache: 8M, 16-way, 64 byte lines");             break;
   case 0xea: printf("L3 cache: 12M, 24-way, 64 byte lines");            break;
   case 0xeb: printf("L3 cache: 18M, 24-way, 64 byte lines");            break;
   case 0xec: printf("L3 cache: 24M, 24-way, 64 byte lines");            break;
   case 0xf0: printf("64 byte prefetching");                             break;
   case 0xf1: printf("128 byte prefetching");                            break;
   case 0xfe: printf("TLB data is in CPUID leaf 0x18");                  break;
   case 0xff: printf("cache data is in CPUID leaf 4");                   break;
   default:   printf("unknown");                                         break;
   }

   /*
   ** WARNING: If you add values here, you probably need to update the code in
   **          stash_intel_cache, too.
   */

   printf("\n");
#undef CONT
}

static void
print_4_eax(unsigned int  value)
{
   static ccstring  cache_type[] = { "no more caches (0)",
                                     "data cache (1)",
                                     "instruction cache (2)",
                                     "unified cache (3)" };
   static named_item  names[]
      = { { "cache type"                              ,  0,  4, cache_type },
          { "cache level"                             ,  5,  7, NIL_IMAGES },
          { "self-initializing cache level"           ,  8,  8, bools },
          { "fully associative cache"                 ,  9,  9, bools },
          { "extra threads sharing this cache"        , 14, 25, NIL_IMAGES },
          { "extra processor cores on this die"       , 26, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 36);
}

static void
print_4_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "system coherency line size"              ,  0, 11, NIL_IMAGES },
          { "physical line partitions"                , 12, 21, NIL_IMAGES },
          { "ways of associativity"                   , 22, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 36);
}

static void
print_4_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "number of sets - 1"                      ,  0, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 36);
}

static void
print_4_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "WBINVD/INVD behavior on lower caches"    ,  0,  0, bools },
          { "inclusive to lower caches"               ,  1,  1, bools },
          { "complex cache indexing"                  ,  2,  2, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 36);
}

static void
print_5_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "smallest monitor-line size (bytes)"      ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_5_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "largest monitor-line size (bytes)"       ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_5_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "enum of Monitor-MWAIT exts supported"    ,  0,  0, bools },
          { "supports intrs as break-event for MWAIT" ,  1,  1, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_5_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "number of C0 sub C-states using MWAIT"   ,  0,  3, NIL_IMAGES },
          { "number of C1 sub C-states using MWAIT"   ,  4,  7, NIL_IMAGES },
          { "number of C2 sub C-states using MWAIT"   ,  8, 11, NIL_IMAGES },
          { "number of C3 sub C-states using MWAIT"   , 12, 15, NIL_IMAGES },
          { "number of C4 sub C-states using MWAIT"   , 16, 19, NIL_IMAGES },
          { "number of C5 sub C-states using MWAIT"   , 20, 23, NIL_IMAGES },
          { "number of C6 sub C-states using MWAIT"   , 24, 27, NIL_IMAGES },
          { "number of C7 sub C-states using MWAIT"   , 28, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_6_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "digital thermometer"                     ,  0,  0, bools },
          { "Intel Turbo Boost Technology"            ,  1,  1, bools },
          { "ARAT always running APIC timer"          ,  2,  2, bools },
          { "PLN power limit notification"            ,  4,  4, bools },
          { "ECMD extended clock modulation duty"     ,  5,  5, bools },
          { "PTM package thermal management"          ,  6,  6, bools },
          { "HWP base registers"                      ,  7,  7, bools },
          { "HWP notification"                        ,  8,  8, bools },
          { "HWP activity window"                     ,  9,  9, bools },
          { "HWP energy performance preference"       , 10, 10, bools },
          { "HWP package level request"               , 11, 11, bools },
          { "HDC base registers"                      , 13, 13, bools },
          { "Intel Turbo Boost Max Technology 3.0"    , 14, 14, bools },
          { "HWP capabilities"                        , 15, 15, bools },
          { "HWP PECI override"                       , 16, 16, bools },
          { "flexible HWP"                            , 17, 17, bools },
          { "IA32_HWP_REQUEST MSR fast access mode"   , 18, 18, bools },
          { "ignoring idle logical processor HWP req" , 20, 20, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 39);
}

static void
print_6_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "digital thermometer thresholds"          ,  0,  3, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 39);
}

static void
print_6_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "hardware coordination feedback"          ,  0,  0, bools },
          { "ACNT2 available"                         ,  1,  1, bools },
          { "performance-energy bias capability"      ,  3,  3, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 39);
}

static void
print_7_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "FSGSBASE instructions"                   ,  0,  0, bools },
          { "IA32_TSC_ADJUST MSR supported"           ,  1,  1, bools },
          { "SGX: Software Guard Extensions supported",  2,  2, bools },
          { "BMI1 instructions"                       ,  3,  3, bools },
          { "HLE hardware lock elision"               ,  4,  4, bools },
          { "AVX2: advanced vector extensions 2"      ,  5,  5, bools },
          { "FDP_EXCPTN_ONLY"                         ,  6,  6, bools },
          { "SMEP supervisor mode exec protection"    ,  7,  7, bools },
          { "BMI2 instructions"                       ,  8,  8, bools },
          { "enhanced REP MOVSB/STOSB"                ,  9,  9, bools },
          { "INVPCID instruction"                     , 10, 10, bools },
          { "RTM: restricted transactional memory"    , 11, 11, bools },
          { "RDT-M: Intel RDT monitoring"             , 12, 12, bools },
          { "deprecated FPU CS/DS"                    , 13, 13, bools },
          { "MPX: intel memory protection extensions" , 14, 14, bools },
          { "RDT-A: Intel RDT allocation"             , 15, 15, bools },
          { "AVX512F: AVX-512 foundation instructions", 16, 16, bools },
          { "AVX512DQ: double & quadword instructions", 17, 17, bools },
          { "RDSEED instruction"                      , 18, 18, bools },
          { "ADX instructions"                        , 19, 19, bools },
          { "SMAP: supervisor mode access prevention" , 20, 20, bools },
          { "AVX512IFMA: fused multiply add"          , 21, 21, bools },
          { "PCOMMIT instruction"                     , 22, 22, bools },
          { "CLFLUSHOPT instruction"                  , 23, 23, bools },
          { "CLWB instruction"                        , 24, 24, bools },
          { "Intel processor trace"                   , 25, 25, bools },
          { "AVX512PF: prefetch instructions"         , 26, 26, bools },
          { "AVX512ER: exponent & reciprocal instrs"  , 27, 27, bools },
          { "AVX512CD: conflict detection instrs"     , 28, 28, bools },
          { "SHA instructions"                        , 29, 29, bools },
          { "AVX512BW: byte & word instructions"      , 30, 30, bools },
          { "AVX512VL: vector length"                 , 31, 31, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_7_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "PREFETCHWT1"                             ,  0,  0, bools },
          { "AVX512VBMI: vector byte manipulation"    ,  1,  1, bools },
          { "UMIP: user-mode instruction prevention"  ,  2,  2, bools },
          { "PKU protection keys for user-mode"       ,  3,  3, bools },
          { "OSPKE CR4.PKE and RDPKRU/WRPKRU"         ,  4,  4, bools },
          { "WAITPKG instructions"                    ,  5,  5, bools },
          { "AVX512_VBMI2"                            ,  6,  6, bools },
          { "CET_SS: CET shadow stack"                ,  7,  7, bools },
          { "GFNI: Galois Field New Instructions"     ,  8,  8, bools },
          { "VAES instructions"                       ,  9,  9, bools },
          { "VPCLMULQDQ instruction"                  , 10, 10, bools },
          { "AVX512_VNNI"                             , 11, 11, bools },
          { "AVX512_BITALG: bit count/shiffle"        , 12, 12, bools },
          { "AVX512: VPOPCNTDQ instruction"           , 14, 14, bools },
          { "5-level paging"                          , 16, 16, bools },
          { "BNDLDX/BNDSTX MAWAU value in 64-bit mode", 17, 21, NIL_IMAGES },
          { "RDPID: read processor D supported"       , 22, 22, bools },
          { "CLDEMOTE supports cache line demote"     , 25, 25, bools },
          { "MOVDIRI instruction"                     , 27, 27, bools },
          { "MOVDIR64B intruction"                    , 28, 28, bools },
          { "SGX_LC: SGX launch config supported"     , 30, 30, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_7_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "AVX512_4VNNIW: neural network instrs"    ,  2,  2, bools },
          { "AVX512_4FMAPS: multiply acc single prec" ,  3,  3, bools },
          { "fast short REP MOV"                      ,  4,  4, bools },
          { "PCONFIG"                                 , 18, 18, bools },
          { "CET_IBT: CET indirect branch tracking"   , 20, 20, bools },
      };
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_a_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "version ID"                              ,  0,  7, NIL_IMAGES },
          { "number of counters per logical processor",  8, 15, NIL_IMAGES },
          { "bit width of counter"                    , 16, 23, NIL_IMAGES },
          { "length of EBX bit vector"                , 24, 31, NIL_IMAGES },
        };

   printf("   Architecture Performance Monitoring Features (0xa/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_a_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "core cycle event not available"          ,  0,  0, bools },
          { "instruction retired event not available" ,  1,  1, bools },
          { "reference cycles event not available"    ,  2,  2, bools },
          { "last-level cache ref event not available",  3,  3, bools },
          { "last-level cache miss event not avail"   ,  4,  4, bools },
          { "branch inst retired event not available" ,  5,  5, bools },
          { "branch mispred retired event not avail"  ,  6,  6, bools },
        };

   printf("   Architecture Performance Monitoring Features (0xa/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_a_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "number of fixed counters"                ,  0,  4, NIL_IMAGES },
          { "bit width of fixed counters"             ,  5, 12, NIL_IMAGES },
          { "anythread deprecation"                   , 15, 15, bools },
        };

   printf("   Architecture Performance Monitoring Features (0xa/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_b_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "bits to shift APIC ID to get next"       ,  0,  4, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 33);
}

static void
print_b_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "logical processors at this level"        ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 33);
}

static void
print_b_ecx(unsigned int  value)
{
   static ccstring  level_type[] = { "invalid (0)",
                                     "thread (1)",
                                     "core (2)" };
   static named_item  names[]
      = { { "level number"                            ,  0,  7, NIL_IMAGES },
          { "level type"                              ,  8, 15, level_type },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 33);
}

static void
print_d_0_eax(unsigned int  value)
{
   /*
   ** State component bitmaps in general are described in 325462: Intel 64 and
   ** IA-32 Architectures Software Developer's Manual Combined Volumes: 1, 2A,
   ** 2B, 2C, 3A, 3B, and 3C, Volume 1: Basic Architecture, section 13.1:
   ** XSAVE-Supported Features and State-Component Bitmaps.  This leaf describes
   ** which of the bits are actually supported by the hardware, and is described
   ** better in 13.2: Enumeration of CPU Support for XSAVE Instructions and
   ** XSAVE-Supported Features.
   ** 
   ** These align with the supported features[] in print_d_n() for values > 1.
   */
   static named_item  names[]
      = { { "   XCR0 supported: x87 state"            ,  0,  0, bools },
          { "   XCR0 supported: SSE state"            ,  1,  1, bools },
          { "   XCR0 supported: AVX state"            ,  2,  2, bools },
          { "   XCR0 supported: MPX BNDREGS"          ,  3,  3, bools },
          { "   XCR0 supported: MPX BNDCSR"           ,  4,  4, bools },
          { "   XCR0 supported: AVX-512 opmask"       ,  5,  5, bools },
          { "   XCR0 supported: AVX-512 ZMM_Hi256"    ,  6,  6, bools },
          { "   XCR0 supported: AVX-512 Hi16_ZMM"     ,  7,  7, bools },
          { "   IA32_XSS supported: PT state"         ,  8,  8, bools },
          { "   XCR0 supported: PKRU state"           ,  9,  9, bools },
          { "   IA32_XSS supported: HDC state"        , 13, 13, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 39);
}

static void
print_d_1_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "XSAVEOPT instruction"                    ,  0,  0, bools },
          { "XSAVEC instruction"                      ,  1,  1, bools },
          { "XGETBV instruction"                      ,  2,  2, bools },
          { "XSAVES/XRSTORS instructions"             ,  3,  3, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 43);
}

static void
print_d_n_ecx(unsigned int  value)
{
   static ccstring  which[] = { "XCR0 (user state)",
                                "IA32_XSS (supervisor state)" };

   static named_item  names[]
      = { { "supported in IA32_XSS or XCR0"           ,  0,  0, which },
          { "64-byte alignment in compacted XSAVE"    ,  1,  1, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 40);
}

static void
print_d_n(const unsigned int  words[WORD_NUM],
          unsigned int        try)
{
   /*
   ** The XSAVE areas are explained in 325462: Intel 64 and IA-32 Architectures
   ** Software Developer's Manual Combined Volumes: 1, 2A, 2B, 2C, 3A, 3B, and
   ** 3C, Volume 1: Basic Architecture, section 13.1: XSAVE-Supported Features
   ** and State-Component Bitmaps.
   ** 
   ** These align with the supported feature names[] in print_d_0_eax() for
   ** values > 1.
   */
   static ccstring features[64] = { /*  0 => */ "internal error",
                                    /*  1 => */ "internal error",
                                    /*  2 => */ "AVX/YMM",
                                    /*  3 => */ "MPX BNDREGS",
                                    /*  4 => */ "MPX BNDCSR",
                                    /*  5 => */ "AVX-512 opmask",
                                    /*  6 => */ "AVX-512 ZMM_Hi256",
                                    /*  7 => */ "AVX-512 Hi16_ZMM",
                                    /*  8 => */ "PT",
                                    /*  9 => */ "PKRU",
                                    /* 10 => */ "unknown",
                                    /* 11 => */ "unknown",
                                    /* 12 => */ "unknown",
                                    /* 13 => */ "HDC",
                                    /* 14 => */ "unknown",
                                    /* 15 => */ "unknown",
                                    /* 16 => */ "unknown",
                                    /* 17 => */ "unknown",
                                    /* 18 => */ "unknown",
                                    /* 19 => */ "unknown",
                                    /* 20 => */ "unknown",
                                    /* 21 => */ "unknown",
                                    /* 22 => */ "unknown",
                                    /* 23 => */ "unknown",
                                    /* 24 => */ "unknown",
                                    /* 25 => */ "unknown",
                                    /* 26 => */ "unknown",
                                    /* 27 => */ "unknown",
                                    /* 28 => */ "unknown",
                                    /* 29 => */ "unknown",
                                    /* 30 => */ "unknown",
                                    /* 31 => */ "unknown",
                                    /* 32 => */ "unknown",
                                    /* 33 => */ "unknown",
                                    /* 34 => */ "unknown",
                                    /* 35 => */ "unknown",
                                    /* 36 => */ "unknown",
                                    /* 37 => */ "unknown",
                                    /* 38 => */ "unknown",
                                    /* 39 => */ "unknown",
                                    /* 40 => */ "unknown",
                                    /* 41 => */ "unknown",
                                    /* 42 => */ "unknown",
                                    /* 43 => */ "unknown",
                                    /* 44 => */ "unknown",
                                    /* 45 => */ "unknown",
                                    /* 46 => */ "unknown",
                                    /* 47 => */ "unknown",
                                    /* 48 => */ "unknown",
                                    /* 49 => */ "unknown",
                                    /* 50 => */ "unknown",
                                    /* 51 => */ "unknown",
                                    /* 52 => */ "unknown",
                                    /* 53 => */ "unknown",
                                    /* 54 => */ "unknown",
                                    /* 55 => */ "unknown",
                                    /* 56 => */ "unknown",
                                    /* 57 => */ "unknown",
                                    /* 58 => */ "unknown",
                                    /* 59 => */ "unknown",
                                    /* 60 => */ "unknown",
                                    /* 61 => */ "unknown",
                                    /* 62 => */ "LWP",     // AMD only
                                    /* 63 => */ "unknown" };

   ccstring  feature     = features[try];
   int       feature_pad = 17-strlen(feature);

   if (try > 9) {
      printf("   %s features (0xd/0x%x):\n", feature, try);
   } else {
      printf("   %s features (0xd/%d):\n", feature, try);
   }
   printf("      %s save state byte size%*s   = 0x%08x (%u)\n",
          feature, feature_pad, "",
          words[WORD_EAX], words[WORD_EAX]);
   printf("      %s save state byte offset%*s = 0x%08x (%u)\n",
          feature, feature_pad, "",
          words[WORD_EBX], words[WORD_EBX]);
   print_d_n_ecx(words[WORD_ECX]);
}

static void
print_f_0_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "supports L3 cache QoS monitoring"        ,  0,  0, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_f_1_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "supports L3 occupancy monitoring"        ,  0,  0, bools },
          { "supports L3 total bandwidth monitoring"  ,  1,  1, bools },
          { "supports L3 local bandwidth monitoring"  ,  2,  2, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_10_0_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "L3 cache allocation technology supported",  1,  1, bools },
          { "L2 cache allocation technology supported",  2,  2, bools },
          { "memory bandwidth allocation supported"   ,  3,  3, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_10_n_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "length of capacity bit mask - 1"         ,  0,  4, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_10_n_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "infrequent updates of COS"               ,  1,  1, bools },
          { "code and data prioritization supported"  ,  2,  2, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_10_n_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "highest COS number supported"            ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_12_0_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "SGX1 supported"                          ,  0,  0, bools },
          { "SGX2 supported"                          ,  1,  1, bools },
          { "SGX ENCLV E*VIRTCHILD, ESETCONTEXT"      ,  5,  5, bools },
          { "SGX ENCLS ETRACKC, ERDINFO, ELDBC, ELDUC",  6,  6, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 38);
}

static void
print_12_0_ebx(unsigned int  value)
{
   /*
   ** MISCSELECT is described in Table 38-4: Bit Vector Layout of MISCSELECT
   ** Field of Extended Information.
   */
   static named_item  names[]
      = { { "MISCSELECT.EXINFO supported: #PF & #GP"  ,  0,  0, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 38);
}

static void
print_12_0_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "MaxEnclaveSize_Not64 (log2)"             ,  0,  7, NIL_IMAGES },
          { "MaxEnclaveSize_64 (log2)"                ,  8, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 38);
}

static void
print_12_n_1_ecx(unsigned int  value)
{
   static ccstring props[16] = { /* 0 => */ "enumerated as 0",
                                 /* 1 => */ "confidentiality & integrity"
                                            " protection" };

   static named_item  names[]
      = { { "section property"                        ,  0,  3, props },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 23);
}

static void
print_14_0_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "IA32_RTIT_CR3_MATCH is accessible"       ,  0,  0, bools },
          { "configurable PSB & cycle-accurate"       ,  1,  1, bools },
          { "IP & TraceStop filtering; PT preserve"   ,  2,  2, bools },
          { "MTC timing packet; suppress COFI-based"  ,  3,  3, bools },
          { "PTWRITE support"                         ,  4,  4, bools },
          { "power event trace support"               ,  5,  5, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_14_0_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "ToPA output scheme support"              ,  0,  0, bools },
          { "ToPA can hold many output entries"       ,  1,  1, bools },
          { "single-range output scheme support"      ,  2,  2, bools },
          { "output to trace transport"               ,  3,  3, bools },
          { "IP payloads have LIP values & CS"        , 31, 31, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_14_1_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "configurable address ranges"             ,  0,  2, NIL_IMAGES },
          { "supported MTC periods bitmask"           , 16, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_14_1_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "supported cycle threshold bitmask"       ,  0, 15, NIL_IMAGES },
          { "supported config PSB freq bitmask"       , 16, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_16_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "Core Base Frequency (MHz)"               ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_16_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "Core Maximum Frequency (MHz)"            ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_16_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "Bus (Reference) Frequency (MHz)"         ,  0, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_17_0_ebx(unsigned int  value)
{
   static ccstring schemes[] = { /* 0 => */ "assigned by intel",
                                 /* 1 => */ "industry standard" };

   static named_item  names[]
      = { { "vendor id"                               ,  0, 15, NIL_IMAGES },
          { "vendor scheme"                           , 16, 16, schemes },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_18_n_ebx(unsigned int  value)
{
   static ccstring parts[] = { /* 0 => */ "soft between logical processors",
                               /* 1 => */ NULL,
                               /* 2 => */ NULL,
                               /* 3 => */ NULL,
                               /* 4 => */ NULL,
                               /* 5 => */ NULL,
                               /* 6 => */ NULL,
                               /* 7 => */ NULL };

   static named_item  names[]
      = { { "4KB page size entries supported"         ,  0,  0, bools },
          { "2MB page size entries supported"         ,  1,  1, bools },
          { "4MB page size entries supported"         ,  2,  2, bools },
          { "1GB page size entries supported"         ,  3,  3, bools },
          { "partitioning"                            ,  8, 10, parts },
          { "ways of associativity"                   , 16, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_18_n_edx(unsigned int  value)
{
   static ccstring tlbs[16] = { /* 0000b => */ "invalid (0)",    
                                /* 0001b => */ "data TLB",       
                                /* 0010b => */ "instruction TLB",
                                /* 0011b => */ "unified TLB", };

   static named_item  names[]
      = { { "translation cache type"                  ,  0,  4, tlbs },
          { "translation cache level - 1"             ,  5,  7, NIL_IMAGES },
          { "fully associative"                       ,  8,  8, bools },
          { "maximum number of addressible IDs"       , 14, 25, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_1b_n_eax(unsigned int  value)
{
   static ccstring types[1<<12] = { /* 0 => */ "invalid (0)",    
                                    /* 1 => */ "target identifier (1)" };

   static named_item  names[]
      = { { "sub-leaf type"                           ,  0, 11, types },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000001_eax_kvm(unsigned int  value)
{
   static named_item  names[]
      = { { "kvmclock available at MSR 0x11"          ,  0,  0, bools },
          { "delays unnecessary for PIO ops"          ,  1,  1, bools },
          { "mmu_op"                                  ,  2,  2, bools },
          { "kvmclock available a MSR 0x4b564d00"     ,  3,  3, bools },
          { "async pf enable available by MSR"        ,  4,  4, bools },
          { "steal clock supported"                   ,  5,  5, bools },
          { "guest EOI optimization enabled"          ,  6,  6, bools },
          { "stable: no guest per-cpu warps expected" , 24, 24, bools },
        };

   printf("   hypervisor features (0x40000001/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000002_ecx_xen(unsigned int  value)
{
   static named_item  names[]
      = { { "MMU_PT_UPDATE_PRESERVE_AD supported"     ,  0,  0, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000003_eax_xen(unsigned int  value)
{
   static named_item  names[]
      = { { "vtsc"                                    ,  0,  0, bools },
          { "host tsc is safe"                        ,  1,  1, bools },
          { "boot cpu has RDTSCP"                     ,  2,  2, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000003_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "VP run time"                             ,  0,  0, bools },
          { "partition reference counter"             ,  1,  1, bools },
          { "basic synIC MSRs"                        ,  2,  2, bools },
          { "synthetic timer MSRs"                    ,  3,  3, bools },
          { "APIC access MSRs"                        ,  4,  4, bools },
          { "hypercall MSRs"                          ,  5,  5, bools },
          { "access virtual process index MSR"        ,  6,  6, bools },
          { "virtual system reset MSR"                ,  7,  7, bools },
          { "map/unmap statistics pages MSR"          ,  8,  8, bools },
          { "reference TSC access"                    ,  9,  9, bools },
          { "guest idle state MSR"                    , 10, 10, bools },
          { "TSC/APIC frequency MSRs"                 , 11, 11, bools },
          { "guest debugging MSRs"                    , 12, 12, bools },
        };
        

   printf("   hypervisor feature identification (0x40000003/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000003_ebx_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "CreatePartitions"                        ,  0,  0, bools },
          { "AccessPartitionId"                       ,  1,  1, bools },
          { "AccessMemoryPool"                        ,  2,  2, bools },
          { "AdjustMessageBuffers"                    ,  3,  3, bools },
          { "PostMessages"                            ,  4,  4, bools },
          { "SignalEvents"                            ,  5,  5, bools },
          { "CreatePort"                              ,  6,  6, bools },
          { "ConnectPort"                             ,  7,  7, bools },
          { "AccessStats"                             ,  8,  8, bools },
          { "Debugging"                               , 11, 11, bools },
          { "CPUManagement"                           , 12, 12, bools },
          { "ConfigureProfiler"                       , 13, 13, bools },
          { "AccessVSM"                               , 16, 16, bools },
          { "AccessVpRegisters"                       , 17, 17, bools },
          { "EnableExtendedHypercalls"                , 20, 20, bools },
          { "StartVirtualProcessor"                   , 21, 21, bools },
        };

   printf("   hypervisor partition creation flags (0x40000003/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000003_ecx_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "maximum process power state"             ,  0,  3, NIL_IMAGES },
        };

   printf("   hypervisor power management features (0x40000003/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000003_edx_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "MWAIT available"                         ,  0,  0, bools },
          { "guest debugging support available"       ,  1,  1, bools },
          { "performance monitor support available"   ,  2,  2, bools },
          { "CPU dynamic partitioning events avail"   ,  3,  3, bools },
          { "hypercall XMM input parameters available",  4,  4, bools },
          { "virtual guest idle state available"      ,  5,  5, bools },
          { "hypervisor sleep state available"        ,  6,  6, bools },
          { "query NUMA distance available"           ,  7,  7, bools },
          { "determine timer frequency available"     ,  8,  8, bools },
          { "inject synthetic machine check available",  9,  9, bools },
          { "guest crash MSRs available"              , 10, 10, bools },
          { "debug MSRs available"                    , 11, 11, bools },
          { "NPIEP available"                         , 12, 12, bools },
          { "disable hypervisor available"            , 13, 13, bools },
          { "extended gva ranges for flush virt addrs", 14, 14, bools },
          { "hypercall XMM register return available" , 15, 15, bools },
          { "sint polling mode available"             , 17, 17, bools },
          { "hypercall MSR lock available"            , 18, 18, bools },
          { "use direct synthetic timers"             , 19, 19, bools },
        };

   printf("   hypervisor feature identification (0x40000003/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000004_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "use hypercalls for AS switches"          ,  0,  0, bools },
          { "use hypercalls for local TLB flushes"    ,  1,  1, bools },
          { "use hypercalls for remote TLB flushes"   ,  2,  2, bools },
          { "use MSRs to access EOI, ICR, TPR"        ,  3,  3, bools },
          { "use MSRs to initiate system RESET"       ,  4,  4, bools },
          { "use relaxed timing"                      ,  5,  5, bools },
          { "use DMA remapping"                       ,  6,  6, bools },
          { "use interrupt remapping"                 ,  7,  7, bools },
          { "use x2APIC MSRs"                         ,  8,  8, bools },
          { "deprecate AutoEOI"                       ,  9,  9, bools },
          { "use SyntheticClusterIpi hypercall"       , 10, 10, bools },
          { "use ExProcessorMasks"                    , 11, 11, bools },
          { "hypervisor is nested with Hyper-V"       , 12, 12, bools },
          { "use INT for MBEC system calls"           , 13, 13, bools },
          { "use enlightened VMCS interface"          , 14, 14, bools },
        };

   printf("   hypervisor recommendations (0x40000004/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000006_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "APIC overlay assist"                     ,  0,  0, bools },
          { "MSR bitmaps"                             ,  1,  1, bools },
          { "performance counters"                    ,  2,  2, bools },
          { "second-level address translation"        ,  3,  3, bools },
          { "DMA remapping"                           ,  4,  4, bools },
          { "interrupt remapping"                     ,  5,  5, bools },
          { "memory patrol scrubber"                  ,  6,  6, bools },
          { "DMA protection"                          ,  7,  7, bools },
          { "HPET requested"                          ,  8,  8, bools },
          { "synthetic timers are volatile"           ,  9,  9, bools },
        };

   printf("   hypervisor hardware features used (0x40000006/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000007_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "StartLogicalProcessor"                   ,  0,  0, bools },
          { "CreateRootvirtualProcessor"              ,  1,  1, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000007_ebx_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "ProcessorPowerManagement"                ,  0,  0, bools },
          { "MwaitIdleStates"                         ,  1,  1, bools },
          { "LogicalProcessorIdling"                  ,  2,  2, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000008_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "SvmSupported"                            ,  0,  0, bools },
          { "MaxPasidSpacePasidCount"                 , 11, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000009_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "AccessSynicRegs"                         ,  2,  2, bools },
          { "AccessIntrCtrlRegs"                      ,  4,  4, bools },
          { "AccessHypercallMsrs"                     ,  5,  5, bools },
          { "AccessVpIndex"                           ,  6,  6, bools },
          { "AccessReenlightenmentControls"           , 12, 12, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_40000009_edx_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "XmmRegistersForFastHypercallAvailable"   ,  4,  4, bools },
          { "FastHypercallOutputAvailable"            , 15, 15, bools },
          { "SintPoillingModeAvailable"               , 17, 17, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_4000000a_eax_microsoft(unsigned int  value)
{
   static named_item  names[]
      = { { "enlightened VMCS version (low)"          ,  0,  7, NIL_IMAGES },
          { "enlightened VMCS version (high)"         ,  8, 15, NIL_IMAGES },
          { "direct virtual flush hypercalls support" , 17, 17, bools },
          { "HvFlushGuestPhysicalAddress* hypercalls" , 18, 18, bools },
          { "enlightened MSR bitmap support"          , 19, 19, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_eax_amd(unsigned int  value)
{
   static ccstring  family[]   = { NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   "AMD Am486 (4)",
                                   "AMD K5 (5)",
                                   "AMD K6 (6)",
                                   "AMD Athlon/Duron (7)",
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   "AMD Athlon 64/Opteron/Sempron/Turion (15)" };
   static named_item  names[]
      = { { "family/generation"                       ,  8, 11, family },
          { "model"                                   ,  4,  7, NIL_IMAGES },
          { "stepping id"                             ,  0,  3, NIL_IMAGES },
          { "extended family"                         , 20, 27, NIL_IMAGES },
          { "extended model"                          , 16, 19, NIL_IMAGES },
        };

   printf("   extended processor signature (0x80000001/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   print_x_synth_amd(value);
}

static void
print_80000001_eax_via(unsigned int  value)
{
   static ccstring  family[16] = { NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   "VIA C3" };
   static named_item  names[]
      = { { "generation"                              ,  8, 11, family },
          { "model"                                   ,  4,  7, NIL_IMAGES },
          { "stepping"                                ,  0,  3, NIL_IMAGES },
        };

   printf("   extended processor signature (0x80000001/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   print_x_synth_via(value);
}

static void
print_80000001_eax_transmeta(unsigned int  value)
{
   static ccstring  family[16] = { NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   "Transmeta Crusoe" };
   static named_item  names[]
      = { { "generation"                              ,  8, 11, family },
          { "model"                                   ,  4,  7, NIL_IMAGES },
          { "stepping"                                ,  0,  3, NIL_IMAGES },
        };

   printf("   extended processor signature (0x80000001/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   print_synth_transmeta("      (simple synth)", value, NULL);
}

static void
print_80000001_eax(unsigned int  value,
                   vendor_t      vendor)
{
   switch (vendor) {
   case VENDOR_AMD:
      print_80000001_eax_amd(value);
      break;
   case VENDOR_VIA:
      print_80000001_eax_via(value);
      break;
   case VENDOR_TRANSMETA:
      print_80000001_eax_transmeta(value);
      break;
   case VENDOR_INTEL:
   case VENDOR_CYRIX:
   case VENDOR_UMC:
   case VENDOR_NEXGEN:
   case VENDOR_RISE:
   case VENDOR_SIS:
   case VENDOR_NSC:
   case VENDOR_VORTEX:
   case VENDOR_RDC:
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

static void
print_80000001_edx_intel(unsigned int  value)
{
   static named_item  names[]
      = { { "SYSCALL and SYSRET instructions"         , 11, 11, bools },
          { "execution disable"                       , 20, 20, bools },
          { "1-GB large page support"                 , 26, 26, bools },
          { "RDTSCP"                                  , 27, 27, bools },
          { "64-bit extensions technology available"  , 29, 29, bools },
        };

   printf("   extended feature flags (0x80000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_edx_amd(unsigned int  value)
{
   static named_item  names[]
      = { { "x87 FPU on chip"                         ,  0,  0, bools },
          { "virtual-8086 mode enhancement"           ,  1,  1, bools },
          { "debugging extensions"                    ,  2,  2, bools },
          { "page size extensions"                    ,  3,  3, bools },
          { "time stamp counter"                      ,  4,  4, bools },
          { "RDMSR and WRMSR support"                 ,  5,  5, bools },
          { "physical address extensions"             ,  6,  6, bools },
          { "machine check exception"                 ,  7,  7, bools },
          { "CMPXCHG8B inst."                         ,  8,  8, bools },
          { "APIC on chip"                            ,  9,  9, bools },
          { "SYSCALL and SYSRET instructions"         , 11, 11, bools },
          { "memory type range registers"             , 12, 12, bools },
          { "global paging extension"                 , 13, 13, bools },
          { "machine check architecture"              , 14, 14, bools },
          { "conditional move/compare instruction"    , 15, 15, bools },
          { "page attribute table"                    , 16, 16, bools },
          { "page size extension"                     , 17, 17, bools },
          { "multiprocessing capable"                 , 19, 19, bools },
          { "no-execute page protection"              , 20, 20, bools },
          { "AMD multimedia instruction extensions"   , 22, 22, bools },
          { "MMX Technology"                          , 23, 23, bools },
          { "FXSAVE/FXRSTOR"                          , 24, 24, bools },
          { "SSE extensions"                          , 25, 25, bools },
          { "1-GB large page support"                 , 26, 26, bools },
          { "RDTSCP"                                  , 27, 27, bools },
          { "long mode (AA-64)"                       , 29, 29, bools },
          { "3DNow! instruction extensions"           , 30, 30, bools },
          { "3DNow! instructions"                     , 31, 31, bools },
        };

   printf("   extended feature flags (0x80000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_edx_cyrix_via(unsigned int  value)
{
   static named_item  names[]
      = { { "x87 FPU on chip"                         ,  0,  0, bools },
          { "virtual-8086 mode enhancement"           ,  1,  1, bools },
          { "debugging extensions"                    ,  2,  2, bools },
          { "page size extensions"                    ,  3,  3, bools },
          { "time stamp counter"                      ,  4,  4, bools },
          { "RDMSR and WRMSR support"                 ,  5,  5, bools },
          { "physical address extensions"             ,  6,  6, bools },
          { "machine check exception"                 ,  7,  7, bools },
          { "CMPXCHG8B inst."                         ,  8,  8, bools },
          { "APIC on chip"                            ,  9,  9, bools },
          { "SYSCALL and SYSRET instructions"         , 11, 11, bools },
          { "memory type range registers"             , 12, 12, bools },
          { "global paging extension"                 , 13, 13, bools },
          { "machine check architecture"              , 14, 14, bools },
          { "conditional move/compare instruction"    , 15, 15, bools },
          { "page attribute table"                    , 16, 16, bools },
          { "page size extension"                     , 17, 17, bools },
          { "multiprocessing capable"                 , 19, 19, bools },
          { "AMD multimedia instruction extensions"   , 22, 22, bools },
          { "MMX Technology"                          , 23, 23, bools },
          { "extended MMX"                            , 24, 24, bools },
          { "SSE extensions"                          , 25, 25, bools },
          { "AA-64"                                   , 29, 29, bools },
          { "3DNow! instruction extensions"           , 30, 30, bools },
          { "3DNow! instructions"                     , 31, 31, bools },
        };

   printf("   extended feature flags (0x80000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_edx_transmeta(unsigned int  value)
{
   static named_item  names[]
      = { { "x87 FPU on chip"                         ,  0,  0, bools },
          { "virtual-8086 mode enhancement"           ,  1,  1, bools },
          { "debugging extensions"                    ,  2,  2, bools },
          { "page size extensions"                    ,  3,  3, bools },
          { "time stamp counter"                      ,  4,  4, bools },
          { "RDMSR and WRMSR support"                 ,  5,  5, bools },
          { "CMPXCHG8B inst."                         ,  8,  8, bools },
          { "APIC on chip"                            ,  9,  9, bools },
          { "memory type range registers"             , 12, 12, bools },
          { "global paging extension"                 , 13, 13, bools },
          { "machine check architecture"              , 14, 14, bools },
          { "conditional move/compare instruction"    , 15, 15, bools },
          { "FP conditional move instructions"        , 16, 16, bools },
          { "page size extension"                     , 17, 17, bools },
          { "AMD multimedia instruction extensions"   , 22, 22, bools },
          { "MMX Technology"                          , 23, 23, bools },
          { "FXSAVE/FXRSTOR"                          , 24, 24, bools },
        };

   printf("   extended feature flags (0x80000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_edx_nsc(unsigned int  value)
{
   static named_item  names[]
      = { { "x87 FPU on chip"                         ,  0,  0, bools },
          { "virtual-8086 mode enhancement"           ,  1,  1, bools },
          { "debugging extensions"                    ,  2,  2, bools },
          { "page size extensions"                    ,  3,  3, bools },
          { "time stamp counter"                      ,  4,  4, bools },
          { "RDMSR and WRMSR support"                 ,  5,  5, bools },
          { "machine check exception"                 ,  7,  7, bools },
          { "CMPXCHG8B inst."                         ,  8,  8, bools },
          { "SYSCALL and SYSRET instructions"         , 11, 11, bools },
          { "global paging extension"                 , 13, 13, bools },
          { "conditional move/compare instruction"    , 15, 15, bools },
          { "FPU conditional move instruction"        , 16, 16, bools },
          { "MMX Technology"                          , 23, 23, bools },
          { "6x86MX multimedia extensions"            , 24, 24, bools },
        };

   printf("   extended feature flags (0x80000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_edx(unsigned int  value,
                   vendor_t      vendor)
{
   switch (vendor) {
   case VENDOR_INTEL:
      print_80000001_edx_intel(value);
      break;
   case VENDOR_AMD:
      print_80000001_edx_amd(value);
      break;
   case VENDOR_CYRIX:
   case VENDOR_VIA:
      print_80000001_edx_cyrix_via(value);
      break;
   case VENDOR_TRANSMETA:
      print_80000001_edx_transmeta(value);
      break;
   case VENDOR_NSC:
      print_80000001_edx_nsc(value);
      break;
   case VENDOR_UMC:
   case VENDOR_NEXGEN:
   case VENDOR_RISE:
   case VENDOR_SIS:
   case VENDOR_VORTEX:
   case VENDOR_RDC:
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

static void
print_80000001_ecx_amd(unsigned int  value)
{
   static named_item  names[]
      = { { "LAHF/SAHF supported in 64-bit mode"      ,  0,  0, bools },
          { "CMP Legacy"                              ,  1,  1, bools },
          { "SVM: secure virtual machine"             ,  2,  2, bools },
          { "extended APIC space"                     ,  3,  3, bools },
          { "AltMovCr8"                               ,  4,  4, bools },
          { "LZCNT advanced bit manipulation"         ,  5,  5, bools },
          { "SSE4A support"                           ,  6,  6, bools },
          { "misaligned SSE mode"                     ,  7,  7, bools },
          { "3DNow! PREFETCH/PREFETCHW instructions"  ,  8,  8, bools },
          { "OS visible workaround"                   ,  9,  9, bools },
          { "instruction based sampling"              , 10, 10, bools },
          { "XOP support"                             , 11, 11, bools },
          { "SKINIT/STGI support"                     , 12, 12, bools },
          { "watchdog timer support"                  , 13, 13, bools },
          { "lightweight profiling support"           , 15, 15, bools },
          { "4-operand FMA instruction"               , 16, 16, bools },
          { "TCE: translation cache extension"        , 17, 17, bools },
          { "NodeId MSR C001100C"                     , 19, 19, bools },
          { "TBM support"                             , 21, 21, bools },
          { "topology extensions"                     , 22, 22, bools },
          { "core performance counter extensions"     , 23, 23, bools },
          { "data breakpoint extension"               , 26, 26, bools },
          { "performance time-stamp counter support"  , 27, 27, bools },
          { "performance counter extensions"          , 28, 28, bools },
          { "MWAITX/MONITORX supported"               , 29, 29, bools },
        };

   printf("   AMD feature flags (0x80000001/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_ecx_intel(unsigned int  value)
{
   static named_item  names[]
      = { { "LAHF/SAHF supported in 64-bit mode"      ,  0,  0, bools },
          { "LZCNT advanced bit manipulation"         ,  5,  5, bools },
          { "3DNow! PREFETCH/PREFETCHW instructions"  ,  8,  8, bools },
        };

   printf("   Intel feature flags (0x80000001/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000001_ecx(unsigned int  value,
                   vendor_t      vendor)
{
   switch (vendor) {
   case VENDOR_AMD:
      print_80000001_ecx_amd(value);
      break;
   case VENDOR_INTEL:
      print_80000001_ecx_intel(value);
      break;
   case VENDOR_CYRIX:
   case VENDOR_VIA:
   case VENDOR_TRANSMETA:
   case VENDOR_UMC:
   case VENDOR_NEXGEN:
   case VENDOR_RISE:
   case VENDOR_SIS:
   case VENDOR_NSC:
   case VENDOR_VORTEX:
   case VENDOR_RDC:
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

static void
print_80000001_ebx_amd(unsigned int  value,
                       unsigned int  val_1_eax)
{
   if (__F(val_1_eax) == _XF(0) + _F(15)
       && __M(val_1_eax) < _XM(4) + _M(0)) {
      static named_item  names[]
         = { { "raw"                                     ,  0, 31, NIL_IMAGES },
             { "BrandId"                                 ,  0, 16, NIL_IMAGES },
             { "BrandTableIndex"                         ,  6, 12, NIL_IMAGES },
             { "NN"                                      ,  0,  6, NIL_IMAGES },
           };

      printf("   extended brand id (0x80000001/ebx):\n");
      print_names(value, names, LENGTH(names, named_item),
                  /* max_len => */ 0);
   } else if (__F(val_1_eax) == _XF(0) + _F(15)
              && __M(val_1_eax) >= _XM(4) + _M(0)) {
      static named_item  names[]
         = { { "raw"                                     ,  0, 31, NIL_IMAGES },
             { "BrandId"                                 ,  0, 16, NIL_IMAGES },
             { "PwrLmt:high"                             ,  6,  8, NIL_IMAGES },
             { "PwrLmt:low"                              , 14, 14, NIL_IMAGES },
             { "BrandTableIndex"                         ,  9, 13, NIL_IMAGES },
             { "NN:high"                                 , 15, 15, NIL_IMAGES },
             { "NN:low"                                  ,  0,  5, NIL_IMAGES },
           };

      printf("   extended brand id (0x80000001/ebx):\n");
      print_names(value, names, LENGTH(names, named_item),
                  /* max_len => */ 0);
   } else if (__F(val_1_eax) == _XF(1) + _F(15)
              || __F(val_1_eax) == _XF(2) + _F(15)) {
      static named_item  names[]
         = { { "raw"                                     ,  0, 31, NIL_IMAGES },
             { "BrandId"                                 ,  0, 15, NIL_IMAGES },
             { "str1"                                    , 11, 14, NIL_IMAGES },
             { "str2"                                    ,  0,  3, NIL_IMAGES },
             { "PartialModel"                            ,  4, 10, NIL_IMAGES },
             { "PG"                                      , 15, 15, NIL_IMAGES },
             { "PkgType"                                 , 28, 31, NIL_IMAGES },
           };

      printf("   extended brand id (0x80000001/ebx):\n");
      print_names(value, names, LENGTH(names, named_item),
                  /* max_len => */ 0);
   } else {
      static named_item  names[]
         = { { "raw"                                     ,  0, 31, NIL_IMAGES },
             { "BrandId"                                 ,  0, 15, NIL_IMAGES },
           };

      printf("   extended brand id (0x80000001/ebx):\n");
      print_names(value, names, LENGTH(names, named_item),
                  /* max_len => */ 0);
   }
}

static void
print_80000001_ebx(unsigned int  value,
                   vendor_t      vendor,
                   unsigned int  val_1_eax)
{
   switch (vendor) {
   case VENDOR_AMD:
      print_80000001_ebx_amd(value, val_1_eax);
      break;
   case VENDOR_INTEL:
   case VENDOR_CYRIX:
   case VENDOR_VIA:
   case VENDOR_TRANSMETA:
   case VENDOR_UMC:
   case VENDOR_NEXGEN:
   case VENDOR_RISE:
   case VENDOR_SIS:
   case VENDOR_NSC:
   case VENDOR_VORTEX:
   case VENDOR_RDC:
   case VENDOR_UNKNOWN:
      /* DO NOTHING */
      break;
   }
}

static void
print_80000005_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0,  7, NIL_IMAGES },
          { "instruction associativity"               ,  8, 15, NIL_IMAGES },
          { "data # entries"                          , 16, 23, NIL_IMAGES },
          { "data associativity"                      , 24, 31, NIL_IMAGES },
        };

   printf("   L1 TLB/cache information: 2M/4M pages & L1 TLB"
          " (0x80000005/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000005_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0,  7, NIL_IMAGES },
          { "instruction associativity"               ,  8, 15, NIL_IMAGES },
          { "data # entries"                          , 16, 23, NIL_IMAGES },
          { "data associativity"                      , 24, 31, NIL_IMAGES },
        };

   printf("   L1 TLB/cache information: 4K pages & L1 TLB"
          " (0x80000005/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000005_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "line size (bytes)"                       ,  0,  7, NIL_IMAGES },
          { "lines per tag"                           ,  8, 15, NIL_IMAGES },
          { "associativity"                           , 16, 23, NIL_IMAGES },
          { "size (KB)"                               , 24, 31, NIL_IMAGES },
        };

   printf("   L1 data cache information (0x80000005/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000005_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "line size (bytes)"                       ,  0,  7, NIL_IMAGES },
          { "lines per tag"                           ,  8, 15, NIL_IMAGES },
          { "associativity"                           , 16, 23, NIL_IMAGES },
          { "size (KB)"                               , 24, 31, NIL_IMAGES },
        };

   printf("   L1 instruction cache information (0x80000005/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static ccstring  l2_assoc[] = { "L2 off (0)",
                                "direct mapped (1)",
                                "2-way (2)",
                                NULL,
                                "4-way (4)",
                                NULL,
                                "8-way (6)",
                                NULL,
                                "16-way (8)",
                                NULL,
                                "32-way (10)",
                                "48-way (11)",
                                "64-way (12)",
                                "96-way (13)",
                                "128-way (14)",
                                "full (15)" };

static void
print_80000006_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0, 11, NIL_IMAGES },
          { "instruction associativity"               , 12, 15, l2_assoc },
          { "data # entries"                          , 16, 27, NIL_IMAGES },
          { "data associativity"                      , 28, 31, l2_assoc },
        };

   printf("   L2 TLB/cache information: 2M/4M pages & L2 TLB"
          " (0x80000006/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000006_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0, 11, NIL_IMAGES },
          { "instruction associativity"               , 12, 15, l2_assoc },
          { "data # entries"                          , 16, 27, NIL_IMAGES },
          { "data associativity"                      , 28, 31, l2_assoc },
        };

   printf("   L2 TLB/cache information: 4K pages & L2 TLB (0x80000006/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000006_ecx(unsigned int   value,
                   code_stash_t*  stash)
{
   static named_item  names[]
      = { { "line size (bytes)"                       ,  0,  7, NIL_IMAGES },
          { "lines per tag"                           ,  8, 11, NIL_IMAGES },
          { "associativity"                           , 12, 15, l2_assoc },
          { "size (KB)"                               , 16, 31, NIL_IMAGES },
        };

   printf("   L2 unified cache information (0x80000006/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   if (((value >> 12) & 0xf) == 4 && (value >> 16) == 256) {
      stash->L2_4w_256K = TRUE;
   } else if (((value >> 12) & 0xf) == 4 && (value >> 16) == 512) {
      stash->L2_4w_512K = TRUE;
   }
}

static void
print_80000006_edx(unsigned int   value)
{
   static named_item  names[]
      = { { "line size (bytes)"                       ,  0,  7, NIL_IMAGES },
          { "lines per tag"                           ,  8, 11, NIL_IMAGES },
          { "associativity"                           , 12, 15, l2_assoc },
          { "size (in 512KB units)"                   , 18, 31, NIL_IMAGES },
        };

   printf("   L3 cache information (0x80000006/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000007_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "MCA overflow recovery support"           ,  0,  0, bools },
          { "SUCCOR support"                          ,  1,  1, bools },
          { "HWA: hardware assert support"            ,  2,  2, bools },
          { "scalable MCA support"                    ,  3,  3, bools },
        };

   printf("   RAS Capability (0x80000007/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000007_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "CmpUnitPwrSampleTimeRatio"               ,  0, 31, NIL_IMAGES },
        };

   printf("   Advanced Power Management Features (0x80000007/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000007_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "TS: temperature sensing diode"           ,  0,  0, bools },
          { "FID: frequency ID control"               ,  1,  1, bools },
          { "VID: voltage ID control"                 ,  2,  2, bools },
          { "TTP: thermal trip"                       ,  3,  3, bools },
          { "TM: thermal monitor"                     ,  4,  4, bools },
          { "STC: software thermal control"           ,  5,  5, bools },
          { "100 MHz multiplier control"              ,  6,  6, bools },
          { "hardware P-State control"                ,  7,  7, bools },
          { "TscInvariant"                            ,  8,  8, bools },
          { "CPB: core performance boost"             ,  9,  9, bools },
          { "read-only effective frequency interface" , 10, 10, bools },
          { "processor feedback interface"            , 11, 11, bools },
          { "APM power reporting"                     , 12, 12, bools },
          { "connected standby"                       , 13, 13, bools },
          { "RAPL: running average power limit"       , 14, 14, bools },
        };

   printf("   Advanced Power Management Features (0x80000007/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000008_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "maximum physical address bits"           ,  0,  7, NIL_IMAGES },
          { "maximum linear (virtual) address bits"   ,  8, 15, NIL_IMAGES },
          { "maximum guest physical address bits"     , 16, 23, NIL_IMAGES },
        };

   printf("   Physical Address and Linear Address Size (0x80000008/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000008_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "CLZERO instruction"                      ,  0,  0, bools },
          { "instructions retired count support"      ,  1,  1, bools },
          { "always save/restore error pointers"      ,  2,  2, bools },
        };

   printf("   Extended Feature Extensions ID (0x80000008/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000008_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "number of CPU cores - 1"                 ,  0,  7, NIL_IMAGES },
          { "ApicIdCoreIdSize"                        , 12, 15, NIL_IMAGES },
        };

   printf("   Logical CPU cores (0x80000008/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000000a_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "SvmRev: SVM revision"                    ,  0,  7, NIL_IMAGES },
        };

   printf("   SVM Secure Virtual Machine (0x8000000a/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000000a_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "nested paging"                           ,  0,  0, bools },
          { "LBR virtualization"                      ,  1,  1, bools },
          { "SVM lock"                                ,  2,  2, bools },
          { "NRIP save"                               ,  3,  3, bools },
          { "MSR based TSC rate control"              ,  4,  4, bools },
          { "VMCB clean bits support"                 ,  5,  5, bools },
          { "flush by ASID"                           ,  6,  6, bools },
          { "decode assists"                          ,  7,  7, bools },
          { "SSSE3/SSE5 opcode set disable"           ,  9,  9, bools },
          { "pause intercept filter"                  , 10, 10, bools },
          { "pause filter threshold"                  , 12, 12, bools },
          { "AVIC: AMD virtual interrupt controller"  , 13, 13, bools },
          { "virtualized VMLOAD/VMSAVE"               , 15, 15, bools },
          { "virtualized GIF"                         , 16, 16, bools },
        };

   printf("   SVM Secure Virtual Machine (0x8000000a/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000000a_ebx(unsigned int  value)
{
   printf("   NASID: number of address space identifiers = 0x%x (%u):\n",
          value, value);
}

static void
print_80000019_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0, 11, NIL_IMAGES },
          { "instruction associativity"               , 12, 15, l2_assoc },
          { "data # entries"                          , 16, 27, NIL_IMAGES },
          { "data associativity"                      , 28, 31, l2_assoc },
        };

   printf("   L1 TLB information: 1G pages (0x80000019/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80000019_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "instruction # entries"                   ,  0, 11, NIL_IMAGES },
          { "instruction associativity"               , 12, 15, l2_assoc },
          { "data # entries"                          , 16, 27, NIL_IMAGES },
          { "data associativity"                      , 28, 31, l2_assoc },
        };

   printf("   L2 TLB information: 1G pages (0x80000019/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001a_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "128-bit SSE executed full-width"         ,  0,  0, bools },
          { "MOVU* better than MOVL*/MOVH*"           ,  1,  1, bools },
          { "256-bit SSE executed full-width"         ,  2,  2, bools },
        };

   printf("   SVM Secure Virtual Machine (0x8000001a/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001b_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "IBS feature flags valid"                 ,  0,  0, bools },
          { "IBS fetch sampling"                      ,  1,  1, bools },
          { "IBS execution sampling"                  ,  2,  2, bools },
          { "read write of op counter"                ,  3,  3, bools },
          { "op counting mode"                        ,  4,  4, bools },
          { "branch target address reporting"         ,  5,  5, bools },
          { "IbsOpCurCnt and IbsOpMaxCnt extend 7"    ,  6,  6, bools },
          { "invalid RIP indication support"          ,  7,  7, bools },
          { "fused branch micro-op indication support",  8,  8, bools },
          { "IBS fetch control extended MSR support"  ,  9,  9, bools },
          { "IBS op data 4 MSR support"               , 10, 10, bools },
        };

   printf("   Instruction Based Sampling Identifiers (0x8000001b/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001c_eax(unsigned int  value)
{
   static named_item  names[]
      = { { "lightweight profiling"                   ,  0,  0, bools },
          { "LWPVAL instruction"                      ,  1,  1, bools },
          { "instruction retired event"               ,  2,  2, bools },
          { "branch retired event"                    ,  3,  3, bools },
          { "DC miss event"                           ,  4,  4, bools },
          { "core clocks not halted event"            ,  5,  5, bools },
          { "core reference clocks not halted event"  ,  6,  6, bools },
          { "interrupt on threshold overflow"         , 31, 31, bools },
        };

   printf("   Lightweight Profiling Capabilities: Availability"
          " (0x8000001c/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001c_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "LWPCB byte size"                         ,  0,  7, NIL_IMAGES },
          { "event record byte size"                  ,  8, 15, NIL_IMAGES },
          { "maximum EventId"                         , 16, 23, NIL_IMAGES },
          { "EventInterval1 field offset"             , 24, 31, NIL_IMAGES },
        };

   printf("   Lightweight Profiling Capabilities (0x8000001c/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001c_ecx(unsigned int  value)
{
   static named_item  names[]
      = { { "latency counter bit size"                ,  0,  4, NIL_IMAGES },
          { "data cache miss address valid"           ,  5,  5, bools },
          { "amount cache latency is rounded"         ,  6,  8, NIL_IMAGES },
          { "LWP implementation version"              ,  9, 15, NIL_IMAGES },
          { "event ring buffer size in records"       , 16, 23, NIL_IMAGES },
          { "branch prediction filtering"             , 28, 28, bools },
          { "IP filtering"                            , 29, 29, bools },
          { "cache level filtering"                   , 30, 30, bools },
          { "cache latency filteing"                  , 31, 31, bools },
        };

   printf("   Lightweight Profiling Capabilities (0x8000001c/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001c_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "lightweight profiling"                   ,  0,  0, bools },
          { "LWPVAL instruction"                      ,  1,  1, bools },
          { "instruction retired event"               ,  2,  2, bools },
          { "branch retired event"                    ,  3,  3, bools },
          { "DC miss event"                           ,  4,  4, bools },
          { "core clocks not halted event"            ,  5,  5, bools },
          { "core reference clocks not halted event"  ,  6,  6, bools },
          { "interrupt on threshold overflow"         , 31, 31, bools },
        };

   printf("   Lightweight Profiling Capabilities: Supported"
          " (0x8000001c/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001d_eax(unsigned int  value)
{
   static ccstring  cache_type[] = { "no more caches (0)",
                                     "data (1)",
                                     "instruction (2)",
                                     "unified (3)",
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL };
   static named_item  names[]
      = { { "type"                                    ,  0,  4, cache_type },
          { "level"                                   ,  5,  7, NIL_IMAGES },
          { "self-initializing"                       ,  8,  8, bools },
          { "fully associative"                       ,  9,  9, bools },
          { "extra cores sharing this cache"          , 14, 25, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 31);
}

static void
print_8000001d_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "line size in bytes"                      ,  0, 11, NIL_IMAGES },
          { "physical line partitions"                , 12, 21, NIL_IMAGES },
          { "number of ways"                          , 22, 31, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 31);
}

static void
print_8000001d_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "write-back invalidate"                   ,  0,  0, bools },
          { "cache inclusive of lower levels"         ,  1,  1, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 31);
}

static void
print_8000001e_ebx(unsigned int  value)
{
   static named_item  names[]
      = { { "compute unit ID"                         ,  0,  7, NIL_IMAGES },
          { "cores per compute unit - 1"              ,  8,  9, NIL_IMAGES },
        };

   printf("   Extended APIC ID (0x8000001e/ebx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001e_ecx(unsigned int  value)
{
   static ccstring  npp[] = { "1 node (0)",
                              "2 nodes (1)",
                              NULL, NULL, NULL,
                              NULL, NULL, NULL };

   static named_item  names[]
      = { { "node ID"                                 ,  0,  7, NIL_IMAGES },
          { "nodes per processor"                     ,  8,  9, npp },
        };

   printf("   Extended APIC ID (0x8000001e/ecx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_8000001f_eax(unsigned int  value)
{
   // This is undocumented as of 17-May-2018, so names are tentative.
   static named_item  names[]
      = { { "SME: secure memory encryption support"   ,  0,  0, bools },
          { "SEV: secure encrypted virtualize support",  1,  1, bools },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_80860001_eax(unsigned int  value)
{
   static ccstring  family[16] = { NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   "Transmeta Crusoe" };
   static named_item  names[]
      = { { "generation"                              ,  8, 11, family },
          { "model"                                   ,  4,  7, NIL_IMAGES },
          { "stepping"                                ,  0,  3, NIL_IMAGES },
        };

   printf("   Transmeta processor signature (0x80860001/eax):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);

   print_synth_transmeta("      (simple synth)", value, NULL);
}

static void
print_80860001_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "recovery CMS active"                     ,  0,  0, bools },
          { "LongRun"                                 ,  1,  1, bools },
          { "LongRun Table Interface LRTI (CMS 4.2)"  ,  3,  3, bools },
          { "persistent translation technology 1.x"   ,  7,  7, bools },
          { "persistent translation technology 2.0"   ,  8,  8, bools },
          { "processor break events"                  , 12, 12, bools },
        };

   printf("   Transmeta feature flags (0x80860001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_transmeta_proc_rev_meaning(unsigned int  proc_rev)
{
   switch (proc_rev & 0xffff0000) {
   case 0x01010000:
      printf("(TM3200)");
      break;
   case 0x01020000:
      printf("(TM5400)");
      break;
   case 0x01030000:
      if ((proc_rev & 0xffffff00) == 0x00000000) {
         printf("(TM5400 / TM5600)");
      } else {
         printf("(unknown)");
      }
      break;
   case 0x01040000:
   case 0x01050000:
      printf("(TM5500 / TM5800)");
      break;
   default:
      printf("(unknown)");
      break;
   }
}

static void
print_80860001_ebx_ecx(unsigned int  val_ebx,
                       unsigned int  val_ecx)
{
   printf("   Transmeta processor revision (0x80000001/edx)"
          " = %u.%u-%u.%u-%u ", 
          (val_ebx >> 24) & 0xff,
          (val_ebx >> 16) & 0xff,
          (val_ebx >>  8) & 0xff,
          (val_ebx >>  0) & 0xff,
          val_ecx);
          
   if (val_ebx == 0x20000000) {
      printf("(see 80860002/eax)");
   } else {
      print_transmeta_proc_rev_meaning(val_ebx);
   }
   printf("\n");
}

static void
print_80860002_eax(unsigned int   value,
                   code_stash_t*  stash)
{
   if (stash->transmeta_proc_rev == 0x02000000) {
      printf("   Transmeta processor revision (0x80860002/eax)"
             " = %u.%u-%u.%u ", 
             (value >> 24) & 0xff,
             (value >> 16) & 0xff,
             (value >>  8) & 0xff,
             (value >>  0) & 0xff);

      print_transmeta_proc_rev_meaning(value);

      printf("\n");

      stash->transmeta_proc_rev = value;
   }
}

static void
print_c0000001_edx(unsigned int  value)
{
   static named_item  names[]
      = { { "alternate instruction set"                ,  0,  0, bools },
          { "alternate instruction set enabled"        ,  1,  1, bools },
          { "random number generator"                  ,  2,  2, bools },
          { "random number generator enabled"          ,  3,  3, bools },
          { "LongHaul MSR 0000_110Ah"                  ,  4,  4, bools },
          { "FEMMS"                                    ,  5,  5, bools },
          { "advanced cryptography engine (ACE)"       ,  6,  6, bools },
          { "advanced cryptography engine (ACE)enabled",  7,  7, bools },
          { "montgomery multiplier/hash (ACE2)"        ,  8,  8, bools },
          { "montgomery multiplier/hash (ACE2) enabled",  9,  9, bools },
          { "padlock hash engine (PHE)"                , 10, 10, bools },
          { "padlock hash engine (PHE) enabled"        , 11, 11, bools },
          { "padlock montgomery mult. (PMM)"           , 12, 12, bools },
          { "padlock montgomery mult. (PMM) enabled"   , 13, 13, bools },
        };

   printf("   extended feature flags (0xc0000001/edx):\n");
   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 0);
}

static void
print_c0000002_eax(unsigned int  value)
{
   // This information is from Juerg Haefliger.
   // TODO: figure out how to decode the rest of this
   static named_item  names[]
      = { { "core temperature (degrees C)"       ,  8, 15, NIL_IMAGES },
        };

   print_names(value, names, LENGTH(names, named_item),
               /* max_len => */ 28);
}

static void
print_c0000002_ebx(unsigned int  value)
{
   // This information is from Juerg Haefliger.
   // TODO: figure out how to decode the rest of this
   printf("      input voltage (mV)           = %d (0x%0x)\n",
          (BIT_EXTRACT_LE(value, 0, 8) << 4) + 700,
          BIT_EXTRACT_LE(value, 0, 8));
}

static void
usage(void)
{
   printf("usage: %s [options...]\n", program);
   printf("\n");
   printf("Dump detailed information about the CPU(s) gathered from the CPUID"
          " instruction,\n");
   printf("and also determine the exact model of CPU(s).\n");
   printf("\n");
   printf("options:\n");
   printf("\n");
   printf("   -1,      --one-cpu    display information only for the current"
                                    " CPU\n");
   printf("   -f FILE, --file=FILE  read raw hex information (-r output) from"
                                    " FILE instead\n");
   printf("                         of from executions of the cpuid"
                                    " instruction.\n");
   printf("                         If FILE is '-', read from stdin.\n");
   printf("   -l V,    --leaf=V     display information for the single specified"
                                    " leaf.\n");
   printf("                         If -s/--subleaf is not specified, 0 is"
                                    " assumed.\n");
   printf("   -s V,    --subleaf=V  display information for the single specified"
                                    " subleaf.\n");
   printf("                         It requires -l/--leaf.\n");
   printf("   -h, -H,  --help       display this help information\n");
   printf("   -i,      --inst       use the CPUID instruction: The information"
                                    " it provides\n");
   printf("                         is reliable.  It is not necessary to"
                                    " be root.\n");
   printf("                         (This option is the default.)\n");
#ifdef USE_CPUID_MODULE
   printf("   -k,      --kernel     use the CPUID kernel module: The"
                                    " information does not\n");
   printf("                         seem to be reliable on all combinations of"
                                    " CPU type\n");
   printf("                         and kernel version.  Typically, it is"
                                    " necessary to be\n");
   printf("                         root.\n");
#endif
   printf("   -r,      --raw        display raw hex information with no"
                                    " decoding\n");
   printf("   -v,      --version    display cpuid version\n");
   printf("\n");
   exit(1);
}

#ifdef USE_CPUID_MODULE
static void
explain_dev_cpu_errno(void)
{
   if (errno == ENODEV || errno == ENXIO) {
      fprintf(stderr,
              "%s: if running a modular kernel, execute"
              " \"modprobe cpuid\",\n",
              program);
      fprintf(stderr,
              "%s: wait a few seconds, and then try again\n",
              program);
      fprintf(stderr,
              "%s: or consider using the -i option\n", program);
   } else if (errno == ENOENT) {
      fprintf(stderr,
              "%s: if running a modular kernel, execute"
              " \"modprobe cpuid\",\n",
              program);
      fprintf(stderr,
              "%s: wait a few seconds, and then try again;\n",
              program);
      fprintf(stderr,
              "%s: if it still fails, try executing:\n",
              program);
      fprintf(stderr,
              "%s:    mknod /dev/cpu/0/cpuid c %u 0\n",
              program, CPUID_MAJOR);
      fprintf(stderr,
              "%s:    mknod /dev/cpu/1/cpuid c %u 1\n",
              program, CPUID_MAJOR);
      fprintf(stderr,
              "%s:    etc.\n",
              program);
      fprintf(stderr,
              "%s: and then try again\n",
              program);
      fprintf(stderr,
              "%s: or consider using the -i option\n", program);
   } else if ((errno == EPERM || errno == EACCES) && getuid() != 0) {
      fprintf(stderr,
              "%s: on most systems,"
              " it is necessary to execute this as root\n",
              program);
      fprintf(stderr,
              "%s: or consider using the -i option\n", program);
   }
   exit(1);
}
#endif

#define FOUR_CHARS_VALUE(s) \
   ((unsigned int)((s)[0] + ((s)[1] << 8) + ((s)[2] << 16) + ((s)[3] << 24)))
#define IS_VENDOR_ID(words, s)                        \
   (   (words)[WORD_EBX] == FOUR_CHARS_VALUE(&(s)[0]) \
    && (words)[WORD_EDX] == FOUR_CHARS_VALUE(&(s)[4]) \
    && (words)[WORD_ECX] == FOUR_CHARS_VALUE(&(s)[8]))
#define IS_HYPERVISOR_ID(words, s)                    \
   (   (words)[WORD_EBX] == FOUR_CHARS_VALUE(&(s)[0]) \
    && (words)[WORD_ECX] == FOUR_CHARS_VALUE(&(s)[4]) \
    && (words)[WORD_EDX] == FOUR_CHARS_VALUE(&(s)[8]))

static void
print_reg_raw (unsigned int        reg,
               unsigned int        try,
               const unsigned int  words[WORD_NUM])
{
   printf("   0x%08x 0x%02x: eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n",
          reg, try,
          words[WORD_EAX], words[WORD_EBX],
          words[WORD_ECX], words[WORD_EDX]);
}

static void 
print_reg (unsigned int        reg,
           const unsigned int  words[WORD_NUM],
           boolean             raw,
           unsigned int        try,
           code_stash_t*       stash)
{
   if (reg == 0) {
      if (IS_VENDOR_ID(words, "GenuineIntel")) {
         stash->vendor = VENDOR_INTEL;
      } else if (IS_VENDOR_ID(words, "AuthenticAMD")) {
         stash->vendor = VENDOR_AMD;
      } else if (IS_VENDOR_ID(words, "CyrixInstead")) {
         stash->vendor = VENDOR_CYRIX;
      } else if (IS_VENDOR_ID(words, "CentaurHauls")) {
         stash->vendor = VENDOR_VIA;
      } else if (IS_VENDOR_ID(words, "UMC UMC UMC ")) {
         stash->vendor = VENDOR_UMC;
      } else if (IS_VENDOR_ID(words, "NexGenDriven")) {
         stash->vendor = VENDOR_NEXGEN;
      } else if (IS_VENDOR_ID(words, "RiseRiseRise")) {
         stash->vendor = VENDOR_RISE;
      } else if (IS_VENDOR_ID(words, "GenuineTMx86")) {
         stash->vendor = VENDOR_TRANSMETA;
      } else if (IS_VENDOR_ID(words, "SiS SiS SiS ")) {
         stash->vendor = VENDOR_SIS;
      } else if (IS_VENDOR_ID(words, "Geode by NSC")) {
         stash->vendor = VENDOR_NSC;
      } else if (IS_VENDOR_ID(words, "Vortex86 SoC")) {
         stash->vendor = VENDOR_VORTEX;
      } else if (IS_VENDOR_ID(words, "Genuine  RDC")) {
         stash->vendor = VENDOR_RDC;
      }
   } else if (reg == 1) {
      stash->val_1_eax = words[WORD_EAX];
      stash->val_1_ebx = words[WORD_EBX];
      stash->val_1_ecx = words[WORD_ECX];
      stash->val_1_edx = words[WORD_EDX];
   } else if (reg == 4) {
      stash->saw_4 = TRUE;
      if (try == 0) {
         stash->val_4_eax = words[WORD_EAX];
      }
   } else if (reg == 0xb) {
      stash->saw_b = TRUE;
      if (try < sizeof(stash->val_b_eax) / sizeof(stash->val_b_eax[0])) {
         stash->val_b_eax[try] = words[WORD_EAX];
      }
      if (try < sizeof(stash->val_b_ebx) / sizeof(stash->val_b_ebx[0])) {
         stash->val_b_ebx[try] = words[WORD_EBX];
      }
   } else if (reg == 0x40000000) {
      if (IS_HYPERVISOR_ID(words, "VMwareVMware")) {
         stash->hypervisor = HYPERVISOR_VMWARE;
      } else if (IS_HYPERVISOR_ID(words, "XenVMMXenVMM")) {
         stash->hypervisor = HYPERVISOR_XEN;
      } else if (IS_HYPERVISOR_ID(words, "KVMKVMKVM\0\0\0")) {
         stash->hypervisor = HYPERVISOR_KVM;
      } else if (IS_HYPERVISOR_ID(words, "Microsoft Hv")) {
         stash->hypervisor = HYPERVISOR_MICROSOFT;
      }
   } else if (reg == 0x80000008) {
      stash->val_80000008_ecx = words[WORD_ECX];
   } else if (reg == 0x80860003) {
      memcpy(&stash->transmeta_info[0], words, sizeof(unsigned int)*WORD_NUM);
   } else if (reg == 0x80860004) {
      memcpy(&stash->transmeta_info[16], words, sizeof(unsigned int)*WORD_NUM);
   } else if (reg == 0x80860005) {
      memcpy(&stash->transmeta_info[32], words, sizeof(unsigned int)*WORD_NUM);
   } else if (reg == 0x80860006) {
      memcpy(&stash->transmeta_info[48], words, sizeof(unsigned int)*WORD_NUM);
   }

   if (raw) {
      print_reg_raw(reg, try, words);
   } else if (reg == 0) {
      // max already set to words[WORD_EAX]
      stash->val_0_eax = words[WORD_EAX];
      printf("   vendor_id = \"%-4.4s%-4.4s%-4.4s\"\n",
             (const char*)&words[WORD_EBX], 
             (const char*)&words[WORD_EDX], 
             (const char*)&words[WORD_ECX]);
   } else if (reg == 1) {
      print_1_eax(words[WORD_EAX], stash->vendor);
      print_1_ebx(words[WORD_EBX]);
      print_brand(words[WORD_EAX], words[WORD_EBX]);
      print_1_edx(words[WORD_EDX]);
      print_1_ecx(words[WORD_ECX]);
   } else if (reg == 2) {
      unsigned int  word = 0;
      for (; word < 4; word++) {
         if ((words[word] & 0x80000000) == 0) {
            const unsigned char*  bytes = (const unsigned char*)&words[word];
            unsigned int          byte  = (try == 0 && word == WORD_EAX ? 1
                                                                        : 0);
            for (; byte < 4; byte++) {
               print_2_byte(bytes[byte], stash->vendor, stash->val_1_eax);
               stash_intel_cache(stash, bytes[byte]);
            }
         }
      }
   } else if (reg == 3) {
      printf("   processor serial number:"
             " %04X-%04X-%04X-%04X-%04X-%04X\n",
             stash->val_1_eax >> 16, stash->val_1_eax & 0xffff, 
             words[WORD_EDX] >> 16, words[WORD_EDX] & 0xffff, 
             words[WORD_ECX] >> 16, words[WORD_ECX] & 0xffff);
   } else if (reg == 4) {
      printf("      --- cache %d ---\n", try);
      print_4_eax(words[WORD_EAX]);
      print_4_ebx(words[WORD_EBX]);
      print_4_ecx(words[WORD_ECX]);
      print_4_edx(words[WORD_EDX]);
      printf("      number of sets - 1 (s)               = %u\n",
             words[WORD_ECX]);
   } else if (reg == 5) {
      printf("   MONITOR/MWAIT (5):\n");
      print_5_eax(words[WORD_EAX]);
      print_5_ebx(words[WORD_EBX]);
      print_5_ecx(words[WORD_ECX]);
      print_5_edx(words[WORD_EDX]);
   } else if (reg == 6) {
      printf("   Thermal and Power Management Features (6):\n");
      print_6_eax(words[WORD_EAX]);
      print_6_ebx(words[WORD_EBX]);
      print_6_ecx(words[WORD_ECX]);
   } else if (reg == 7) {
      if (try == 0) {
         print_7_ebx(words[WORD_EBX]);
         print_7_ecx(words[WORD_ECX]);
         print_7_edx(words[WORD_EDX]);
      } else {
         /* Reserved: DO NOTHING */
      }
   } else if (reg == 8) {
      /* Reserved: DO NOTHING */
   } else if (reg == 9) {
      printf("   Direct Cache Access Parameters (9):\n");
      printf("      PLATFORM_DCA_CAP MSR bits = %u\n", words[WORD_EAX]);
   } else if (reg == 0xa) {
      print_a_eax(words[WORD_EAX]);
      print_a_ebx(words[WORD_EBX]);
      print_a_edx(words[WORD_EDX]);
   } else if (reg == 0xb) {
      printf("      --- level %d (%s) ---\n", try,  (try == 0 ? "thread" :
                                                     try == 1 ? "core" :
                                                                "package"));
      print_b_eax(words[WORD_EAX]);
      print_b_ebx(words[WORD_EBX]);
      print_b_ecx(words[WORD_ECX]);
      printf("      extended APIC ID                  = %u\n", words[WORD_EDX]);
   } else if (reg == 0xc) {
      /* Reserved: DO NOTHING */
   } else if (reg == 0xd) {
      if (try == 0) {
         printf("   XSAVE features (0xd/0):\n");
         printf("      XCR0 lower 32 bits valid bit field mask = 0x%08x\n",
                words[WORD_EAX]);
         printf("      XCR0 upper 32 bits valid bit field mask = 0x%08x\n",
                words[WORD_EDX]);
         print_d_0_eax(words[WORD_EAX]);
         // No bits current are defined in d_0_edx
         printf("      bytes required by fields in XCR0        = 0x%08x (%u)\n",
                words[WORD_EBX], words[WORD_EBX]);
         printf("      bytes required by XSAVE/XRSTOR area     = 0x%08x (%u)\n",
                words[WORD_ECX], words[WORD_ECX]);
      } else if (try == 1) {
         printf("   XSAVE features (0xd/1):\n");
         print_d_1_eax(words[WORD_EAX]);
         printf("      SAVE area size in bytes                    "
                " = 0x%08x (%u)\n",
                words[WORD_EBX], words[WORD_EBX]);
         printf("      IA32_XSS lower 32 bits valid bit field mask"
                " = 0x%08x\n",
                words[WORD_ECX]);
         printf("      IA32_XSS upper 32 bits valid bit field mask"
                " = 0x%08x\n",
                words[WORD_EDX]);
      } else if (try >= 2 && try < 63) {
         print_d_n(words, try);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0xe) {
      /* Reserved: DO NOTHING */
   } else if (reg == 0xf) {
      if (try == 0) {
         printf("   Quality of Service Monitoring Resource Type (0xf/0):\n");
         printf("      Maximum range of RMID = %u\n", words[WORD_EBX]);
         print_f_0_edx(words[WORD_EDX]);
      } else if (try == 1) {
         printf("   L3 Cache Quality of Service Monitoring (0xf/1):\n");
         printf("      Conversion factor from IA32_QM_CTR to bytes = %u\n",
                words[WORD_EBX]);
         printf("      Maximum range of RMID                       = %u\n",
                words[WORD_ECX]);
         print_f_1_edx(words[WORD_EDX]);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0x10) {
      if (try == 0) {
         printf("   Resource Director Technology allocation (0x10/0):\n");
         print_10_0_ebx(words[WORD_EBX]);
      } else if (try == 1 || try == 2) {
         if (try == 1) {
            printf("   L3 Cache Allocation Technology (0x10/1):\n");
         } else if (try == 2) {
            printf("   L2 Cache Allocation Technology (0x10/2):\n");
         }
         print_10_n_eax(words[WORD_EAX]);
         printf("      Bit-granular map of isolation/contention    = 0x%08x\n",
                words[WORD_EBX]);
         print_10_n_ecx(words[WORD_ECX]);
         print_10_n_edx(words[WORD_EDX]);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0x12) {
      if (try == 0) {
         printf("   Software Guard Extensions (SGX) capability (0x12/0):\n");
         print_12_0_eax(words[WORD_EAX]);
         print_12_0_ebx(words[WORD_EBX]);
         print_12_0_edx(words[WORD_EDX]);
      } else if (try == 1) {
         printf("   SGX attributes (0x12/1):\n");
         printf("      ECREATE SECS.ATTRIBUTES valid bit mask ="
                " 0x%08x%08x%08x%08x\n",
                words[WORD_EDX],
                words[WORD_ECX],
                words[WORD_EBX],
                words[WORD_EAX]);
      } else {
         if ((words[WORD_EAX] & 0xf) == 1) {
            printf("   SGX EPC enumeration (0x12/n):\n");
            printf("      section physical address = 0x%08x%08x\n",
                   words[WORD_EBX], words[WORD_EAX] & 0xfffff000);
            printf("      section size             = 0x%08x%08x\n",
                   words[WORD_EDX], words[WORD_ECX] & 0xfffff000);
            print_12_n_1_ecx(words[WORD_ECX]);
         } else {
            print_reg_raw(reg, try, words);
         }
      }
   } else if (reg == 0x14) {
      if (try == 0) {
         printf("   Intel Processor Trace (0x14):\n");
         print_14_0_ebx(words[WORD_EBX]);
         print_14_0_ecx(words[WORD_ECX]);
      } else if (try == 1) {
         print_14_1_eax(words[WORD_EAX]);
         print_14_1_ebx(words[WORD_EBX]);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0x15) {
      printf("   Time Stamp Counter/Core Crystal Clock Information (0x15):\n");
      printf("      TSC/clock ratio = %u/%u\n",
             words[WORD_EBX], words[WORD_EAX]);
      printf("      nominal core crystal clock = %u Hz\n", words[WORD_ECX]);
   } else if (reg == 0x16) {
      printf("   Processor Frequency Information (0x16):\n");
      print_16_eax(words[WORD_EAX]);
      print_16_ebx(words[WORD_EBX]);
      print_16_ecx(words[WORD_ECX]);
   } else if (reg == 0x17) {
      if (try == 0) {
         printf("   system-on-chip vendor attribute (0x17/0):\n");
         print_17_0_ebx(words[WORD_EBX]);
         printf("      project id  = 0x%08x (%u)\n",
                words[WORD_ECX], words[WORD_ECX]);
         printf("      stepping id = 0x%08x (%u)\n",
                words[WORD_EDX], words[WORD_EDX]);
      } else if (try == 1) {
         memcpy(&stash->soc_brand[0], words, sizeof(unsigned int)*WORD_NUM);
      } else if (try == 2) {
         memcpy(&stash->soc_brand[16], words, sizeof(unsigned int)*WORD_NUM);
      } else if (try == 3) {
         memcpy(&stash->soc_brand[32], words, sizeof(unsigned int)*WORD_NUM);
         printf("      SoC brand   = \"%s\"\n", stash->soc_brand);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0x18) {
      printf("   deterministic address translation parameters (0x18/0):\n");
      print_18_n_ebx(words[WORD_EBX]);
      printf("      number of sets = 0x%08x (%u)\n",
             words[WORD_ECX], words[WORD_ECX]);
      print_18_n_edx(words[WORD_EDX]);
   } else if (reg == 0x1b) {
      printf("   PCONFIG information (0x1b/n):\n");
      print_1b_n_eax(words[WORD_EAX]);
      printf("      identifier of target %d = 0x%08x (%u)\n",
             3 * try + 1, words[WORD_EBX], words[WORD_EBX]);
      printf("      identifier of target %d = 0x%08x (%u)\n",
             3 * try + 2, words[WORD_ECX], words[WORD_ECX]);
      printf("      identifier of target %d = 0x%08x (%u)\n",
             3 * try + 3, words[WORD_EDX], words[WORD_EDX]);
   } else if (reg == 0x40000000) {
      // max already set to words[WORD_EAX]
      printf("   hypervisor_id = \"%-4.4s%-4.4s%-4.4s\"\n",
             (const char*)&words[WORD_EBX], 
             (const char*)&words[WORD_ECX], 
             (const char*)&words[WORD_EDX]);
   } else if (reg == 0x40000001 && stash->hypervisor == HYPERVISOR_XEN) {
      printf("   hypervisor version (0x40000001/eax):\n");
      printf("      version = %d.%d\n", 
             BIT_EXTRACT_LE(words[WORD_EAX], 16, 32),
             BIT_EXTRACT_LE(words[WORD_EAX],  0, 16));
   } else if (reg == 0x40000001 && stash->hypervisor == HYPERVISOR_KVM) {
      print_40000001_eax_kvm(words[WORD_EAX]);
   } else if (reg == 0x40000001 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor interface identification (0x40000001/eax):\n");
      printf("      version = \"%-4.4s\"\n",
             (const char*)&words[WORD_EAX]);
   } else if (reg == 0x40000002 && stash->hypervisor == HYPERVISOR_XEN) {
      printf("   hypervisor features (0x40000002):\n");
      printf("      number of hypercall-transfer pages = 0x%0x (%u)\n",
             words[WORD_EAX], words[WORD_EAX]);
      printf("      MSR base address                   = 0x%0x\n",
             words[WORD_EBX]);
      print_40000002_ecx_xen(words[WORD_ECX]);
   } else if (reg == 0x40000003 && stash->hypervisor == HYPERVISOR_XEN
              && try == 0) {
      print_40000003_eax_xen(words[WORD_EAX]);
      printf("      tsc mode            = 0x%0x (%u)\n",
             words[WORD_EBX], words[WORD_EBX]);
      printf("      tsc frequency (kHz) = %u\n",
             words[WORD_ECX]);
      printf("      incarnation         = 0x%0x (%u)\n",
             words[WORD_EDX], words[WORD_EDX]);
   } else if (reg == 0x40000003 && stash->hypervisor == HYPERVISOR_XEN
              && try == 1) {
      unsigned long long  vtsc_offset
         = ((unsigned long long)words[WORD_EAX]
            + ((unsigned long long)words[WORD_EAX] << 32));
      printf("      vtsc offset   = 0x%0llx (%llu)\n", vtsc_offset, vtsc_offset);
      printf("      vtsc mul_frac = 0x%0x (%u)\n",
             words[WORD_ECX], words[WORD_ECX]);
      printf("      vtsc shift    = 0x%0x (%u)\n",
             words[WORD_EDX], words[WORD_EDX]);
   } else if (reg == 0x40000003 && stash->hypervisor == HYPERVISOR_XEN
              && try == 2) {
      printf("      cpu frequency (kHZ) = %u\n", words[WORD_EAX]);
   } else if (reg == 0x40000002 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor system identity (0x40000002):\n");
      printf("      build          = %d\n", words[WORD_EAX]);
      printf("      version        = %d.%d\n",
             BIT_EXTRACT_LE(words[WORD_EBX], 16, 32),
             BIT_EXTRACT_LE(words[WORD_EBX],  0, 16));
      printf("      service pack   = %d\n", words[WORD_ECX]);
      printf("      service branch = %d\n",
             BIT_EXTRACT_LE(words[WORD_EDX], 24, 32));
      printf("      service number = %d\n",
             BIT_EXTRACT_LE(words[WORD_EDX],  0, 24));
   } else if (reg == 0x40000003 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      print_40000003_eax_microsoft(words[WORD_EAX]);
      print_40000003_ebx_microsoft(words[WORD_EBX]);
      print_40000003_ecx_microsoft(words[WORD_ECX]);
      print_40000003_edx_microsoft(words[WORD_EDX]);
   } else if (reg == 0x40000004 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      print_40000004_eax_microsoft(words[WORD_EAX]);
      printf("      maximum number of spinlock retry attempts = 0x%0x (%u)\n",
             words[WORD_EBX], words[WORD_EBX]);
   } else if (reg == 0x40000005 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor implementation limits (0x40000005):\n");
      printf("      maximum number of virtual processors                      "
             " = 0x%0x (%u)\n",
             words[WORD_EAX], words[WORD_EAX]);
      printf("      maximum number of logical processors                      "
             " = 0x%0x (%u)\n",
             words[WORD_EBX], words[WORD_EBX]);
      printf("      maximum number of physical interrupt vectors for remapping"
             " = 0x%0x (%u)\n",
             words[WORD_ECX], words[WORD_ECX]);
   } else if (reg == 0x40000006 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      print_40000006_eax_microsoft(words[WORD_EAX]);
   } else if (reg == 0x40000007 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor root partition enlightenments (0x40000007):\n");
      print_40000007_eax_microsoft(words[WORD_EAX]);
      print_40000007_ebx_microsoft(words[WORD_EBX]);
   } else if (reg == 0x40000008 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor shared virtual memory (0x40000008):\n");
      print_40000008_eax_microsoft(words[WORD_EAX]);
   } else if (reg == 0x40000009 && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor nested hypervisor features (0x40000009):\n");
      print_40000009_eax_microsoft(words[WORD_EAX]);
      print_40000009_edx_microsoft(words[WORD_EAX]);
   } else if (reg == 0x4000000a && stash->hypervisor == HYPERVISOR_MICROSOFT) {
      printf("   hypervisor nested virtualization features (0x4000000a):\n");
      print_4000000a_eax_microsoft(words[WORD_EAX]);
   } else if (reg == 0x40000010) {
      printf("   hypervisor generic timing information (0x40000010):\n");
      printf("      TSC frequency (Hz) = %d\n", words[WORD_EAX]);
      printf("      bus frequency (Hz) = %d\n", words[WORD_EBX]);
   } else if (reg == 0x80000000) {
      // max already set to words[WORD_EAX]
   } else if (reg == 0x80000001) {
      print_80000001_eax(words[WORD_EAX], stash->vendor);
      print_80000001_edx(words[WORD_EDX], stash->vendor);
      print_80000001_ebx(words[WORD_EBX], stash->vendor, stash->val_1_eax);
      print_80000001_ecx(words[WORD_ECX], stash->vendor);
      stash->val_80000001_eax = words[WORD_EAX];
      stash->val_80000001_ebx = words[WORD_EBX];
      stash->val_80000001_ecx = words[WORD_ECX];
      stash->val_80000001_edx = words[WORD_EDX];
   } else if (reg == 0x80000002) {
      memcpy(&stash->brand[0], words, sizeof(unsigned int)*WORD_NUM);
   } else if (reg == 0x80000003) {
      memcpy(&stash->brand[16], words, sizeof(unsigned int)*WORD_NUM);
   } else if (reg == 0x80000004) {
      memcpy(&stash->brand[32], words, sizeof(unsigned int)*WORD_NUM);
      printf("   brand = \"%s\"\n", stash->brand);
   } else if (reg == 0x80000005) {
      print_80000005_eax(words[WORD_EAX]);
      print_80000005_ebx(words[WORD_EBX]);
      print_80000005_ecx(words[WORD_ECX]);
      print_80000005_edx(words[WORD_EDX]);
   } else if (reg == 0x80000006) {
      print_80000006_eax(words[WORD_EAX]);
      print_80000006_ebx(words[WORD_EBX]);
      print_80000006_ecx(words[WORD_ECX], stash);
      print_80000006_edx(words[WORD_EDX]);
   } else if (reg == 0x80000007) {
      print_80000007_ebx(words[WORD_EBX]);
      print_80000007_ecx(words[WORD_ECX]);
      print_80000007_edx(words[WORD_EDX]);
   } else if (reg == 0x80000008) {
      print_80000008_eax(words[WORD_EAX]);
      print_80000008_ebx(words[WORD_EBX]);
      print_80000008_ecx(words[WORD_ECX]);
   } else if (reg == 0x80000009) {
      /* reserved for Intel feature flag expansion */
   } else if (reg == 0x8000000a) {
      print_8000000a_eax(words[WORD_EAX]);
      print_8000000a_edx(words[WORD_EDX]);
      print_8000000a_ebx(words[WORD_EBX]);
   } else if (0x8000000b <= reg && reg <= 0x80000018) {
      /* reserved for vendors to be determined feature flag expansion */
   } else if (reg == 0x80000019) {
      print_80000019_eax(words[WORD_EAX]);
      print_80000019_ebx(words[WORD_EBX]);
   } else if (reg == 0x8000001a) {
      print_8000001a_eax(words[WORD_EAX]);
   } else if (reg == 0x8000001b) {
      print_8000001b_eax(words[WORD_EAX]);
   } else if (reg == 0x8000001c) {
      print_8000001c_eax(words[WORD_EAX]);
      print_8000001c_edx(words[WORD_EDX]);
      print_8000001c_ebx(words[WORD_EBX]);
      print_8000001c_ecx(words[WORD_ECX]);
   } else if (reg == 0x8000001d) {
      printf("      --- cache %d ---\n", try);
      print_8000001d_eax(words[WORD_EAX]);
      print_8000001d_ebx(words[WORD_EBX]);
      printf("      number of sets                  = %u\n", words[WORD_ECX]);
      print_8000001d_edx(words[WORD_EDX]);
   } else if (reg == 0x8000001e) {
      printf("   extended APIC ID = %u\n", words[WORD_EAX]);
      print_8000001e_ebx(words[WORD_EBX]);
      print_8000001e_ecx(words[WORD_ECX]);
   } else if (reg == 0x8000001f) {
      // This is undocumented as of 17-May-2018, so names are tentative.
      printf("   SME/SEV (0x8000001f):\n");
      print_8000001f_eax(words[WORD_EAX]);
      printf("      MIN_SEV_ASID = 0x%0x (%u)\n",
             words[WORD_EDX], words[WORD_EDX]);
      printf("      MAX_SEV_ASID = 0x%0x (%u)\n",
             words[WORD_ECX], words[WORD_ECX]);
   } else if (reg == 0x80860000) {
      // max already set to words[WORD_EAX]
   } else if (reg == 0x80860001) {
      print_80860001_eax(words[WORD_EAX]);
      print_80860001_edx(words[WORD_EDX]);
      print_80860001_ebx_ecx(words[WORD_EBX], words[WORD_ECX]);
   } else if (reg == 0x80860002) {
      print_80860002_eax(words[WORD_EAX], stash);
      printf("   Transmeta CMS revision (0x80000002/ecx)"
             " = %u.%u-%u.%u-%u\n", 
             (words[WORD_EBX] >> 24) & 0xff,
             (words[WORD_EBX] >> 16) & 0xff,
             (words[WORD_EBX] >>  8) & 0xff,
             (words[WORD_EBX] >>  0) & 0xff,
             words[WORD_ECX]);
   } else if (reg == 0x80860003) {
      // DO NOTHING
   } else if (reg == 0x80860004) {
      // DO NOTHING
   } else if (reg == 0x80860005) {
      // DO NOTHING
   } else if (reg == 0x80860006) {
      printf("   Transmeta information = \"%s\"\n", stash->transmeta_info);
   } else if (reg == 0x80860007) {
      printf("   Transmeta core clock frequency = %u MHz\n",
             words[WORD_EAX]);
      printf("   Transmeta processor voltage    = %u mV\n",
             words[WORD_EBX]);
      printf("   Transmeta performance          = %u%%\n",
             words[WORD_ECX]);
      printf("   Transmeta gate delay           = %u fs\n",
             words[WORD_EDX]);
   } else if (reg == 0xc0000000) {
      // max already set to words[WORD_EAX]
   } else if (reg == 0xc0000001) {
      if (stash->vendor == VENDOR_VIA) {
         /* TODO: figure out how to decode 0xc0000001:eax */
         printf("   0x%08x 0x%02x: eax=0x%08x\n", 
                (unsigned int)reg, try, words[WORD_EAX]);
         print_c0000001_edx(words[WORD_EDX]);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else if (reg == 0xc0000002) {
      if (stash->vendor == VENDOR_VIA) {
         printf("   VIA C7 Temperature/Voltage Sensors (0xc0000002):\n");
         print_c0000002_eax(words[WORD_EAX]);
         print_c0000002_ebx(words[WORD_EBX]);
         /* TODO: figure out how to decode 0xc0000001:ecx & edx */
         printf("   0x%08x 0x%02x: ebx=0x%08x ecx=0x%08x edx=0x%08x\n",
                (unsigned int)reg, try,
                words[WORD_EBX], words[WORD_ECX], words[WORD_EDX]);
      } else {
         print_reg_raw(reg, try, words);
      }
   } else {
      print_reg_raw(reg, try, words);
   }
}

#define USE_INSTRUCTION  (-2)

#define MAX_CPUS  1024

static int
real_setup(unsigned int  cpu,
           boolean       one_cpu,
           boolean       inst)
{
   if (inst) {
      if (!one_cpu) {
         /*
         ** This test is necessary because some versions of Linux will accept
         ** a sched_setaffinity mask that includes only nonexistent CPUs.
         */
         static unsigned int  num_cpus = 0;
         if (num_cpus == 0) {
            num_cpus = sysconf(_SC_NPROCESSORS_CONF);
         }
         if (cpu >= num_cpus) return -1;

#ifdef USE_KERNEL_SCHED_SETAFFINITY
         /*
         ** The interface for sched_setaffinity and cpusets has changed many
         ** times.  Insulate this tool from all that by calling the system
         ** service directly.
         */
         unsigned int  mask[MAX_CPUS / (sizeof(unsigned int)*8)];
         bzero(&mask, sizeof(mask));
         mask[cpu / (sizeof(unsigned int)*8)]
            = (1 << cpu % (sizeof(unsigned int)*8));

         int  status;
         status = syscall(__NR_sched_setaffinity, 0, sizeof(mask), &mask);
#else
         cpu_set_t  cpuset;
         CPU_ZERO(&cpuset);
         CPU_SET(cpu, &cpuset);
         int  status;
         status = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
         if (status == -1) {
            if (cpu > 0) {
               if (errno == EINVAL) return -1;
            }

            fprintf(stderr, 
                    "%s: unable to setaffinity to cpu %d; errno = %d (%s)\n", 
                    program, cpu, errno, strerror(errno));
            fprintf(stderr, 
                    "%s: using -1 will avoid trying to setaffinity and run"
                    " on an arbitrary CPU\n",
                    program);
            exit(1);
         }
         
         sleep(0); /* to have a chance to migrate */
      }

      return USE_INSTRUCTION;
   } else {
#ifdef USE_CPUID_MODULE
      int    cpuid_fd = -1;
      char   cpuid_name[20];

      if (cpuid_fd == -1 && cpu == 0) {
         cpuid_fd = open("/dev/cpuid", O_RDONLY);
         if (cpuid_fd == -1 && errno != ENOENT) {
            fprintf(stderr, 
                    "%s: cannot open /dev/cpuid; errno = %d (%s)\n", 
                    program, errno, strerror(errno));
            explain_dev_cpu_errno();
         }
      }

      if (cpuid_fd == -1) {
         sprintf(cpuid_name, "/dev/cpu/%u/cpuid", cpu);
         cpuid_fd = open(cpuid_name, O_RDONLY);
         if (cpuid_fd == -1) {
            if (cpu > 0) {
               if (errno == ENXIO)  return -1;
               if (errno == ENODEV) return -1;
            }
            if (errno != ENOENT) {
               fprintf(stderr, 
                       "%s: cannot open /dev/cpuid or %s; errno = %d (%s)\n", 
                       program, cpuid_name, errno, strerror(errno));
               explain_dev_cpu_errno();
            }
         }
      }

      if (cpuid_fd == -1) {
         /*
         ** Lots of Linux's omit the /dev/cpuid or /dev/cpu/%u/cpuid files.
         ** Try creating a temporary file with mknod.
         **
         ** mkstemp is of absolutely no security value here because I can't
         ** use the actual file it generates, and have to delete it and
         ** re-create it with mknod.  But I have to use it anyway to
         ** eliminate errors from smartypants gcc/glibc during the link if I
         ** attempt to use tempnam.
         */
         char  tmpname[20];
         int   dummy_fd;
         strcpy(tmpname, "/tmp/cpuidXXXXXX");
         dummy_fd = mkstemp(tmpname);
         if (dummy_fd != -1) {
            close(dummy_fd);
            remove(tmpname);
            {
               int  status = mknod(tmpname,
                                   (S_IFCHR | S_IRUSR),
                                   makedev(CPUID_MAJOR, cpu));
               if (status == 0) {
                  cpuid_fd = open(tmpname, O_RDONLY);
                  remove(tmpname);
               }
            }
         }
         if (cpuid_fd == -1) {
            if (cpu > 0) {
               if (errno == ENXIO)  return -1;
               if (errno == ENODEV) return -1;
            }
            fprintf(stderr, 
                    "%s: cannot open /dev/cpuid or %s; errno = %d (%s)\n", 
                    program, cpuid_name, errno, strerror(errno));
            explain_dev_cpu_errno();
         }
      }

      return cpuid_fd;
#else
      return -1;
#endif
   }
}

static int real_get (int           cpuid_fd,
                     unsigned int  reg,
                     unsigned int  words[],
                     unsigned int  ecx,
                     boolean       quiet)
{
   if (cpuid_fd == USE_INSTRUCTION) {
#ifdef USE_CPUID_COUNT
      __cpuid_count(reg, ecx,
                    words[WORD_EAX],
                    words[WORD_EBX],
                    words[WORD_ECX],
                    words[WORD_EDX]);
#else
      asm("cpuid"
          : "=a" (words[WORD_EAX]),
            "=b" (words[WORD_EBX]),
            "=c" (words[WORD_ECX]),
            "=d" (words[WORD_EDX])
          : "a" (reg), 
            "c" (ecx));
#endif
   } else {
      off64_t  result;
      off64_t  offset = ((off64_t)ecx << 32) + reg;
      int      status;

      result = lseek64(cpuid_fd, offset, SEEK_SET);
      if (result == -1) {
         if (quiet) {
            return FALSE;
         } else {
            fprintf(stderr,
                    "%s: unable to seek cpuid file to offset 0x%llx;"
                    " errno = %d (%s)\n",
                    program, (long long unsigned)offset, 
                    errno, strerror(errno));
            exit(1);
         }
      }

      unsigned int  old_words[WORD_NUM];
      if (ecx != 0) memcpy(old_words, words, sizeof(old_words));

      status = read(cpuid_fd, words, 16);
      if (status == -1) {
         if (quiet) {
            return FALSE;
         } else {
            fprintf(stderr,
                    "%s: unable to read cpuid file at offset 0x%llx;"
                    " errno = %d (%s)\n",
                    program, (long long unsigned)offset,
                    errno, strerror(errno));
            exit(1);
         }
      }

      if (ecx != 0 && memcmp(old_words, words, sizeof(old_words)) == 0) {
         if (quiet) {
            return FALSE;
         } else {
            static boolean  said = FALSE;
            if (!said) {
               fprintf(stderr,
                       "%s: reading cpuid file at offset 0x%llx produced"
                       " duplicate results\n",
                       program, (long long unsigned)offset);
               fprintf(stderr,
                       "%s: older kernels do not support cpuid ecx control\n",
                       program);
               fprintf(stderr,
                       "%s: consider not using -k\n",
                       program);
               said = TRUE;
            }
            bzero(words, sizeof(old_words));
            return FALSE;
         }
      }
   }

   return TRUE;
}

static void
print_header (unsigned int  reg,
              unsigned int  try,
              boolean       raw)
{
   if (!raw && try == 0) {
      if (reg == 2) {
         printf("   cache and TLB information (2):\n");
      } else if (reg == 4) {
         printf("   deterministic cache parameters (4):\n");
      } else if (reg == 7) {
         printf("   extended feature flags (7):\n");
      } else if (reg == 0xb) {
         printf("   x2APIC features / processor topology (0xb):\n");
      } else if (reg == 0x40000003 && try == 0) {
         printf("   hypervisor time features (0x40000003/00):\n");
      } else if (reg == 0x40000003 && try == 1) {
         printf("   hypervisor time scale & offset (0x40000003/01):\n");
      } else if (reg == 0x40000003 && try == 2) {
         printf("   hypervisor time physical cpu frequency (0x40000003/02):\n");
      } else if (reg == 0x8000001d) {
         printf("   Cache Properties (0x8000001d):\n");
      }
   }
}

static void
do_real_one(unsigned int  reg,
            unsigned int  try,
            boolean       one_cpu,
            boolean       inst,
            boolean       raw,
            boolean       debug)
{
   unsigned int  cpu;

   for (cpu = 0;; cpu++) {
      int            cpuid_fd   = -1;
      code_stash_t   stash      = NIL_STASH;

      if (one_cpu && cpu > 0) break;

      cpuid_fd = real_setup(cpu, one_cpu, inst);
      if (cpuid_fd == -1) break;

      if (inst && one_cpu) {
         printf("CPU:\n");
      } else {
         printf("CPU %u:\n", cpu);
      }

      unsigned int  words[WORD_NUM];
      real_get(cpuid_fd, reg, words, try, FALSE);
      print_reg(reg, words, raw, try, &stash);
   }
}

static void
do_real(boolean  one_cpu,
        boolean  inst,
        boolean  raw,
        boolean  debug)
{
   unsigned int  cpu;

   for (cpu = 0;; cpu++) {
      int            cpuid_fd   = -1;
      code_stash_t   stash      = NIL_STASH;
      unsigned int   max;
      unsigned int   reg;

      if (one_cpu && cpu > 0) break;

      cpuid_fd = real_setup(cpu, one_cpu, inst);
      if (cpuid_fd == -1) break;

      if (inst && one_cpu) {
         printf("CPU:\n");
      } else {
         printf("CPU %u:\n", cpu);
      }

      max = 0;
      for (reg = 0; reg <= max; reg++) {
         unsigned int  words[WORD_NUM];

         real_get(cpuid_fd, reg, words, 0, FALSE);

         if (reg == 0) {
            max = words[WORD_EAX];
         }

         if (reg == 2) {
            unsigned int  max_tries = words[WORD_EAX] & 0xff;
            unsigned int  try       = 0;

            print_header(reg, try, raw);

            for (;;) {
               print_reg(reg, words, raw, try, &stash);

               try++;
               if (try >= max_tries) break;

               real_get(cpuid_fd, reg, words, 0, FALSE);
            }
         } else if (reg == 4) {
            unsigned int  try = 0;
            while ((words[WORD_EAX] & 0x1f) != 0) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               try++;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else if (reg == 7) {
            unsigned int  try = 0;
            unsigned int  max_tries;
            for (;;) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               if (try == 0) {
                  max_tries = words[WORD_EAX];
               }
               try++;
               if (try > max_tries) break;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else if (reg == 0xb) {
            unsigned int  try = 0;
            while (words[WORD_EAX] != 0 || words[WORD_EBX] != 0) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               try++;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else if (reg == 0xd) {
            /* 
            ** ecx values 0 & 1 are special.
            ** 
            ** Intel:
            **    For ecx values 2..63, the leaf is present if the corresponding
            **    bit is present in the bit catenation of 0xd/0/edx + 0xd/0/eax,
            **    or the bit catenation of 0xd/1/edx + 0xd/1/ecx.
            ** AMD:
            **    Only 4 ecx values are defined and it's gappy.  It's unclear
            **    what the upper bound of any loop would be, so it seems
            **    inappropriate to use one.
            */
            print_header(reg, 0, raw);
            print_reg(reg, words, raw, 0, &stash);
            unsigned long long  valid_xcr0
               = ((unsigned long long)words[WORD_EDX] << 32) | words[WORD_EAX];
            real_get(cpuid_fd, reg, words, 1, FALSE);
            print_reg(reg, words, raw, 1, &stash);
            unsigned long long  valid_xss
               = ((unsigned long long)words[WORD_EDX] << 32) | words[WORD_ECX];
            unsigned long long  valid_tries = valid_xcr0 | valid_xss;
            unsigned int  try;
            for (try = 2; try < 63; try++) {
               if (valid_tries & (1ull << try)) {
                  real_get(cpuid_fd, reg, words, try, FALSE);
                  print_reg(reg, words, raw, try, &stash);
               }
            }
         } else if (reg == 0xf) {
            unsigned int  mask = words[WORD_EDX];
            print_header(reg, 0, raw);
            print_reg(reg, words, raw, 0, &stash);
            if (BIT_EXTRACT_LE(mask, 1, 2)) {
               real_get(cpuid_fd, reg, words, 1, FALSE);
               print_reg(reg, words, raw, 1, &stash);
            }
         } else if (reg == 0x10) {
            unsigned int  mask = words[WORD_EBX];
            print_header(reg, 0, raw);
            print_reg(reg, words, raw, 0, &stash);
            unsigned int  try;
            for (try = 1; try < 32; try++) {
               if (mask & (1 << try)) {
                  real_get(cpuid_fd, reg, words, try, FALSE);
                  print_reg(reg, words, raw, try, &stash);
               }
            }
         } else if (reg == 0x12) {
            unsigned int  mask = words[WORD_EAX];
            print_header(reg, 0, raw);
            print_reg(reg, words, raw, 0, &stash);
            unsigned int  try;
            for (try = 1; try < 33; try++) {
               if (mask & (1 << (try-1))) {
                  real_get(cpuid_fd, reg, words, try, FALSE);
                  print_reg(reg, words, raw, try, &stash);
               }
            }
         } else if (reg == 0x14) {
            unsigned int  try = 0;
            unsigned int  max_tries;
            for (;;) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               if (try == 0) {
                  max_tries = words[WORD_EAX];
               }
               try++;
               if (try > max_tries) break;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else if (reg == 0x17) {
            unsigned int  try = 0;
            unsigned int  max_tries;
            for (;;) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               if (try == 0) {
                  max_tries = words[WORD_EAX];
               }
               try++;
               if (try > max_tries) break;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else if (reg == 0x18) {
            unsigned int  try = 0;
            unsigned int  max_tries;
            for (;;) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               if (try == 0) {
                  max_tries = words[WORD_EAX];
               }
               try++;
               if (try > max_tries) break;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else {
            print_reg(reg, words, raw, 0, &stash);
         }
      }

      if (BIT_EXTRACT_LE(stash.val_1_ecx, 31, 32)) {
         max = 0x40000000;
         for (reg = 0x40000000; reg <= max; reg++) {
            boolean       success;
            unsigned int  words[WORD_NUM];

            success = real_get(cpuid_fd, reg, words, 0, TRUE);
            if (!success) break;

            if (reg == 0x40000000) {
               max = words[WORD_EAX];
            }

            if (reg == 0x40000003 && stash.hypervisor == HYPERVISOR_XEN) {
               unsigned int  try = 0;
               for (; try <= 2; try++) {
                  print_header(reg, try, raw);
                  print_reg(reg, words, raw, try, &stash);
                  try++;
                  real_get(cpuid_fd, reg, words, try, FALSE);
               }
            } else {
               print_reg(reg, words, raw, 0, &stash);
            }

            if (reg == 0x40000000
                && stash.hypervisor == HYPERVISOR_KVM
                && max == 0) {
               max = 0x40000001;
            }
            if (reg == 0x40000000
                && stash.hypervisor == HYPERVISOR_UNKNOWN
                && max > 0x40001000) {
               // Assume some busted cpuid information and stop walking
               // further 0x4xxxxxxx registers.
               max = 0x40000000;
            }
         }
      }

      max = 0x80000000;
      for (reg = 0x80000000; reg <= max; reg++) {
         boolean       success;
         unsigned int  words[WORD_NUM];

         success = real_get(cpuid_fd, reg, words, 0, TRUE);
         if (!success) break;

         if (reg == 0x80000000) {
            max = words[WORD_EAX];
         }

         if (reg == 0x8000001d) {
            unsigned int  try = 0;
            while ((words[WORD_EAX] & 0x1f) != 0) {
               print_header(reg, try, raw);
               print_reg(reg, words, raw, try, &stash);
               try++;
               real_get(cpuid_fd, reg, words, try, FALSE);
            }
         } else {
            print_reg(reg, words, raw, 0, &stash);
         }
      }

      max = 0x80860000;
      for (reg = 0x80860000; reg <= max; reg++) {
         boolean       success;
         unsigned int  words[WORD_NUM];

         success = real_get(cpuid_fd, reg, words, 0, TRUE);
         if (!success) break;

         if (reg == 0x80860000) {
            max = words[WORD_EAX];
         }

         print_reg(reg, words, raw, 0, &stash);
      }

      max = 0xc0000000;
      for (reg = 0xc0000000; reg <= max; reg++) {
         boolean       success;
         unsigned int  words[WORD_NUM];

         success = real_get(cpuid_fd, reg, words, 0, TRUE);
         if (!success) break;

         if (reg == 0xc0000000) {
            max = words[WORD_EAX];
         }

         if (max > 0xc0001000) {
            // Assume some busted cpuid information and stop walking
            // further 0x4xxxxxxx registers.
            max = 0xc0000000;
         }

         print_reg(reg, words, raw, 0, &stash);
      }
      
      do_final(raw, debug, &stash);

      close(cpuid_fd);
   }
}

static void
do_file(ccstring  filename,
        boolean   raw,
        boolean   debug)
{
   boolean       seen_cpu    = FALSE;
   unsigned int  cpu         = -1;
   /*
   ** The try* variables are a kludge to deal with those leaves that depended on
   ** the try (a.k.a. ecx) values that existed with cpuid's old-style method of
   ** dumping raw leaves, which lacked an explicit indication of the try number.
   ** It is not necessary to add more kludges for more modern ecx-dependent
   ** leaves.
   */
   unsigned int  try2        = -1;
   unsigned int  try4        = -1;
   unsigned int  try7        = -1;
   unsigned int  tryb        = -1;
   unsigned int  try8000001d = -1;
   code_stash_t  stash       = NIL_STASH;

   FILE*  file;
   if (strcmp(filename, "-") == 0) {
      file = stdin;
   } else {
      file = fopen(filename, "r");
      if (file == NULL) {
         fprintf(stderr,
                 "%s: unable to open %s; errno = %d (%s)\n",
                 program, filename, errno, strerror(errno));
         exit(1);
      }
   }

   while (!feof(file)) {
      char          buffer[88];
      char*         ptr;
      int           status;
      unsigned int  reg;
      unsigned int  try;
      unsigned int  words[WORD_NUM];

      ptr = fgets(buffer, LENGTH(buffer, char), file);
      if (ptr == NULL && errno == 0) break;
      if (ptr == NULL) {
         if (errno != EPIPE) {
            fprintf(stderr,
                    "%s: unable to read a line of text from %s;"
                    " errno = %d (%s)\n",
                    program, filename, errno, strerror(errno));
         }
         exit(1);
      }

      status = sscanf(ptr, "CPU %u:\r", &cpu);
      if (status == 1 || strcmp(ptr, "CPU:\n") == SAME) {
         if (seen_cpu) {
            do_final(raw, debug, &stash);
         }

         seen_cpu = TRUE;

         if (status == 1) {
            printf("CPU %u:\n", cpu);
         } else {
            printf("CPU:\n");
         }
         try2        = 0;
         try4        = 0;
         try7        = 0;
         tryb        = 0;
         try8000001d = 0;
         {
            static code_stash_t  empty_stash = NIL_STASH;
            stash = empty_stash;
         }
         continue;
      }

      status = sscanf(ptr,
                      "   0x%x 0x%x: eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\r",
                      &reg, &try,
                      &words[WORD_EAX], &words[WORD_EBX],
                      &words[WORD_ECX], &words[WORD_EDX]);
      if (status == 6) {
         print_header(reg, try, raw);
         print_reg(reg, words, raw, try, &stash);
         continue;
      }
      status = sscanf(ptr,
                      "   0x%x: eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\r",
                      &reg, 
                      &words[WORD_EAX], &words[WORD_EBX],
                      &words[WORD_ECX], &words[WORD_EDX]);
      if (status == 5) {
         if (reg == 2) {
            print_header(reg, try2, raw);
            print_reg(reg, words, raw, try2++, &stash);
         } else if (reg == 4) {
            print_header(reg, try4, raw);
            print_reg(reg, words, raw, try4++, &stash);
         } else if (reg == 7) {
            print_header(reg, try7, raw);
            print_reg(reg, words, raw, try7++, &stash);
         } else if (reg == 0xb) {
            print_header(reg, tryb, raw);
            print_reg(reg, words, raw, tryb++, &stash);
         } else if (reg == 0x8000001d) {
            print_header(reg, try8000001d, raw);
            print_reg(reg, words, raw, try8000001d++, &stash);
         } else {
            print_reg(reg, words, raw, 0, &stash);
         }
         continue;
      }

      fprintf(stderr,
              "%s: unexpected input with -f option: %s\n",
              program, ptr);
      exit(1);
   }

   if (seen_cpu) {
      do_final(raw, debug, &stash);
   }

   if (file != stdin) {
      fclose(file);
   }
}

int
main(int     argc,
     string  argv[])
{
   static ccstring             shortopts  = "+hH1ikrdf:vl:s:";
   static const struct option  longopts[] = {
      { "help",    no_argument,       NULL, 'h'  },
      { "one-cpu", no_argument,       NULL, '1'  },
      { "inst",    no_argument,       NULL, 'i'  },
      { "kernel",  no_argument,       NULL, 'k'  },
      { "raw",     no_argument,       NULL, 'r'  },
      { "debug",   no_argument,       NULL, 'd'  },
      { "file",    required_argument, NULL, 'f'  },
      { "version", no_argument,       NULL, 'v'  },
      { "leaf",    required_argument, NULL, 'l'  },
      { "subleaf", required_argument, NULL, 's'  },
      { NULL,      no_argument,       NULL, '\0' }
   };

   boolean        opt_one_cpu     = FALSE;
   boolean        opt_inst        = FALSE;
   boolean        opt_kernel      = FALSE;
   boolean        opt_raw         = FALSE;
   boolean        opt_debug       = FALSE;
   cstring        opt_filename    = NULL;
   boolean        opt_version     = FALSE;
   boolean        opt_leaf        = FALSE;
   unsigned long  opt_leaf_val    = 0;
   boolean        opt_subleaf     = FALSE;
   unsigned long  opt_subleaf_val = 0;

   program = strrchr(argv[0], '/');
   if (program == NULL) {
      program = argv[0];
   } else {
      program++;
   }

   opterr = 0;

   for (;;) {
      int  longindex;
      int  opt = getopt_long(argc, argv, shortopts, longopts, &longindex);

      if (opt == EOF) break;

      switch (opt) {
      case 'h':
      case 'H':
         usage();
         /*NOTREACHED*/
      case '1':
         opt_one_cpu = TRUE;
         break;
      case 'i':
         opt_inst = TRUE;
         break;
      case 'k':
         opt_kernel = TRUE;
         break;
      case 'r':
         opt_raw = TRUE;
         break;
      case 'd':
         opt_debug = TRUE;
         break;
      case 'f':
         opt_filename = optarg;
         break;
      case 'v':
         opt_version = TRUE;
         break;
      case 'l':
         opt_leaf = TRUE;
         {
            errno = 0;
            char* endptr = NULL;
            opt_leaf_val = strtoul(optarg, &endptr, 0);
            if (errno != 0) {
               fprintf(stderr,
                       "%s: argument to -l/--leaf not understood: %s\n",
                       program, argv[optind-1]);
               exit(1);
            }
         }
         break;
      case 's':
         opt_subleaf = TRUE;
         {
            errno = 0;
            char* endptr = NULL;
            opt_subleaf_val = strtoul(optarg, &endptr, 0);
            if (errno != 0) {
               fprintf(stderr,
                       "%s: argument to -s/--subleaf not understood: %s\n",
                       program, argv[optind-1]);
               exit(1);
            }
         }
         break;
      case '?':
      default:
         if (optopt == '\0') {
            fprintf(stderr,
                    "%s: unrecogized option: %s\n", program, argv[optind-1]);
         } else {
            fprintf(stderr, 
                    "%s: unrecognized option letter: %c\n", program, optopt);
         }
         usage();
         /*NOTREACHED*/
      }
   }

   if (optind < argc) {
      fprintf(stderr, "%s: unrecognized argument: %s\n", program, argv[optind]);
      usage();
      /*NOTREACHED*/
   }

#ifndef USE_CPUID_MODULE
   if (opt_kernel) {
      fprintf(stderr, "%s: unrecognized argument: -k\n", program);
      usage();
      /*NOTREACHED*/
   }
#endif

   if (opt_inst && opt_kernel) {
      fprintf(stderr,
              "%s: -i/--inst and -k/--kernel are incompatible options\n", 
              program);
      exit(1);
   }

   if (opt_filename != NULL && opt_leaf) {
      fprintf(stderr,
              "%s: -f/--file and -l/--leaf are incompatible options\n",
              program);
      exit(1);
   }

   if (opt_subleaf && !opt_leaf) {
      fprintf(stderr,
              "%s: -s/--subleaf requires that -l/--leaf also be specified\n",
              program);
      exit(1);
   }

   // Default to -i.  So use inst unless -k is specified.
   boolean  inst = !opt_kernel;

   if (opt_version) {
      printf("cpuid version %s\n", XSTR(VERSION));
   } else {
      if (opt_filename != NULL) {
         do_file(opt_filename, opt_raw, opt_debug);
      } else if (opt_leaf) {
         do_real_one(opt_leaf_val, opt_subleaf_val,
                     opt_one_cpu, inst, opt_raw, opt_debug);
      } else {
         do_real(opt_one_cpu, inst, opt_raw, opt_debug);
      }
   }

   exit(0);
   /*NOTREACHED*/
}
