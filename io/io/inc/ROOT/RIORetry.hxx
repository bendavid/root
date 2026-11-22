/// \file ROOT/RIORetry.hxx
/// \date 2026-11-22

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RIORetry
#define ROOT_RIORetry

#include <string_view>

namespace ROOT {
namespace Internal {

/// Retries of I/O operations that failed with a transient error, e.g. because a storage server was temporarily
/// unreachable or overloaded. Used when opening files for reading (TFile::Open()) and when reading them (TNetXNGFile,
/// RRawFile); which errors are transient is decided by the file implementations.
///
/// The retries are configured with the rootrc settings
///  - `IO.TransientErrorRetries` (default 3): the number of retries; 0 disables them
///  - `IO.TransientErrorRetryDelay` (default 5): the delay before the first retry, in seconds. It grows by a factor of
///    3 with every further retry and is varied randomly by +-50%, so that clients that failed at the same time (e.g.
///    the threads of a multi-threaded event loop) do not retry in lockstep.
namespace RIORetry {

/// The maximum number of retries of an operation
int GetMaxRetries();

/// The delay before retry number `retry` (counting from 0), in seconds, including the random variation
double GetDelay(int retry);

/// Print a warning about retry number `retry` (counting from 0) of the operation `what` that failed because of `reason`
/// and wait for GetDelay(retry)
void WaitBeforeRetry(int retry, const char *location, std::string_view what, std::string_view reason);

/// Whether a failure of a system call with the given errno is a transient error of the storage (e.g. a network file
/// system)
bool IsTransientErrno(int errnum);

} // namespace RIORetry

} // namespace Internal
} // namespace ROOT

#endif
