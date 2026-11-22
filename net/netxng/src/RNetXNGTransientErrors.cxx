/// \file RNetXNGTransientErrors.cxx
/// \date 2026-11-22

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/RNetXNGTransientErrors.hxx"

#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClStatus.hh>

bool ROOT::Internal::IsTransientXRootDError(std::uint16_t code, std::uint32_t errNo)
{
   using namespace XrdCl;
   if (code == errErrorResponse) {
      // An error response from the server. Not kXR_noserver: the redirector has no data server holding the file
      // available (e.g. for EOS, all the file systems with replicas of the file are offline), which typically lasts
      // longer than the retries. (Short unavailabilities of data servers are handled by the XRootD client: it gets
      // connection errors from them, and retries at the redirector.)
      switch (errNo) {
      case kXR_Overloaded:
      case kXR_ServerError:
      case kXR_IOError:
      case kXR_FSError:
      case kXR_NoMemory:
      case kXR_ReqTimedOut:
      case kXR_TimerExpired: return true;
      default: return false;
      }
   }
   switch (code) {
   // Connection problems that remain after the recovery of the XRootD client, and operations that timed out
   case errSocketError:
   case errSocketTimeout:
   case errSocketDisconnected:
   case errStreamDisconnect:
   case errConnectionError:
   case errInvalidSession:
   case errOperationExpired:
   case errOperationInterrupted:
   case errTlsError:
   case errNoMoreReplicas: return true;
   default: return false;
   }
}
