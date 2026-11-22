// Author: Josh Bendavid 2026-11-22

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RDF_RFILEERRORPOLICY
#define ROOT_RDF_RFILEERRORPOLICY

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace ROOT {
class RDataFrame;
namespace Detail {
namespace RDF {
class RNodeBase;
}
} // namespace Detail
namespace RDF {
template <typename Proxied>
class RInterface;
using RNode = RInterface<::ROOT::Detail::RDF::RNodeBase>;

namespace Experimental {

/// What the event loops of an RDataFrame do if one of their input files cannot be opened (after the retries of
/// transient errors, see the rootrc setting IO.TransientErrorRetries). By default, they stop with an error.
///
/// If fSkipFiles is set, the event loops over TTrees and RNTuples skip the files that cannot be opened, with a
/// warning, and process the other files. The skipped files of the last event loop are returned by GetSkippedFiles(),
/// all its input files (processed or skipped) by GetFileReport(). Note that the entries of the skipped files are not
/// processed: e.g. normalizations derived from the processed entries do not include them.
///
/// Limitations:
///  - With friend trees, entry lists or a global entry range (RDatasetSpec::WithGlobalRange()), the event loops still
///    stop with an error: skipping files would change the global entry numbers.
///  - The first file of the dataset is opened when constructing the RDataFrame (to determine the format of the dataset
///    and its columns): it needs to be readable.
///  - In multi-threaded event loops, the files are only skipped if they cannot be opened by the worker thread that
///    opens them first. If they cannot be opened by other worker threads that process some of their entries later, the
///    event loops stop with an error.
struct RFileErrorPolicy {
   /// Skip the files that cannot be opened, rather than stopping the event loop with an error
   bool fSkipFiles = false;
   /// The event loop stops with an error if more files than this cannot be opened
   std::size_t fMaxSkippedFiles = std::numeric_limits<std::size_t>::max();
};

/// An input file that was skipped by an event loop because it could not be opened (see RFileErrorPolicy)
struct RSkippedFile {
   std::string fFileName; ///< The name of the file
   std::string fError;    ///< The error that occurred when opening the file
};

/// The status of an input file of an event loop, see GetFileReport()
struct RFileStatus {
   std::string fFileName; ///< The name of the file, as in the input of the RDataFrame (after the expansion of globs)
   bool fSkipped = false; ///< Whether the file was skipped because it could not be opened (see RFileErrorPolicy)
   std::string fError;    ///< If the file was skipped: the error that occurred when opening it
};

/// Set what the event loops of the computation graph of `node` do if one of their input files cannot be opened
void SetFileErrorPolicy(ROOT::RDF::RNode node, const RFileErrorPolicy &policy);
/// Set what the event loops of `df` do if one of their input files cannot be opened
void SetFileErrorPolicy(ROOT::RDataFrame df, const RFileErrorPolicy &policy);

/// The input files that the last event loop of the computation graph of `node` skipped, see RFileErrorPolicy
std::vector<RSkippedFile> GetSkippedFiles(ROOT::RDF::RNode node);
/// The input files that the last event loop of `df` skipped, see RFileErrorPolicy
std::vector<RSkippedFile> GetSkippedFiles(ROOT::RDataFrame df);

/// The input files of the last event loop of the computation graph of `node`, in the order of the input: the files that
/// were processed, and those that were skipped because they could not be opened (see RFileErrorPolicy). If the event
/// loop completed (without an exception), the files that were not skipped were processed completely. With a global
/// entry range, the files outside of the range are not listed.
std::vector<RFileStatus> GetFileReport(ROOT::RDF::RNode node);
/// The input files of the last event loop of `df`, see GetFileReport(ROOT::RDF::RNode)
std::vector<RFileStatus> GetFileReport(ROOT::RDataFrame df);

} // namespace Experimental
} // namespace RDF
} // namespace ROOT

#endif
