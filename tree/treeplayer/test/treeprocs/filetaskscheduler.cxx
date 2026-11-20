#include "ROOT/RFileTaskScheduler.hxx"

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using ROOT::Internal::RFileTaskScheduler;

namespace {

struct RFileData {
   std::size_t fFileIdx;
};

struct RResult {
   /// Number of times each task was processed
   std::vector<std::vector<int>> fNProcessed;
   /// Number of handles opened per file (by the opener or by joining workers)
   std::vector<int> fNHandles;
   /// Number of times a worker had a handle to more than one file
   int fNViolations = 0;
};

/// Run the scheduler with nWorkers threads on files with the given number of tasks
RResult RunScheduler(const std::vector<std::size_t> &nTasksPerFile, unsigned nWorkers,
                     std::chrono::microseconds taskDuration = std::chrono::microseconds(50),
                     std::chrono::microseconds openDuration = std::chrono::microseconds(0))
{
   using Scheduler_t = RFileTaskScheduler<RFileData>;
   Scheduler_t scheduler(nTasksPerFile.size(), nWorkers);
   RResult result;
   result.fNProcessed.resize(nTasksPerFile.size());
   for (std::size_t f = 0; f < nTasksPerFile.size(); ++f)
      result.fNProcessed[f].resize(nTasksPerFile[f], 0);
   result.fNHandles.resize(nTasksPerFile.size(), 0);
   std::mutex mutex;

   auto workerLoop = [&]() {
      Scheduler_t::RWorker worker;
      std::size_t handle = Scheduler_t::kInvalid; // the file the worker has a handle to
      while (auto a = scheduler.Next(worker)) {
         std::lock_guard<std::mutex> lock(mutex);
         if (a->fLeftFile)
            handle = Scheduler_t::kInvalid;
         if (a->fKind == Scheduler_t::EKind::kOpenFile) {
            if (handle != Scheduler_t::kInvalid)
               result.fNViolations++;
            handle = a->fFileIdx;
            result.fNHandles[a->fFileIdx]++;
            if (openDuration.count() > 0) {
               // later files take longer to open
               mutex.unlock();
               std::this_thread::sleep_for(openDuration * (1 + a->fFileIdx));
               mutex.lock();
            }
            scheduler.Publish(worker, nTasksPerFile[a->fFileIdx],
                              std::make_shared<RFileData>(RFileData{a->fFileIdx}));
            continue;
         }
         if (a->fFileData) {
            // joining a file: the published data is passed along
            EXPECT_EQ(a->fFileIdx, a->fFileData->fFileIdx);
            if (handle != Scheduler_t::kInvalid)
               result.fNViolations++;
            handle = a->fFileIdx;
            result.fNHandles[a->fFileIdx]++;
         }
         if (handle != a->fFileIdx)
            result.fNViolations++;
         EXPECT_LT(a->fTaskIdx, a->fRunEnd);
         EXPECT_LE(a->fRunEnd, nTasksPerFile[a->fFileIdx]);
         result.fNProcessed[a->fFileIdx][a->fTaskIdx]++;
         mutex.unlock();
         std::this_thread::sleep_for(taskDuration);
         mutex.lock();
      }
   };
   std::vector<std::thread> threads;
   for (unsigned i = 0; i < nWorkers; ++i)
      threads.emplace_back(workerLoop);
   for (auto &t : threads)
      t.join();
   return result;
}

void CheckAllProcessedOnce(const RResult &result)
{
   for (const auto &file : result.fNProcessed)
      for (auto n : file)
         EXPECT_EQ(1, n);
   EXPECT_EQ(0, result.fNViolations);
}

} // anonymous namespace

TEST(RFileTaskScheduler, NoFiles)
{
   auto result = RunScheduler({}, 4);
   EXPECT_TRUE(result.fNHandles.empty());
}

TEST(RFileTaskScheduler, OneWorker)
{
   // A single worker opens each file once and processes the tasks in order
   const std::vector<std::size_t> nTasks{3, 0, 5, 1};
   auto result = RunScheduler(nTasks, 1);
   CheckAllProcessedOnce(result);
   for (auto n : result.fNHandles)
      EXPECT_EQ(1, n);
}

TEST(RFileTaskScheduler, ManyFiles)
{
   // With many more files than workers, almost all files are processed by one worker only
   const std::vector<std::size_t> nTasks(64, 10);
   auto result = RunScheduler(nTasks, 4);
   CheckAllProcessedOnce(result);
   int nHandles = 0;
   for (auto n : result.fNHandles) {
      EXPECT_GE(n, 1);
      nHandles += n;
   }
   // Only the tail of the processing (the last files) is shared by several workers
   EXPECT_LE(nHandles, 64 + 4 * 4);
}

TEST(RFileTaskScheduler, FewFiles)
{
   // With fewer files than workers, the workers spread over the files; each worker has a handle to one file at a time
   const std::vector<std::size_t> nTasks{40, 3, 0, 17};
   auto result = RunScheduler(nTasks, 16);
   CheckAllProcessedOnce(result);
   for (std::size_t f = 0; f < nTasks.size(); ++f) {
      EXPECT_GE(result.fNHandles[f], 1);
      EXPECT_LE(result.fNHandles[f], 1 + static_cast<int>(nTasks[f]));
   }
}

TEST(RFileTaskScheduler, Abort)
{
   using Scheduler_t = RFileTaskScheduler<RFileData>;
   Scheduler_t scheduler(8);
   std::atomic<int> nProcessed{0};
   std::vector<std::thread> threads;
   for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&, i]() {
         Scheduler_t::RWorker worker;
         while (auto a = scheduler.Next(worker)) {
            if (a->fKind == Scheduler_t::EKind::kOpenFile) {
               if (i == 0) {
                  // This worker fails to open its file: without Abort() the others would wait for it forever
                  scheduler.Abort();
                  return;
               }
               std::this_thread::sleep_for(std::chrono::milliseconds(10));
               scheduler.Publish(worker, 1000, std::make_shared<RFileData>(RFileData{a->fFileIdx}));
               continue;
            }
            nProcessed++;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
         }
      });
   }
   for (auto &t : threads)
      t.join();
   EXPECT_LT(nProcessed.load(), 8 * 1000);
   Scheduler_t::RWorker worker;
   EXPECT_FALSE(scheduler.Next(worker).has_value());
}

TEST(RFileTaskScheduler, FairShare)
{
   // With fewer files than workers, the workers spread over the files as they are published, rather than all joining
   // the first published file and moving on to the next ones: most workers have a handle to only one file
   const std::vector<std::size_t> nTasks(4, 62);
   // (the processing of a file takes longer than the differences in the time to open the files, as in practice)
   auto result = RunScheduler(nTasks, 64, std::chrono::milliseconds(4), std::chrono::milliseconds(1));
   CheckAllProcessedOnce(result);
   int nHandles = 0;
   for (auto n : result.fNHandles)
      nHandles += n;
   EXPECT_LE(nHandles, 64 + 32);
}

TEST(RFileTaskScheduler, ReservedTasks)
{
   // The next task of a run is reserved for its worker: another worker never takes over the task following the one
   // the owner is processing
   using Scheduler_t = RFileTaskScheduler<RFileData>;
   Scheduler_t scheduler(1, 2, /*nReserved=*/1);
   Scheduler_t::RWorker w1, w2;
   auto a = scheduler.Next(w1);
   ASSERT_TRUE(a && a->fKind == Scheduler_t::EKind::kOpenFile);
   scheduler.Publish(w1, 2, std::make_shared<RFileData>(RFileData{0}));
   a = scheduler.Next(w1);
   ASSERT_TRUE(a);
   EXPECT_EQ(0u, a->fTaskIdx);
   // Task 1 is reserved for w1: nothing for w2, which terminates since no file is being opened
   EXPECT_FALSE(scheduler.Next(w2).has_value());
   a = scheduler.Next(w1);
   ASSERT_TRUE(a);
   EXPECT_EQ(1u, a->fTaskIdx);
   EXPECT_FALSE(scheduler.Next(w1).has_value());

   // With more tasks, the back half of the remaining tasks after the reserved one is taken over
   Scheduler_t scheduler2(1, 2, /*nReserved=*/1);
   Scheduler_t::RWorker w3, w4;
   a = scheduler2.Next(w3);
   scheduler2.Publish(w3, 6, std::make_shared<RFileData>(RFileData{0}));
   a = scheduler2.Next(w3); // task 0; remaining 1-5, 1 reserved, 2-5 can be taken over
   ASSERT_TRUE(a);
   EXPECT_EQ(0u, a->fTaskIdx);
   a = scheduler2.Next(w4);
   ASSERT_TRUE(a);
   EXPECT_EQ(4u, a->fTaskIdx);
   EXPECT_EQ(6u, a->fRunEnd);
   std::set<std::size_t> seen{0, 4};
   while (auto b = scheduler2.Next(w3))
      seen.insert(b->fTaskIdx);
   while (auto b = scheduler2.Next(w4))
      seen.insert(b->fTaskIdx);
   EXPECT_EQ(6u, seen.size());
}
