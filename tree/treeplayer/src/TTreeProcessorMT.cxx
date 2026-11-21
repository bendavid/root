// @(#)root/thread:$Id$
// Authors: Enric Tejedor, Enrico Guiraud CERN  05/06/2018

/*************************************************************************
 * Copyright (C) 1995-2020, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class ROOT::TTreeProcessorMT
    \brief A class to process the entries of a TTree in parallel.

By means of its Process method, ROOT::TTreeProcessorMT provides a way to process the
entries of a TTree in parallel. When invoking TTreeProcessor::Process, the user
passes a function whose only parameter is a TTreeReader. The function iterates
on a subrange of entries by using that TTreeReader.

The implementation of ROOT::TTreeProcessorMT parallelizes the processing of the subranges,
each corresponding to a cluster in the TTree. This is possible thanks to the use
of a ROOT::TThreadedObject, so that each thread works with its own TFile and TTree
objects.
*/

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <tuple>
#include <utility>

#include "TEnv.h"
#include "TROOT.h"
#include "ROOT/TTreeProcessorMT.hxx"
#include "ROOT/RFileTaskScheduler.hxx"

using namespace ROOT;

namespace {

using EntryRange = std::pair<Long64_t, Long64_t>;

// note that this routine assumes global entry numbers
bool ClustersAreSortedAndContiguous(const std::vector<std::vector<EntryRange>> &cls)
{
   Long64_t last_end = 0ll;
   for (const auto &fcl : cls) {
      for (const auto &c : fcl) {
         if (last_end != c.first)
            return false;
         last_end = c.second;
      }
   }
   return true;
}

/// Take a vector of vectors of EntryRanges (a vector per file), filter the entries according to entryList, and
/// and return a new vector of vectors of EntryRanges where cluster start/end entry numbers have been converted to
/// TEntryList-local entry numbers.
///
/// This routine assumes that entry numbers in the TEntryList (and, if present, in the sub-entrylists) are in
/// ascending order, i.e., for n > m:
///   elist.GetEntry(n) + tree_offset_for_entry_from_elist(n) > elist.GetEntry(m) + tree_offset_for_entry_from_elist(m)
std::vector<std::vector<EntryRange>>
ConvertToElistClusters(std::vector<std::vector<EntryRange>> &&clusters, TEntryList &entryList,
                       const std::vector<std::string> &treeNames, const std::vector<std::string> &fileNames,
                       const std::vector<Long64_t> &entriesPerFile)
{
   R__ASSERT(entryList.GetN() > 0); // wasteful to call this function if it has nothing to do
   R__ASSERT(ClustersAreSortedAndContiguous(clusters));

   const bool listHasGlobalEntryNumbers = entryList.GetLists() == nullptr;
   const auto nFiles = clusters.size();

   std::unique_ptr<TChain> chain;
   using NextFn_t = Long64_t (*)(Long64_t &, TEntryList &, TChain *);
   // A function that advances TEntryList and returns global entry numbers or -1 if we reached the end
   // (might or might not need a TChain depending on whether listHasGlobalEntryNumbers)
   NextFn_t Next;
   if (listHasGlobalEntryNumbers) {
      Next = [](Long64_t &elEntry, TEntryList &elist, TChain *) {
         ++elEntry;
         return elist.Next();
      };
   } else {
      // we need `chain` to be able to convert local entry numbers to global entry numbers in `Next`
      chain = ROOT::Internal::TreeUtils::MakeChainForMT();
      for (auto i = 0u; i < nFiles; ++i)
         chain->Add((fileNames[i] + "?#" + treeNames[i]).c_str(), entriesPerFile[i]);
      Next = [](Long64_t &elEntry, TEntryList &elist, TChain *ch) {
         ++elEntry;
         int treenum = -1;
         Long64_t localEntry = elist.GetEntryAndTree(elEntry, treenum);
         if (localEntry == -1ll)
            return localEntry;
         return localEntry + ch->GetTreeOffset()[treenum];
      };
   }

   // the call to GetEntry also serves the purpose to reset TEntryList::fLastIndexQueried,
   // so we can be sure TEntryList::Next will return the correct thing
   Long64_t elistEntry = 0ll;
   Long64_t entry = entryList.GetEntry(elistEntry);

   std::vector<std::vector<EntryRange>> elistClusters;

   for (auto fileN = 0u; fileN < nFiles; ++fileN) {
      std::vector<EntryRange> elistClustersForFile;
      for (const auto &c : clusters[fileN]) {
         if (entry >= c.second || entry == -1ll) // no entrylist entries in this cluster
            continue;
         R__ASSERT(entry >= c.first); // current entry should never come before the cluster we are looking at
         const Long64_t elistRangeStart = elistEntry;
         // advance entry list until the entrylist entry goes beyond the end of the cluster
         while (entry < c.second && entry != -1ll)
            entry = Next(elistEntry, entryList, chain.get());
         elistClustersForFile.emplace_back(EntryRange{elistRangeStart, elistEntry});
      }
      elistClusters.emplace_back(std::move(elistClustersForFile));
   }

   R__ASSERT(elistClusters.size() == clusters.size()); // same number of files
   R__ASSERT(ClustersAreSortedAndContiguous(elistClusters));

   entryList.GetEntry(0ll); // reset TEntryList internal state, lest we incur in ROOT-10807
   return elistClusters;
}

// EntryRanges and number of entries per file
using ClustersAndEntries = std::pair<std::vector<std::vector<EntryRange>>, std::vector<Long64_t>>;

////////////////////////////////////////////////////////////////////////
/// Open a file and get the tree to be processed from it; the tree is owned by the file.
std::pair<std::unique_ptr<TFile>, TTree *> OpenTreeForProcessing(const std::string &fileName, const std::string &treeName)
{
   TDirectory::TContext c;
   std::unique_ptr<TFile> f(TFile::Open(fileName.c_str(),
                                        "READ_WITHOUT_GLOBALREGISTRATION")); // need TFile::Open to load plugins if need be
   if (!f || f->IsZombie()) {
      const auto msg = "TTreeProcessorMT::Process: an error occurred while opening file \"" + fileName + "\"";
      throw std::runtime_error(msg);
   }
   auto *t = f->Get<TTree>(treeName.c_str()); // t will be deleted by f

   if (!t) {
      const auto msg = "TTreeProcessorMT::Process: an error occurred while getting tree \"" + treeName +
                       "\" from file \"" + fileName + "\"";
      throw std::runtime_error(msg);
   }

   // Avoid calling TROOT::RecursiveRemove for this tree, it takes the read lock and we don't need it.
   t->ResetBit(kMustCleanup);
   ROOT::Internal::TreeUtils::ClearMustCleanupBits(*t->GetListOfBranches());
   return {std::move(f), t};
}

////////////////////////////////////////////////////////////////////////
/// Merge the clusters of a file into at most maxTasksPerFile tasks.
///
/// Here we "fuse" clusters together if the number of clusters is too big with respect to
/// the number of slots, otherwise we can incur in an overhead which is big enough
/// to make parallelisation detrimental to performance.
/// For example, this is the case when, following a merging of many small files, a file
/// contains a tree with many entries and with clusters of just a few entries each.
/// Another problematic case is a high number of slots (e.g. 256) coupled with a high number
/// of files (e.g. 1000 files): the large amount of files might result in a large amount
/// of tasks, but the elevated concurrency level makes the little synchronization required by
/// task initialization very expensive. In this case it's better to simply process fewer, larger tasks.
/// Cluster-merging can help reduce the number of tasks down to a minumum of one task per file.
///
/// The criterion according to which we fuse clusters together is to have around
/// TTreeProcessorMT::GetTasksPerWorkerHint() clusters per slot.
/// Concretely, for each file we will cap the number of tasks to ceil(GetTasksPerWorkerHint() * nWorkers / nFiles).
std::vector<EntryRange> FuseClusters(std::vector<EntryRange> clustersInThisFile, const unsigned int maxTasksPerFile)
{
   const auto clustersInThisFileSize = clustersInThisFile.size();
   const auto nFolds = clustersInThisFileSize / maxTasksPerFile;
   // If the number of clusters is less than maxTasksPerFile
   // we take the clusters as they are
   if (nFolds == 0)
      return clustersInThisFile;

   // Otherwise, we have to merge clusters, distributing the reminder evenly
   // onto the first clusters
   std::vector<EntryRange> eventRanges;
   auto nReminderClusters = clustersInThisFileSize % maxTasksPerFile;
   for (auto i = 0ULL; i < clustersInThisFileSize; ++i) {
      const auto start = clustersInThisFile[i].first;
      // We lump together at least nFolds clusters, therefore
      // we need to jump ahead of nFolds-1.
      i += (nFolds - 1);
      // We now add a cluster if we have some reminder left
      if (nReminderClusters > 0) {
         i += 1U;
         nReminderClusters--;
      }
      const auto end = clustersInThisFile[i].second;
      eventRanges.emplace_back(EntryRange({start, end}));
   }
   return eventRanges;
}

////////////////////////////////////////////////////////////////////////
/// Return the cluster boundaries of a tree, with entry numbers local to the tree.
std::vector<EntryRange> GetClusterRanges(TTree &tree)
{
   std::vector<EntryRange> clusters;
   auto clusterIter = tree.GetClusterIterator(0);
   Long64_t clusterStart = 0;
   const Long64_t entries = tree.GetEntries();
   while ((clusterStart = clusterIter()) < entries)
      clusters.emplace_back(EntryRange{clusterStart, clusterIter.GetNextEntry()});
   return clusters;
}

////////////////////////////////////////////////////////////////////////
/// Return a vector of cluster boundaries for the given tree and files.
///
/// The files are opened concurrently with the given thread pool, to determine their clusters and entries. If the
/// requested range ends before the end of the dataset, the files are opened in batches of the size of the pool, in
/// order, until the end of the range is reached.
ClustersAndEntries MakeClusters(const std::vector<std::string> &treeNames,
                                       const std::vector<std::string> &fileNames, const unsigned int maxTasksPerFile,
                                       ROOT::TThreadExecutor &pool,
                                       const EntryRange &range = {0, std::numeric_limits<Long64_t>::max()})
{
   // Note that as a side-effect of opening all files that are going to be used in the
   // analysis once, all necessary streamers will be loaded into memory.
   TDirectory::TContext c;
   const auto nFileNames = fileNames.size();
   const std::size_t batchSize =
      range.second == std::numeric_limits<Long64_t>::max() ? nFileNames : std::max(1u, pool.GetPoolSize());
   std::vector<std::vector<EntryRange>> localClusters(nFileNames);
   std::vector<Long64_t> localEntries(nFileNames, 0);
   // Errors opening a file are reported when the file is reached in order, as without concurrent opening (files
   // beyond the end of the range may be opened as part of a batch)
   std::vector<std::exception_ptr> errors(nFileNames);
   std::vector<std::vector<EntryRange>> clustersPerFile;
   std::vector<Long64_t> entriesPerFile;
   entriesPerFile.reserve(nFileNames);
   Long64_t offset = 0ll;
   bool rangeEndReached = false; // flag to break the outer loop
   for (std::size_t batchBegin = 0; batchBegin < nFileNames && !rangeEndReached; batchBegin += batchSize) {
      const auto batchEnd = std::min(nFileNames, batchBegin + batchSize);
      pool.Foreach(
         [&](unsigned int i) {
            try {
               auto [f, t] = OpenTreeForProcessing(fileNames[i], treeNames[i]); // t is owned by f
               localClusters[i] = GetClusterRanges(*t);
               localEntries[i] = t->GetEntries();
            } catch (...) {
               errors[i] = std::current_exception();
            }
         },
         ROOT::TSeq<unsigned int>(batchBegin, batchEnd));

      for (auto i = batchBegin; i < batchEnd && !rangeEndReached; ++i) {
         if (errors[i])
            std::rethrow_exception(errors[i]);
         const Long64_t entries = localEntries[i];
         // Iterate over the clusters in the current file
         std::vector<EntryRange> entryRanges;
         for (const auto &[clusterStart, clusterEnd] : localClusters[i]) {
            if (rangeEndReached)
               break;
            // Currently, if a user specified a range, the clusters will be only globally obtained
            // Assume that there are 3 files with entries: [0, 100], [0, 150], [0, 200] (in this order)
            // Since the cluster boundaries are obtained sequentially, applying the offsets, the boundaries
            // would be: 0, 100, 250, 450. Now assume that the user provided the range (150, 300)
            // Then, in the first iteration, nothing is going to be added to entryRanges since:
            // std::max(0, 150) < std::min(100, max). Then, by the same logic only a subset of the second
            // tree is added, i.e.: currentStart is now 200 and currentEnd is 250 (locally from 100 to 150).
            // Lastly, the last tree would take entries from 250 to 300 (or from 0 to 50 locally).
            // The current file's offset to start and end is added to make them (chain) global
            const auto currentStart = std::max(clusterStart + offset, range.first);
            const auto currentEnd = std::min(clusterEnd + offset, range.second);
            // This is not satified if the desired start is larger than the last entry of some cluster
            // In this case, this cluster is not going to be processes further
            if (currentStart < currentEnd)
               entryRanges.emplace_back(EntryRange{currentStart, currentEnd});
            if (currentEnd == range.second) // if the desired end is reached, stop reading further
               rangeEndReached = true;
         }
         offset += entries; // consistently keep track of the total number of entries
         clustersPerFile.emplace_back(std::move(entryRanges));
         // Keep track of the entries, even if their corresponding tree is out of the range, e.g. entryRanges is empty
         entriesPerFile.emplace_back(entries);
      }
   }
   if (range.first >= offset && offset > 0) // do not error out on an empty tree
      throw std::logic_error(std::string("A range of entries was passed in the creation of the TTreeProcessorMT, ") +
                             "but the starting entry (" + range.first + ") is larger than the total number of " +
                             "entries (" + offset + ") in the dataset.");

   // Merge clusters into fewer tasks if needed, see FuseClusters()
   std::vector<std::vector<EntryRange>> eventRangesPerFile;
   eventRangesPerFile.reserve(clustersPerFile.size());
   for (auto &clustersInThisFile : clustersPerFile)
      eventRangesPerFile.emplace_back(FuseClusters(std::move(clustersInThisFile), maxTasksPerFile));

   return std::make_pair(std::move(eventRangesPerFile), std::move(entriesPerFile));
}

////////////////////////////////////////////////////////////////////////
/// The entry range of the TTreeCache of a worker: a window of its current task and the next tasks of its run of
/// consecutive tasks in a file, so that the cache reads ahead into the next tasks.
///
/// The window is set as the cache entry range by the reader of the first task of the window, and kept by the readers
/// of the following tasks, since setting it makes the cache fill its buffer again. A new window is set when the
/// worker gets past the window, if the task does not continue the previous one, or if the run of the worker became
/// shorter than the window (other workers took over tasks).
class RCacheWindow {
   int fNTasksAhead;            ///< Tasks to read ahead into; < 0: to the end of the run; 0: none (reader's range)
   Long64_t fEnd = -1;          ///< End of the current window, -1 if none
   Long64_t fPreviousTaskEnd = -1;

public:
   explicit RCacheWindow(int nTasksAhead) : fNTasksAhead(nTasksAhead) {}

   void Reset() { fEnd = fPreviousTaskEnd = -1; }

   /// Configure the reader of a task. treeRanges are the (tree) entry ranges of the tasks of the file, runEnd is the
   /// end (exclusive) of the run of tasks of the worker. A task without entries to process (with an entry list) does
   /// not set up its reader, and thus cannot set the window.
   void ConfigureReader(TTreeReader &reader, const std::vector<EntryRange> &treeRanges, std::size_t taskIdx,
                        std::size_t runEnd, bool emptyTask = false)
   {
      if (fNTasksAhead == 0 || emptyTask)
         return;
      const auto &range = treeRanges[taskIdx];
      const Long64_t runEndEntry = treeRanges[runEnd - 1].second;
      const bool inWindow = range.first == fPreviousTaskEnd && range.second <= fEnd && fEnd <= runEndEntry;
      if (inWindow) {
         reader.SetUpdateCacheEntryRange(false);
      } else {
         const std::size_t lastTask =
            fNTasksAhead < 0 ? runEnd - 1 : std::min<std::size_t>(taskIdx + fNTasksAhead, runEnd - 1);
         fEnd = treeRanges[lastTask].second;
         reader.SetCacheEntryRange(range.first, fEnd);
      }
      fPreviousTaskEnd = range.second;
   }
};

/// Number of tasks to read ahead into with the TTreeCache, from TTreeProcessorMT.CacheAcrossTasks, and the
/// corresponding number of tasks reserved for a worker
int GetNTasksAhead()
{
   // N > 0 lets the TTreeCache of a worker read ahead across the next N tasks of its run of consecutive tasks in a
   // file, rather than only within each task (N < 0: to the end of the run); 0 to read within each task only. These
   // next tasks are reserved for the worker, since their baskets may have been read already (to the end of the run:
   // only the next task). The default of 1 saves up to a third of the cache fills (vector reads, i.e. round trips for
   // remote files) when tasks are short, without reading baskets twice.
   return gEnv->GetValue("TTreeProcessorMT.CacheAcrossTasks", 1);
}
std::size_t GetNTasksReserved(int nTasksAhead)
{
   return nTasksAhead > 0 ? static_cast<std::size_t>(nTasksAhead) : (nTasksAhead < 0 ? 1 : 0);
}

} // anonymous namespace

namespace ROOT {

unsigned int TTreeProcessorMT::fgTasksPerWorkerHint = 10U;

namespace Internal {

////////////////////////////////////////////////////////////////////////////////
/// Construct fChain, also adding friends if needed and injecting knowledge of offsets if available.
/// \param[in] treeNames Name of the tree for each file in `fileNames`.
/// \param[in] fileNames Files to be opened.
/// \param[in] friendInfo Information about TTree friends, if any.
/// \param[in] nEntries Number of entries to be processed.
/// \param[in] friendEntries Number of entries in each friend. Expected to have same ordering as friendInfo.
void TTreeView::MakeChain(const std::vector<std::string> &treeNames, const std::vector<std::string> &fileNames,
                          const ROOT::TreeUtils::RFriendInfo &friendInfo, const std::vector<Long64_t> &nEntries)
{
   fChain = ROOT::Internal::TreeUtils::MakeChainForMT();
   // Because of the range, we might have stopped reading entries earlier,
   // hence the size of nEntries can be smaller than the number of all files
   // TODO: pass "firstFileToProcess" index in case of a range,
   // and do not add files to the chain, which are before the desired start entry of the range
   const auto nFilesToProcess = nEntries.size();
   for (auto i = 0u; i < nFilesToProcess; ++i) {
      fChain->Add((fileNames[i] + "?#" + treeNames[i]).c_str(), nEntries[i]);
   }
   fNoCleanupNotifier.RegisterChain(*fChain.get());

   fFriends = ROOT::Internal::TreeUtils::MakeFriends(friendInfo);
   const auto nFriends = friendInfo.fFriendNames.size();
   R__ASSERT(nFriends == fFriends.size() && "Created the wrong number of friends from the available information.");
   for (std::size_t i = 0ul; i < nFriends; i++) {
      const auto &thisFriendAlias = friendInfo.fFriendNames[i].second;
      fChain->AddFriend(fFriends[i].get(), thisFriendAlias.c_str());
   }
}

//////////////////////////////////////////////////////////////////////////
/// Get a TTreeReader for the current tree of this view.
std::unique_ptr<TTreeReader>
TTreeView::GetTreeReader(Long64_t start, Long64_t end, const std::vector<std::string> &treeNames,
                         const std::vector<std::string> &fileNames, const ROOT::TreeUtils::RFriendInfo &friendInfo,
                         const TEntryList &entryList, const std::vector<Long64_t> &nEntries,
                         const std::set<std::string> &suppressErrorsForMissingBranches)
{
   const bool hasEntryList = entryList.GetN() > 0;
   const bool usingLocalEntries = friendInfo.fFriendNames.empty() && !hasEntryList;
   const bool needNewChain =
      fChain == nullptr || (usingLocalEntries && (fileNames[0] != fChain->GetListOfFiles()->At(0)->GetTitle() ||
                                                  treeNames[0] != fChain->GetListOfFiles()->At(0)->GetName()));
   if (needNewChain) {
      MakeChain(treeNames, fileNames, friendInfo, nEntries);
      if (hasEntryList) {
         fEntryList = std::make_unique<TEntryList>(entryList);
         if (fEntryList->GetLists() != nullptr) {
            // need to associate the TEntryList to the TChain for the latter to set entry the fTreeNumbers of the
            // sub-lists of the former...
            fChain->SetEntryList(fEntryList.get());
            fEntryList->ResetBit(TObject::kCanDelete); // ...but we want to retain ownership
         }
      }
   }
   auto reader = std::make_unique<TTreeReader>(fChain.get(), fEntryList.get(), /*warnAboutLongerFriends*/ false,
                                               suppressErrorsForMissingBranches);
   reader->SetEntriesRange(start, end);
   return reader;
}

////////////////////////////////////////////////////////////////////////
/// Clear the resources
void TTreeView::Reset()
{
   fChain.reset();
   fEntryList.reset();
   fFriends.clear();
}

} // namespace Internal
} // namespace ROOT

/////////////////////////////////////////////////////////////////////////////////////////////////
/// Retrieve the names of the TTrees in each of the input files, throw if a TTree cannot be found.
std::vector<std::string> TTreeProcessorMT::FindTreeNames()
{
   std::vector<std::string> treeNames;

   if (fFileNames.empty()) // This can never happen
      throw std::runtime_error("Empty list of files and no tree name provided");

   ::TDirectory::TContext ctxt(gDirectory);
   for (const auto &fname : fFileNames) {
      std::string treeName;
      std::unique_ptr<TFile> f(TFile::Open(fname.c_str()));
      TIter next(f->GetListOfKeys());
      while (auto *key = static_cast<TKey *>(next())) {
         const char *className = key->GetClassName();
         if (strcmp(className, "TTree") == 0) {
            treeName = key->GetName();
            break;
         }
      }
      if (treeName.empty())
         throw std::runtime_error("Cannot find any tree in file " + fname);
      treeNames.emplace_back(std::move(treeName));
   }

   return treeNames;
}

////////////////////////////////////////////////////////////////////////
/// Constructor based on a file name.
/// \param[in] filename Name of the file containing the tree to process.
/// \param[in] treename Name of the tree to process. If not provided, the implementation will search
///            for a TTree key in the file and will use the first one it finds.
/// \param[in] nThreads Number of threads to create in the underlying thread-pool. The semantics of this argument are
///                     the same as for TThreadExecutor.
/// \param[in] /// \param[in] globalRange Global entry range to process, {begin (inclusive), end (exclusive)}.
TTreeProcessorMT::TTreeProcessorMT(std::string_view filename, std::string_view treename, UInt_t nThreads,
                                   const EntryRange &globalRange)
   : fFileNames({std::string(filename)}),
     fTreeNames(treename.empty() ? FindTreeNames() : std::vector<std::string>{std::string(treename)}), fFriendInfo(),
     fPool(nThreads), fGlobalRange(globalRange)
{
   ROOT::EnableThreadSafety();
}

std::vector<std::string> CheckAndConvert(const std::vector<std::string_view> &views)
{
   if (views.empty())
      throw std::runtime_error("The provided list of file names is empty");

   std::vector<std::string> strings;
   strings.reserve(views.size());
   for (const auto &v : views)
      strings.emplace_back(v);
   return strings;
}

////////////////////////////////////////////////////////////////////////
/// Constructor based on a collection of file names.
/// \param[in] filenames Collection of the names of the files containing the tree to process.
/// \param[in] treename Name of the tree to process. If not provided, the implementation will
///                     search filenames for a TTree key and will use the first one it finds in each file.
/// \param[in] nThreads Number of threads to create in the underlying thread-pool. The semantics of this argument are
///                     the same as for TThreadExecutor.
/// \param[in] globalRange Global entry range to process, {begin (inclusive), end (exclusive)}.
///
/// If different files contain TTrees with different names and automatic TTree name detection is not an option
/// (for example, because some of the files contain multiple TTrees) please manually create a TChain and pass
/// it to the appropriate TTreeProcessorMT constructor.
TTreeProcessorMT::TTreeProcessorMT(const std::vector<std::string_view> &filenames, std::string_view treename,
                                   UInt_t nThreads, const EntryRange &globalRange)
   : fFileNames(CheckAndConvert(filenames)),
     fTreeNames(treename.empty() ? FindTreeNames()
                                 : std::vector<std::string>(fFileNames.size(), std::string(treename))),
     fFriendInfo(), fPool(nThreads), fGlobalRange(globalRange)
{
   ROOT::EnableThreadSafety();
}

////////////////////////////////////////////////////////////////////////
/// Constructor based on a TTree and a TEntryList.
/// \param[in] tree Tree or chain of files containing the tree to process.
/// \param[in] entries List of entry numbers to process.
/// \param[in] nThreads Number of threads to create in the underlying thread-pool. The semantics of this argument are
///                     the same as for TThreadExecutor.
TTreeProcessorMT::TTreeProcessorMT(TTree &tree, const TEntryList &entries, UInt_t nThreads,
                                   const std::set<std::string> &suppressErrorsForMissingBranches)
   : fFileNames(Internal::TreeUtils::GetFileNamesFromTree(tree)),
     fTreeNames(Internal::TreeUtils::GetTreeFullPaths(tree)),
     fEntryList(entries),
     fFriendInfo(Internal::TreeUtils::GetFriendInfo(tree, /*retrieveEntries*/ true)),
     fPool(nThreads),
     fSuppressErrorsForMissingBranches(suppressErrorsForMissingBranches)
{
   ROOT::EnableThreadSafety();
}

////////////////////////////////////////////////////////////////////////
/// Constructor based on a TTree.
/// \param[in] tree Tree or chain of files containing the tree to process.
/// \param[in] nThreads Number of threads to create in the underlying thread-pool. The semantics of this argument are
///                     the same as for TThreadExecutor.
/// \param[in] globalRange Global entry range to process, {begin (inclusive), end (exclusive)}.
TTreeProcessorMT::TTreeProcessorMT(TTree &tree, UInt_t nThreads, const EntryRange &globalRange,
                                   const std::set<std::string> &suppressErrorsForMissingBranches)
   : fFileNames(Internal::TreeUtils::GetFileNamesFromTree(tree)),
     fTreeNames(Internal::TreeUtils::GetTreeFullPaths(tree)),
     fFriendInfo(Internal::TreeUtils::GetFriendInfo(tree, /*retrieveEntries*/ true)),
     fPool(nThreads),
     fGlobalRange(globalRange),
     fSuppressErrorsForMissingBranches(suppressErrorsForMissingBranches)
{
}

//////////////////////////////////////////////////////////////////////////////
/// Process the entries of a TTree in parallel. The user-provided function
/// receives a TTreeReader which can be used to iterate on a subrange of
/// entries
/// ~~~{.cpp}
/// TTreeProcessorMT::Process([](TTreeReader& readerSubRange) {
///                            // Select branches to read
///                            while (readerSubRange.Next()) {
///                                // Use content of current entry
///                            }
///                         });
/// ~~~
/// The user needs to be aware that each of the subranges can potentially
/// be processed in parallel. This means that the code of the user function
/// should be thread safe.
///
/// \param[in] func User-defined function that processes a subrange of entries
void TTreeProcessorMT::Process(std::function<void(TTreeReader &)> func)
{
   // compute number of tasks per file
   const unsigned int maxTasksPerFile =
      std::ceil(float(GetTasksPerWorkerHint() * fPool.GetPoolSize()) / float(fFileNames.size()));

   // If an entry list or friend trees are present, we need to generate clusters with global entry numbers,
   // so we do it here for all files.
   // Otherwise we can do it later, concurrently for each file, and clusters will contain local entry numbers.
   // TODO: in practice we could also find clusters per-file in the case of no friends and a TEntryList with
   // sub-entrylists.
   const bool hasFriends = !fFriendInfo.fFriendNames.empty();
   const bool hasEntryList = fEntryList.GetN() > 0;
   const bool shouldRetrieveAllClusters = hasFriends || hasEntryList || fGlobalRange.first > 0 ||
                                          fGlobalRange.second != std::numeric_limits<Long64_t>::max();
   if (!shouldRetrieveAllClusters) {
      ProcessFilesWithLocalClusters(func, maxTasksPerFile);
      return;
   }

   auto [allClusters, allEntries] = MakeClusters(fTreeNames, fFileNames, maxTasksPerFile, fPool, fGlobalRange);
   // The (global) entry ranges of the tasks, for the TTreeCache: with an entry list, the tasks are ranges of entry
   // list positions instead, one per cluster
   auto treeRanges = allClusters;
   if (hasEntryList)
      allClusters = ConvertToElistClusters(std::move(allClusters), fEntryList, fTreeNames, fFileNames, allEntries);
   ProcessFilesWithGlobalClusters(func, allClusters, treeRanges, allEntries);
}

////////////////////////////////////////////////////////////////////////
/// Process the files in the case with friends, an entry list or a global entry range, in which the tasks of all
/// files (with global entry numbers, or entry list positions) were determined before.
///
/// As for ProcessFilesWithLocalClusters(), one worker loop per thread gets tasks from an RFileTaskScheduler, which
/// keeps workers on files: each worker processes its tasks with its own TChain over all files (with the friends and
/// the entry list), which thus switches files only when the worker moves to another file.
void TTreeProcessorMT::ProcessFilesWithGlobalClusters(
   const std::function<void(TTreeReader &)> &func, const std::vector<std::vector<std::pair<Long64_t, Long64_t>>> &tasks,
   const std::vector<std::vector<std::pair<Long64_t, Long64_t>>> &treeRanges, const std::vector<Long64_t> &entries)
{
   struct RNoFileData {
   };
   using Scheduler_t = ROOT::Internal::RFileTaskScheduler<RNoFileData>;
   const int nTasksAhead = GetNTasksAhead();
   // tasks has an element per file up to the end of the global entry range
   Scheduler_t scheduler(tasks.size(), fPool.GetPoolSize(), GetNTasksReserved(nTasksAhead));
   for (std::size_t f = 0; f < tasks.size(); ++f)
      scheduler.PublishInAdvance(f, tasks[f].size());
   const bool hasEntryList = fEntryList.GetN() > 0;

   auto workerLoop = [&](unsigned int) {
      Scheduler_t::RWorker worker;
      ROOT::Internal::TTreeView view;
      RCacheWindow cacheWindow(nTasksAhead);
      try {
         while (auto assignment = scheduler.Next(worker)) {
            R__ASSERT(assignment->fKind == Scheduler_t::EKind::kProcessTask);
            if (assignment->fLeftFile)
               cacheWindow.Reset();
            const auto &range = tasks[assignment->fFileIdx][assignment->fTaskIdx];
            auto reader = view.GetTreeReader(range.first, range.second, fTreeNames, fFileNames, fFriendInfo,
                                             fEntryList, entries, fSuppressErrorsForMissingBranches);
            // With an entry list, a task (cluster) may have no entries to process
            cacheWindow.ConfigureReader(*reader, treeRanges[assignment->fFileIdx], assignment->fTaskIdx,
                                        assignment->fRunEnd, hasEntryList && range.first == range.second);
            func(*reader);
         }
      } catch (...) {
         scheduler.Abort();
         throw;
      }
      // the TChain and TFiles of the view are not globally tracked, they are cleaned up with the view
   };

   fPool.Foreach(workerLoop, ROOT::TSeq<unsigned int>(fPool.GetPoolSize()));
}

////////////////////////////////////////////////////////////////////////
/// Process the files in the case without friends, entry lists and global entry range, in which the clusters of each
/// file can be determined when the file is first opened, with entry numbers local to the file.
///
/// One worker loop per thread of the pool gets tasks from an RFileTaskScheduler: a worker opens a file, determines its
/// clusters and processes the corresponding tasks with the same TFile and TTree. Other workers join a file (with their
/// own TFile and TTree) only when there are no unstarted files left. Each worker keeps one file open at a time, so
/// that every file is opened once, plus once for every worker joining it at the end of the processing.
void TTreeProcessorMT::ProcessFilesWithLocalClusters(const std::function<void(TTreeReader &)> &func,
                                                     unsigned int maxTasksPerFile)
{
   struct RFileTasks {
      std::vector<EntryRange> fRanges;
   };
   using Scheduler_t = ROOT::Internal::RFileTaskScheduler<RFileTasks>;
   const int nTasksAhead = GetNTasksAhead();
   Scheduler_t scheduler(fFileNames.size(), fPool.GetPoolSize(), GetNTasksReserved(nTasksAhead));

   auto workerLoop = [&](unsigned int) {
      Scheduler_t::RWorker worker;
      // The file and tree the worker processes; the tree is owned by the file
      std::unique_ptr<TFile> file;
      TTree *tree = nullptr;
      std::shared_ptr<RFileTasks> tasks;
      RCacheWindow cacheWindow(nTasksAhead);
      try {
         while (auto assignment = scheduler.Next(worker)) {
            if (assignment->fLeftFile) {
               tree = nullptr;
               file.reset();
               tasks.reset();
               cacheWindow.Reset();
            }
            const auto &fileName = fFileNames[assignment->fFileIdx];
            const auto &treeName = fTreeNames[assignment->fFileIdx];
            if (assignment->fKind == Scheduler_t::EKind::kOpenFile) {
               std::tie(file, tree) = OpenTreeForProcessing(fileName, treeName);
               tasks = std::make_shared<RFileTasks>(RFileTasks{FuseClusters(GetClusterRanges(*tree), maxTasksPerFile)});
               scheduler.Publish(worker, tasks->fRanges.size(), tasks);
               continue;
            }
            if (assignment->fFileData) {
               // Joining a file opened by another worker
               tasks = std::move(assignment->fFileData);
               std::tie(file, tree) = OpenTreeForProcessing(fileName, treeName);
            }
            const auto &range = tasks->fRanges[assignment->fTaskIdx];
            TTreeReader reader(tree, nullptr, /*warnAboutLongerFriends*/ false, fSuppressErrorsForMissingBranches);
            reader.SetEntriesRange(range.first, range.second);
            cacheWindow.ConfigureReader(reader, tasks->fRanges, assignment->fTaskIdx, assignment->fRunEnd);
            func(reader);
         }
      } catch (...) {
         scheduler.Abort();
         throw;
      }
   };

   fPool.Foreach(workerLoop, ROOT::TSeq<unsigned int>(fPool.GetPoolSize()));
}

////////////////////////////////////////////////////////////////////////
/// \brief Retrieve the current value for the desired number of tasks per worker.
/// \return The desired number of tasks to be created per worker. TTreeProcessorMT uses this value as an hint.
unsigned int TTreeProcessorMT::GetTasksPerWorkerHint()
{
   return fgTasksPerWorkerHint;
}

////////////////////////////////////////////////////////////////////////
/// \brief Set the hint for the desired number of tasks created per worker.
/// \param[in] tasksPerWorkerHint Desired number of tasks per worker.
///
/// This allows to create a reasonable number of tasks even if any of the
/// processed files features a bad clustering, for example with a lot of
/// entries and just a few entries per cluster, or to limit the number of
/// tasks spawned when a very large number of files and workers is used.
void TTreeProcessorMT::SetTasksPerWorkerHint(unsigned int tasksPerWorkerHint)
{
   fgTasksPerWorkerHint = tasksPerWorkerHint;
}
