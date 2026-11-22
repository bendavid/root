#include "RConfigure.h"

#include <memory>
#include <string>

#include "gtest/gtest.h"

#include "TEnv.h"
#include "TFile.h"

#include "ROOT/RNetXNGTransientErrors.hxx"
#include "ROOT/TestSupport.hxx"

#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClStatus.hh>

const char *fileName = "root://eospublic.cern.ch//eos/root-eos/h1/dstarmb.root";

TEST(TNetXNGFileTest, Plugin)
{
   auto f = TFile::Open(fileName);
   EXPECT_TRUE(f != nullptr);
   EXPECT_TRUE(f->IsOpen());
   EXPECT_FALSE(f->IsZombie());
   EXPECT_STREQ("TNetXNGFile", f->ClassName());
   delete f;
}

TEST(TNetXNGFileTest, DoubleClose)
{
   auto f = TFile::Open(fileName);
   EXPECT_TRUE(f != nullptr);
   EXPECT_FALSE(f->IsZombie());
   EXPECT_TRUE(f->IsOpen());
   EXPECT_STREQ("TNetXNGFile", f->ClassName());
   f->Close();
   EXPECT_FALSE(f->IsOpen());
   f->Close();
   EXPECT_FALSE(f->IsOpen());
   delete f;
}

TEST(TNetXNGFileTest, TransientErrors)
{
   using ROOT::Internal::IsTransientXRootDError;
   // Error responses of the server: an unreachable data server or an overloaded server are transient, a missing file
   // or missing permissions are not
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errErrorResponse, kXR_noserver));
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errErrorResponse, kXR_Overloaded));
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errErrorResponse, kXR_IOError));
   EXPECT_FALSE(IsTransientXRootDError(XrdCl::errErrorResponse, kXR_NotFound));
   EXPECT_FALSE(IsTransientXRootDError(XrdCl::errErrorResponse, kXR_NotAuthorized));
   // Errors of the client: connection problems and timeouts are transient, failed authentication or invalid arguments
   // are not
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errConnectionError, 0));
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errSocketTimeout, 0));
   EXPECT_TRUE(IsTransientXRootDError(XrdCl::errOperationExpired, 0));
   EXPECT_FALSE(IsTransientXRootDError(XrdCl::errAuthFailed, 0));
   EXPECT_FALSE(IsTransientXRootDError(XrdCl::errInvalidArgs, 0));
}

namespace {
int gNumOpenErrors = 0;
int gNumOpenRetries = 0;
void CountOpenDiagnostics(int level, Bool_t, const char *location, const char *msg)
{
   if (level >= kError && std::string(location) == "TNetXNGFile::Open")
      ++gNumOpenErrors;
   else if (level == kWarning && std::string(location) == "TFile::Open" &&
            std::string(msg).find("failed with a transient error") != std::string::npos)
      ++gNumOpenRetries;
   else
      ADD_FAILURE() << "unexpected diagnostic from " << location << ": " << msg;
}
} // anonymous namespace

// Opening a file on a server that cannot be reached fails with a transient error: TFile::Open() retries it
TEST(TNetXNGFileTest, OpenRetriedAfterTransientError)
{
   // fail fast: a single connection attempt per open
   XrdCl::DefaultEnv::GetEnv()->PutInt("ConnectionRetry", 1);
   XrdCl::DefaultEnv::GetEnv()->PutInt("ConnectionWindow", 1);
   gEnv->SetValue("IO.TransientErrorRetries", 2);
   gEnv->SetValue("IO.TransientErrorRetryDelay", 0.);

   {
      ROOT::TestSupport::FilterDiagsRAII filter(CountOpenDiagnostics);
      std::unique_ptr<TFile> f(TFile::Open("root://localhost:1//nonexistent.root"));
      EXPECT_EQ(nullptr, f.get());
   }
   // the first attempt and the two retries fail
   EXPECT_EQ(3, gNumOpenErrors);
   EXPECT_EQ(2, gNumOpenRetries);
}
