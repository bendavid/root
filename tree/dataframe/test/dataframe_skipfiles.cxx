#include "RConfigure.h" // R__USE_IMT

#include "ROOT/RDataFrame.hxx"
#include "ROOT/RDF/RDatasetSpec.hxx"
#include "ROOT/RDFHelpers.hxx"
#include "ROOT/RNTupleDS.hxx"
#include "ROOT/RNTupleModel.hxx"
#include "ROOT/RNTupleWriter.hxx"
#include "ROOT/TestSupport.hxx"
#include "TFile.h"
#include "TROOT.h"
#include "TSystem.h"
#include "TTree.h"

#include "gtest/gtest.h"

#include <limits>
#include <memory>
#include <string>
#include <vector>

using ROOT::RDF::Experimental::GetFileReport;
using ROOT::RDF::Experimental::GetSkippedFiles;
using ROOT::RDF::Experimental::RFileErrorPolicy;
using ROOT::RDF::Experimental::SetFileErrorPolicy;

namespace {

constexpr int kNFiles = 4;
constexpr int kEntriesPerFile = 1000;
// The sum of x over the entries of a file
constexpr long long kSumPerFile = kEntriesPerFile * (kEntriesPerFile - 1) / 2;
constexpr const char *kMissingFile = "dataframe_skipfiles_nonexistent.root";

std::vector<std::string> gTreeFiles;
std::vector<std::string> gNTupleFiles;

/// Writes the input files (kNFiles TTree and RNTuple files with the same content) for all tests
class RDFSkipFilesEnvironment : public ::testing::Environment {
public:
   void SetUp() override
   {
      for (int i = 0; i < kNFiles; ++i) {
         const auto treeFile = "dataframe_skipfiles_tree_" + std::to_string(i) + ".root";
         {
            TFile f(treeFile.c_str(), "RECREATE");
            TTree t("t", "t");
            int x;
            t.Branch("x", &x);
            for (x = 0; x < kEntriesPerFile; ++x)
               t.Fill();
            t.Write();
         }
         gTreeFiles.push_back(treeFile);

         const auto ntupleFile = "dataframe_skipfiles_ntuple_" + std::to_string(i) + ".root";
         {
            auto model = ROOT::RNTupleModel::Create();
            auto x = model->MakeField<int>("x");
            auto writer = ROOT::RNTupleWriter::Recreate(std::move(model), "nt", ntupleFile);
            for (*x = 0; *x < kEntriesPerFile; ++*x)
               writer->Fill();
         }
         gNTupleFiles.push_back(ntupleFile);
      }
   }

   void TearDown() override
   {
      for (const auto &f : gTreeFiles)
         gSystem->Unlink(f.c_str());
      for (const auto &f : gNTupleFiles)
         gSystem->Unlink(f.c_str());
   }
};

[[maybe_unused]] auto *const gSkipFilesEnvironment = ::testing::AddGlobalTestEnvironment(new RDFSkipFilesEnvironment);

/// The parameter: whether the event loops are multi-threaded
class RDFSkipFiles : public ::testing::TestWithParam<bool> {
protected:
   void SetUp() override
   {
      if (GetParam()) {
#ifdef R__USE_IMT
         ROOT::EnableImplicitMT(4);
#else
         GTEST_SKIP() << "multi-threading is not available";
#endif
      }
   }
   void TearDown() override
   {
#ifdef R__USE_IMT
      if (GetParam())
         ROOT::DisableImplicitMT();
#endif
   }

   /// The (kNFiles readable) files with one that does not exist at the given position
   static std::vector<std::string> WithMissingFile(const std::vector<std::string> &files, std::size_t pos = 2)
   {
      auto withMissing = files;
      withMissing.insert(withMissing.begin() + pos, kMissingFile);
      return withMissing;
   }

   static RFileErrorPolicy SkipFiles(std::size_t maxSkippedFiles = std::numeric_limits<std::size_t>::max())
   {
      RFileErrorPolicy policy;
      policy.fSkipFiles = true;
      policy.fMaxSkippedFiles = maxSkippedFiles;
      return policy;
   }
};

/// Check that the report lists the given files in this order, with the one at skippedIdx (if any) skipped
void CheckFileReport(const std::vector<ROOT::RDF::Experimental::RFileStatus> &report,
                     const std::vector<std::string> &files, std::size_t skippedIdx = std::size_t(-1))
{
   ASSERT_EQ(files.size(), report.size());
   for (std::size_t i = 0; i < files.size(); ++i) {
      EXPECT_NE(std::string::npos, report[i].fFileName.find(files[i])) << report[i].fFileName << " vs " << files[i];
      EXPECT_EQ(i == skippedIdx, report[i].fSkipped) << report[i].fFileName;
      EXPECT_EQ(i == skippedIdx, !report[i].fError.empty()) << report[i].fFileName;
   }
}

/// Count the entries and sum x with the readable files and a missing one at missingPos, skipping the missing one
void CheckSkip(ROOT::RDF::RNode df, const std::vector<std::string> &files, std::size_t missingPos, bool isTTree)
{
   RFileErrorPolicy policy;
   policy.fSkipFiles = true;
   SetFileErrorPolicy(df, policy);
   auto count = df.Count();
   auto sum = df.Sum<int>("x");
   {
      ROOT::TestSupport::CheckDiagsRAII diags;
      if (isTTree) {
         diags.optionalDiag(kError, "TFile::TFile", "does not exist", /*matchFullMessage=*/false);
         // single-threaded event loops, if the last file is missing
         diags.optionalDiag(kWarning, "TTreeReader::SetEntryBase()", "issue opening the last file",
                            /*matchFullMessage=*/false);
      }
      diags.requiredDiag(kWarning, "RDataFrame::Run", "skipping input file", /*matchFullMessage=*/false);
      diags.requiredDiag(kWarning, "RDataFrame::Run", "were skipped", /*matchFullMessage=*/false);
      EXPECT_EQ(kNFiles * kEntriesPerFile, *count);
   }
   EXPECT_EQ(kNFiles * kSumPerFile, static_cast<long long>(*sum));
   const auto skipped = GetSkippedFiles(df);
   ASSERT_EQ(1u, skipped.size());
   EXPECT_NE(std::string::npos, skipped[0].fFileName.find(kMissingFile));
   EXPECT_FALSE(skipped[0].fError.empty());
   auto withMissing = files;
   withMissing.insert(withMissing.begin() + missingPos, kMissingFile);
   CheckFileReport(GetFileReport(df), withMissing, missingPos);
}

} // anonymous namespace

TEST_P(RDFSkipFiles, TTreeErrorByDefault)
{
   ROOT::RDataFrame df("t", WithMissingFile(gTreeFiles));
   ROOT::TestSupport::CheckDiagsRAII diags;
   diags.requiredDiag(kError, "TFile::TFile", "does not exist", /*matchFullMessage=*/false);
   EXPECT_THROW(df.Count().GetValue(), std::runtime_error);
   EXPECT_TRUE(GetSkippedFiles(df).empty());
}

TEST_P(RDFSkipFiles, TTreeSkip)
{
   // (the first file is opened when constructing the RDataFrame, to determine the format: it needs to be readable)
   for (std::size_t pos : {std::size_t(1), std::size_t(2), std::size_t(kNFiles)}) {
      SCOPED_TRACE("missing file at position " + std::to_string(pos));
      CheckSkip(ROOT::RDF::AsRNode(ROOT::RDataFrame("t", WithMissingFile(gTreeFiles, pos))), gTreeFiles, pos,
                /*isTTree=*/true);
   }
}

TEST_P(RDFSkipFiles, TTreeMaxSkippedFiles)
{
   ROOT::RDataFrame df("t", WithMissingFile(gTreeFiles));
   SetFileErrorPolicy(df, SkipFiles(/*maxSkippedFiles=*/0));
   ROOT::TestSupport::CheckDiagsRAII diags;
   diags.requiredDiag(kError, "TFile::TFile", "does not exist", /*matchFullMessage=*/false);
   EXPECT_THROW(df.Count().GetValue(), std::runtime_error);
   EXPECT_TRUE(GetSkippedFiles(df).empty());
}

// With a global entry range, skipping a file would change the global entry numbers: the event loop stops with the error
TEST_P(RDFSkipFiles, TTreeGlobalRangeNotSkipped)
{
   ROOT::RDF::Experimental::RDatasetSpec spec;
   spec.AddSample({"sample", "t", WithMissingFile(gTreeFiles)});
   spec.WithGlobalRange({0, 3500});
   ROOT::RDataFrame df(spec);
   SetFileErrorPolicy(df, SkipFiles());
   ROOT::TestSupport::CheckDiagsRAII diags;
   diags.requiredDiag(kError, "TFile::TFile", "does not exist", /*matchFullMessage=*/false);
   if (GetParam())
      diags.requiredDiag(kWarning, "TTreeProcessorMT::Process", "cannot be skipped", /*matchFullMessage=*/false);
   EXPECT_THROW(df.Count().GetValue(), std::runtime_error);
}

// The files processed by an event loop are reported also without skipping
TEST_P(RDFSkipFiles, TTreeFileReport)
{
   ROOT::RDataFrame df("t", gTreeFiles);
   EXPECT_EQ(kNFiles * kEntriesPerFile, *df.Count());
   CheckFileReport(GetFileReport(df), gTreeFiles);
   EXPECT_TRUE(GetSkippedFiles(df).empty());
   // and again by a second event loop
   EXPECT_EQ(kNFiles * kSumPerFile, static_cast<long long>(*df.Sum<int>("x")));
   CheckFileReport(GetFileReport(df), gTreeFiles);
}

// The files are reported with their names after the expansion of globs
TEST_P(RDFSkipFiles, TTreeFileReportGlob)
{
   ROOT::RDataFrame df("t", "dataframe_skipfiles_tree_*.root");
   EXPECT_EQ(kNFiles * kEntriesPerFile, *df.Count());
   CheckFileReport(GetFileReport(df), gTreeFiles);
}

// With a global entry range, only the files with entries in the range are reported
TEST_P(RDFSkipFiles, TTreeFileReportGlobalRange)
{
   ROOT::RDF::Experimental::RDatasetSpec spec;
   spec.AddSample({"sample", "t", gTreeFiles});
   spec.WithGlobalRange({0, kEntriesPerFile + kEntriesPerFile / 2});
   ROOT::RDataFrame df(spec);
   EXPECT_EQ(kEntriesPerFile + kEntriesPerFile / 2, *df.Count());
   CheckFileReport(GetFileReport(df), {gTreeFiles[0], gTreeFiles[1]});
}

TEST_P(RDFSkipFiles, RNTupleErrorByDefault)
{
   auto df = ROOT::RDF::FromRNTuple("nt", WithMissingFile(gNTupleFiles));
   EXPECT_THROW(df.Count().GetValue(), std::runtime_error);
   EXPECT_TRUE(GetSkippedFiles(df).empty());
}

TEST_P(RDFSkipFiles, RNTupleSkip)
{
   // (the first file is opened when constructing the RDataFrame, to determine the format: it needs to be readable)
   for (std::size_t pos : {std::size_t(1), std::size_t(2), std::size_t(kNFiles)}) {
      SCOPED_TRACE("missing file at position " + std::to_string(pos));
      CheckSkip(ROOT::RDF::AsRNode(ROOT::RDF::FromRNTuple("nt", WithMissingFile(gNTupleFiles, pos))), gNTupleFiles, pos,
                /*isTTree=*/false);
   }
}

TEST_P(RDFSkipFiles, RNTupleFileReport)
{
   auto df = ROOT::RDF::FromRNTuple("nt", gNTupleFiles);
   EXPECT_EQ(kNFiles * kEntriesPerFile, *df.Count());
   CheckFileReport(GetFileReport(df), gNTupleFiles);
}

INSTANTIATE_TEST_SUITE_P(SingleAndMultiThreaded, RDFSkipFiles, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool> &info) { return info.param ? "MT" : "ST"; });
