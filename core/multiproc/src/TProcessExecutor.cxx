/* @(#)root/multiproc:$Id$ */
// Author: Enrico Guiraud July 2015
// Modified: G Ganis Jan 2017

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "TEnv.h"
#include "ROOT/TProcessExecutor.hxx"

//////////////////////////////////////////////////////////////////////////
///
/// \class TProcessExecutor
/// \brief This class provides a simple interface to execute the same task
/// multiple times in parallel, possibly with different arguments every
/// time. This mimics the behaviour of python's pool.Map method.
///
/// ###TProcessExecutor::Map
/// The two possible usages of the Map method are:\n
/// * Map(F func, unsigned nTimes): func is executed nTimes with no arguments
/// * Map(F func, T& args): func is executed on each element of the collection of arguments args
///
/// For either signature, func is executed as many times as needed by a pool of
/// fNWorkers workers; the number of workers can be passed to the constructor
/// or set via SetNWorkers. It defaults to the number of cores.\n
/// A collection containing the result of each execution is returned.\n
/// **Note:** the user is responsible for the deletion of any object that might
/// be created upon execution of func, returned objects included: TProcessExecutor never
/// deletes what it returns, it simply forgets it.\n
/// **Note:** that the usage of TProcessExecutor::Map is indicated only when the task to be
/// executed takes more than a few seconds, otherwise the overhead introduced
/// by Map will outrun the benefits of parallel execution on most machines.
///
/// \param func
/// \parblock
/// a lambda expression, an std::function, a loaded macro, a
/// functor class or a function that takes zero arguments (for the first signature)
/// or one (for the second signature).
/// \endparblock
/// \param args
/// \parblock
/// a standard container (vector, list, deque), an initializer list
/// or a pointer to a TCollection (TList*, TObjArray*, ...).
/// \endparblock
/// **Note:** the version of TProcessExecutor::Map that takes a TCollection* as argument incurs
/// in the overhead of copying data from the TCollection to an STL container. Only
/// use it when absolutely necessary.\n
/// **Note:** in cases where the function to be executed takes more than
/// zero/one argument but all are fixed except zero/one, the function can be wrapped
/// in a lambda or via std::bind to give it the right signature.\n
/// **Note:** the user should take care of initializing random seeds differently in each
/// process (e.g. using the process id in the seed). Otherwise several parallel executions
/// might generate the same sequence of pseudo-random numbers.
///
/// #### Return value:
/// If T derives from TCollection Map returns a TObjArray, otherwise it
/// returns an std::vector. In both cases, the elements in the container
/// will be the objects returned by func.
///
///
/// #### Examples:
///
/// ~~~{.cpp}
/// root[] TProcessExecutor pool; auto hists = pool.Map(CreateHisto, 10);
/// root[] TProcessExecutor pool(2); auto squares = pool.Map([](int a) { return a*a; }, {1,2,3});
/// ~~~
///
/// ###TProcessExecutor::MapReduce
/// This set of methods behaves exactly like Map, but takes an additional
/// function as a third argument. This function is applied to the set of
/// objects returned by the corresponding Map execution to "squash" them
/// to a single object.
///
/// ####Examples:
/// ~~~{.cpp}
/// root[] TProcessExecutor pool; auto ten = pool.MapReduce([]() { return 1; }, 10, [](std::vector<int> v) { return std::accumulate(v.begin(), v.end(), 0); })
/// root[] TProcessExecutor pool; auto hist = pool.MapReduce(CreateAndFillHists, 10, PoolUtils::ReduceObjects);
/// ~~~
///
//////////////////////////////////////////////////////////////////////////

namespace ROOT {
//////////////////////////////////////////////////////////////////////////
/// Class constructor.
/// nWorkers is the number of times this ROOT session will be forked, i.e.
/// the number of workers that will be spawned.
TProcessExecutor::TProcessExecutor(unsigned nWorkers) : TMPClient(nWorkers)
{
   Reset();
}

//////////////////////////////////////////////////////////////////////////
/// Reset TProcessExecutor's state.
void TProcessExecutor::Reset()
{
   fNProcessed = 0;
   fNToProcess = 0;
   fTaskType = ETask::kNoTask;
}

//////////////////////////////////////////////////////////////////////////
/// Reply to a worker who just sent a result.
/// If another argument to process exists, tell the worker. Otherwise
/// send a shutdown order.
void TProcessExecutor::ReplyToFuncResult(TSocket *s)
{
   if (fNProcessed < fNToProcess) {
      //this cannot be a "greedy worker" task
      if (fTaskType == ETask::kMap)
         MPSend(s, MPCode::kExecFunc);
      else if (fTaskType == ETask::kMapWithArg)
         MPSend(s, MPCode::kExecFuncWithArg, fNProcessed);
      ++fNProcessed;
   } else //whatever the task is, we are done
      MPSend(s, MPCode::kShutdownOrder);
}


//////////////////////////////////////////////////////////////////////////
/// Reply to a worker who is idle.
/// If another argument to process exists, tell the worker. Otherwise
/// ask for a result
void TProcessExecutor::ReplyToIdle(TSocket *s)
{
   if (fNProcessed < fNToProcess) {
      //we are executing a "greedy worker" task
      if (fTaskType == ETask::kMapWithArg)
         MPSend(s, MPCode::kExecFuncWithArg, fNProcessed);
      else if (fTaskType == ETask::kMap)
         MPSend(s, MPCode::kExecFunc);
      else if (fTaskType == ETask::kProcByRange)
         MPSend(s, MPCode::kProcRange, fNProcessed);
      else if (fTaskType == ETask::kProcByFile)
         MPSend(s, MPCode::kProcFile, fNProcessed);
      ++fNProcessed;
   } else
      MPSend(s, MPCode::kSendResult);
}

} // namespace ROOT
