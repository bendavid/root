/// \file ROOT/RNetXNGTransientErrors.hxx
/// \date 2026-11-22

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RNetXNGTransientErrors
#define ROOT_RNetXNGTransientErrors

#include <cstdint>

namespace ROOT {
namespace Internal {

/// Whether a failed XRootD client operation, with the status code `code` and the server error number `errNo` of its
/// XrdCl::XRootDStatus, failed with a transient error that is worth retrying (see ROOT::Internal::RIORetry): an
/// unreachable, overloaded or failing server, or a lost or timed-out connection. Errors such as a missing file or
/// missing permissions are permanent.
bool IsTransientXRootDError(std::uint16_t code, std::uint32_t errNo);

} // namespace Internal
} // namespace ROOT

#endif
