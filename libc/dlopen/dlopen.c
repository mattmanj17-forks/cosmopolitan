/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2023 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/atomic.h"
#include "libc/calls/calls.h"
#include "libc/calls/internal.h"
#include "libc/calls/struct/sigset.h"
#include "libc/calls/struct/sigset.internal.h"
#include "libc/calls/syscall-sysv.internal.h"
#include "libc/calls/syscall_support-nt.internal.h"
#include "libc/cosmo.h"
#include "libc/dce.h"
#include "libc/dlopen/dlfcn.h"
#include "libc/elf/def.h"
#include "libc/elf/elf.h"
#include "libc/elf/struct/auxv.h"
#include "libc/elf/struct/ehdr.h"
#include "libc/elf/struct/phdr.h"
#include "libc/errno.h"
#include "libc/intrin/bits.h"
#include "libc/intrin/kprintf.h"
#include "libc/intrin/strace.internal.h"
#include "libc/limits.h"
#include "libc/nt/dll.h"
#include "libc/nt/enum/filemapflags.h"
#include "libc/nt/enum/pageflags.h"
#include "libc/nt/errors.h"
#include "libc/nt/memory.h"
#include "libc/nt/runtime.h"
#include "libc/runtime/runtime.h"
#include "libc/runtime/syslib.internal.h"
#include "libc/stdio/stdio.h"
#include "libc/stdio/sysparam.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/auxv.h"
#include "libc/sysv/consts/map.h"
#include "libc/sysv/consts/o.h"
#include "libc/sysv/consts/prot.h"
#include "libc/sysv/consts/sig.h"
#include "libc/sysv/errfuns.h"
#include "libc/thread/thread.h"
#include "libc/thread/tls.h"

/**
 * @fileoverview Cosmopolitan Dynamic Linker.
 *
 * Every program built using Cosmopolitan is statically-linked. However
 * there are some cases, e.g. GUIs and video drivers, where linking the
 * host platform libraries is desirable. So what we do in such cases is
 * launch a stub executable using the host platform's libc, and longjmp
 * back into this executable. The stub executable passes back to us the
 * platform-specific dlopen() implementation, which we shall then wrap.
 *
 * @kudos jacereda for figuring out how to do this
 */

__static_yoink(".dlopen.x86_64.musl.elf");
__static_yoink(".dlopen.x86_64.glibc.elf");
__static_yoink(".dlopen.x86_64.freebsd.elf");
__static_yoink(".dlopen.aarch64.glibc.elf");

#define PAGE_SIZE 4096

#define XNU_RTLD_LAZY   1
#define XNU_RTLD_NOW    2
#define XNU_RTLD_LOCAL  4
#define XNU_RTLD_GLOBAL 8

struct Loaded {
  char *base;
  char *entry;
  Elf64_Ehdr eh;
  Elf64_Phdr ph[30];
};

struct Context {
  sigset_t mask;
  struct CosmoTib *tib;
};

static struct {
  atomic_uint once;
  bool is_supported;
  struct CosmoTib *tib;
  void *(*dlopen)(const char *, int);
  void *(*dlsym)(void *, const char *);
  int (*dlclose)(void *);
  char *(*dlerror)(void);
  jmp_buf jb;
} foreign;

long __sysv2nt14();

static _Thread_local char dlerror_buf[128];

static void foreign_begin(struct Context *ctx) {
  sigset_t block = -1;
  sys_sigprocmask(SIG_SETMASK, &block, &ctx->mask);
  ctx->tib = __get_tls();
  __set_tls(foreign.tib);
}

static void foreign_end(struct Context *ctx) {
  __set_tls(ctx->tib);
  sys_sigprocmask(SIG_SETMASK, &ctx->mask, 0);
}

// on system five we sadly need this brutal trampoline
// todo(jart): add tls trampoline to sigaction() handlers
// todo(jart): morph binary to get tls from host c library
static long foreign_tramp(long a, long b, long c, long d, long e,
                          long func(long, long, long, long, long)) {
  long res;
  struct Context ctx;
  foreign_begin(&ctx);
  res = func(a, b, c, d, e);
  foreign_end(&ctx);
  return res;
}

static unsigned get_elf_prot(unsigned x) {
  unsigned r = 0;
  if (x & PF_R) r += PROT_READ;
  if (x & PF_W) r += PROT_WRITE;
  if (x & PF_X) r += PROT_EXEC;
  return r;
}

static int get_host_elf_machine(void) {
#ifdef __x86_64__
  return EM_NEXGEN32E;
#elif defined(__aarch64__)
  return EM_AARCH64;
#elif defined(__powerpc64__)
  return EM_PPC64;
#elif defined(__riscv)
  return EM_RISCV;
#elif defined(__s390x__)
  return EM_S390;
#else
#error "unsupported architecture"
#endif
}

static char *elf_map(int fd, Elf64_Ehdr *ehdr, Elf64_Phdr *phdr) {
  uintptr_t maxva = 0;
  uintptr_t minva = -1;
  for (Elf64_Phdr *p = phdr; p < &phdr[ehdr->e_phnum]; p++) {
    if (p->p_type != PT_LOAD) {
      continue;
    }
    if (p->p_vaddr < minva) {
      minva = p->p_vaddr;
    }
    if (p->p_vaddr + p->p_memsz > maxva) {
      maxva = p->p_vaddr + p->p_memsz;
    }
  }
  minva = minva & -PAGE_SIZE;
  uint8_t *base =
      __sys_mmap(0, maxva - minva, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0, 0);
  if (base == MAP_FAILED) {
    return MAP_FAILED;
  }
  __sys_munmap(base, maxva - minva);
  for (Elf64_Phdr *p = phdr; p < &phdr[ehdr->e_phnum]; p++) {
    if (p->p_type != PT_LOAD) {
      continue;
    }
    uintptr_t skew = p->p_vaddr & (PAGE_SIZE - 1);
    uint8_t *start = base + p->p_vaddr - skew;
    size_t mapsize = skew + p->p_memsz;
    uint8_t *m = __sys_mmap(start, mapsize, PROT_READ | PROT_WRITE,
                            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 0);
    if (m == MAP_FAILED) {
      return MAP_FAILED;
    }
    ssize_t rr = pread(fd, m + skew, p->p_filesz, p->p_offset);
    if (rr != (ssize_t)p->p_filesz) {
      return MAP_FAILED;
    }
    if (sys_mprotect(m, mapsize, get_elf_prot(p->p_flags))) {
      return MAP_FAILED;
    }
  }
  return (void *)base;
}

static int elf_open(const char *file) {
  return open(file, O_RDONLY | O_CLOEXEC);
}

static bool elf_slurp(struct Loaded *l, int fd, const char *file) {
  if (pread(fd, &l->eh, 64, 0) != 64) {
    return false;
  }
  if (!IsElf64Binary(&l->eh, 64) ||                      //
      l->eh.e_phnum > sizeof(l->ph) / sizeof(*l->ph) ||  //
      l->eh.e_machine != get_host_elf_machine()) {
    enoexec();
    return false;
  }
  int bytes = l->eh.e_phnum * sizeof(l->ph[0]);
  if (pread(fd, l->ph, bytes, l->eh.e_phoff) != bytes) {
    return false;
  }
  l->entry = (char *)l->eh.e_entry;
  return true;
}

static bool elf_load(struct Loaded *l, const char *file) {
  int fd;
  if ((fd = elf_open(file)) == -1) {
    return false;
  }
  if (!elf_slurp(l, fd, file)) {
    close(fd);
    return false;
  }
  if ((l->base = elf_map(fd, &l->eh, l->ph)) == MAP_FAILED) {
    close(fd);
    return false;
  }
  l->entry += (uintptr_t)l->base;
  close(fd);
  return true;
}

static bool elf_interp(char *buf, size_t bsz, const char *file) {
  int fd;
  if ((fd = elf_open(file)) == -1) {
    return false;
  }
  struct Loaded l;
  if (!elf_slurp(&l, fd, file)) {
    close(fd);
    return false;
  }
  for (unsigned i = 0; i < l.eh.e_phnum; i++) {
    if (l.ph[i].p_type == PT_INTERP) {
      if (l.ph[i].p_filesz >= bsz ||
          pread(fd, buf, l.ph[i].p_filesz, l.ph[i].p_offset) !=
              l.ph[i].p_filesz) {
        close(fd);
        return false;
      }
      break;
    }
  }
  close(fd);
  return true;
}

static long *push_strs(long *sp, char **list, int count) {
  *--sp = 0;
  while (count) *--sp = (long)list[--count];
  return sp;
}

static void elf_exec(const char *file, const char *iinterp, int argc,
                     char **argv, char **envp) {
  struct Loaded prog;
  if (!elf_load(&prog, file)) return;

  struct Loaded interp;
  if (!elf_load(&interp, iinterp)) return;

  // count environment variables
  int envc = 0;
  while (envp[envc]) envc++;

  // count auxiliary values
  int auxc = 0;
  Elf64_auxv_t *av;
  for (av = (Elf64_auxv_t *)__auxv; av->a_type; ++av) auxc++;

  // create environment block for embedded process
  // the platform libc will save its location for getenv(), etc.
  // we need just enough stack memory beneath it for initialization
  char *map;
  size_t stksize = 65536;
  size_t stkalign = sizeof(char *) * 2;
  size_t argsize = (argc + 1 + envc + 1 + auxc * 2 + 1) * sizeof(char *);
  size_t mapsize = (stksize + argsize + (PAGE_SIZE - 1)) & -PAGE_SIZE;
  size_t skew = (mapsize - argsize) & (stkalign - 1);
  map = __sys_mmap(0, mapsize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 0);
  if (map == MAP_FAILED) return;
  long *sp = (long *)(map + mapsize - skew);

  // push auxiliary values
  *--sp = 0;
  unsigned long key, val;
  for (av = (Elf64_auxv_t *)__auxv; (key = av->a_type); ++av) {
    val = av->a_un.a_val;
    if (key == AT_PHDR) val = (long)(prog.base + prog.eh.e_phoff);
    if (key == AT_PHENT) val = prog.eh.e_phentsize;
    if (key == AT_PHNUM) val = prog.eh.e_phnum;
    if (key == AT_PAGESZ) val = PAGE_SIZE;
    if (key == AT_BASE) val = (long)interp.base;
    if (key == AT_FLAGS) val = 0;
    if (key == AT_ENTRY) val = (long)prog.entry;
    if (key == AT_EXECFN) val = (long)argv[0];
    *--sp = val;
    *--sp = key;
  }

  // push main() arguments
  sp = push_strs(sp, envp, envc);
  sp = push_strs(sp, argv, argc);
  *--sp = argc;

  STRACE("running dlopen importer %p...", interp.entry);

  // XXX: ideally we should set most registers to zero
#ifdef __x86_64__
  struct ps_strings {
    char **argv;
    int argc;
    char **envp;
    int envc;
  } pss = {argv, argc, envp, envc};
  asm volatile("mov\t%2,%%rsp\n\t"
               "jmpq\t*%1"
               : /* no outputs */
               : "D"(IsFreebsd() ? sp : 0), "S"(interp.entry), "d"(sp),
                 "b"(IsNetbsd() ? &pss : 0)
               : "memory");
  __builtin_unreachable();
#elif defined(__aarch64__)
  register long x0 asm("x0") = IsFreebsd() ? (long)sp : 0;
  register long x9 asm("x9") = (long)sp;
  register long x16 asm("x16") = (long)interp.entry;
  asm volatile("mov\tsp,x9\n\t"
               "br\tx16"
               : /* no outputs */
               : "r"(x0), "r"(x9), "r"(x16)
               : "memory");
  __builtin_unreachable();
#else
#error "unsupported architecture"
#endif
}

static char *dlerror_set(const char *str) {
  strlcpy(dlerror_buf, str, sizeof(dlerror_buf));
  return dlerror_buf;
}

static char *foreign_alloc_block(void) {
  char *p = 0;
  size_t sz = 65536;
  if (!IsWindows()) {
    p = __sys_mmap(0, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 0);
    if (p == MAP_FAILED) {
      p = 0;
    }
  } else {
    uintptr_t h;
    if ((h = CreateFileMapping(-1, 0, kNtPageExecuteReadwrite, 0, sz, 0))) {
      p = MapViewOfFileEx(h, kNtFileMapWrite | kNtFileMapExecute, 0, 0, sz, 0);
      CloseHandle(h);
    }
  }
  if (p) {
    WRITE32LE(p, 4);  // store used index
  } else {
    dlerror_set("out of memory");
  }
  return p;
}

static void *foreign_alloc(size_t n) {
  void *res;
  static char *block;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&lock);
  if (!block || READ32LE(block) + n > 65536) {
    if (!(block = foreign_alloc_block())) {
      return 0;
    }
  }
  res = block + READ32LE(block);
  WRITE32LE(block, READ32LE(block) + n);
  pthread_mutex_unlock(&lock);
  return res;
}

static void *foreign_thunk_sysv(void *func) {
  unsigned char *code;
  if (!(code = foreign_alloc(23))) return 0;
  // movabs $func,%r9
  code[0] = 0x49;
  code[1] = 0xb9;
  WRITE64LE(code + 2, (uintptr_t)func);
  // movabs $tramp,%r10
  code[10] = 0x49;
  code[11] = 0xba;
  WRITE64LE(code + 12, (uintptr_t)foreign_tramp);
  // jmp *%r10
  code[20] = 0x41;
  code[21] = 0xff;
  code[22] = 0xe2;
  return code;
}

static void *foreign_thunk_nt(void *func) {
  unsigned char *code;
  if (!(code = foreign_alloc(27))) return 0;
  // push %rbp
  code[0] = 0x55;
  // mov %rsp,%rbp
  code[1] = 0x48;
  code[2] = 0x89;
  code[3] = 0xe5;
  // movabs $func,%rax
  code[4] = 0x48;
  code[5] = 0xb8;
  WRITE64LE(code + 6, (uintptr_t)func);
  // movabs $tramp,%r10
  code[14] = 0x49;
  code[15] = 0xba;
  WRITE64LE(code + 16, (uintptr_t)__sysv2nt14);
  // jmp *%r10
  code[24] = 0x41;
  code[25] = 0xff;
  code[26] = 0xe2;
  return code;
}

static wontreturn dontinstrument void foreign_helper(void **p) {
  foreign.dlopen = foreign_thunk_sysv(p[0]);
  foreign.dlsym = foreign_thunk_sysv(p[1]);
  foreign.dlclose = foreign_thunk_sysv(p[2]);
  foreign.dlerror = foreign_thunk_sysv(p[3]);
  longjmp(foreign.jb, 1);
}

static bool foreign_setup(void) {
  char interp[256] = {0};
  if (!elf_interp(interp, sizeof(interp), "/usr/bin/env")) {
    return false;
  }
  const char *dlopen_helper = 0;
#ifdef __x86_64__
  if (IsFreebsd()) {
    dlopen_helper = "/zip/.dlopen.x86_64.freebsd.elf";
  } else if (IsLinux()) {
    if (fileexists("/lib64/ld-linux-x86-64.so.2")) {
      dlopen_helper = "/zip/.dlopen.x86_64.glibc.elf";
    } else {
      dlopen_helper = "/zip/.dlopen.x86_64.musl.elf";
    }
  }
#elif defined(__aarch64__)
  if (0 && IsLinux()) {  // TODO(jart): implement me
    dlopen_helper = "/zip/.dlopen.aarch64.glibc.elf";
  }
#endif
  if (!dlopen_helper) {
    enosys();
    return false;  // this platform isn't supported yet
  }
  struct CosmoTib *cosmo_tib = __get_tls();
  if (!setjmp(foreign.jb)) {
    elf_exec(dlopen_helper, interp, 2,
             (char *[]){
                 program_invocation_name,
                 (char *)foreign_helper,
                 NULL,
             },
             environ);
    return false;  // if elf_exec() returns, it failed
  }
  foreign.tib = __get_tls();
  __set_tls(cosmo_tib);
  foreign.is_supported = true;
  return true;
}

static void foreign_once(void) {
  foreign_setup();
}

static bool foreign_init(void) {
  bool res;
  cosmo_once(&foreign.once, foreign_once);
  if (!(res = foreign.is_supported)) {
    dlerror_set("dlopen() isn't supported on this platform");
  }
  return res;
}

static int dlclose_nt(void *handle) {
  int res;
  if (FreeLibrary((uintptr_t)handle)) {
    res = 0;
  } else {
    dlerror_set("FreeLibrary() failed");
    res = -1;
  }
  return res;
}

static void *dlopen_nt(const char *path, int mode) {
  int n;
  uintptr_t handle;
  char16_t path16[PATH_MAX + 2];
  if (mode & ~(RTLD_LOCAL | RTLD_LAZY | RTLD_NOW)) {
    dlerror_set("invalid mode");
    return 0;
  }
  if ((n = __mkntpath(path, path16)) == -1) {
    dlerror_set("path invalid");
    return 0;
  }
  if (n > 3 &&                 //
      path16[n - 3] == '.' &&  //
      path16[n - 2] == 's' &&  //
      path16[n - 1] == 'o') {
    path16[n - 2] = 'd';
    path16[n - 1] = 'l';
    path16[n + 0] = 'l';
    path16[n + 1] = 0;
  }
  if (!(handle = LoadLibrary(path16))) {
    dlerror_set("library not found");
  }
  return (void *)handle;
}

static void *dlsym_nt(void *handle, const char *name) {
  void *x64_abi_func;
  void *sysv_abi_func = 0;
  if ((x64_abi_func = GetProcAddress((uintptr_t)handle, name))) {
    sysv_abi_func = foreign_thunk_nt(x64_abi_func);
  } else {
    dlerror_set("symbol not found: ");
    strlcat(dlerror_buf, name, sizeof(dlerror_buf));
  }
  return sysv_abi_func;
}

static void *dlopen_silicon(const char *path, int mode) {
  int n;
  int xnu_mode = 0;
  char path2[PATH_MAX + 5];
  if (mode & ~(RTLD_LOCAL | RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL)) {
    xnu_mode = -1;  // punt error to system dlerror() impl
  }
  if (!(mode & RTLD_GLOBAL)) {
    xnu_mode |= XNU_RTLD_LOCAL;  // unlike Linux, XNU defaults to RTLD_GLOBAL
  }
  if (mode & RTLD_NOW) {
    xnu_mode |= XNU_RTLD_NOW;
  }
  if (mode & RTLD_LAZY) {
    xnu_mode |= XNU_RTLD_LAZY;
  }
  if ((n = strlen(path)) < PATH_MAX && n > 3 &&  //
      path[n - 3] == '.' &&                      //
      path[n - 2] == 's' &&                      //
      path[n - 1] == 'o') {
    memcpy(path2, path, n);
    path2[n - 2] = 'd';
    path2[n - 1] = 'y';
    path2[n + 0] = 'l';
    path2[n + 1] = 'i';
    path2[n + 2] = 'b';
    path2[n + 3] = 0;
    path = path2;
  }
  return __syslib->__dlopen(path, xnu_mode);
}

/**
 * Opens dynamic shared object using host platform libc.
 *
 * If a `path` ending with `.so` is passed on Windows or MacOS, then
 * this wrapper will automatically change it to `.dll` or `.dylib` to
 * increase its chance of successfully loading.
 *
 * WARNING: This isn't supported on MacOS x86-64, OpenBSD, and NetBSD.
 *
 * WARNING: This dlopen() implementation is highly limited. Cosmo
 * binaries are always statically linked. You can import functions from
 * dynamic shared objects, but you can't export any. This dlopen() won't
 * work for language plugins, but might help you access GUI and GPU DRM.
 *
 * WARNING: Do not expect this dlopen() is in any way safe to use. It's
 * mostly due to thread local storage. On Windows it should be safe. On
 * Apple Silicon it should be safe so long as foreign functions don't
 * issue callbacks. On libre OSes we currently only make dlopen() APIs
 * safe to use. In order for it to be safe, four system calls need to be
 * issued for every dlopen() related API call, and that's assuming this
 * API is only used from the main of your program. Most importantly
 * there are no safeguards added around imported functions, since it'd
 * make them go 1000x slower. It's the responsibility of the caller to
 * ensure that imported functions never touch TLS, don't install signal
 * handlers, will never spawn threads, and don't issue callbacks. Care
 * should also be taken on all platforms, to ensure dynamic memory is
 * being passed to the correct malloc() and free() implementations.
 *
 * @param mode is a bitmask that can contain:
 *     - `RTLD_LOCAL` (default)
 *     - `RTLD_GLOBAL` (not supported on Windows)
 *     - `RTLD_LAZY`
 *     - `RTLD_NOW`
 * @return dso handle, or NULL w/ dlerror()
 */
void *cosmo_dlopen(const char *path, int mode) {
  void *res;
  if (IsWindows()) {
    res = dlopen_nt(path, mode);
  } else if (IsXnuSilicon()) {
    res = dlopen_silicon(path, mode);
  } else if (IsXnu()) {
    dlerror_set("dlopen() isn't supported on x86-64 MacOS");
    res = 0;
  } else if (foreign_init()) {
    struct Context ctx;
    STRACE("calling platform dlopen %p tib %p...", foreign.dlopen, foreign.tib);
    foreign_begin(&ctx);
    res = foreign.dlopen(path, mode);
    foreign_end(&ctx);
  } else {
    res = 0;
  }
  STRACE("dlopen(%#s, %d) → %p% m", path, mode, res);
  return res;
}

/**
 * Obtains address of symbol from dynamic shared object.
 *
 * On Windows you can only use this to lookup function addresses.
 * Returned functions are trampolined to conform to System V ABI.
 *
 * @param handle was opened by dlopen()
 * @return address of symbol, or NULL w/ dlerror()
 */
void *cosmo_dlsym(void *handle, const char *name) {
  void *func;
  if (IsWindows()) {
    func = dlsym_nt(handle, name);
  } else if (IsXnuSilicon()) {
    func = __syslib->__dlsym(handle, name);
  } else if (IsXnu()) {
    dlerror_set("dlopen() isn't supported on x86-64 MacOS");
    func = 0;
  } else if (foreign_init()) {
    struct Context ctx;
    foreign_begin(&ctx);
    if ((func = foreign.dlsym(handle, name))) {
      func = foreign_thunk_sysv(func);
    }
    foreign_end(&ctx);
  } else {
    func = 0;
  }
  STRACE("dlsym(%p, %#s) → %p", handle, name, func);
  return func;
}

/**
 * Closes dynamic shared object.
 *
 * @param handle was opened by dlopen()
 * @return 0 on success, or -1 w/ dlerror()
 */
int cosmo_dlclose(void *handle) {
  int res;
  if (IsWindows()) {
    res = dlclose_nt(handle);
  } else if (IsXnuSilicon()) {
    res = __syslib->__dlclose(handle);
  } else if (IsXnu()) {
    dlerror_set("dlopen() isn't supported on x86-64 MacOS");
    res = -1;
  } else if (foreign_init()) {
    struct Context ctx;
    foreign_begin(&ctx);
    res = foreign.dlclose(handle);
    foreign_end(&ctx);
  } else {
    res = -1;
  }
  STRACE("dlclose(%p) → %d", handle, res);
  return res;
}

/**
 * Returns string describing last dlopen/dlsym/dlclose error.
 */
char *cosmo_dlerror(void) {
  char *res;
  if (IsXnuSilicon()) {
    res = __syslib->__dlerror();
  } else if (IsWindows() || IsXnu()) {
    res = dlerror_buf;
  } else if (foreign_init()) {
    struct Context ctx;
    foreign_begin(&ctx);
    res = foreign.dlerror();
    foreign_end(&ctx);
    res = dlerror_set(res);
  } else {
    res = dlerror_buf;
  }
  STRACE("dlerror() → %#s", res);
  return res;
}
