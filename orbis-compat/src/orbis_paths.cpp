// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
#include "orbis_paths.h"

#include <orbis_log.h>

#include <orbis/libkernel.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>


namespace {

bool s_rootDecided = false;
std::string s_rootOverride;

// Validated because it becomes a directory name under /data (a '/' would escape it). A real
// title id is nine characters of A-Z0-9; anything else is treated as unavailable.
bool titleIdLooksReal(const char* id) {
  size_t n = 0;
  for(; id[n]!='\0'; ++n) {
    if(n>=9)
      return false;
    const char c = id[n];
    if(!((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')))
      return false;
    }
  return n>0;
  }

// The console's own answer to "which title am I".
//
// TitleId is a fixed 10-byte field that may not be terminated, so it is copied bounded.
std::string titleIdRoot() {
  OrbisAppInfo  info = {};
  const int32_t rc   = sceKernelGetAppInfo(getpid(), &info);
  char          id[10] = {};
  std::memcpy(id, info.TitleId, sizeof(info.TitleId) - 1);

  if(rc!=0) {
    orbis_log("paths: sceKernelGetAppInfo returned 0x%08x - no title id, so the anchor cannot be "
            "named after this title",unsigned(rc));
    return std::string();
    }
  if(!titleIdLooksReal(id)) {
    orbis_log("paths: sceKernelGetAppInfo succeeded but the title id is not nine characters of "
            "A-Z0-9 - ignored rather than made into a directory name");
    return std::string();
    }
  orbis_log("paths: title id is '%s'",id);
  return std::string("/data/") + id + "/";
  }

}

extern "C" void orbis_set_anchor_root(const char* path) {
  if(path==nullptr || path[0]!='/') {
    orbis_log("paths: orbis_set_anchor_root('%s') ignored - the anchor must be an absolute path",
            path==nullptr ? "(null)" : path);
    return;
    }
  if(s_rootDecided) {
    // Not applied, but logged: paths already anchored used the old root.
    orbis_log("paths: orbis_set_anchor_root('%s') arrived AFTER the anchor was already decided as "
            "'%s' and is ignored. Call it before anything opens a file",path,
            orbis::anchorRoot().c_str());
    return;
    }
  s_rootOverride = path;
  if(s_rootOverride.back()!='/')
    s_rootOverride += '/';
  }

namespace orbis {

const std::string& anchorRoot() {
  static std::string root = [](){
    const char* how = "set by the application";
    std::string r   = s_rootOverride;
    if(r.empty()) {
      r   = titleIdRoot();
      how = "this title's id";
      }
    if(r.empty()) {
      // Generic, not a title's name (this used to be '/data/OpenGothic/' for every consumer).
      r   = "/data/orbis-compat/";
      how = "the fallback - the application named no anchor and the title id was not available";
      }
    s_rootDecided = true;

    std::string dir = r.substr(0,r.size()-1);
    // 0777: a homebrew title's own directory; the umask decides the rest.
    if(mkdir(dir.c_str(),0777)==0)
      orbis_log("paths: anchor '%s' created (%s)",r.c_str(),how);
    else if(errno==EEXIST)
      orbis_log("paths: anchor '%s' (%s)",r.c_str(),how);
    else
      // Not fatal, but logged: every anchored path will fail too, and this errno explains why.
      orbis_log("paths: anchor '%s' (%s) could not be created, errno %d - every relative path will "
              "fail, and this is why",r.c_str(),how,errno);
    return r;
    }();
  return root;
  }

void anchorSayOnce(const char* original, const char* rewritten) {
  static bool said = false;
  if(said)
    return;
  said = true;
  orbis_log("paths: relative paths are anchored - '%s' -> '%s'. This process has NO working "
          "directory (getcwd = ENOSYS) and a relative open returns EINVAL, so without this "
          "nothing relative can be opened at all",original,rewritten);
  }

const char* anchorPath(const char* path, char* buf, size_t bufSize) {
  if(path==nullptr || path[0]=='/' || path[0]=='\0')
    return path;

  const std::string& root = anchorRoot();
  const size_t       n    = std::strlen(path);
  if(root.size()+n+1 > bufSize) {
    // Longer than the caller's buffer. Returned unchanged rather than truncated: EINVAL is
    // better than silently opening a different file.
    orbis_log("paths: '%s' is too long to anchor into %zu B - passed through unchanged, so it "
            "will fail with EINVAL",path,bufSize);
    return path;
    }
  std::memcpy(buf,root.data(),root.size());
  std::memcpy(buf+root.size(),path,n+1);
  anchorSayOnce(path,buf);
  return buf;
  }

}

// ---------------------------------------------------------------- the open interposer
//
// OpenOrbis' musl is statically linked and reaches the kernel through one path for a
// path-based open:
//
//   fopen (libc.a fopen.lo) -> open (libc.a open.lo) -> _open
//
// A strong definition the linker sees first keeps open.lo from being pulled in. This open
// delegates to `_open`, the same libkernel import open.lo calls, which keeps errno semantics
// exact (sceKernelOpen would report SCE codes).
extern "C" int _open(const char* path, int flags, int mode);

extern "C" int open(const char* path, int flags, ...) {
  // Like musl, read mode only when the flags say one was passed (va_arg otherwise is undefined).
  int  mode      = 0;
  bool wantsMode = (flags & O_CREAT)!=0;
#ifdef O_TMPFILE
  wantsMode = wantsMode || (flags & O_TMPFILE)==O_TMPFILE;
#endif
  if(wantsMode) {
    va_list ap;
    va_start(ap,flags);
    mode = va_arg(ap,int);
    va_end(ap);
    }

  char      buf[512] = {};
  const int fd = _open(orbis::anchorPath(path,buf,sizeof(buf)),flags,mode);

#ifdef O_CLOEXEC
  // Same as open.lo.
  if(fd>=0 && (flags & O_CLOEXEC)!=0)
    ::fcntl(fd,F_SETFD,FD_CLOEXEC);
#endif
  return fd;
  }

// `open64` is a weak alias inside open.lo; leaving it undefined would pull that member in
// and its `open` would collide with the one above.
extern "C" int open64(const char* path, int flags, ...) __attribute__((alias("open")));

// ---------------------------------------------------------------- unlink and rename
//
// Anchored too, otherwise relative unlink/rename fail with EINVAL while the matching open
// succeeded (a probe file created by probeSavePaths() was never deleted). Overwriting,
// deleting or rename-into-place of save files all depend on this.
//
// These delegate to sceKernelUnlink / sceKernelRename: musl uses raw syscalls here, so there
// is no lower symbol. The SCE calls return 0x8002_0000 | errno instead of setting errno, so
// errno is decoded from that. (Mapping every failure to EIO broke musl's remove(), which only
// tries rmdir on EISDIR; libc++'s remove_all then threw "I/O error" on an empty directory.)
extern "C" int32_t sceKernelUnlink(const char*);
extern "C" int32_t sceKernelRename(const char* from, const char* to);
extern "C" int32_t sceKernelRmdir(const char*);

static int sceFailure(int32_t rc) {
  errno = (static_cast<uint32_t>(rc)>>16)==0x8002 ? (rc & 0xFFFF) : EIO;
  return -1;
  }

extern "C" int unlink(const char* path) {
  char        buf[512] = {};
  const int32_t rc = sceKernelUnlink(orbis::anchorPath(path,buf,sizeof(buf)));
  return rc!=0 ? sceFailure(rc) : 0;
  }

extern "C" int rename(const char* from, const char* to) {
  char        fbuf[512] = {};
  char        tbuf[512] = {};
  // Both ends anchored to the same root.
  const char* f = orbis::anchorPath(from,fbuf,sizeof(fbuf));
  const char* t = orbis::anchorPath(to,tbuf,sizeof(tbuf));
  const int32_t rc = sceKernelRename(f,t);
  return rc!=0 ? sceFailure(rc) : 0;
  }

// Decided by stat, not by unlink's errno: musl expects Linux's EISDIR for unlink(directory),
// but this FreeBSD-derived kernel returns EPERM. Interposed so libc.a's remove.lo is not used.
extern "C" int remove(const char* path) {
  struct stat st;
  if(stat(path,&st)!=0)
    return -1;
  if(!S_ISDIR(st.st_mode))
    return unlink(path);
  char          buf[512] = {};
  const int32_t rc       = sceKernelRmdir(orbis::anchorPath(path,buf,sizeof(buf)));
  return rc!=0 ? sceFailure(rc) : 0;
  }

// ---------------------------------------------------------------- realpath, getcwd, readlink
//
// libc.a's realpath.lo resolves through readlink("/proc/self/fd/N"), and readlink.lo and
// getcwd.lo are stubs (errno = ENOSYS), so realpath always failed. libc++'s weakly_canonical()
// then cleared that error and returned an empty path with no error code, which made every
// package installation in RPCS3 fail before its first file ("Failed to normalize installation
// path"). Each of those libc.a members defines only its own symbol, so defining them here keeps
// the members out of the link.
//
// The file systems a title sees have no symbolic links (lstat == stat in orbis_stat.cpp), so the
// canonical form of an existing path is its lexical normal form: '.', '..' and repeated '/'
// removed. Relative paths are anchored the same way open() and stat() anchor them.
extern "C" char* realpath(const char* __restrict path, char* __restrict resolved) {
  if(path==nullptr) {
    errno = EINVAL;
    return nullptr;
    }
  if(path[0]=='\0') {
    errno = ENOENT;
    return nullptr;
    }

  char        abuf[PATH_MAX] = {};
  const char* src = orbis::anchorPath(path,abuf,sizeof(abuf));
  if(src[0]!='/') {
    // anchorPath passes a relative path through only when it does not fit the buffer.
    errno = ENAMETOOLONG;
    return nullptr;
    }

  char   out[PATH_MAX] = {'/'};
  size_t len           = 1;
  struct stat st;

  for(const char* p = src; *p!='\0';) {
    while(*p=='/')
      ++p;
    const char* end = p;
    while(*end!='\0' && *end!='/')
      ++end;
    const size_t n = size_t(end-p);
    if(n==0)
      break;

    if(n==1 && p[0]=='.') {
      p = end;
      continue;
      }
    if(n==2 && p[0]=='.' && p[1]=='.') {
      // POSIX resolves 'dir/..' only when dir exists and is a directory.
      if(len>1) {
        out[len] = '\0';
        if(stat(out,&st)!=0)
          return nullptr;
        if(!S_ISDIR(st.st_mode)) {
          errno = ENOTDIR;
          return nullptr;
          }
        while(len>1 && out[len-1]!='/')
          --len;
        if(len>1)
          --len;
        }
      p = end;
      continue;
      }

    const size_t sep = len>1 ? 1 : 0;
    if(len+sep+n >= sizeof(out)) {
      errno = ENAMETOOLONG;
      return nullptr;
      }
    if(sep!=0)
      out[len++] = '/';
    std::memcpy(out+len,p,n);
    len += n;
    p = end;
    }
  out[len] = '\0';

  // realpath() names an existing file; the caller (e.g. weakly_canonical) handles the rest.
  if(stat(out,&st)!=0)
    return nullptr;

  if(resolved==nullptr) {
    char* copy = static_cast<char*>(std::malloc(len+1));
    if(copy==nullptr) {
      errno = ENOMEM;
      return nullptr;
      }
    std::memcpy(copy,out,len+1);
    return copy;
    }
  std::memcpy(resolved,out,len+1);
  return resolved;
  }

// The directory relative paths are anchored under, without its trailing '/', so that
// current_path(), absolute() and canonical() of a relative path agree with where open() and
// stat() put it. chdir() stays unsupported.
extern "C" char* getcwd(char* buf, size_t size) {
  const std::string& root = orbis::anchorRoot();
  const size_t       n    = root.size()>1 && root.back()=='/' ? root.size()-1 : root.size();

  if(buf==nullptr) {
    if(size==0)
      size = n+1;
    else if(size<n+1) {
      errno = ERANGE;
      return nullptr;
      }
    buf = static_cast<char*>(std::malloc(size));
    if(buf==nullptr) {
      errno = ENOMEM;
      return nullptr;
      }
    }
  else if(size==0) {
    errno = EINVAL;
    return nullptr;
    }
  else if(size<n+1) {
    errno = ERANGE;
    return nullptr;
    }

  std::memcpy(buf,root.data(),n);
  buf[n] = '\0';
  return buf;
  }

// No symbolic links exist here: an existing path is "not a link" (EINVAL), a missing one keeps
// stat's errno (ENOENT for /proc/self/exe, as /proc does not exist).
extern "C" ssize_t readlink(const char* __restrict path, char* __restrict buf, size_t size) {
  (void)buf;
  (void)size;
  struct stat st;
  if(stat(path,&st)!=0)
    return -1;
  errno = EINVAL;
  return -1;
  }
