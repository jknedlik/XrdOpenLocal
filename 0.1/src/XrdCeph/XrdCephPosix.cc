//------------------------------------------------------------------------------
// Copyright (c) 2014-2015 by European Organization for Nuclear Research (CERN)
// Author: Sebastien Ponce <sebastien.ponce@cern.ch>
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//
// In applying this licence, CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//------------------------------------------------------------------------------

/*
 * This interface provides wrapper methods for using ceph through a POSIX API.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <radosstriper/libradosstriper.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <sstream>
#include <sys/xattr.h>
#include <time.h>
#include <limits>
#include "XrdSfs/XrdSfsAio.hh"

#include "XrdCeph/XrdCephPosix.hh"

/// small structs to store file metadata
struct CephFile {
  std::string name;
  std::string pool;
  std::string userId;
  unsigned int nbStripes;
  unsigned long long stripeUnit;
  unsigned long long objectSize;
};

struct CephFileRef : CephFile {
  int flags;
  mode_t mode;
  unsigned long long offset;
};

/// small struct for directory listing
struct DirIterator {
  librados::ObjectIterator m_iterator;
  librados::IoCtx *m_ioctx;
};

/// small struct for aio API callbacks
struct AioArgs {
  AioArgs(XrdSfsAio* a, AioCB *b, size_t n) : aiop(a), callback(b), nbBytes(n) {}
  XrdSfsAio* aiop;
  AioCB *callback;
  size_t nbBytes;
};

/// global variables holding stripers and ioCtxs for each ceph pool plus the cluster object
std::map<std::string, libradosstriper::RadosStriper*> g_radosStripers;
std::map<std::string, librados::IoCtx*> g_ioCtx;
librados::Rados* g_cluster = 0;
/// global variable holding a map of file descriptor to file reference
std::map<unsigned int, CephFileRef> g_fds;
/// global variable holding a list of files currently opened for write
std::multiset<std::string> g_filesOpenForWrite;
/// global variable remembering the next available file descriptor
unsigned int g_nextCephFd = 0;
/// global variable containing defaults for CephFiles
CephFile g_defaultParams = { "",
                             "default",        // default pool
                             "admin",          // default user
                             1,                // default nbStripes
                             4 * 1024 * 1024,  // default stripeUnit : 4 MB
                             4 * 1024 * 1024}; // default objectSize : 4 MB

std::string g_defaultUserId = "admin";
std::string g_defaultPool = "default";

/// global variable for the log function
static void (*g_logfunc) (char *, va_list argp) = 0;

static void logwrapper(char* format, ...) {
  if (0 == g_logfunc) return;
  va_list arg;
  va_start(arg, format);
  (*g_logfunc)(format, arg);
  va_end(arg);
}

/// simple integer parsing, to be replaced by std::stoll when C++11 can be used
static unsigned long long int stoull(const std::string &s) {
  char* end;
  errno = 0;
  unsigned long long int res = strtoull(s.c_str(), &end, 10);
  if (0 != *end) {
    throw std::invalid_argument(s);
  }
  if (ERANGE == errno) {
    throw std::out_of_range(s);
  }
  return res;
}

/// simple integer parsing, to be replaced by std::stoi when C++11 can be used
static unsigned int stoui(const std::string &s) {
  char* end;
  errno = 0;
  unsigned long int res = strtoul(s.c_str(), &end, 10);
  if (0 != *end) {
    throw std::invalid_argument(s);
  }
  if (ERANGE == errno || res > std::numeric_limits<unsigned int>::max()) {
    throw std::out_of_range(s);
  }
  return (unsigned int)res;
}

/// fills the userId of a ceph file struct from a string and an environment
/// returns position of first character after the userId
static int fillCephUserId(const std::string &params, XrdOucEnv *env, CephFile &file) {
  // default
  file.userId = g_defaultParams.userId;
  // parsing
  size_t atPos = params.find('@');
  if (std::string::npos != atPos) {
    file.userId = params.substr(0, atPos);
    return atPos+1;
  } else {
    if (0 != env) {
      char* cuser = env->Get("cephUserId");
      if (0 != cuser) {
        file.userId = cuser;
      }
    }
    return 0;
  }
}

/// fills the pool of a ceph file struct from a string and an environment
/// returns position of first character after the pool
static int fillCephPool(const std::string &params, unsigned int offset, XrdOucEnv *env, CephFile &file) {
  // default
  file.pool = g_defaultParams.pool;
  // parsing
  size_t comPos = params.find(',', offset);
  if (std::string::npos == comPos) {
    if (params.size() == offset) {
      if (NULL != env) {
        char* cpool = env->Get("cephPool");
        if (0 != cpool) {
          file.pool = cpool;
        }
      }
    } else {
      file.pool = params.substr(offset);
    }
    return params.size();
  } else {
    file.pool = params.substr(offset, comPos-offset);
    return comPos+1;
  }
}

/// fills the nbStriped of a ceph file struct from a string and an environment
/// returns position of first character after the nbStripes
// this may raise std::invalid_argument and std::out_of_range
static int fillCephNbStripes(const std::string &params, unsigned int offset, XrdOucEnv *env, CephFile &file) {
  // default
  file.nbStripes = g_defaultParams.nbStripes;
  // parsing
  size_t comPos = params.find(',', offset);
  if (std::string::npos == comPos) {
    if (params.size() == offset) {
      if (NULL != env) {
        char* cNbStripes = env->Get("cephNbStripes");
        if (0 != cNbStripes) {
          file.nbStripes = stoui(cNbStripes);
        }
      }
    } else {
      file.nbStripes = stoui(params.substr(offset));
    }
    return params.size();
  } else {
    file.nbStripes = stoui(params.substr(offset, comPos-offset));
    return comPos+1;
  }
}

/// fills the stripeUnit of a ceph file struct from a string and an environment
/// returns position of first character after the stripeUnit
// this may raise std::invalid_argument and std::out_of_range
static int fillCephStripeUnit(const std::string &params, unsigned int offset, XrdOucEnv *env, CephFile &file) {
  // default
  file.stripeUnit = g_defaultParams.stripeUnit;
  // parsing
  size_t comPos = params.find(',', offset);
  if (std::string::npos == comPos) {
    if (params.size() == offset) {
      if (NULL != env) {
        char* cStripeUnit = env->Get("cephStripeUnit");
        if (0 != cStripeUnit) {
          file.stripeUnit = ::stoull(cStripeUnit);
        }
      }
    } else {
      file.stripeUnit = ::stoull(params.substr(offset));
    }
    return params.size();
  } else {
    file.stripeUnit = ::stoull(params.substr(offset, comPos-offset));
    return comPos+1;
  }
}

/// fills the objectSize of a ceph file struct from a string and an environment
/// returns position of first character after the objectSize
// this may raise std::invalid_argument and std::out_of_range
static void fillCephObjectSize(const std::string &params, unsigned int offset, XrdOucEnv *env, CephFile &file) {
  // default
  file.objectSize = g_defaultParams.objectSize;
  // parsing
  if (params.size() == offset) {
    if (NULL != env) {
      char* cObjectSize = env->Get("cephObjectSize");
      if (0 != cObjectSize) {
        file.objectSize = ::stoull(cObjectSize);
      }
    }
  } else {
    file.objectSize = ::stoull(params.substr(offset));
  }
}

/// fill the parameters of a ceph file struct (all but name) from a string and an environment
/// see fillCephFile for the detailed syntax
void fillCephFileParams(const std::string &params, XrdOucEnv *env, CephFile &file) {
  // parse the params one by one
  unsigned int afterUser = fillCephUserId(params, env, file);
  unsigned int afterPool = fillCephPool(params, afterUser, env, file);
  unsigned int afterNbStripes = fillCephNbStripes(params, afterPool, env, file);
  unsigned int afterStripeUnit = fillCephStripeUnit(params, afterNbStripes, env, file);
  fillCephObjectSize(params, afterStripeUnit, env, file);
}

/// sets the default userId, pool and file layout
/// syntax is [user@]pool[,nbStripes[,stripeUnit[,objectSize]]]
/// may throw std::invalid_argument or std::out_of_range in case of error
void ceph_posix_set_defaults(const char* value) {
  if (value) {
    CephFile newdefault;
    fillCephFileParams(value, NULL, newdefault);
    g_defaultParams = newdefault;
  }
}

/// fill a ceph file struct from a path and an environment
void fillCephFile(const char *path, XrdOucEnv *env, CephFile &file) {
  // Syntax of the given path is :
  //   [[userId@]pool[,nbStripes[,stripeUnit[,objectSize]]]:]<actual path>
  // for the missing parts, if env is not null the entries
  // cephUserId, cephPool, cephNbStripes, cephStripeUnit, and cephObjectSize
  // of env will be used.
  // If env is null or no entry is found for what is missing, defaults are
  // applied. These defaults are initially set to 'admin', 'default', 1, 4MB and 4MB
  // but can be changed via a call to ceph_posix_set_defaults
  std::string spath = path;
  size_t colonPos = spath.find(':');
  if (std::string::npos == colonPos) {
    file.name = spath;
    fillCephFileParams("", env, file);
  } else {
    file.name = spath.substr(colonPos+1);
    fillCephFileParams(spath.substr(0, colonPos), env, file);
  }
}

static CephFile getCephFile(const char *path, XrdOucEnv *env) {
  CephFile file;
  fillCephFile(path, env, file);
  return file;
}

static CephFileRef getCephFileRef(const char *path, XrdOucEnv *env, int flags,
                                  mode_t mode, unsigned long long offset) {
  CephFileRef fr;
  fillCephFile(path, env, fr);
  fr.flags = flags;
  fr.mode = mode;
  fr.offset = 0;
  return fr;
}

static libradosstriper::RadosStriper* getRadosStriper(const CephFile& file) {
  std::stringstream ss;
  ss << file.userId << '@' << file.pool << ',' << file.nbStripes << ','
     << file.stripeUnit << ',' << file.objectSize;
  std::string userAtPool = ss.str();
  std::map<std::string, libradosstriper::RadosStriper*>::iterator it =
    g_radosStripers.find(userAtPool);
  if (it == g_radosStripers.end()) {
    // we need to create a new radosStriper
    // Do we already have a cluster
    if (0 == g_cluster) {
      // create connection to cluster
      g_cluster = new librados::Rados;
      if (0 == g_cluster) {
        return 0;
      }
      int rc = g_cluster->init(file.userId.c_str());
      if (rc) {
        delete g_cluster;
        g_cluster = 0;
        return 0;
      }
      rc = g_cluster->conf_read_file(NULL);
      if (rc) {
        g_cluster->shutdown();
        delete g_cluster;
        g_cluster = 0;
        return 0;
      }
      g_cluster->conf_parse_env(NULL);
      rc = g_cluster->connect();
      if (rc) {
        g_cluster->shutdown();
        delete g_cluster;
        g_cluster = 0;
        return 0;
      }
    }
    // create IoCtx for our pool
    librados::IoCtx *ioctx = new librados::IoCtx;
    if (0 == ioctx) {
      g_cluster->shutdown();
      delete g_cluster;
      return 0;
    }
    int rc = g_cluster->ioctx_create(file.pool.c_str(), *ioctx);
    if (rc != 0) {
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      delete ioctx;
      return 0;
    }
    // create RadosStriper connection
    libradosstriper::RadosStriper *striper = new libradosstriper::RadosStriper;
    if (0 == striper) {
      delete ioctx;
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      return 0;
    }
    rc = libradosstriper::RadosStriper::striper_create(*ioctx, striper);
    if (rc != 0) {
      delete striper;
      delete ioctx;
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      return 0;
    }
    // setup layout
    rc = striper->set_object_layout_stripe_count(file.nbStripes);
    if (rc != 0) {
      logwrapper((char*)"getRadosStriper : invalid nbStripes %d", file.nbStripes);
      delete striper;
      delete ioctx;
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      return 0;
    }
    rc = striper->set_object_layout_stripe_unit(file.stripeUnit);
    if (rc != 0) {
      logwrapper((char*)"getRadosStriper : invalid stripeUnit %d (must be non0, multiple of 64K)", file.stripeUnit);
      delete striper;
      delete ioctx;
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      return 0;
    }
    rc = striper->set_object_layout_object_size(file.objectSize);
    if (rc != 0) {
      logwrapper((char*)"getRadosStriper : invalid objectSize %d (must be non 0, multiple of stripe_unit)", file.objectSize);
      delete striper;
      delete ioctx;
      g_cluster->shutdown();
      delete g_cluster;
      g_cluster = 0;
      return 0;
    }
    g_ioCtx.insert(std::pair<std::string, librados::IoCtx*>(userAtPool, ioctx));
    it = g_radosStripers.insert(std::pair<std::string, libradosstriper::RadosStriper*>
                                (userAtPool, striper)).first;
  }
  return it->second;
}

static librados::IoCtx* getIoCtx(const CephFile& file) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return 0;
  }
  std::stringstream ss;
  ss << file.userId << '@' << file.pool << ',' << file.nbStripes << ','
     << file.stripeUnit << ',' << file.objectSize;
  std::string userAtPool = ss.str();
  return g_ioCtx[userAtPool];
}

void ceph_posix_disconnect_all() {
  for (std::map<std::string, libradosstriper::RadosStriper*>::iterator it =
         g_radosStripers.begin();
       it != g_radosStripers.end();
       it++) {
    delete it->second;
  }
  g_radosStripers.clear();
  for (std::map<std::string, librados::IoCtx*>::iterator it = g_ioCtx.begin();
       it != g_ioCtx.end();
       it++) {
    delete it->second;
  }
  g_ioCtx.clear();
  delete g_cluster;
}

void ceph_posix_set_logfunc(void (*logfunc) (char *, va_list argp)) {
  g_logfunc = logfunc;
};

static int ceph_posix_internal_truncate(const CephFile &file, unsigned long long size);

int ceph_posix_open(XrdOucEnv* env, const char *pathname, int flags, mode_t mode) {
  logwrapper((char*)"ceph_open : fd %d associated to %s", g_nextCephFd, pathname);
  CephFileRef fr = getCephFileRef(pathname, env, flags, mode, 0);
  g_fds[g_nextCephFd] = fr;
  g_nextCephFd++;
  if (flags & (O_WRONLY|O_RDWR)) {
    g_filesOpenForWrite.insert(fr.name);
  }
  // in case of O_CREAT and O_EXCL, we should complain if the file exists
  if ((flags & O_CREAT) && (flags & O_EXCL)) {
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) return -EINVAL;
    struct stat buf;
    int rc = striper->stat(fr.name, (uint64_t*)&(buf.st_size), &(buf.st_atime));
    if (rc != -ENOENT) {
      if (0 == rc) return -EEXIST;
      return rc;
    }
  }
  // in case of O_TRUNC, we should truncate the file
  if (flags & O_TRUNC) {
    int rc = ceph_posix_internal_truncate(fr, 0);
    // fail only if file exists and cannot be truncated
    if (rc < 0 && rc != -ENOENT) return rc;
  }
  return g_nextCephFd-1;
}

int ceph_posix_close(int fd) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    logwrapper((char*)"ceph_close: closed fd %d", fd);
    if (it->second.flags & (O_WRONLY|O_RDWR)) {
      g_filesOpenForWrite.erase(g_filesOpenForWrite.find(it->second.name));
    }
    g_fds.erase(it);
    return 0;
  } else {
    return -EBADF;
  }
}

static off64_t lseek_compute_offset(CephFileRef &fr, off64_t offset, int whence) {
  switch (whence) {
  case SEEK_SET:
    fr.offset = offset;
    break;
  case SEEK_CUR:
    fr.offset += offset;
    break;
  default:
    return -EINVAL;
  }
  return fr.offset;
}

off_t ceph_posix_lseek(int fd, off_t offset, int whence) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_lseek: for fd %d, offset=%lld, whence=%d", fd, offset, whence);
    return (off_t)lseek_compute_offset(fr, offset, whence);
  } else {
    return -EBADF;
  }
}

off64_t ceph_posix_lseek64(int fd, off64_t offset, int whence) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_lseek64: for fd %d, offset=%lld, whence=%d", fd, offset, whence);
    return lseek_compute_offset(fr, offset, whence);
  } else {
    return -EBADF;
  }
}

ssize_t ceph_posix_write(int fd, const void *buf, size_t count) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_write: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) == 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    ceph::bufferlist bl;
    bl.append((const char*)buf, count);
    int rc = striper->write(fr.name, bl, count, fr.offset);
    if (rc) return rc;
    fr.offset += count;
    return count;
  } else {
    return -EBADF;
  }
}

ssize_t ceph_posix_pwrite(int fd, const void *buf, size_t count, off64_t offset) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_write: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) == 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    ceph::bufferlist bl;
    bl.append((const char*)buf, count);
    int rc = striper->write(fr.name, bl, count, offset);
    if (rc) return rc;
    return count;
  } else {
    return -EBADF;
  }
}

static void ceph_aio_complete(rados_completion_t c, void *arg) {
  AioArgs *awa = reinterpret_cast<AioArgs*>(arg);
  size_t rc = rados_aio_get_return_value(c);
  awa->callback(awa->aiop, rc == 0 ? awa->nbBytes : rc);
  delete(awa);
}

ssize_t ceph_aio_write(int fd, XrdSfsAio *aiop, AioCB *cb) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    // get the parameters from the Xroot aio object
    size_t count = aiop->sfsAio.aio_nbytes;
    const char *buf = (const char*)aiop->sfsAio.aio_buf;
    size_t offset = aiop->sfsAio.aio_offset;
    // get the striper object
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_aio_write: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) == 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    // prepare a bufferlist around the given buffer
    ceph::bufferlist bl;
    bl.append(buf, count);
    // prepare a ceph AioCompletion object and do async call
    AioArgs *args = new AioArgs(aiop, cb, count);
    librados::AioCompletion *completion =
      g_cluster->aio_create_completion(args, ceph_aio_complete, NULL);
    int rc = striper->aio_write(fr.name, completion, bl, count, offset);
    completion->release();
    return rc;
  } else {
    return -EBADF;
  }
} 

ssize_t ceph_posix_read(int fd, void *buf, size_t count) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_read: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) != 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    ceph::bufferlist bl;
    int rc = striper->read(fr.name, &bl, count, fr.offset);
    if (rc < 0) return rc;
    bl.copy(0, rc, (char*)buf);
    fr.offset += rc;
    return rc;
  } else {
    return -EBADF;
  }
}

ssize_t ceph_posix_pread(int fd, void *buf, size_t count, off64_t offset) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_read: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) != 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    ceph::bufferlist bl;
    int rc = striper->read(fr.name, &bl, count, offset);
    if (rc < 0) return rc;
    bl.copy(0, rc, (char*)buf);
    return rc;
  } else {
    return -EBADF;
  }
}

ssize_t ceph_aio_read(int fd, XrdSfsAio *aiop, AioCB *cb) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    // get the parameters from the Xroot aio object
    size_t count = aiop->sfsAio.aio_nbytes;
    const char *buf = (const char*)aiop->sfsAio.aio_buf;
    size_t offset = aiop->sfsAio.aio_offset;
    // get the striper object
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_read: for fd %d, count=%d", fd, count);
    if ((fr.flags & (O_WRONLY|O_RDWR)) != 0) {
      return -EBADF;
    }
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    // prepare a bufferlist to receive data
    ceph::bufferlist bl;
    // prepare a ceph AioCompletion object and do async call
    AioArgs *args = new AioArgs(aiop, cb, count);
    librados::AioCompletion *completion =
      g_cluster->aio_create_completion(args, ceph_aio_complete, NULL);
    int rc = striper->aio_read(fr.name, completion, &bl, count, offset);
    completion->release();
    if (rc < 0) return rc;
    bl.copy(0, rc, (char*)buf);
    return rc;
  } else {
    return -EBADF;
  }
}

int ceph_posix_fstat(int fd, struct stat *buf) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_stat: fd %d", fd);
    // minimal stat : only size and times are filled
    // atime, mtime and ctime are set all to the same value
    // mode is set arbitrarily to 0666
    libradosstriper::RadosStriper *striper = getRadosStriper(fr);
    if (0 == striper) {
      return -EINVAL;
    }
    memset(buf, 0, sizeof(*buf));
    int rc = striper->stat(fr.name, (uint64_t*)&(buf->st_size), &(buf->st_atime));
    if (rc != 0) {
      return -rc;
    }
    buf->st_mtime = buf->st_atime;
    buf->st_ctime = buf->st_atime;
    buf->st_mode = 0666;
    return 0;
  } else {
    return -EBADF;
  }
}

int ceph_posix_stat(XrdOucEnv* env, const char *pathname, struct stat *buf) {
  logwrapper((char*)"ceph_stat : %s", pathname);
  // minimal stat : only size and times are filled
  // atime, mtime and ctime are set all to the same value
  // mode is set arbitrarily to 0666
  CephFile file = getCephFile(pathname, env);
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  memset(buf, 0, sizeof(*buf));
  int rc = striper->stat(file.name, (uint64_t*)&(buf->st_size), &(buf->st_atime));
  if (rc != 0) {
    // for non existing file. Check that we did not open it for write recently
    // in that case, we return 0 size and current time
    if (-ENOENT == rc && g_filesOpenForWrite.find(file.name) != g_filesOpenForWrite.end()) {
      buf->st_size = 0;
      buf->st_atime = time(NULL);
    } else {
      return -rc;
    }
  }
  buf->st_mtime = buf->st_atime;
  buf->st_ctime = buf->st_atime;
  buf->st_mode = 0666;
  return 0;
}

int ceph_posix_fsync(int fd) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    logwrapper((char*)"ceph_sync: fd %d", fd);
    return 0;
  } else {
    return -EBADF;
  }
}

int ceph_posix_fcntl(int fd, int cmd, ... /* arg */ ) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_fcntl: fd %d cmd=%d", fd, cmd);
    // minimal implementation
    switch (cmd) {
    case F_GETFL:
      return fr.mode;
    default:
      return -EINVAL;
    }
  } else {
    return -EBADF;
  }
}

static ssize_t ceph_posix_internal_getxattr(const CephFile &file, const char* name,
                                            void* value, size_t size) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  ceph::bufferlist bl;
  int rc = striper->getxattr(file.name, name, bl);
  if (rc < 0) return rc;
  size_t returned_size = (size_t)rc<size?rc:size;
  bl.copy(0, returned_size, (char*)value);
  return returned_size;
}

ssize_t ceph_posix_getxattr(XrdOucEnv* env, const char* path,
                            const char* name, void* value,
                            size_t size) {
  logwrapper((char*)"ceph_getxattr: path %s name=%s", path, name);
  return ceph_posix_internal_getxattr(getCephFile(path, env), name, value, size);
}

ssize_t ceph_posix_fgetxattr(int fd, const char* name,
                             void* value, size_t size) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_fgetxattr: fd %d name=%s", fd, name);
    return ceph_posix_internal_getxattr(fr, name, value, size);
  } else {
    return -EBADF;
  }
}

static ssize_t ceph_posix_internal_setxattr(const CephFile &file, const char* name,
                                            const void* value, size_t size, int flags) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  ceph::bufferlist bl;
  bl.append((const char*)value, size);
  int rc = striper->setxattr(file.name, name, bl);
  if (rc) {
    return -rc;
  }
  return 0;
}

ssize_t ceph_posix_setxattr(XrdOucEnv* env, const char* path,
                            const char* name, const void* value,
                            size_t size, int flags) {
  logwrapper((char*)"ceph_setxattr: path %s name=%s value=%s", path, name, value);
  return ceph_posix_internal_setxattr(getCephFile(path, env), name, value, size, flags);
}

int ceph_posix_fsetxattr(int fd,
                         const char* name, const void* value,
                         size_t size, int flags)  {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_fsetxattr: fd %d name=%s value=%s", fd, name, value);
    return ceph_posix_internal_setxattr(fr, name, value, size, flags);
  } else {
    return -EBADF;
  }
}

static int ceph_posix_internal_removexattr(const CephFile &file, const char* name) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  int rc = striper->rmxattr(file.name, name);
  if (rc) {
    return -rc;
  }
  return 0;
}

int ceph_posix_removexattr(XrdOucEnv* env, const char* path,
                           const char* name) {
  logwrapper((char*)"ceph_removexattr: path %s name=%s", path, name);
  return ceph_posix_internal_removexattr(getCephFile(path, env), name);
}

int ceph_posix_fremovexattr(int fd, const char* name) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_fremovexattr: fd %d name=%s", fd, name);
    return ceph_posix_internal_removexattr(fr, name);
  } else {
    return -EBADF;
  }
}

static int ceph_posix_internal_listxattrs(const CephFile &file, XrdSysXAttr::AList **aPL, int getSz) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  // call ceph
  std::map<std::string, ceph::bufferlist> attrset;
  int rc = striper->getxattrs(file.name, attrset);
  if (rc) {
    return -rc;
  }
  // build result
  *aPL = 0;
  int maxSize = 0;
  for (std::map<std::string, ceph::bufferlist>::const_iterator it = attrset.begin();
       it != attrset.end();
       it++) {
    XrdSysXAttr::AList* newItem = (XrdSysXAttr::AList*)malloc(sizeof(XrdSysXAttr::AList)+it->first.size());
    newItem->Next = *aPL;
    newItem->Vlen = it->second.length();
    if (newItem->Vlen > maxSize) {
      maxSize = newItem->Vlen;
    }
    newItem->Nlen = it->first.size();
    strncpy(newItem->Name, it->first.c_str(), newItem->Vlen+1);
    *aPL = newItem;
  }
  if (getSz) {
    return 0;
  } else {
    return maxSize;
  }
}

int ceph_posix_listxattrs(XrdOucEnv* env, const char* path, XrdSysXAttr::AList **aPL, int getSz) {
  logwrapper((char*)"ceph_listxattrs: path %s", path);
  return ceph_posix_internal_listxattrs(getCephFile(path, env), aPL, getSz);
}

int ceph_posix_flistxattrs(int fd, XrdSysXAttr::AList **aPL, int getSz) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_flistxattrs: fd %d", fd);
    return ceph_posix_internal_listxattrs(fr, aPL, getSz);
  } else {
    return -EBADF;
  }
}

void ceph_posix_freexattrlist(XrdSysXAttr::AList *aPL) {
  while (aPL) {
    free(aPL->Name);
    XrdSysXAttr::AList *cur = aPL;
    aPL = aPL->Next;
    free(cur);
  }
}

int ceph_posix_statfs(long long *totalSpace, long long *freeSpace) {
  logwrapper((char*)"ceph_posix_statfs");
  librados::cluster_stat_t result;
  int rc = g_cluster->cluster_stat(result);
  if (0 == rc) {
    *totalSpace = result.kb * 1024;
    *freeSpace = result.kb_avail * 1024;
  }
  return rc;
}

static int ceph_posix_internal_truncate(const CephFile &file, unsigned long long size) {
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  return striper->trunc(file.name, size);
}

int ceph_posix_ftruncate(int fd, unsigned long long size) {
  std::map<unsigned int, CephFileRef>::iterator it = g_fds.find(fd);
  if (it != g_fds.end()) {
    CephFileRef &fr = it->second;
    logwrapper((char*)"ceph_posix_ftruncate: fd %d, size %d", fd, size);
    return ceph_posix_internal_truncate(fr, size);
  } else {
    return -EBADF;
  }
}

int ceph_posix_truncate(XrdOucEnv* env, const char *pathname, unsigned long long size) {
  logwrapper((char*)"ceph_posix_truncate : %s", pathname);
  // minimal stat : only size and times are filled
  CephFile file = getCephFile(pathname, env);
  return ceph_posix_internal_truncate(file, size);
}

int ceph_posix_unlink(XrdOucEnv* env, const char *pathname) {
  logwrapper((char*)"ceph_posix_unlink : %s", pathname);
  // minimal stat : only size and times are filled
  CephFile file = getCephFile(pathname, env);
  libradosstriper::RadosStriper *striper = getRadosStriper(file);
  if (0 == striper) {
    return -EINVAL;
  }
  return striper->remove(file.name);
}

DIR* ceph_posix_opendir(XrdOucEnv* env, const char *pathname) {
  logwrapper((char*)"ceph_posix_opendir : %s", pathname);
  // only accept root dir, as there is no concept of dirs in object stores
  CephFile file = getCephFile(pathname, env);
  if (file.name.size() != 1 || file.name[0] != '/') {
    errno = -ENOENT;
    return 0;
  }
  librados::IoCtx *ioctx = getIoCtx(file);
  if (0 == ioctx) {
    errno = EINVAL;
    return 0;
  }
  DirIterator* res = new DirIterator();
  res->m_iterator = ioctx->objects_begin();
  res->m_ioctx = ioctx;
  return (DIR*)res;
}

int ceph_posix_readdir(DIR *dirp, char *buff, int blen) {
  librados::ObjectIterator &iterator = ((DirIterator*)dirp)->m_iterator;
  librados::IoCtx *ioctx = ((DirIterator*)dirp)->m_ioctx;
  while (iterator->first.compare(iterator->first.size()-17, 17, ".0000000000000000") &&
         iterator != ioctx->objects_end()) {
    iterator++;
  }
  if (iterator == ioctx->objects_end()) {
    buff[0] = 0;
  } else {
    int l = iterator->first.size()-17;
    if (l < blen) blen = l;
    strncpy(buff, iterator->first.c_str(), blen-1);
    buff[blen-1] = 0;
    iterator++;
  }
  return 0;
}

int ceph_posix_closedir(DIR *dirp) {
  delete ((DirIterator*)dirp);
  return 0;
}
