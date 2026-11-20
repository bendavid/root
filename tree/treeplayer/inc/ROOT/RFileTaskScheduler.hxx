// Author: Josh Bendavid, 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RFileTaskScheduler
#define ROOT_RFileTaskScheduler

#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ROOT {
namespace Internal {

/**
\class ROOT::Internal::RFileTaskScheduler
\ingroup tree
\brief Distributes the tasks of a dataset made of several files to a fixed set of worker loops, keeping workers on files.

Used by the multi-threaded event loops over TTrees and RNTuples. Each worker (thread) runs a loop requesting assignments
with Next() until there is no work left. The tasks of a file (typically ranges of clusters) are only known once the
file is open, so a worker is first assigned to open a file and to publish its tasks with Publish(). It then processes
them in order, from the front. A worker that has no tasks left takes over work in this order of preference:
 1. an unstarted file: while there are more files than workers, each worker processes its own files, so that every
    file is opened once and as many different files as there are workers are read in parallel;
 2. the back half of the remaining tasks of another worker on the same file (no new file handle needed);
 3. the back half of the largest remaining run of tasks of any other file (needs a new handle to that file). While
    other files are still being opened, a worker only joins another file if the tasks it takes are at least its fair
    share of the remaining work (estimated from the files published so far); otherwise it waits for these files.
    This spreads the workers over the files when there are fewer files than workers, rather than all idle workers
    joining the first file that is published and moving on again shortly after.
Thus a worker only needs a handle to one file at a time, it leaves a file only when it has no tasks left there and it
never returns to a file it left. Leaving a file is reported in the next assignment, so that the handle can be released.
Workers that find no work wait while files are being opened by other workers, since these may publish work.

Optionally, the first few remaining tasks of a run are reserved for its worker: if the worker prefetches the data of
its next task(s) while processing the current one, taking these tasks over would make the data be read twice.

The data published for a file (FileData_t, e.g. the tasks and a prototype of the file handle) is passed to every worker
that joins the file; the scheduler releases its reference once all the tasks of the file are assigned.

If a worker loop fails, it must call Abort() so that the other worker loops terminate.
*/
template <typename FileData_t>
class RFileTaskScheduler {
public:
   static constexpr std::size_t kInvalid = std::numeric_limits<std::size_t>::max();

   enum class EKind {
      kOpenFile,   ///< Open the file and publish its tasks
      kProcessTask ///< Process a task of a file
   };

   struct RAssignment {
      EKind fKind = EKind::kProcessTask;
      std::size_t fFileIdx = kInvalid;
      /// kProcessTask: index of the task in the file
      std::size_t fTaskIdx = kInvalid;
      /// kProcessTask: end (exclusive) of the run of consecutive tasks the worker has in this file at the time of the
      /// assignment. The run can shrink later, if other workers take over some of its tasks. Can be used to look ahead,
      /// e.g. to prefetch data.
      std::size_t fRunEnd = kInvalid;
      /// The worker left the file it was on before: its handle to that file can be released
      bool fLeftFile = false;
      /// kProcessTask on a file the worker did not open itself and is new on: the data published for the file
      std::shared_ptr<FileData_t> fFileData;
   };

   /// The scheduling state of a worker, owned by the worker loop
   class RWorker {
      friend class RFileTaskScheduler;
      std::size_t fFileIdx = kInvalid;
      std::size_t fRunIdx = kInvalid;
   };

private:
   /// A run of consecutive tasks [fNext, fEnd) of a file assigned to one worker
   struct RRun {
      std::size_t fNext;
      std::size_t fEnd;
      std::size_t Size() const { return fEnd - fNext; }
   };
   struct RFileState {
      std::vector<RRun> fRuns;
      std::shared_ptr<FileData_t> fData;
      std::size_t fNUnassigned = 0;
   };

   std::mutex fMutex;
   std::condition_variable fCvWork;
   std::vector<RFileState> fFiles;
   std::size_t fNWorkers = 0; ///< Number of workers, for the fair share; 0 if unknown
   std::size_t fNReserved = 0; ///< Number of next tasks of a run that other workers do not take over
   std::size_t fNextUnstarted = 0;
   std::size_t fNOpening = 0;      ///< Number of files being opened, i.e. assigned but not published yet
   std::size_t fNPublished = 0;    ///< Number of files published
   std::size_t fNTasksPublished = 0;
   std::size_t fNUnassigned = 0;   ///< Number of tasks published but not assigned yet, in all files
   bool fAborted = false;

   /// Whether a worker should take nTake tasks of another file rather than wait for the files being opened
   bool IsWorthJoining(std::size_t nTake) const
   {
      if (fNOpening == 0 || fNWorkers == 0 || fNPublished == 0)
         return true;
      const double nExpected =
         fNUnassigned + fNOpening * (static_cast<double>(fNTasksPublished) / static_cast<double>(fNPublished));
      return static_cast<double>(nTake) >= nExpected / static_cast<double>(fNWorkers);
   }

   /// Number of tasks of a run that other workers can take over
   std::size_t GetNStealable(const RRun &run) const { return run.Size() > fNReserved ? run.Size() - fNReserved : 0; }

   /// Index of the run of the file with the most tasks that can be taken over, kInvalid if there are none
   std::size_t FindLargestRun(std::size_t fileIdx) const
   {
      std::size_t best = kInvalid;
      std::size_t bestSize = 0;
      const auto &runs = fFiles[fileIdx].fRuns;
      for (std::size_t i = 0; i < runs.size(); ++i) {
         if (GetNStealable(runs[i]) > bestSize) {
            bestSize = GetNStealable(runs[i]);
            best = i;
         }
      }
      return best;
   }

   /// Move the back half (rounded up) of the tasks of the given run that can be taken over to a new run of the worker
   void Split(RWorker &worker, std::size_t fileIdx, std::size_t runIdx)
   {
      auto &runs = fFiles[fileIdx].fRuns;
      const auto nTake = (GetNStealable(runs[runIdx]) + 1) / 2;
      const RRun newRun{runs[runIdx].fEnd - nTake, runs[runIdx].fEnd};
      runs[runIdx].fEnd -= nTake;
      runs.push_back(newRun);
      worker.fFileIdx = fileIdx;
      worker.fRunIdx = runs.size() - 1;
   }

   /// Assign the next task of the current run of the worker
   RAssignment Take(RWorker &worker, bool leftFile, bool newOnFile)
   {
      auto &file = fFiles[worker.fFileIdx];
      auto &run = file.fRuns[worker.fRunIdx];
      RAssignment a;
      a.fKind = EKind::kProcessTask;
      a.fFileIdx = worker.fFileIdx;
      a.fTaskIdx = run.fNext++;
      a.fRunEnd = run.fEnd;
      a.fLeftFile = leftFile;
      if (newOnFile)
         a.fFileData = file.fData;
      --fNUnassigned;
      if (--file.fNUnassigned == 0)
         file.fData.reset();
      return a;
   }

public:
   /// \param nFiles The number of files
   /// \param nWorkers The number of worker loops (0 if unknown), used to estimate their fair share of the work
   /// \param nReserved The number of next tasks of a run that are reserved for its worker
   explicit RFileTaskScheduler(std::size_t nFiles, std::size_t nWorkers = 0, std::size_t nReserved = 0)
      : fFiles(nFiles), fNWorkers(nWorkers), fNReserved(nReserved)
   {
   }
   RFileTaskScheduler(const RFileTaskScheduler &) = delete;
   RFileTaskScheduler &operator=(const RFileTaskScheduler &) = delete;

   /// Get the next assignment for the worker; blocks while there is no work available now but some may still be
   /// published. Returns std::nullopt if there is no work left (or if processing was aborted).
   std::optional<RAssignment> Next(RWorker &worker)
   {
      std::unique_lock<std::mutex> lock(fMutex);
      while (true) {
         if (fAborted)
            return std::nullopt;

         const bool onFile = worker.fFileIdx != kInvalid;

         // 1. The next task of the run of the worker
         if (onFile && worker.fRunIdx != kInvalid && fFiles[worker.fFileIdx].fRuns[worker.fRunIdx].Size() > 0)
            return Take(worker, /*leftFile=*/false, /*newOnFile=*/false);

         // 2. An unstarted file
         if (fNextUnstarted < fFiles.size()) {
            RAssignment a;
            a.fKind = EKind::kOpenFile;
            a.fFileIdx = fNextUnstarted++;
            a.fLeftFile = onFile;
            worker.fFileIdx = a.fFileIdx;
            worker.fRunIdx = kInvalid;
            ++fNOpening;
            return a;
         }

         // 3. Remaining tasks of the same file
         if (onFile) {
            const auto runIdx = FindLargestRun(worker.fFileIdx);
            if (runIdx != kInvalid) {
               Split(worker, worker.fFileIdx, runIdx);
               return Take(worker, /*leftFile=*/false, /*newOnFile=*/false);
            }
         }

         // 4. The largest remaining run of tasks of any other file
         std::size_t bestFile = kInvalid;
         std::size_t bestRun = kInvalid;
         std::size_t bestSize = 0;
         for (std::size_t f = 0; f < fFiles.size(); ++f) {
            if (f == worker.fFileIdx || fFiles[f].fNUnassigned == 0)
               continue;
            const auto runIdx = FindLargestRun(f);
            if (runIdx != kInvalid && GetNStealable(fFiles[f].fRuns[runIdx]) > bestSize) {
               bestSize = GetNStealable(fFiles[f].fRuns[runIdx]);
               bestFile = f;
               bestRun = runIdx;
            }
         }
         if (bestFile != kInvalid && IsWorthJoining((bestSize + 1) / 2)) {
            Split(worker, bestFile, bestRun);
            return Take(worker, /*leftFile=*/onFile, /*newOnFile=*/true);
         }

         if (fNOpening == 0)
            return std::nullopt;
         fCvWork.wait(lock);
      }
   }

   /// Publish the tasks of the file that the worker was assigned to open; the worker gets them as its run.
   void Publish(RWorker &worker, std::size_t nTasks, std::shared_ptr<FileData_t> data)
   {
      {
         std::lock_guard<std::mutex> lock(fMutex);
         --fNOpening;
         ++fNPublished;
         fNTasksPublished += nTasks;
         fNUnassigned += nTasks;
         if (nTasks > 0) {
            auto &file = fFiles[worker.fFileIdx];
            file.fRuns.push_back(RRun{0, nTasks});
            file.fNUnassigned = nTasks;
            file.fData = std::move(data);
            worker.fRunIdx = file.fRuns.size() - 1;
         }
      }
      fCvWork.notify_all();
   }

   /// Stop handing out work, e.g. after an error in one of the worker loops
   void Abort()
   {
      {
         std::lock_guard<std::mutex> lock(fMutex);
         fAborted = true;
      }
      fCvWork.notify_all();
   }
};

} // namespace Internal
} // namespace ROOT

#endif
