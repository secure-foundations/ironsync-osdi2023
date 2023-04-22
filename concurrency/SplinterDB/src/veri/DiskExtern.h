#pragma once

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <libaio.h>

// for mmap
#include <sys/mman.h>
#include <sys/stat.h>

#include "Extern.h"

static_assert(sizeof(long long) == 8);
static_assert(sizeof(size_t) == 8);
static_assert(sizeof(off_t) == 8);
static_assert(sizeof(uintptr_t) == 8);

namespace InstantiatedDiskInterface {
  extern int fd;

  struct IOCtx {
    io_context_t ctx;
  };

  inline IOCtx get_IOCtx_default() {
    IOCtx ioctx;
    ioctx.ctx = 0;
    return ioctx;
  }

  inline IOCtx init__ctx() {
    IOCtx ioctx;
    ioctx.ctx = 0; // this is needed or io_setup might return EINVAL
    int ret = io_setup(256, &ioctx.ctx);
    if (ret != 0) {
      std::cerr << "io_setup failed" << std::endl;
      exit(1);
    }
    return ioctx;
  }

  inline bool operator==(const IOCtx &left, const IOCtx &right) {
    std::cerr << "Error: IOCtx == called" << std::endl;
    exit(1);
  }

  inline void async__submit(IOCtx& ioctx, Ptrs::Ptr i) {
    iocb* iocb_ptr = (iocb*) i.ptr;
    int ret = io_submit(ioctx.ctx, 1, &iocb_ptr);
    //printf("%d\n", ret);
    //printf("%d %d %d %d %d %d\n",
    //    EAGAIN, EBADF, EFAULT, EINVAL, ENOSYS, EPERM);
    if (ret != 1) {
      std::cerr << "io_submit failed" << std::endl;
      exit(1);
    }
  }

  inline void async__read(IOCtx& ioctx, Ptrs::Ptr i) {
    async__submit(ioctx, i);
  }

  inline void async__write(IOCtx& ioctx, Ptrs::Ptr i) {
    async__submit(ioctx, i);
  }

  inline void async__writev(IOCtx& ioctx, Ptrs::Ptr i) {
    async__submit(ioctx, i);
  }

  inline void async__readv(IOCtx& ioctx, Ptrs::Ptr i) {
    async__submit(ioctx, i);
  }

  inline void sync__read(Ptrs::Ptr buf, uint64 nbytes, int64_t offset)
  {
    int ret = pread(fd, (void*)buf.ptr, nbytes, offset * 4096);
    if (ret != nbytes) {
      std::cerr << "pread failed " << ret << std::endl;
      exit(1);
    }
  }

  inline void sync__write(Ptrs::Ptr buf, uint64 nbytes, int64_t offset)
  {
    int ret = pwrite(fd, (void*)buf.ptr, nbytes, offset * 4096);
    if (ret != nbytes) {
      std::cerr << "pwrite failed " << ret << std::endl;
      exit(1);
    }
  }

  inline Ptrs::Ptr get__event(IOCtx& ioctx) {
    struct io_event event;
    int status = io_getevents(ioctx.ctx, 0, 1, &event, NULL);
    if (status == 0) return Ptrs::null_ptr();
    assert (status == 1);
    assert (event.res > 0);
    iocb* i = event.obj;
    return Ptrs::Ptr((uintptr_t)i);
  }
}

namespace IocbStruct {
  struct Iovec {
    iovec iov;

    Iovec* operator->() { return this; }

    Ptrs::Ptr iov__base() {
      return Ptrs::Ptr((uintptr_t)iov.iov_base);
    }

    uint64_t iov__len() {
      return (uint64_t)iov.iov_len;
    }
  };

  inline Iovec new__iovec(Ptrs::Ptr buf, uint64_t len) {
    Iovec i;
    i.iov.iov_base = (void*)buf.ptr;
    i.iov.iov_len = len;
    return i;
  }

  inline Ptrs::Ptr new__iocb() {
    return Ptrs::Ptr((uintptr_t)(new iocb));
  }

  inline Ptrs::Ptr new__iocb__array(uint64_t len) {
    return Ptrs::Ptr((uintptr_t)(new iocb[len]));
  }

  inline void iocb__prepare__read(Ptrs::Ptr i, int64_t offset, uint64_t nbytes, Ptrs::Ptr buf) {
    io_prep_pread((iocb *)i.ptr, InstantiatedDiskInterface::fd,
        (void*)buf.ptr, nbytes, offset * 4096);
  }

  inline void iocb__prepare__write(Ptrs::Ptr i, int64_t offset, uint64_t nbytes, Ptrs::Ptr buf) {
    io_prep_pwrite((iocb *)i.ptr, InstantiatedDiskInterface::fd,
        (void*)buf.ptr, nbytes, offset * 4096);
  }

  inline void iocb__prepare__writev(Ptrs::Ptr i, int64_t offset, Ptrs::Ptr iovec, uint64_t len) {
    io_prep_pwritev((iocb *)i.ptr, InstantiatedDiskInterface::fd,
        (const struct iovec *)iovec.ptr, len, offset * 4096);
  }

  inline void iocb__prepare__readv(Ptrs::Ptr i, int64_t offset, Ptrs::Ptr iovec, uint64_t len) {
    io_prep_preadv((iocb *)i.ptr, InstantiatedDiskInterface::fd,
        (const struct iovec *)iovec.ptr, len, offset * 4096);
  }

  inline bool iocb__is__write(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return i->aio_lio_opcode == IO_CMD_PWRITE;
  }

  inline bool iocb__is__read(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return i->aio_lio_opcode == IO_CMD_PREAD;
  }

  inline bool iocb__is__writev(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return i->aio_lio_opcode == IO_CMD_PWRITEV;
  }

  inline bool iocb__is__readv(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return i->aio_lio_opcode == IO_CMD_PREADV;
  }

  inline Ptrs::Ptr iocb__buf(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return Ptrs::Ptr((uint64_t)i->u.c.buf);
  }

  inline Ptrs::Ptr iocb__iovec(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return Ptrs::Ptr((uint64_t)i->u.c.buf);
  }

  inline uint64_t iocb__iovec__len(Ptrs::Ptr p) {
    iocb* i = ((iocb*)p.ptr);
    return (uint64_t)(i->u.c.nbytes);
  }

  inline uint64_t SizeOfIocb() {
    return sizeof(iocb);
  }
}

template <>
struct std::hash<InstantiatedDiskInterface::IOCtx> {
  std::size_t operator()(const InstantiatedDiskInterface::IOCtx& x) const {
    std::cerr << "Error: Cell hash called" << std::endl;
    exit(1);
  }
};

namespace Ptrs {
  template <typename V>
  Ptr alloc__array__hugetables(uint64_t len, V init_v) {
    static_assert(sizeof(size_t) == 8);
    size_t byte_len = len * sizeof(V); // pre-condition requires this to not overflow

    int prot= PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_HUGETLB;
    void* void_p = mmap(NULL, byte_len, prot, flags, -1, 0);

    if (void_p == MAP_FAILED) {
      std::cerr << "alloc__arayy__hugetables: mmap failed" << std::endl;
      exit(1);
    }

    V* ptr = (V*)void_p;
    for (uint64_t i = 0; i < len; i++) {
      new (&ptr[i]) V(init_v);
    }
    return Ptr((uintptr_t)ptr);
  }
}

namespace LinearExtern {
  template <typename A>
  lseq<A> lseq_alloc_raw_hugetables(uint64 length) {
    size_t byte_len = length * sizeof(A);
    if (length != 0 && byte_len / length != sizeof(A)) {
      std::cerr << "lseq_alloc_raw_hugetables: overflow detected" << std::endl;
      exit(1);
    }

    byte_len = ((byte_len + 4095) / 4096) * 4096;
    if (byte_len == 0) {
      std::cerr << "lseq_alloc_raw_hugetables: overflow detected" << std::endl;
      exit(1);
    }

    int prot= PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_HUGETLB;
    void* void_p = mmap(NULL, byte_len, prot, flags, -1, 0);

    if (void_p == MAP_FAILED) {
      std::cerr << "alloc__arayy__hugetables: mmap failed" << std::endl;
      exit(1);
    }

    LinearMaybe::maybe<A>* ptr = (LinearMaybe::maybe<A>*)void_p;
    for (uint64_t i = 0; i < length; i++) {
      new (&ptr[i]) LinearMaybe::maybe<A>();
    }

    lseq<A> ret;
    ret.ptr = ptr;
    ret.len = length;
    return ret;
  }
}
