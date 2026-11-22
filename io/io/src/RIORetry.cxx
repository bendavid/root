/// \file RIORetry.cxx
/// \date 2026-11-22

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/RIORetry.hxx"

#include "TEnv.h"
#include "TError.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

int ROOT::Internal::RIORetry::GetMaxRetries()
{
   const int n = gEnv->GetValue("IO.TransientErrorRetries", 3);
   return n > 0 ? n : 0;
}

double ROOT::Internal::RIORetry::GetDelay(int retry)
{
   const double base = gEnv->GetValue("IO.TransientErrorRetryDelay", 5.);
   if (!(base > 0))
      return 0;
   thread_local std::mt19937 generator{std::random_device{}()};
   std::uniform_real_distribution<double> variation(0.5, 1.5);
   return base * std::pow(3., retry) * variation(generator);
}

void ROOT::Internal::RIORetry::WaitBeforeRetry(int retry, const char *location, std::string_view what,
                                               std::string_view reason)
{
   const double delay = GetDelay(retry);
   ::Warning(location, "%.*s failed with a transient error (%.*s), retry %d of %d in %.1f s",
             static_cast<int>(what.size()), what.data(), static_cast<int>(reason.size()), reason.data(), retry + 1,
             GetMaxRetries(), delay);
   if (delay > 0)
      std::this_thread::sleep_for(std::chrono::duration<double>(delay));
}

bool ROOT::Internal::RIORetry::IsTransientErrno(int errnum)
{
   switch (errnum) {
   case EIO:
   case ETIMEDOUT:
#ifdef ESTALE
   case ESTALE:
#endif
#ifdef ENETUNREACH
   case ENETUNREACH:
#endif
#ifdef EHOSTUNREACH
   case EHOSTUNREACH:
#endif
#ifdef ENOTCONN
   case ENOTCONN:
#endif
      return true;
   default: return false;
   }
}
