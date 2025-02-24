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
#include "libc/calls/createfileflags.internal.h"
#include "libc/calls/internal.h"
#include "libc/calls/struct/fd.internal.h"
#include "libc/calls/syscall_support-nt.internal.h"
#include "libc/nt/createfile.h"
#include "libc/nt/enum/fileflagandattributes.h"
#include "libc/nt/files.h"
#include "libc/nt/runtime.h"
#include "libc/sysv/consts/o.h"

textwindows int sys_fcntl_nt_setfl(int fd, unsigned flags) {

  // you may change the following:
  //
  // - O_NONBLOCK     make read() raise EAGAIN
  // - O_APPEND       for toggling append mode
  // - O_RANDOM       alt. for posix_fadvise()
  // - O_SEQUENTIAL   alt. for posix_fadvise()
  // - O_DIRECT       works but haven't tested
  //
  // the other bits are ignored.
  unsigned allowed =
      _O_APPEND | _O_SEQUENTIAL | _O_RANDOM | _O_DIRECT | _O_NONBLOCK;
  unsigned needreo = _O_APPEND | _O_SEQUENTIAL | _O_RANDOM | _O_DIRECT;
  unsigned newflag = (g_fds.p[fd].flags & ~allowed) | (flags & allowed);

  if (g_fds.p[fd].kind == kFdFile &&
      ((g_fds.p[fd].flags & needreo) ^ (flags & needreo))) {
    unsigned perm, share, attr;
    if (GetNtOpenFlags(newflag, g_fds.p[fd].mode, &perm, &share, 0, &attr) ==
        -1) {
      return -1;
    }
    // MSDN says only these are allowed, otherwise it returns EINVAL.
    attr &= kNtFileFlagBackupSemantics | kNtFileFlagDeleteOnClose |
            kNtFileFlagNoBuffering | kNtFileFlagOpenNoRecall |
            kNtFileFlagOpenReparsePoint | kNtFileFlagOverlapped |
            kNtFileFlagPosixSemantics | kNtFileFlagRandomAccess |
            kNtFileFlagSequentialScan | kNtFileFlagWriteThrough;
    intptr_t hand;
    if ((hand = ReOpenFile(g_fds.p[fd].handle, perm, share, attr)) != -1) {
      if (hand != g_fds.p[fd].handle) {
        CloseHandle(g_fds.p[fd].handle);
        g_fds.p[fd].handle = hand;
      }
    } else {
      return __winerr();
    }
  }

  // 1. ignore flags that aren't access mode flags
  // 2. return zero if nothing's changed
  g_fds.p[fd].flags = newflag;
  return 0;
}
