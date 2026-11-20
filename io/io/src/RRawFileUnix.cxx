// @(#)root/io:$Id$
// Author: Jakob Blomer

/*************************************************************************
 * Copyright (C) 1995-2018, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/RConfig.hxx"
#include "ROOT/RRawFileUnix.hxx"

#ifdef R__HAS_URING
  #include "ROOT/RIoUring.hxx"
#endif

#include "TEnv.h"
#include "TError.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int kDefaultBlockSize = 4096; // If fstat() does not provide a block size hint, use this value instead

/// Whether ReadV() announces its requests to the kernel (POSIX_FADV_WILLNEED) before reading them when io_uring is
/// not available; can be turned off with `RRawFile.ReadVHints: 0` in .rootrc
bool AreReadVHintsEnabled()
{
   static const bool enabled = gEnv ? (gEnv->GetValue("RRawFile.ReadVHints", 1) != 0) : true;
   return enabled;
}
} // anonymous namespace

ROOT::Internal::RRawFileUnix::RRawFileUnix(std::string_view url, ROptions options) : RRawFile(url, options) {}

ROOT::Internal::RRawFileUnix::~RRawFileUnix()
{
   if (fFileDes >= 0)
      close(fFileDes);
}

std::unique_ptr<ROOT::Internal::RRawFile> ROOT::Internal::RRawFileUnix::Clone() const
{
   return std::make_unique<RRawFileUnix>(fUrl, fOptions);
}

std::unique_ptr<ROOT::Internal::RRawFile> ROOT::Internal::RRawFileUnix::ShallowClone() const
{
   if (!IsOpen())
      return Clone();
   // A duplicated file descriptor refers to the same open file description: there is no path lookup or open on the
   // file system, and the read-ahead advice is shared. pread() does not use the shared file offset.
   const int fd = dup(fFileDes);
   if (fd < 0)
      return Clone();
   auto clone = std::make_unique<RRawFileUnix>(fUrl, fOptions);
   clone->fFileDes = fd;
   InitShallowClone(*clone);
   return clone;
}

std::uint64_t ROOT::Internal::RRawFileUnix::GetSizeImpl()
{
#ifdef R__SEEK64
   struct stat64 info;
   int res = fstat64(fFileDes, &info);
#else
   struct stat info;
   int res = fstat(fFileDes, &info);
#endif
   if (res != 0)
      throw std::runtime_error("Cannot call fstat on '" + fUrl + "', error: " + std::string(strerror(errno)));
   return info.st_size;
}

void ROOT::Internal::RRawFileUnix::OpenImpl()
{
#ifdef R__SEEK64
   fFileDes = open64(GetLocation(fUrl).c_str(), O_RDONLY);
#else
   fFileDes = open(GetLocation(fUrl).c_str(), O_RDONLY);
#endif
   if (fFileDes < 0) {
      throw std::runtime_error("Cannot open '" + fUrl + "', error: " + std::string(strerror(errno)));
   }

   if (fOptions.fBlockSize != ROptions::kUseDefaultBlockSize)
      return;

#ifdef R__SEEK64
   struct stat64 info;
   int res = fstat64(fFileDes, &info);
#else
   struct stat info;
   int res = fstat(fFileDes, &info);
#endif
   if (res != 0) {
      throw std::runtime_error("Cannot call fstat on '" + fUrl + "', error: " + std::string(strerror(errno)));
   }
   if (info.st_blksize > 0) {
      fOptions.fBlockSize = info.st_blksize;
   } else {
      fOptions.fBlockSize = kDefaultBlockSize;
   }
}

void ROOT::Internal::RRawFileUnix::SetDiscourageReadAheadImpl(bool value)
{
#ifndef __APPLE__
   posix_fadvise(fFileDes, 0, 0, value ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
#else
   (void)value;
#endif
}

void ROOT::Internal::RRawFileUnix::ReadVImpl(RIOVec *ioVec, unsigned int nReq)
{
#ifdef R__HAS_URING
   // ROOT may have been built with liburing support while the running kernel does not
   // support io_uring (GH #12701); probe once per process and warn at the first attempt.
   static const bool sIoUringAvailable = [] {
      std::string errMsg;
      if (RIoUring::IsAvailable(&errMsg))
         return true;
      Warning("RIoUring", "io_uring is not available because:\n%s", errMsg.c_str());
      Warning("RRawFileUnix", "io_uring setup failed, falling back to blocking I/O in ReadV");
      return false;
   }();
   thread_local bool uring_failed = false;
   if (!uring_failed && sIoUringAvailable) {
      try {
         RIoUring ring; // throws std::runtime_error
         std::vector<RIoUring::RReadEvent> reads;
         reads.reserve(nReq);
         for (std::size_t i = 0; i < nReq; ++i) {
            RIoUring::RReadEvent ev;
            ev.fBuffer = ioVec[i].fBuffer;
            ev.fOffset = ioVec[i].fOffset;
            ev.fSize = ioVec[i].fSize;
            ev.fFileDes = fFileDes;
            reads.push_back(ev);
         }
         ring.SubmitReadsAndWait(reads.data(), nReq);
         for (std::size_t i = 0; i < nReq; ++i) {
            ioVec[i].fOutBytes = reads.at(i).fOutBytes;
         }
         return;
      }
      catch(const std::runtime_error &e) {
         Warning("RIoUring", "io_uring is not available because:\n%s", e.what());
         Warning("RRawFileUnix",
              "io_uring setup failed, falling back to blocking I/O in ReadV");
         uring_failed = true;
      }
   }
#endif
#ifndef __APPLE__
   // Without io_uring, announce all requests to the kernel first, so that it fetches them concurrently in the
   // background rather than one after the other, then read them in order (mostly from the page cache). With the
   // read-ahead advice for unbuffered reads (POSIX_FADV_RANDOM), the kernel fetches exactly the requested ranges.
   if (nReq > 1 && AreReadVHintsEnabled()) {
      for (unsigned int i = 0; i < nReq; ++i) {
         if (ioVec[i].fSize > 0)
            posix_fadvise(fFileDes, ioVec[i].fOffset, ioVec[i].fSize, POSIX_FADV_WILLNEED);
      }
   }
#endif
   RRawFile::ReadVImpl(ioVec, nReq);
}

size_t ROOT::Internal::RRawFileUnix::ReadAtImpl(void *buffer, size_t nbytes, std::uint64_t offset)
{
   size_t total_bytes = 0;
   while (nbytes) {
#ifdef R__SEEK64
      ssize_t res = pread64(fFileDes, buffer, nbytes, offset);
#else
      ssize_t res = pread(fFileDes, buffer, nbytes, offset);
#endif
      if (res < 0) {
         if (errno == EINTR)
            continue;
         throw std::runtime_error("Cannot read from '" + fUrl + "', error: " + std::string(strerror(errno)));
      } else if (res == 0) {
         return total_bytes;
      }
      R__ASSERT(static_cast<size_t>(res) <= nbytes);
      buffer = reinterpret_cast<unsigned char *>(buffer) + res;
      nbytes -= res;
      total_bytes += res;
      offset += res;
   }
   return total_bytes;
}
