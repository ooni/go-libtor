/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file sandbox.c
 * \brief Code to enable sandboxing.
 **/

#include "orconfig.h"

#ifndef _LARGEFILE64_SOURCE
/**
 * Temporarily required for O_LARGEFILE flag. Needs to be removed
 * with the libevent fix.
 */
#define _LARGEFILE64_SOURCE
#endif /* !defined(_LARGEFILE64_SOURCE) */

/** Malloc mprotect limit in bytes.
 *
 * 28/06/2017: This value was increased from 16 MB to 20 MB after we introduced
 * LZMA support in Tor (0.3.1.1-alpha). We limit our LZMA coder to 16 MB, but
 * liblzma have a small overhead that we need to compensate for to avoid being
 * killed by the sandbox.
 */
#define MALLOC_MP_LIM (20*1024*1024)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "lib/sandbox/sandbox.h"
#include "lib/container/map.h"
#include "lib/err/torerr.h"
#include "lib/log/log.h"
#include "lib/cc/torint.h"
#include "lib/malloc/malloc.h"
#include "lib/string/scanf.h"

#include "ext/tor_queue.h"
#include "ext/ht.h"
#include "ext/siphash.h"

#define DEBUGGING_CLOSE

#if defined(USE_LIBSECCOMP)

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <linux/futex.h>
#include <sys/file.h>

#include <stdarg.h>
#include <seccomp.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#ifdef HAVE_GNU_LIBC_VERSION_H
#include <gnu/libc-version.h>
#endif
#ifdef HAVE_LINUX_NETFILTER_IPV4_H
#include <linux/netfilter_ipv4.h>
#endif
#ifdef HAVE_LINUX_IF_H
#include <linux/if.h>
#endif
#ifdef HAVE_LINUX_NETFILTER_IPV6_IP6_TABLES_H
#include <linux/netfilter_ipv6/ip6_tables.h>
#endif

#if defined(HAVE_EXECINFO_H) && defined(HAVE_BACKTRACE) && \
  defined(HAVE_BACKTRACE_SYMBOLS_FD) && defined(HAVE_SIGACTION)
#define USE_BACKTRACE
#define BACKTRACE_PRIVATE
#include "lib/err/backtrace.h"
#endif /* defined(HAVE_EXECINFO_H) && defined(HAVE_BACKTRACE) && ... */

#ifdef USE_BACKTRACE
#include <execinfo.h>
#endif

/**
 * Linux 32 bit definitions
 */
#if defined(__i386__)

#define REG_SYSCALL REG_EAX
#define M_SYSCALL gregs[REG_SYSCALL]

/**
 * Linux 64 bit definitions
 */
#elif defined(__x86_64__)

#define REG_SYSCALL REG_RAX
#define M_SYSCALL gregs[REG_SYSCALL]

#elif defined(__arm__)

#define M_SYSCALL arm_r7

#elif defined(__aarch64__) && defined(__LP64__)

#define REG_SYSCALL 8
#define M_SYSCALL regs[REG_SYSCALL]

#endif /* defined(__i386__) || ... */

#ifdef M_SYSCALL
#define SYSCALL_NAME_DEBUGGING
#endif

/**Determines if at least one sandbox is active.*/
static int sandbox_active = 0;
/** Holds the parameter list configuration for the sandbox.*/
static sandbox_cfg_t *filter_dynamic = NULL;

#undef SCMP_CMP
#define SCMP_CMP(a,b,c) ((struct scmp_arg_cmp){(a),(b),(c),0})
#define SCMP_CMP_STR(a,b,c) \
  ((struct scmp_arg_cmp) {(a),(b),(intptr_t)(void*)(c),0})
#define SCMP_CMP4(a,b,c,d) ((struct scmp_arg_cmp){(a),(b),(c),(d)})
/* We use a wrapper here because these masked comparisons seem to be pretty
 * verbose. Also, it's important to cast to scmp_datum_t before negating the
 * mask, since otherwise the negation might get applied to a 32 bit value, and
 * the high bits of the value might get masked out improperly. */
#define SCMP_CMP_MASKED(a,b,c) \
  SCMP_CMP4((a), SCMP_CMP_MASKED_EQ, ~(scmp_datum_t)(b), (c))
/* For negative constants, the rule to add depends on the glibc version. */
#define SCMP_CMP_NEG(a,op,b) (libc_negative_constant_needs_cast() ? \
                              (SCMP_CMP((a), (op), (unsigned int)(b))) : \
                              (SCMP_CMP_STR((a), (op), (b))))

/** Variable used for storing all syscall numbers that will be allowed with the
 * stage 1 general Tor sandbox.
 */
static int filter_nopar_gen[] = {
    SCMP_SYS(access),
    SCMP_SYS(brk),
    SCMP_SYS(clock_gettime),
    SCMP_SYS(close),
    SCMP_SYS(clone),
    SCMP_SYS(dup),
#ifdef __NR_clone3
    SCMP_SYS(clone3),
#endif
    SCMP_SYS(epoll_create),
    SCMP_SYS(epoll_wait),
#ifdef __NR_epoll_pwait
    SCMP_SYS(epoll_pwait),
#endif
#ifdef HAVE_EVENTFD
    SCMP_SYS(eventfd2),
#endif
#ifdef HAVE_PIPE2
    SCMP_SYS(pipe2),
#endif
#ifdef HAVE_PIPE
    SCMP_SYS(pipe),
#endif
#ifdef __NR_fchmod
    SCMP_SYS(fchmod),
#endif
    SCMP_SYS(fcntl),
    SCMP_SYS(fstat),
#ifdef __NR_fstat64
    SCMP_SYS(fstat64),
#endif
    SCMP_SYS(fsync),
    SCMP_SYS(futex),
    SCMP_SYS(getdents),
    SCMP_SYS(getdents64),
    SCMP_SYS(getegid),
#ifdef __NR_getegid32
    SCMP_SYS(getegid32),
#endif
    SCMP_SYS(geteuid),
#ifdef __NR_geteuid32
    SCMP_SYS(geteuid32),
#endif
    SCMP_SYS(getgid),
#ifdef __NR_getgid32
    SCMP_SYS(getgid32),
#endif
    SCMP_SYS(getpid),
#ifdef __NR_getrlimit
    SCMP_SYS(getrlimit),
#endif
    SCMP_SYS(gettimeofday),
    SCMP_SYS(gettid),
    SCMP_SYS(getuid),
#ifdef __NR_getuid32
    SCMP_SYS(getuid32),
#endif
    SCMP_SYS(lseek),
#ifdef __NR__llseek
    SCMP_SYS(_llseek),
#endif
    // glob uses this..
    SCMP_SYS(lstat),
    SCMP_SYS(mkdir),
    SCMP_SYS(mlockall),
#ifdef __NR_mmap
    /* XXXX restrict this in the same ways as mmap2 */
    SCMP_SYS(mmap),
#endif
    SCMP_SYS(munmap),
#ifdef __NR_nanosleep
    SCMP_SYS(nanosleep),
#endif
#ifdef __NR_prlimit
    SCMP_SYS(prlimit),
#endif
#ifdef __NR_prlimit64
    SCMP_SYS(prlimit64),
#endif
    SCMP_SYS(read),
    SCMP_SYS(rt_sigreturn),
#ifdef __NR_rseq
    SCMP_SYS(rseq),
#endif
    SCMP_SYS(sched_getaffinity),
#ifdef __NR_sched_yield
    SCMP_SYS(sched_yield),
#endif
    SCMP_SYS(sendmsg),
    SCMP_SYS(set_robust_list),
#ifdef __NR_setrlimit
    SCMP_SYS(setrlimit),
#endif
    SCMP_SYS(shutdown),
#ifdef __NR_sigaltstack
    SCMP_SYS(sigaltstack),
#endif
#ifdef __NR_sigreturn
    SCMP_SYS(sigreturn),
#endif
    SCMP_SYS(stat),
    SCMP_SYS(uname),
    SCMP_SYS(wait4),
    SCMP_SYS(write),
    SCMP_SYS(writev),
    SCMP_SYS(exit_group),
    SCMP_SYS(exit),

    SCMP_SYS(madvise),
#ifdef __NR_stat64
    // getaddrinfo uses this..
    SCMP_SYS(stat64),
#endif

#ifdef __NR_getrandom
    SCMP_SYS(getrandom),
#endif

#ifdef __NR_sysinfo
    // qsort uses this..
    SCMP_SYS(sysinfo),
#endif
    /*
     * These socket syscalls are not required on x86_64 and not supported with
     * some libseccomp versions (eg: 1.0.1)
     */
#if defined(__i386)
    SCMP_SYS(recv),
    SCMP_SYS(send),
#endif

    // socket syscalls
    SCMP_SYS(bind),
    SCMP_SYS(listen),
    SCMP_SYS(connect),
    SCMP_SYS(getsockname),
#ifdef ENABLE_NSS
#ifdef __NR_getpeername
    SCMP_SYS(getpeername),
#endif
#endif
    SCMP_SYS(recvmsg),
    SCMP_SYS(recvfrom),
    SCMP_SYS(sendto),
    SCMP_SYS(unlink),
#ifdef __NR_unlinkat
    SCMP_SYS(unlinkat),
#endif
    SCMP_SYS(poll)
};

/* opendir is not a syscall but it will use either open or openat. We do not
 * want the decision to allow open/openat to be the callers reponsability, so
 * we create a phony syscall number for opendir and sb_opendir will choose the
 * correct syscall. */
#define PHONY_OPENDIR_SYSCALL -2

/* These macros help avoid the error where the number of filters we add on a
 * single rule don't match the arg_cnt param. */
#define seccomp_rule_add_0(ctx,act,call) \
  seccomp_rule_add((ctx),(act),(call),0)
#define seccomp_rule_add_1(ctx,act,call,f1) \
  seccomp_rule_add((ctx),(act),(call),1,(f1))
#define seccomp_rule_add_2(ctx,act,call,f1,f2)  \
  seccomp_rule_add((ctx),(act),(call),2,(f1),(f2))
#define seccomp_rule_add_3(ctx,act,call,f1,f2,f3)       \
  seccomp_rule_add((ctx),(act),(call),3,(f1),(f2),(f3))
#define seccomp_rule_add_4(ctx,act,call,f1,f2,f3,f4)      \
  seccomp_rule_add((ctx),(act),(call),4,(f1),(f2),(f3),(f4))

static const char *sandbox_get_interned_string(const char *str);

/**
 * Function responsible for setting up the rt_sigaction syscall for
 * the seccomp filter sandbox.
 */
static int
sb_rt_sigaction(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  unsigned i;
  int rc;
  int param[] = { SIGINT, SIGTERM, SIGPIPE, SIGUSR1, SIGUSR2, SIGHUP, SIGCHLD,
                  SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGSYS, SIGIO,
#ifdef SIGXFSZ
      SIGXFSZ
#endif
      };
  (void) filter;

  for (i = 0; i < ARRAY_LENGTH(param); i++) {
    rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigaction),
        SCMP_CMP(0, SCMP_CMP_EQ, param[i]));
    if (rc)
      break;
  }

  return rc;
}

/**
 * Function responsible for setting up the time syscall for
 * the seccomp filter sandbox.
 */
static int
sb_time(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  (void) filter;
#ifdef __NR_time
  return seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(time),
       SCMP_CMP(0, SCMP_CMP_EQ, 0));
#else
  return 0;
#endif /* defined(__NR_time) */
}

/**
 * Function responsible for setting up the accept4 syscall for
 * the seccomp filter sandbox.
 */
static int
sb_accept4(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void)filter;

#ifdef __i386__
  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socketcall),
      SCMP_CMP(0, SCMP_CMP_EQ, 18));
  if (rc) {
    return rc;
  }
#endif /* defined(__i386__) */

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(accept4),
                   SCMP_CMP_MASKED(3, SOCK_CLOEXEC|SOCK_NONBLOCK, 0));
  if (rc) {
    return rc;
  }

  return 0;
}

#ifdef __NR_mmap2
/**
 * Function responsible for setting up the mmap2 syscall for
 * the seccomp filter sandbox.
 */
static int
sb_mmap2(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void)filter;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
       SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ),
       SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
       SCMP_CMP(2, SCMP_CMP_EQ, PROT_NONE),
       SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
       SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE),
       SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE|MAP_ANONYMOUS));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
       SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE),
       SCMP_CMP(3, SCMP_CMP_EQ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE),
      SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE),
      SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS));
  if (rc) {
    return rc;
  }

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_EXEC),
      SCMP_CMP(3, SCMP_CMP_EQ, MAP_PRIVATE|MAP_DENYWRITE));
  if (rc) {
    return rc;
  }

  return 0;
}
#endif /* defined(__NR_mmap2) */

#ifdef HAVE_GNU_LIBC_VERSION_H
#ifdef HAVE_GNU_GET_LIBC_VERSION
#define CHECK_LIBC_VERSION
#endif
#endif

/* Return true the libc version is greater or equal than
 * <b>major</b>.<b>minor</b>. Returns false otherwise. */
static int
is_libc_at_least(int major, int minor)
{
#ifdef CHECK_LIBC_VERSION
  const char *version = gnu_get_libc_version();
  if (version == NULL)
    return 0;

  int libc_major = -1;
  int libc_minor = -1;

  tor_sscanf(version, "%d.%d", &libc_major, &libc_minor);
  if (libc_major > major)
    return 1;
  else if (libc_major == major && libc_minor >= minor)
    return 1;
  else
    return 0;
#else /* !defined(CHECK_LIBC_VERSION) */
  (void)major;
  (void)minor;
  return 0;
#endif /* defined(CHECK_LIBC_VERSION) */
}

/* Return true if we think we're running with a libc that uses openat for the
 * open function on linux. */
static int
libc_uses_openat_for_open(void)
{
  return is_libc_at_least(2, 26);
}

/* Return true if we think we're running with a libc that uses openat for the
 * opendir function on linux. */
static int
libc_uses_openat_for_opendir(void)
{
  // libc 2.27 and above or between 2.15 (inclusive) and 2.22 (exclusive)
  return is_libc_at_least(2, 27) ||
         (is_libc_at_least(2, 15) && !is_libc_at_least(2, 22));
}

/* Return true if we think we're running with a libc that needs to cast
 * negative arguments like AT_FDCWD for seccomp rules. */
static int
libc_negative_constant_needs_cast(void)
{
  return is_libc_at_least(2, 27);
}

/** Allow a single file to be opened.  If <b>use_openat</b> is true,
 * we're using a libc that remaps all the opens into openats. */
static int
allow_file_open(scmp_filter_ctx ctx, int use_openat, const char *file)
{
  if (use_openat) {
    return seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat),
                              SCMP_CMP_NEG(0, SCMP_CMP_EQ, AT_FDCWD),
                              SCMP_CMP_STR(1, SCMP_CMP_EQ, file));
  } else {
    return seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(open),
                              SCMP_CMP_STR(0, SCMP_CMP_EQ, file));
  }
}

/**
 * Function responsible for setting up the open syscall for
 * the seccomp filter sandbox.
 */
static int
sb_open(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  int use_openat = libc_uses_openat_for_open();

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && param->syscall
        == SCMP_SYS(open)) {
      rc = allow_file_open(ctx, use_openat, param->value);
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add open syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

static int
sb_chmod(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && param->syscall
        == SCMP_SYS(chmod)) {
      rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(chmod),
            SCMP_CMP_STR(0, SCMP_CMP_EQ, param->value));
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add chmod syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

static int
sb_chown(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && param->syscall
        == SCMP_SYS(chown)) {
      rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(chown),
            SCMP_CMP_STR(0, SCMP_CMP_EQ, param->value));
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add chown syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

/**
 * Function responsible for setting up the rename syscall for
 * the seccomp filter sandbox.
 */
static int
sb_rename(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 &&
        param->syscall == SCMP_SYS(rename)) {

      rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rename),
            SCMP_CMP_STR(0, SCMP_CMP_EQ, param->value),
            SCMP_CMP_STR(1, SCMP_CMP_EQ, param->value2));
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add rename syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

/**
 * Function responsible for setting up the openat syscall for
 * the seccomp filter sandbox.
 */
static int
sb_openat(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && param->syscall
        == SCMP_SYS(openat)) {
      rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat),
          SCMP_CMP_NEG(0, SCMP_CMP_EQ, AT_FDCWD),
          SCMP_CMP_STR(1, SCMP_CMP_EQ, param->value),
          SCMP_CMP(2, SCMP_CMP_EQ, O_RDONLY|O_NONBLOCK|O_LARGEFILE|O_DIRECTORY|
              O_CLOEXEC));
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add openat syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

static int
sb_opendir(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && param->syscall
        == PHONY_OPENDIR_SYSCALL) {
      rc = allow_file_open(ctx, libc_uses_openat_for_opendir(), param->value);
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add openat syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}

/**
 * Function responsible for setting up the socket syscall for
 * the seccomp filter sandbox.
 */
static int
sb_socket(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  int i, j;
  (void) filter;

#ifdef __i386__
  rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket));
  if (rc)
    return rc;
#endif

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
      SCMP_CMP(0, SCMP_CMP_EQ, PF_FILE),
      SCMP_CMP_MASKED(1, SOCK_CLOEXEC|SOCK_NONBLOCK, SOCK_STREAM));
  if (rc)
    return rc;

  for (i = 0; i < 2; ++i) {
    const int pf = i ? PF_INET : PF_INET6;
    for (j=0; j < 3; ++j) {
      const int type     = (j == 0) ? SOCK_STREAM :
                                      SOCK_DGRAM;
      const int protocol = (j == 0) ? IPPROTO_TCP :
                           (j == 1) ? IPPROTO_IP :
                                      IPPROTO_UDP;
      rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
        SCMP_CMP(0, SCMP_CMP_EQ, pf),
        SCMP_CMP_MASKED(1, SOCK_CLOEXEC|SOCK_NONBLOCK, type),
        SCMP_CMP(2, SCMP_CMP_EQ, protocol));
      if (rc)
        return rc;
    }
  }

#ifdef ENABLE_NSS
  rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
    SCMP_CMP(0, SCMP_CMP_EQ, PF_INET),
    SCMP_CMP(1, SCMP_CMP_EQ, SOCK_STREAM),
    SCMP_CMP(2, SCMP_CMP_EQ, IPPROTO_IP));
  if (rc)
    return rc;
#endif /* defined(ENABLE_NSS) */

  rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
      SCMP_CMP(0, SCMP_CMP_EQ, PF_UNIX),
      SCMP_CMP_MASKED(1, SOCK_CLOEXEC|SOCK_NONBLOCK, SOCK_STREAM),
      SCMP_CMP(2, SCMP_CMP_EQ, 0));
  if (rc)
    return rc;

  rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
      SCMP_CMP(0, SCMP_CMP_EQ, PF_UNIX),
      SCMP_CMP_MASKED(1, SOCK_CLOEXEC|SOCK_NONBLOCK, SOCK_DGRAM),
      SCMP_CMP(2, SCMP_CMP_EQ, 0));
  if (rc)
    return rc;

  rc = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket),
      SCMP_CMP(0, SCMP_CMP_EQ, PF_NETLINK),
      SCMP_CMP_MASKED(1, SOCK_CLOEXEC, SOCK_RAW),
      SCMP_CMP(2, SCMP_CMP_EQ, 0));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the socketpair syscall for
 * the seccomp filter sandbox.
 */
static int
sb_socketpair(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

#ifdef __i386__
  rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socketpair));
  if (rc)
    return rc;
#endif

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socketpair),
      SCMP_CMP(0, SCMP_CMP_EQ, PF_FILE),
      SCMP_CMP(1, SCMP_CMP_EQ, SOCK_STREAM|SOCK_CLOEXEC));
  if (rc)
    return rc;

  return 0;
}

#ifdef HAVE_KIST_SUPPORT

#include <linux/sockios.h>

static int
sb_ioctl(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl),
                          SCMP_CMP(1, SCMP_CMP_EQ, SIOCOUTQNSD));
  if (rc)
    return rc;
  return 0;
}

#endif /* defined(HAVE_KIST_SUPPORT) */

/**
 * Function responsible for setting up the setsockopt syscall for
 * the seccomp filter sandbox.
 */
static int
sb_setsockopt(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

#ifdef __i386__
  rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt));
  if (rc)
    return rc;
#endif

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_REUSEADDR));
  if (rc)
    return rc;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_SNDBUF));
  if (rc)
    return rc;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_RCVBUF));
  if (rc)
    return rc;

#ifdef HAVE_SYSTEMD
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_SNDBUFFORCE));
  if (rc)
    return rc;
#endif /* defined(HAVE_SYSTEMD) */

#ifdef IP_TRANSPARENT
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_IP),
      SCMP_CMP(2, SCMP_CMP_EQ, IP_TRANSPARENT));
  if (rc)
    return rc;
#endif /* defined(IP_TRANSPARENT) */

#ifdef IPV6_V6ONLY
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, IPPROTO_IPV6),
      SCMP_CMP(2, SCMP_CMP_EQ, IPV6_V6ONLY));
  if (rc)
    return rc;
#endif /* defined(IPV6_V6ONLY) */

  return 0;
}

/**
 * Function responsible for setting up the getsockopt syscall for
 * the seccomp filter sandbox.
 */
static int
sb_getsockopt(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

#ifdef __i386__
  rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt));
  if (rc)
    return rc;
#endif

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_ERROR));
  if (rc)
    return rc;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_ACCEPTCONN));
  if (rc)
    return rc;

#ifdef HAVE_SYSTEMD
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_SOCKET),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_SNDBUF));
  if (rc)
    return rc;
#endif /* defined(HAVE_SYSTEMD) */

#ifdef HAVE_LINUX_NETFILTER_IPV4_H
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_IP),
      SCMP_CMP(2, SCMP_CMP_EQ, SO_ORIGINAL_DST));
  if (rc)
    return rc;
#endif /* defined(HAVE_LINUX_NETFILTER_IPV4_H) */

#ifdef HAVE_LINUX_NETFILTER_IPV6_IP6_TABLES_H
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_IPV6),
      SCMP_CMP(2, SCMP_CMP_EQ, IP6T_SO_ORIGINAL_DST));
  if (rc)
    return rc;
#endif /* defined(HAVE_LINUX_NETFILTER_IPV6_IP6_TABLES_H) */

#ifdef HAVE_KIST_SUPPORT
#include <netinet/tcp.h>
  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getsockopt),
      SCMP_CMP(1, SCMP_CMP_EQ, SOL_TCP),
      SCMP_CMP(2, SCMP_CMP_EQ, TCP_INFO));
  if (rc)
    return rc;
#endif /* defined(HAVE_KIST_SUPPORT) */

  return 0;
}

#ifdef __NR_fcntl64
/**
 * Function responsible for setting up the fcntl64 syscall for
 * the seccomp filter sandbox.
 */
static int
sb_fcntl64(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl64),
      SCMP_CMP(1, SCMP_CMP_EQ, F_GETFL));
  if (rc)
    return rc;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl64),
      SCMP_CMP(1, SCMP_CMP_EQ, F_SETFL),
      SCMP_CMP(2, SCMP_CMP_EQ, O_RDWR|O_NONBLOCK));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl64),
      SCMP_CMP(1, SCMP_CMP_EQ, F_GETFD));
  if (rc)
    return rc;

  rc = seccomp_rule_add_2(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl64),
      SCMP_CMP(1, SCMP_CMP_EQ, F_SETFD),
      SCMP_CMP(2, SCMP_CMP_EQ, FD_CLOEXEC));
  if (rc)
    return rc;

  return 0;
}
#endif /* defined(__NR_fcntl64) */

/**
 * Function responsible for setting up the epoll_ctl syscall for
 * the seccomp filter sandbox.
 *
 *  Note: basically allows everything but will keep for now..
 */
static int
sb_epoll_ctl(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_ctl),
      SCMP_CMP(1, SCMP_CMP_EQ, EPOLL_CTL_ADD));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_ctl),
      SCMP_CMP(1, SCMP_CMP_EQ, EPOLL_CTL_MOD));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_ctl),
      SCMP_CMP(1, SCMP_CMP_EQ, EPOLL_CTL_DEL));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the prctl syscall for
 * the seccomp filter sandbox.
 *
 * NOTE: if multiple filters need to be added, the PR_SECCOMP parameter needs
 * to be allowlisted in this function.
 */
static int
sb_prctl(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(prctl),
      SCMP_CMP(0, SCMP_CMP_EQ, PR_SET_DUMPABLE));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the mprotect syscall for
 * the seccomp filter sandbox.
 *
 * NOTE: does not NEED to be here.. currently only occurs before filter; will
 * keep just in case for the future.
 */
static int
sb_mprotect(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_NONE));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the rt_sigprocmask syscall for
 * the seccomp filter sandbox.
 */
static int
sb_rt_sigprocmask(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask),
      SCMP_CMP(0, SCMP_CMP_EQ, SIG_UNBLOCK));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask),
      SCMP_CMP(0, SCMP_CMP_EQ, SIG_SETMASK));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the flock syscall for
 * the seccomp filter sandbox.
 *
 *  NOTE: does not need to be here, occurs before filter is applied.
 */
static int
sb_flock(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(flock),
      SCMP_CMP(1, SCMP_CMP_EQ, LOCK_EX|LOCK_NB));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(flock),
      SCMP_CMP(1, SCMP_CMP_EQ, LOCK_UN));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the futex syscall for
 * the seccomp filter sandbox.
 */
static int
sb_futex(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  // can remove
  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex),
      SCMP_CMP(1, SCMP_CMP_EQ,
          FUTEX_WAIT_BITSET_PRIVATE|FUTEX_CLOCK_REALTIME));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex),
      SCMP_CMP(1, SCMP_CMP_EQ, FUTEX_WAKE_PRIVATE));
  if (rc)
    return rc;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex),
      SCMP_CMP(1, SCMP_CMP_EQ, FUTEX_WAIT_PRIVATE));
  if (rc)
    return rc;

  return 0;
}

/**
 * Function responsible for setting up the mremap syscall for
 * the seccomp filter sandbox.
 *
 *  NOTE: so far only occurs before filter is applied.
 */
static int
sb_mremap(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  (void) filter;

  rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mremap),
      SCMP_CMP(3, SCMP_CMP_EQ, MREMAP_MAYMOVE));
  if (rc)
    return rc;

  return 0;
}

#ifdef __NR_stat64
/**
 * Function responsible for setting up the stat64 syscall for
 * the seccomp filter sandbox.
 */
static int
sb_stat64(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  int rc = 0;
  sandbox_cfg_t *elem = NULL;

  // for each dynamic parameter filters
  for (elem = filter; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param != NULL && param->prot == 1 && (param->syscall == SCMP_SYS(open)
        || param->syscall == SCMP_SYS(stat64))) {
      rc = seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(stat64),
          SCMP_CMP_STR(0, SCMP_CMP_EQ, param->value));
      if (rc != 0) {
        log_err(LD_BUG,"(Sandbox) failed to add stat64 syscall, received "
            "libseccomp error %d", rc);
        return rc;
      }
    }
  }

  return 0;
}
#endif /* defined(__NR_stat64) */

static int
sb_kill(scmp_filter_ctx ctx, sandbox_cfg_t *filter)
{
  (void) filter;
#ifdef __NR_kill
  /* Allow killing anything with signal 0 -- it isn't really a kill. */
  return seccomp_rule_add_1(ctx, SCMP_ACT_ALLOW, SCMP_SYS(kill),
       SCMP_CMP(1, SCMP_CMP_EQ, 0));
#else
  return 0;
#endif /* defined(__NR_kill) */
}

/**
 * Array of function pointers responsible for filtering different syscalls at
 * a parameter level.
 */
static sandbox_filter_func_t filter_func[] = {
    sb_rt_sigaction,
    sb_rt_sigprocmask,
    sb_time,
    sb_accept4,
#ifdef __NR_mmap2
    sb_mmap2,
#endif
    sb_chown,
    sb_chmod,
    sb_open,
    sb_openat,
    sb_opendir,
    sb_rename,
#ifdef __NR_fcntl64
    sb_fcntl64,
#endif
    sb_epoll_ctl,
    sb_prctl,
    sb_mprotect,
    sb_flock,
    sb_futex,
    sb_mremap,
#ifdef __NR_stat64
    sb_stat64,
#endif

    sb_socket,
    sb_setsockopt,
    sb_getsockopt,
    sb_socketpair,
#ifdef HAVE_KIST_SUPPORT
    sb_ioctl,
#endif
    sb_kill
};

/**
 * Return the interned (and hopefully sandbox-permitted) string equal
 * to @a str.
 *
 * Return NULL if `str` is NULL, or `str` is not an interned string.
 **/
const char *
sandbox_intern_string(const char *str)
{
  const char *interned = sandbox_get_interned_string(str);

  if (sandbox_active && str != NULL && interned == NULL) {
    log_warn(LD_BUG, "No interned sandbox parameter found for %s", str);
  }

  return interned ? interned : str;
}

/**
 * Return true if the sandbox is running and we are missing an interned string
 * equal to @a str.
 */
bool
sandbox_interned_string_is_missing(const char *str)
{
  return sandbox_active && sandbox_get_interned_string(str) == NULL;
}

/**
 * Try to find and return the interned string equal to @a str.
 *
 * If there is no such string, return NULL.
 **/
static const char *
sandbox_get_interned_string(const char *str)
{
  sandbox_cfg_t *elem;

  if (str == NULL)
    return NULL;

  for (elem = filter_dynamic; elem != NULL; elem = elem->next) {
    smp_param_t *param = elem->param;

    if (param->prot) {
      if (!strcmp(str, (char*)(param->value))) {
        return (char*)param->value;
      }
      if (param->value2 && !strcmp(str, (char*)param->value2)) {
        return (char*)param->value2;
      }
    }
  }

  return NULL;
}

/* DOCDOC */
static int
prot_strings_helper(strmap_t *locations,
                    char **pr_mem_next_p,
                    size_t *pr_mem_left_p,
                    char **value_p)
{
  char *param_val;
  size_t param_size;
  void *location;

  if (*value_p == 0)
    return 0;

  param_val = (char*) *value_p;
  param_size = strlen(param_val) + 1;
  location = strmap_get(locations, param_val);

  if (location) {
    // We already interned this string.
    tor_free(param_val);
    *value_p = location;
    return 0;
  } else if (*pr_mem_left_p >= param_size) {
    // copy to protected
    location = *pr_mem_next_p;
    memcpy(location, param_val, param_size);

    // re-point el parameter to protected
    tor_free(param_val);
    *value_p = location;

    strmap_set(locations, location, location); /* good real estate advice */

    // move next available protected memory
    *pr_mem_next_p += param_size;
    *pr_mem_left_p -= param_size;
    return 0;
  } else {
    log_err(LD_BUG,"(Sandbox) insufficient protected memory!");
    return -1;
  }
}

/**
 * Protects all the strings in the sandbox's parameter list configuration. It
 * works by calculating the total amount of memory required by the parameter
 * list, allocating the memory using mmap, and protecting it from writes with
 * mprotect().
 */
static int
prot_strings(scmp_filter_ctx ctx, sandbox_cfg_t* cfg)
{
  int ret = 0;
  size_t pr_mem_size = 0, pr_mem_left = 0;
  char *pr_mem_next = NULL, *pr_mem_base;
  sandbox_cfg_t *el = NULL;
  strmap_t *locations = NULL;

  // get total number of bytes required to mmap. (Overestimate.)
  for (el = cfg; el != NULL; el = el->next) {
    pr_mem_size += strlen((char*) el->param->value) + 1;
    if (el->param->value2)
      pr_mem_size += strlen((char*) el->param->value2) + 1;
  }

  // allocate protected memory with MALLOC_MP_LIM canary
  pr_mem_base = (char*) mmap(NULL, MALLOC_MP_LIM + pr_mem_size,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (pr_mem_base == MAP_FAILED) {
    log_err(LD_BUG,"(Sandbox) failed allocate protected memory! mmap: %s",
        strerror(errno));
    ret = -1;
    goto out;
  }

  pr_mem_next = pr_mem_base + MALLOC_MP_LIM;
  pr_mem_left = pr_mem_size;

  locations = strmap_new();

  // change el value pointer to protected
  for (el = cfg; el != NULL; el = el->next) {
    if (prot_strings_helper(locations, &pr_mem_next, &pr_mem_left,
                            &el->param->value) < 0) {
      ret = -2;
      goto out;
    }
    if (prot_strings_helper(locations, &pr_mem_next, &pr_mem_left,
                            &el->param->value2) < 0) {
      ret = -2;
      goto out;
    }
    el->param->prot = 1;
  }

  // protecting from writes
  if (mprotect(pr_mem_base, MALLOC_MP_LIM + pr_mem_size, PROT_READ)) {
    log_err(LD_BUG,"(Sandbox) failed to protect memory! mprotect: %s",
        strerror(errno));
    ret = -3;
    goto out;
  }

  /*
   * Setting sandbox restrictions so the string memory cannot be tampered with
   */
  // no mremap of the protected base address
  ret = seccomp_rule_add_1(ctx, SCMP_ACT_KILL, SCMP_SYS(mremap),
      SCMP_CMP(0, SCMP_CMP_EQ, (intptr_t) pr_mem_base));
  if (ret) {
    log_err(LD_BUG,"(Sandbox) mremap protected memory filter fail!");
    goto out;
  }

  // no munmap of the protected base address
  ret = seccomp_rule_add_1(ctx, SCMP_ACT_KILL, SCMP_SYS(munmap),
        SCMP_CMP(0, SCMP_CMP_EQ, (intptr_t) pr_mem_base));
  if (ret) {
    log_err(LD_BUG,"(Sandbox) munmap protected memory filter fail!");
    goto out;
  }

  /*
   * Allow mprotect with PROT_READ|PROT_WRITE because openssl uses it, but
   * never over the memory region used by the protected strings.
   *
   * PROT_READ|PROT_WRITE was originally fully allowed in sb_mprotect(), but
   * had to be removed due to limitation of libseccomp regarding intervals.
   *
   * There is a restriction on how much you can mprotect with R|W up to the
   * size of the canary.
   */
  ret = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect),
      SCMP_CMP(0, SCMP_CMP_LT, (intptr_t) pr_mem_base),
      SCMP_CMP(1, SCMP_CMP_LE, MALLOC_MP_LIM),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE));
  if (ret) {
    log_err(LD_BUG,"(Sandbox) mprotect protected memory filter fail (LT)!");
    goto out;
  }

  ret = seccomp_rule_add_3(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect),
      SCMP_CMP(0, SCMP_CMP_GT, (intptr_t) pr_mem_base + pr_mem_size +
          MALLOC_MP_LIM),
      SCMP_CMP(1, SCMP_CMP_LE, MALLOC_MP_LIM),
      SCMP_CMP(2, SCMP_CMP_EQ, PROT_READ|PROT_WRITE));
  if (ret) {
    log_err(LD_BUG,"(Sandbox) mprotect protected memory filter fail (GT)!");
    goto out;
  }

 out:
  strmap_free(locations, NULL);
  return ret;
}

/**
 * Auxiliary function used in order to allocate a sandbox_cfg_t element and set
 * its values according the parameter list. All elements are initialised
 * with the 'prot' field set to false, as the pointer is not protected at this
 * point.
 */
static sandbox_cfg_t*
new_element2(int syscall, char *value, char *value2)
{
  smp_param_t *param = NULL;

  sandbox_cfg_t *elem = tor_malloc_zero(sizeof(sandbox_cfg_t));
  param = elem->param = tor_malloc_zero(sizeof(smp_param_t));

  param->syscall = syscall;
  param->value = value;
  param->value2 = value2;
  param->prot = 0;

  return elem;
}

static sandbox_cfg_t*
new_element(int syscall, char *value)
{
  return new_element2(syscall, value, NULL);
}

#ifdef __NR_stat64
#define SCMP_stat SCMP_SYS(stat64)
#else
#define SCMP_stat SCMP_SYS(stat)
#endif

int
sandbox_cfg_allow_stat_filename(sandbox_cfg_t **cfg, char *file)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(SCMP_stat, file);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_open_filename(sandbox_cfg_t **cfg, char *file)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(SCMP_SYS(open), file);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_chmod_filename(sandbox_cfg_t **cfg, char *file)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(SCMP_SYS(chmod), file);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_chown_filename(sandbox_cfg_t **cfg, char *file)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(SCMP_SYS(chown), file);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_rename(sandbox_cfg_t **cfg, char *file1, char *file2)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element2(SCMP_SYS(rename), file1, file2);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_openat_filename(sandbox_cfg_t **cfg, char *file)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(SCMP_SYS(openat), file);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

int
sandbox_cfg_allow_opendir_dirname(sandbox_cfg_t **cfg, char *dir)
{
  sandbox_cfg_t *elem = NULL;

  elem = new_element(PHONY_OPENDIR_SYSCALL, dir);

  elem->next = *cfg;
  *cfg = elem;

  return 0;
}

/**
 * Function responsible for going through the parameter syscall filters and
 * call each function pointer in the list.
 */
static int
add_param_filter(scmp_filter_ctx ctx, sandbox_cfg_t* cfg)
{
  unsigned i;
  int rc = 0;

  // function pointer
  for (i = 0; i < ARRAY_LENGTH(filter_func); i++) {
    rc = filter_func[i](ctx, cfg);
    if (rc) {
      log_err(LD_BUG,"(Sandbox) failed to add syscall %d, received libseccomp "
          "error %d", i, rc);
      return rc;
    }
  }

  return 0;
}

/**
 * Function responsible of loading the libseccomp syscall filters which do not
 * have parameter filtering.
 */
static int
add_noparam_filter(scmp_filter_ctx ctx)
{
  unsigned i;
  int rc = 0;

  // add general filters
  for (i = 0; i < ARRAY_LENGTH(filter_nopar_gen); i++) {
    rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, filter_nopar_gen[i]);
    if (rc != 0) {
      log_err(LD_BUG,"(Sandbox) failed to add syscall index %d (NR=%d), "
          "received libseccomp error %d", i, filter_nopar_gen[i], rc);
      return rc;
    }
  }

  if (is_libc_at_least(2, 33)) {
#ifdef __NR_newfstatat
    // Libc 2.33 uses this syscall to implement both fstat() and stat().
    //
    // The trouble is that to implement fstat(fd, &st), it calls:
    //     newfstatat(fs, "", &st, AT_EMPTY_PATH)
    // We can't detect this usage in particular, because "" is a pointer
    // we don't control.  And we can't just look for AT_EMPTY_PATH, since
    // AT_EMPTY_PATH only has effect when the path string is empty.
    //
    // So our only solution seems to be allowing all fstatat calls, which
    // means that an attacker can stat() anything on the filesystem. That's
    // not a great solution, but I can't find a better one.
    rc = seccomp_rule_add_0(ctx, SCMP_ACT_ALLOW, SCMP_SYS(newfstatat));
    if (rc != 0) {
      log_err(LD_BUG,"(Sandbox) failed to add newfstatat() syscall; "
          "received libseccomp error %d", rc);
      return rc;
    }
#endif
  }

  return 0;
}

/**
 * Function responsible for setting up and enabling a global syscall filter.
 * The function is a prototype developed for stage 1 of sandboxing Tor.
 * Returns 0 on success.
 */
static int
install_syscall_filter(sandbox_cfg_t* cfg)
{
  int rc = 0;
  scmp_filter_ctx ctx;

  ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
  if (ctx == NULL) {
    log_err(LD_BUG,"(Sandbox) failed to initialise libseccomp context");
    rc = -1;
    goto end;
  }

  // protecting sandbox parameter strings
  if ((rc = prot_strings(ctx, cfg))) {
    goto end;
  }

  // add parameter filters
  if ((rc = add_param_filter(ctx, cfg))) {
    log_err(LD_BUG, "(Sandbox) failed to add param filters!");
    goto end;
  }

  // adding filters with no parameters
  if ((rc = add_noparam_filter(ctx))) {
    log_err(LD_BUG, "(Sandbox) failed to add param filters!");
    goto end;
  }

  // loading the seccomp2 filter
  if ((rc = seccomp_load(ctx))) {
    log_err(LD_BUG, "(Sandbox) failed to load: %d (%s)! "
            "Are you sure that your kernel has seccomp2 support? The "
            "sandbox won't work without it.", rc,
            strerror(-rc));
    goto end;
  }

  // marking the sandbox as active
  sandbox_active = 1;

 end:
  seccomp_release(ctx);
  return (rc < 0 ? -rc : rc);
}

#ifdef SYSCALL_NAME_DEBUGGING
#include "lib/sandbox/linux_syscalls.inc"

/** Return a string containing the name of a given syscall (if we know it) */
static const char *
get_syscall_name(int syscall_num)
{
  int i;
  for (i = 0; SYSCALLS_BY_NUMBER[i].syscall_name; ++i) {
    if (SYSCALLS_BY_NUMBER[i].syscall_num == syscall_num)
      return SYSCALLS_BY_NUMBER[i].syscall_name;
  }

  {
     static char syscall_name_buf[64];
     format_dec_number_sigsafe(syscall_num,
                               syscall_name_buf, sizeof(syscall_name_buf));
     return syscall_name_buf;
  }
}

/** Return the syscall number from a ucontext_t that we got in a signal
 * handler (if we know how to do that). */
static int
get_syscall_from_ucontext(const ucontext_t *ctx)
{
  return (int) ctx->uc_mcontext.M_SYSCALL;
}
#else /* !defined(SYSCALL_NAME_DEBUGGING) */
static const char *
get_syscall_name(int syscall_num)
{
  (void) syscall_num;
  return "unknown";
}
static int
get_syscall_from_ucontext(const ucontext_t *ctx)
{
  (void) ctx;
  return -1;
}
#endif /* defined(SYSCALL_NAME_DEBUGGING) */

#ifdef USE_BACKTRACE
#define MAX_DEPTH 256
static void *syscall_cb_buf[MAX_DEPTH];
#endif

/**
 * Function called when a SIGSYS is caught by the application. It notifies the
 * user that an error has occurred and either terminates or allows the
 * application to continue execution, based on the DEBUGGING_CLOSE symbol.
 */
static void
sigsys_debugging(int nr, siginfo_t *info, void *void_context)
{
  ucontext_t *ctx = (ucontext_t *) (void_context);
  const char *syscall_name;
#ifdef USE_BACKTRACE
  size_t depth;
  int n_fds, i;
  const int *fds = NULL;
#endif

  (void) nr;

  if (info->si_code != SYS_SECCOMP)
    return;

  if (!ctx)
    return;

  int syscall = get_syscall_from_ucontext(ctx);

#ifdef USE_BACKTRACE
  depth = backtrace(syscall_cb_buf, MAX_DEPTH);
  /* Clean up the top stack frame so we get the real function
   * name for the most recently failing function. */
  clean_backtrace(syscall_cb_buf, depth, ctx);
#endif /* defined(USE_BACKTRACE) */

  syscall_name = get_syscall_name(syscall);

  tor_log_err_sigsafe("(Sandbox) Caught a bad syscall attempt (syscall ",
                      syscall_name,
                      ")\n",
                      NULL);

#ifdef USE_BACKTRACE
  n_fds = tor_log_get_sigsafe_err_fds(&fds);
  for (i=0; i < n_fds; ++i)
    backtrace_symbols_fd(syscall_cb_buf, (int)depth, fds[i]);
#endif

#if defined(DEBUGGING_CLOSE)
  _exit(1); // exit ok: programming error has led to sandbox failure.
#endif // DEBUGGING_CLOSE
}

/**
 * Function that adds a handler for SIGSYS, which is the signal thrown
 * when the application is issuing a syscall which is not allowed. The
 * main purpose of this function is to help with debugging by identifying
 * filtered syscalls.
 */
static int
install_sigsys_debugging(void)
{
  struct sigaction act;
  sigset_t mask;

  memset(&act, 0, sizeof(act));
  sigemptyset(&mask);
  sigaddset(&mask, SIGSYS);

  act.sa_sigaction = &sigsys_debugging;
  act.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSYS, &act, NULL) < 0) {
    log_err(LD_BUG,"(Sandbox) Failed to register SIGSYS signal handler");
    return -1;
  }

  if (sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
    log_err(LD_BUG,"(Sandbox) Failed call to sigprocmask()");
    return -2;
  }

  return 0;
}

/**
 * Function responsible of registering the sandbox_cfg_t list of parameter
 * syscall filters to the existing parameter list. This is used for incipient
 * multiple-sandbox support.
 */
static int
register_cfg(sandbox_cfg_t* cfg)
{
  sandbox_cfg_t *elem = NULL;

  if (filter_dynamic == NULL) {
    filter_dynamic = cfg;
    return 0;
  }

  for (elem = filter_dynamic; elem->next != NULL; elem = elem->next)
    ;

  elem->next = cfg;

  return 0;
}

#endif /* defined(USE_LIBSECCOMP) */

#ifdef USE_LIBSECCOMP
/**
 * Initialises the syscall sandbox filter for any linux architecture, taking
 * into account various available features for different linux flavours.
 */
static int
initialise_libseccomp_sandbox(sandbox_cfg_t* cfg)
{
  /* Prevent glibc from trying to open /dev/tty on fatal error */
  setenv("LIBC_FATAL_STDERR_", "1", 1);

  if (install_sigsys_debugging())
    return -1;

  if (install_syscall_filter(cfg))
    return -2;

  if (register_cfg(cfg))
    return -3;

  return 0;
}

int
sandbox_is_active(void)
{
  return sandbox_active != 0;
}
#endif /* defined(USE_LIBSECCOMP) */

sandbox_cfg_t*
sandbox_cfg_new(void)
{
  return NULL;
}

int
sandbox_init(sandbox_cfg_t *cfg)
{
#if defined(USE_LIBSECCOMP)
  return initialise_libseccomp_sandbox(cfg);

#elif defined(__linux__)
  (void)cfg;
  log_warn(LD_GENERAL,
           "This version of Tor was built without support for sandboxing. To "
           "build with support for sandboxing on Linux, you must have "
           "libseccomp and its necessary header files (e.g. seccomp.h).");
  return 0;

#else
  (void)cfg;
  log_warn(LD_GENERAL,
           "Currently, sandboxing is only implemented on Linux. The feature "
           "is disabled on your platform.");
  return 0;
#endif /* defined(USE_LIBSECCOMP) || ... */
}

#ifndef USE_LIBSECCOMP
int
sandbox_cfg_allow_open_filename(sandbox_cfg_t **cfg, char *file)
{
  (void)cfg; (void)file;
  return 0;
}

int
sandbox_cfg_allow_openat_filename(sandbox_cfg_t **cfg, char *file)
{
  (void)cfg; (void)file;
  return 0;
}

int
sandbox_cfg_allow_opendir_dirname(sandbox_cfg_t **cfg, char *dir)
{
  (void)cfg; (void)dir;
  return 0;
}

int
sandbox_cfg_allow_stat_filename(sandbox_cfg_t **cfg, char *file)
{
  (void)cfg; (void)file;
  return 0;
}

int
sandbox_cfg_allow_chown_filename(sandbox_cfg_t **cfg, char *file)
{
  (void)cfg; (void)file;
  return 0;
}

int
sandbox_cfg_allow_chmod_filename(sandbox_cfg_t **cfg, char *file)
{
  (void)cfg; (void)file;
  return 0;
}

int
sandbox_cfg_allow_rename(sandbox_cfg_t **cfg, char *file1, char *file2)
{
  (void)cfg; (void)file1; (void)file2;
  return 0;
}

int
sandbox_is_active(void)
{
  return 0;
}

#endif /* !defined(USE_LIBSECCOMP) */
