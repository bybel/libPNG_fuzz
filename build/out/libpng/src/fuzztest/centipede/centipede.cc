// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Centipede: an experimental distributed fuzzing engine.
// Very simple / naive so far.
// Main use case: large out-of-process fuzz targets with relatively slow
// execution (< 100 exec/s).
//
// Basic approach (subject to change):
// * All state is stored in a local or remote directory `workdir`.
// * State consists of a corpus (inputs) and feature sets (see feature_t).
// * Feature sets are associated with a binary, so that two binaries
//   have independent feature sets stored in different subdirs in `workdir`,
//   like binaryA-sha1-of-A and binaryB-sha1-of-B.
//   If the binary is recompiled at different revision or with different
//   compiler options, it is a different binary and feature sets will need to be
//   recomputed for the new binary in its separate dir.
// * The corpus is not tied to the binary. It is stored in `workdir`/.
// * The fuzzer runs in `total_shards` independent processes.
// * Each shard appends data to its own files in `workdir`: corpus and features;
//   no other process writes to those files.
// * Each shard may periodically read some other shard's corpus and features.
//   Since all files are append-only (no renames, no deletions) we may only
//   have partial reads, and the algorithm is expected to tolerate those.
// * Fuzzing can be run locally in multiple processes, with a local `workdir`
//   or on a cluster, which supports `workdir` on a remote file system.
// * The intent is to scale to an arbitrary number of shards,
//   currently tested with total_shards = 10000.
//
//  Differential fuzzing is not yet properly implemented.
//  Currently, one can run target A in a given workdir, then target B, and so
//  on, and the corpus will grow over time benefiting from all targets.
#include "./centipede/centipede.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "./centipede/blob_file.h"
#include "./centipede/control_flow.h"
#include "./centipede/coverage.h"
#include "./centipede/defs.h"
#include "./centipede/environment.h"
#include "./centipede/execution_result.h"
#include "./centipede/feature.h"
#include "./centipede/feature_set.h"
#include "./centipede/logging.h"
#include "./centipede/remote_file.h"
#include "./centipede/rusage_profiler.h"
#include "./centipede/rusage_stats.h"
#include "./centipede/shard_reader.h"
#include "./centipede/util.h"

namespace centipede {

using perf::RUsageProfiler;

Centipede::Centipede(const Environment &env, CentipedeCallbacks &user_callbacks,
                     const BinaryInfo &binary_info,
                     CoverageLogger &coverage_logger, Stats &stats)
    : env_(env),
      user_callbacks_(user_callbacks),
      rng_(env_.seed),
      // TODO(kcc): [impl] find a better way to compute frequency_threshold.
      fs_(env_.feature_frequency_threshold),
      coverage_frontier_(binary_info),
      binary_info_(binary_info),
      pc_table_(binary_info_.pc_table),
      symbols_(binary_info_.symbols),
      function_filter_(env_.function_filter, symbols_),
      coverage_logger_(coverage_logger),
      stats_(stats),
      input_filter_path_(std::filesystem::path(TemporaryLocalDirPath())
                             .append("filter-input")),
      input_filter_cmd_(env_.input_filter, {input_filter_path_}, {/*env*/},
                        "/dev/null", "/dev/null"),
      rusage_profiler_(
          /*scope=*/perf::RUsageScope::ThisProcess(),
          /*metrics=*/env.DumpRUsageTelemetryInThisShard()
              ? RUsageProfiler::kAllMetrics
              : RUsageProfiler::kMetricsOff,
          /*raii_actions=*/RUsageProfiler::kRaiiOff,
          /*location=*/{__FILE__, __LINE__},
          /*description=*/"Engine") {
  CHECK(env_.seed) << "env_.seed must not be zero";
  if (!env_.input_filter.empty() && env_.fork_server)
    input_filter_cmd_.StartForkServer(TemporaryLocalDirPath(), "input_filter");
}

void Centipede::SaveCorpusToLocalDir(
    const Environment &env, std::string_view save_corpus_to_local_dir) {
  for (size_t shard = 0; shard < env.total_shards; shard++) {
    auto reader = DefaultBlobFileReaderFactory();
    reader->Open(env.MakeCorpusPath(shard)).IgnoreError();  // may not exist.
    absl::Span<uint8_t> blob;
    size_t num_read = 0;
    while (reader->Read(blob).ok()) {
      ++num_read;
      WriteToLocalHashedFileInDir(save_corpus_to_local_dir, blob);
    }
    LOG(INFO) << "Read " << num_read << " from " << env.MakeCorpusPath(shard);
  }
}

void Centipede::ExportCorpusFromLocalDir(const Environment &env,
                                         std::string_view local_dir) {
  // Shard the file paths in `local_dir` based on hashes of filenames.
  // Such partition is stable: a given file always goes to a specific shard.
  std::vector<std::vector<std::string>> sharded_paths(env.total_shards);
  size_t total_paths = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(local_dir)) {
    if (entry.is_regular_file()) {
      size_t filename_hash = std::hash<std::string>{}(entry.path().filename());
      sharded_paths[filename_hash % env.total_shards].push_back(entry.path());
      ++total_paths;
    }
  }
  // Iterate over all shards.
  size_t inputs_added = 0;
  size_t inputs_ignored = 0;
  for (size_t shard = 0; shard < env.total_shards; shard++) {
    size_t num_shard_bytes = 0;
    // Read the shard (if it exists), collect input hashes from it.
    absl::flat_hash_set<std::string> existing_hashes;
    {
      auto reader = DefaultBlobFileReaderFactory();
      // May fail to open if file doesn't exist.
      reader->Open(env.MakeCorpusPath(shard)).IgnoreError();
      absl::Span<uint8_t> blob;
      while (reader->Read(blob).ok()) {
        existing_hashes.insert(Hash(blob));
      }
    }
    // Add inputs to the current shard, if the shard doesn't have them already.
    auto appender = DefaultBlobFileWriterFactory();
    std::string corpus_path = env.MakeCorpusPath(shard);
    CHECK_OK(appender->Open(corpus_path, "a"))
        << "Failed to open corpus file: " << corpus_path;
    ByteArray shard_data;
    for (const auto &path : sharded_paths[shard]) {
      ByteArray input;
      ReadFromLocalFile(path, input);
      if (input.empty() || existing_hashes.contains(Hash(input))) {
        ++inputs_ignored;
        continue;
      }
      CHECK_OK(appender->Write(input));
      ++inputs_added;
    }
    LOG(INFO) << VV(shard) << VV(inputs_added) << VV(inputs_ignored)
              << VV(num_shard_bytes) << VV(shard_data.size());
  }
  CHECK_EQ(total_paths, inputs_added + inputs_ignored);
}

void Centipede::UpdateAndMaybeLogStats(std::string_view log_type,
                                       size_t min_log_level) {
  auto [max_corpus_size, avg_corpus_size] = corpus_.MaxAndAvgSize();

  stats_.corpus_size = corpus_.NumActive();
  stats_.num_covered_pcs = fs_.CountFeatures(feature_domains::kPCs);
  stats_.max_corpus_element_size = max_corpus_size;
  stats_.avg_corpus_element_size = avg_corpus_size;
  stats_.num_executions = num_runs_;

  if (env_.log_level < min_log_level) return;

  const double fuzz_time_secs =
      absl::ToDoubleSeconds(absl::Now() - fuzz_start_time_);
  // NOTE: By construction, if `fuzz_time_secs` <= 0, then the actual fuzzing
  // hasn't started yet.
  double execs_per_sec =
      fuzz_time_secs > 0 ? static_cast<double>(num_runs_) / fuzz_time_secs : 0;
  if (execs_per_sec > 1.) execs_per_sec = std::round(execs_per_sec);
  static const auto rusage_scope = perf::RUsageScope::ThisProcess();
  auto num_cmp_features = fs_.CountFeatures(feature_domains::kCMP) +
                          fs_.CountFeatures(feature_domains::kCMPEq) +
                          fs_.CountFeatures(feature_domains::kCMPModDiff) +
                          fs_.CountFeatures(feature_domains::kCMPHamming) +
                          fs_.CountFeatures(feature_domains::kCMPDiffLog);
  std::ostringstream os;
  auto LogIfNotZero = [&os](size_t value, std::string_view name) {
    if (!value) return;
    os << " " << name << ": " << value;
  };
  if (!env_.experiment_name.empty()) os << env_.experiment_name << " ";
  os << "[S" << env_.my_shard_index << "." << num_runs_ << "] " << log_type
     << ": ft: " << fs_.size();
  LogIfNotZero(fs_.CountFeatures(feature_domains::kPCs), "cov");
  LogIfNotZero(fs_.CountFeatures(feature_domains::k8bitCounters), "cnt");
  LogIfNotZero(fs_.CountFeatures(feature_domains::kDataFlow), "df");
  LogIfNotZero(num_cmp_features, "cmp");
  LogIfNotZero(fs_.CountFeatures(feature_domains::kBoundedPath), "path");
  LogIfNotZero(fs_.CountFeatures(feature_domains::kPCPair), "pair");
  LogIfNotZero(fs_.CountFeatures(feature_domains::kCallStack), "stk");
  for (size_t i = 0; i < std::size(feature_domains::kUserDomains); ++i) {
    LogIfNotZero(fs_.CountFeatures(feature_domains::kUserDomains[i]),
                 absl::StrCat("usr", i));
  }
  os << " corp: " << corpus_.NumActive() << "/" << corpus_.NumTotal();
  LogIfNotZero(coverage_frontier_.NumFunctionsInFrontier(), "fr");
  LogIfNotZero(num_crashes_, "crash");
  os << " max/avg: " << max_corpus_size << "/" << avg_corpus_size << " "
     << corpus_.MemoryUsageString();
  os << " exec/s: " << execs_per_sec;
  os << " mb: " << (perf::RUsageMemory::Snapshot(rusage_scope).mem_rss >> 20);
  LOG(INFO) << os.str();
}

void Centipede::LogFeaturesAsSymbols(const FeatureVec &fv) {
  if (!env_.LogFeaturesInThisShard()) return;
  for (auto feature : fv) {
    if (!feature_domains::kPCs.Contains(feature)) continue;
    PCIndex pc_index = ConvertPCFeatureToPcIndex(feature);
    auto description = coverage_logger_.ObserveAndDescribeIfNew(pc_index);
    if (description.empty()) continue;
    LOG(INFO) << description;
  }
}

bool Centipede::InputPassesFilter(const ByteArray &input) {
  if (env_.input_filter.empty()) return true;
  WriteToLocalFile(input_filter_path_, input);
  bool result = input_filter_cmd_.Execute() == EXIT_SUCCESS;
  std::filesystem::remove(input_filter_path_);
  return result;
}

bool Centipede::ExecuteAndReportCrash(std::string_view binary,
                                      const std::vector<ByteArray> &input_vec,
                                      BatchResult &batch_result) {
  bool success = user_callbacks_.Execute(binary, input_vec, batch_result);
  if (!success) ReportCrash(binary, input_vec, batch_result);
  return success;
}

// *** Highly experimental and risky. May not scale well for large targets. ***
//
// The idea: an unordered pair of two features {a, b} is by itself a feature.
// In the worst case, the number of such synthetic features is a square of
// the number of regular features, which may not scale.
// For now, we only treat pairs of PCs as features, which is still quadratic
// by the number of PCs. But in moderate-sized programs this may be tolerable.
//
// Rationale: if two different parts of the target are exercised simultaneously,
// this may create interesting behaviour that is hard to capture with regular
// control flow (or other) features.
size_t Centipede::AddPcPairFeatures(FeatureVec &fv) {
  // Using a scratch vector to avoid allocations.
  auto &pcs = add_pc_pair_scratch_;
  pcs.clear();

  size_t num_pcs = pc_table_.size();
  size_t num_added_pairs = 0;

  // Collect PCs from fv.
  for (auto feature : fv) {
    if (feature_domains::kPCs.Contains(feature))
      pcs.push_back(ConvertPCFeatureToPcIndex(feature));
  }

  // The quadratic loop: iterate all PC pairs (!!).
  for (size_t i = 0, n = pcs.size(); i < n; ++i) {
    size_t pc1 = pcs[i];
    for (size_t j = i + 1; j < n; ++j) {
      size_t pc2 = pcs[j];
      feature_t f = feature_domains::kPCPair.ConvertToMe(
          ConvertPcPairToNumber(pc1, pc2, num_pcs));
      // If we have seen this pair at least once, ignore it.
      if (fs_.Frequency(f) != 0) continue;
      fv.push_back(f);
      ++num_added_pairs;
    }
  }
  return num_added_pairs;
}

bool Centipede::RunBatch(const std::vector<ByteArray> &input_vec,
                         BlobFileWriter *corpus_file,
                         BlobFileWriter *features_file,
                         BlobFileWriter *unconditional_features_file) {
  BatchResult batch_result;
  bool success = ExecuteAndReportCrash(env_.binary, input_vec, batch_result);
  CHECK_EQ(input_vec.size(), batch_result.results().size());

  for (const auto &extra_binary : env_.extra_binaries) {
    BatchResult extra_batch_result;
    success =
        ExecuteAndReportCrash(extra_binary, input_vec, extra_batch_result) &&
        success;
  }
  if (!success && env_.exit_on_crash) {
    LOG(INFO) << "--exit_on_crash is enabled; exiting soon";
    RequestEarlyExit(EXIT_FAILURE);
    return false;
  }
  CHECK_EQ(batch_result.results().size(), input_vec.size());
  num_runs_ += input_vec.size();
  bool batch_gained_new_coverage = false;
  for (size_t i = 0; i < input_vec.size(); i++) {
    if (EarlyExitRequested()) break;
    FeatureVec &fv = batch_result.results()[i].mutable_features();
    bool function_filter_passed = function_filter_.filter(fv);
    bool input_gained_new_coverage =
        fs_.CountUnseenAndPruneFrequentFeatures(fv) != 0;
    if (env_.use_pcpair_features && AddPcPairFeatures(fv) != 0)
      input_gained_new_coverage = true;
    if (unconditional_features_file != nullptr) {
      CHECK_OK(unconditional_features_file->Write(
          PackFeaturesAndHash(input_vec[i], fv)));
    }
    if (input_gained_new_coverage) {
      // TODO(kcc): [impl] add stats for filtered-out inputs.
      if (!InputPassesFilter(input_vec[i])) continue;
      fs_.IncrementFrequencies(fv);
      LogFeaturesAsSymbols(fv);
      batch_gained_new_coverage = true;
      CHECK_GT(fv.size(), 0UL);
      if (function_filter_passed) {
        const auto &cmp_args = batch_result.results()[i].cmp_args();
        corpus_.Add(input_vec[i], fv, cmp_args, fs_, coverage_frontier_);
      }
      if (corpus_file != nullptr) {
        CHECK_OK(corpus_file->Write(input_vec[i]));
      }
      if (!env_.corpus_dir.empty()) {
        WriteToLocalHashedFileInDir(env_.corpus_dir[0], input_vec[i]);
      }
      if (features_file != nullptr) {
        CHECK_OK(features_file->Write(PackFeaturesAndHash(input_vec[i], fv)));
      }
    }
  }
  return batch_gained_new_coverage;
}

// TODO(kcc): [impl] don't reread the same corpus twice.
void Centipede::LoadShard(const Environment &load_env, size_t shard_index,
                          bool rerun) {
  VLOG(1) << "Loading shard " << shard_index
          << (rerun ? " with rerunning" : " without rerunning");
  size_t num_added_inputs = 0;
  size_t num_skipped_inputs = 0;
  std::vector<ByteArray> inputs_to_rerun;
  auto input_features_callback = [&](const ByteArray &input,
                                     FeatureVec &input_features) {
    if (EarlyExitRequested()) return;
    if (input_features.empty()) {
      if (rerun) {
        inputs_to_rerun.push_back(input);
      }
    } else {
      LogFeaturesAsSymbols(input_features);
      const auto num_new_features =
          fs_.CountUnseenAndPruneFrequentFeatures(input_features);
      if (num_new_features != 0) {
        VLOG(10) << "Adding input " << Hash(input)
                 << "; new features: " << num_new_features;
        fs_.IncrementFrequencies(input_features);
        // TODO(kcc): cmp_args are currently not saved to disk and not reloaded.
        corpus_.Add(input, input_features, {}, fs_, coverage_frontier_);
        ++num_added_inputs;
      } else {
        VLOG(10) << "Skipping input: " << Hash(input);
        ++num_skipped_inputs;
      }
    }
  };

  // See serialize_shard_loads on why we may want to serialize shard loads.
  // TODO(kcc): remove serialize_shard_loads when LoadShards() uses less RAM.
  if (env_.serialize_shard_loads) {
    ABSL_CONST_INIT static absl::Mutex load_shard_mu{absl::kConstInit};
    absl::MutexLock lock(&load_shard_mu);
    ReadShard(load_env.MakeCorpusPath(shard_index),
              load_env.MakeFeaturesPath(shard_index), input_features_callback);
  } else {
    ReadShard(load_env.MakeCorpusPath(shard_index),
              load_env.MakeFeaturesPath(shard_index), input_features_callback);
  }

  VLOG(1) << "Loaded shard " << shard_index << ": added " << num_added_inputs
          << " / skipped " << num_skipped_inputs << " inputs";

  if (num_added_inputs > 0) UpdateAndMaybeLogStats("load-shard", 1);
  if (!inputs_to_rerun.empty()) Rerun(inputs_to_rerun);
}

void Centipede::LoadAllShardsInRandomOrder(const Environment &load_env,
                                           bool rerun_my_shard) {
  // TODO(ussuri): It seems logical to reset `corpus_` before this, but
  //  that broke `ShardsAndDistillTest` in testing/centipede_test.cc.
  //  Investigate.
  std::vector<size_t> shard_idxs(env_.total_shards);
  std::iota(shard_idxs.begin(), shard_idxs.end(), 0);
  std::shuffle(shard_idxs.begin(), shard_idxs.end(), rng_);
  size_t num_shards_loaded = 0;
  for (size_t shard_idx : shard_idxs) {
    const bool rerun = rerun_my_shard && shard_idx == env_.my_shard_index;
    LoadShard(load_env, shard_idx, rerun);
    LOG_IF(INFO, (++num_shards_loaded % 100) == 0) << VV(num_shards_loaded);
  }
}

void Centipede::Rerun(std::vector<ByteArray> &to_rerun) {
  if (to_rerun.empty()) return;
  auto features_file = DefaultBlobFileWriterFactory();
  CHECK_OK(
      features_file->Open(env_.MakeFeaturesPath(env_.my_shard_index), "a"));

  LOG(INFO) << to_rerun.size() << " inputs to rerun";
  // Re-run all inputs for which we don't know their features.
  // Run in batches of at most env_.batch_size inputs each.
  while (!to_rerun.empty()) {
    if (EarlyExitRequested()) break;
    size_t batch_size = std::min(to_rerun.size(), env_.batch_size);
    std::vector<ByteArray> batch(to_rerun.end() - batch_size, to_rerun.end());
    to_rerun.resize(to_rerun.size() - batch_size);
    if (RunBatch(batch, nullptr, nullptr, features_file.get())) {
      UpdateAndMaybeLogStats("rerun-old", 1);
    }
  }
}

void Centipede::GenerateCoverageReport(std::string_view filename_annotation,
                                       std::string_view description) {
  if (pc_table_.empty()) return;

  auto coverage_path = env_.MakeCoverageReportPath(filename_annotation);
  LOG(INFO) << "Generate coverage report: " << description << " "
            << VV(coverage_path);
  auto pci_vec = fs_.ToCoveragePCs();
  Coverage coverage(pc_table_, pci_vec);
  std::stringstream out;
  out << "# " << description << ":\n\n";
  coverage.Print(symbols_, out);
  RemoteFileSetContents(coverage_path, out.str());
}

void Centipede::GenerateCorpusStats(std::string_view filename_annotation,
                                    std::string_view description) {
  auto stats_path = env_.MakeCorpusStatsPath(filename_annotation);
  LOG(INFO) << "Generate corpus stats: " << description << " "
            << VV(stats_path);
  std::ostringstream os;
  os << "# " << description << ":\n\n";
  corpus_.PrintStats(os, fs_);
  RemoteFileSetContents(stats_path, os.str());
}

// TODO(nedwill): add integration test once tests are refactored per b/255660879
void Centipede::GenerateSourceBasedCoverageReport(
    std::string_view filename_annotation, std::string_view description) {
  if (env_.clang_coverage_binary.empty()) return;

  auto report_path =
      env_.MakeSourceBasedCoverageReportPath(filename_annotation);
  LOG(INFO) << "Generate source based coverage report: " << description << " "
            << VV(report_path);
  RemoteMkdir(report_path);

  std::vector<std::string> raw_profiles = env_.EnumerateRawCoverageProfiles();

  if (raw_profiles.empty()) {
    LOG(ERROR) << "No raw profiles found for coverage report";
    return;
  }

  std::string indexed_profile_path =
      env_.MakeSourceBasedCoverageIndexedProfilePath();

  std::vector<std::string> merge_arguments = {"merge", "-o",
                                              indexed_profile_path, "-sparse"};
  for (const std::string &raw_profile : raw_profiles) {
    merge_arguments.push_back(raw_profile);
  }

  Command merge_command("llvm-profdata", merge_arguments);
  if (merge_command.Execute() != EXIT_SUCCESS) {
    LOG(ERROR) << "Failed to run command " << merge_command.ToString();
    return;
  }

  Command generate_report_command(
      "llvm-cov",
      {"show", "-format=html", absl::StrCat("-output-dir=", report_path),
       absl::StrCat("-instr-profile=", indexed_profile_path),
       env_.clang_coverage_binary});
  if (generate_report_command.Execute() != EXIT_SUCCESS) {
    LOG(ERROR) << "Failed to run command "
               << generate_report_command.ToString();
    return;
  }
}

void Centipede::GenerateRUsageReport(std::string_view filename_annotation,
                                     std::string_view description) {
  class ReportDumper : public RUsageProfiler::ReportSink {
   public:
    explicit ReportDumper(std::string_view path)
        : file_{RemoteFileOpen(path, "w")} {
      CHECK(file_ != nullptr) << VV(path);
    }

    ~ReportDumper() override { RemoteFileClose(file_); }

    ReportDumper &operator<<(const std::string &fragment) override {
      RemoteFileAppend(file_, ByteArray{fragment.cbegin(), fragment.cend()});
      return *this;
    }

   private:
    RemoteFile *file_;
  };

  const auto &snapshot = rusage_profiler_.TakeSnapshot(
      {__FILE__, __LINE__}, std::string{description});
  VLOG(1) << "Rusage @ " << description << ": " << snapshot.ShortMetricsStr();
  auto path = env_.MakeRUsageReportPath(filename_annotation);
  LOG(INFO) << "Generate rusage report: " << VV(env_.my_shard_index)
            << description << " " << VV(path);
  ReportDumper dumper{path};
  rusage_profiler_.GenerateReport(&dumper);
}

void Centipede::MaybeGenerateTelemetry(std::string_view filename_annotation,
                                       std::string_view description) {
  if (env_.DumpCorpusTelemetryInThisShard()) {
    GenerateCoverageReport(filename_annotation, description);
    GenerateCorpusStats(filename_annotation, description);
    GenerateSourceBasedCoverageReport(filename_annotation, description);
  }
  if (env_.DumpRUsageTelemetryInThisShard()) {
    GenerateRUsageReport(filename_annotation, description);
  }
}

void Centipede::MaybeGenerateTelemetryAfterBatch(
    std::string_view filename_annotation, size_t batch_index) {
  if (env_.DumpTelemetryForThisBatch(batch_index)) {
    MaybeGenerateTelemetry(  //
        filename_annotation, absl::StrCat("After batch ", batch_index));
  }
}

void Centipede::MergeFromOtherCorpus(std::string_view merge_from_dir,
                                     size_t shard_index_to_merge) {
  LOG(INFO) << __func__ << ": " << merge_from_dir;
  Environment merge_from_env = env_;
  merge_from_env.workdir = merge_from_dir;
  size_t initial_corpus_size = corpus_.NumActive();
  LoadShard(merge_from_env, shard_index_to_merge, /*rerun=*/true);
  size_t new_corpus_size = corpus_.NumActive();
  CHECK_GE(new_corpus_size, initial_corpus_size);  // Corpus can't shrink here.
  if (new_corpus_size > initial_corpus_size) {
    auto appender = DefaultBlobFileWriterFactory();
    CHECK_OK(appender->Open(env_.MakeCorpusPath(env_.my_shard_index), "a"));
    for (size_t idx = initial_corpus_size; idx < new_corpus_size; ++idx) {
      CHECK_OK(appender->Write(corpus_.Get(idx)));
    }
    LOG(INFO) << "Merge: " << (new_corpus_size - initial_corpus_size)
              << " new inputs added";
  }
}

void Centipede::ReloadAllShardsAndWriteDistilledCorpus() {
  // Reload the shards. This automatically distills the corpus by discarding
  // inputs with duplicate feature sets as they are being added. Reloading
  // randomly leaves random winners from such sets of duplicates in the
  // distilled output: so multiple distilling shards will produce different
  // outputs from the same inputs (the property that we want).
  LoadAllShardsInRandomOrder(env_, /*rerun_my_shard=*/false);

  // Save the distilled corpus to a file in workdir and possibly to a hashed
  // file in the first corpus dir passed in `--corpus_dir`.
  const auto distill_to_path = env_.MakeDistilledPath();
  LOG(INFO) << "Distilling: shard: " << env_.my_shard_index
            << " output: " << distill_to_path << " "
            << " distilled size: " << corpus_.NumActive();
  const auto appender = DefaultBlobFileWriterFactory();
  // NOTE: Always overwrite distilled corpus files -- never append, unlike
  // "regular", per-shard corpus files.
  CHECK_OK(appender->Open(distill_to_path, "w"));
  for (size_t i = 0; i < corpus_.NumActive(); ++i) {
    const ByteArray &input = corpus_.Get(i);
    CHECK_OK(appender->Write(input));
    if (!env_.corpus_dir.empty()) {
      WriteToLocalHashedFileInDir(env_.corpus_dir[0], input);
    }
  }
}

void Centipede::FuzzingLoop() {
  LOG(INFO) << "Shard: " << env_.my_shard_index << "/" << env_.total_shards
            << " " << TemporaryLocalDirPath() << " "
            << "seed: " << env_.seed << "\n\n\n";

  {
    // Execute a dummy input.
    BatchResult batch_result;
    user_callbacks_.Execute(env_.binary, {user_callbacks_.DummyValidInput()},
                            batch_result);
  }

  UpdateAndMaybeLogStats("begin-fuzz", 0);

  if (env_.full_sync) {
    LoadAllShardsInRandomOrder(env_, /*rerun_my_shard=*/true);
  } else {
    LoadShard(env_, env_.my_shard_index, /*rerun=*/true);
  }

  if (!env_.merge_from.empty()) {
    // Merge a shard with the same index from another corpus.
    MergeFromOtherCorpus(env_.merge_from, env_.my_shard_index);
  }

  auto corpus_file = DefaultBlobFileWriterFactory();
  auto features_file = DefaultBlobFileWriterFactory();
  CHECK_OK(corpus_file->Open(env_.MakeCorpusPath(env_.my_shard_index), "a"));
  CHECK_OK(
      features_file->Open(env_.MakeFeaturesPath(env_.my_shard_index), "a"));

  if (corpus_.NumTotal() == 0) {
    corpus_.Add(user_callbacks_.DummyValidInput(), {}, {}, fs_,
                coverage_frontier_);
  }

  UpdateAndMaybeLogStats("init-done", 0);

  // Clear fuzz_start_time_ and num_runs_, so that the pre-init work doesn't
  // affect them.
  fuzz_start_time_ = absl::Now();
  num_runs_ = 0;

  // If we're going to fuzz, dump the initial telemetry files. For a brand-new
  // run, these will be functionally empty, e.g. the coverage report will list
  // all target functions as not covered (NONE). For a bootstrapped run (the
  // workdir already has data), these may or may not coincide with the final
  // "latest" report of the previous run, depending on how the runs are
  // configured (the same number of shards, for example).
  if (env_.num_runs != 0) MaybeGenerateTelemetry("initial", "Before fuzzing");

  // num_runs / batch_size, rounded up.
  size_t number_of_batches = env_.num_runs / env_.batch_size;
  if (env_.num_runs % env_.batch_size != 0) ++number_of_batches;
  size_t new_runs = 0;
  size_t corpus_size_at_last_prune = corpus_.NumActive();
  for (size_t batch_index = 0; batch_index < number_of_batches; batch_index++) {
    if (EarlyExitRequested()) break;
    CHECK_LT(new_runs, env_.num_runs);
    auto remaining_runs = env_.num_runs - new_runs;
    auto batch_size = std::min(env_.batch_size, remaining_runs);
    std::vector<ByteArray> inputs, mutants;
    inputs.resize(env_.mutate_batch_size);
    for (size_t i = 0; i < env_.mutate_batch_size; i++) {
      const auto &corpus_record = env_.use_corpus_weights
                                      ? corpus_.WeightedRandom(rng_())
                                      : corpus_.UniformRandom(rng_());
      inputs[i] = corpus_record.data;
      // Use the cmp_args of the first input.
      // See the related TODO around SetCmpDictionary.
      if (i == 0) user_callbacks_.SetCmpDictionary(corpus_record.cmp_args);
    }

    user_callbacks_.Mutate(inputs, batch_size, mutants);
    bool gained_new_coverage =
        RunBatch(mutants, corpus_file.get(), features_file.get(), nullptr);
    new_runs += mutants.size();

    if (gained_new_coverage) {
      UpdateAndMaybeLogStats("new-feature", 1);
    } else if (((batch_index - 1) & batch_index) == 0) {
      // Log if batch_index is a power of two.
      UpdateAndMaybeLogStats("pulse", 1);
    }

    // Dump the intermediate telemetry files.
    MaybeGenerateTelemetryAfterBatch("latest", batch_index);

    if (env_.load_other_shard_frequency != 0 && batch_index != 0 &&
        (batch_index % env_.load_other_shard_frequency) == 0 &&
        env_.total_shards > 1) {
      size_t rand = rng_() % (env_.total_shards - 1);
      size_t other_shard_index =
          (env_.my_shard_index + 1 + rand) % env_.total_shards;
      CHECK_NE(other_shard_index, env_.my_shard_index);
      LoadShard(env_, other_shard_index, /*rerun=*/false);
    }

    // Prune if we added enough new elements since last prune.
    if (env_.prune_frequency != 0 &&
        corpus_.NumActive() >
            corpus_size_at_last_prune + env_.prune_frequency) {
      if (env_.use_coverage_frontier) coverage_frontier_.Compute(corpus_);
      corpus_.Prune(fs_, coverage_frontier_, env_.max_corpus_size, rng_);
      corpus_size_at_last_prune = corpus_.NumActive();
    }
  }

  // The tests rely on this stat being logged last.
  UpdateAndMaybeLogStats("end-fuzz", 0);

  // If we've fuzzed anything, dump the final telemetry files, possibly
  // overwriting the last intermediate version dumped inside the loop.
  if (env_.num_runs != 0) MaybeGenerateTelemetry("latest", "After fuzzing");

  // If requested, distill the corpus. Note that with `--num_runs` == 0, this
  // will essentially be the single action this run will carry out, with the
  // fuzzing loop being a no-op.
  if (env_.DistillingInThisShard()) {
    ReloadAllShardsAndWriteDistilledCorpus();

    // Dump the distillation telemetry so the post-distillation vs. post-fuzzing
    // stats can be compared.
    MaybeGenerateTelemetry("distilled", "After distillation");
  }
}

void Centipede::ReportCrash(std::string_view binary,
                            const std::vector<ByteArray> &input_vec,
                            const BatchResult &batch_result) {
  CHECK_EQ(input_vec.size(), batch_result.results().size());
  if (EarlyExitRequested()) return;

  if (++num_crashes_ > env_.max_num_crash_reports) return;

  const size_t suspect_input_idx = std::clamp<size_t>(
      batch_result.num_outputs_read(), 0, input_vec.size() - 1);

  const std::string log_prefix =
      absl::StrCat("ReportCrash[", num_crashes_, "]: ");

  LOG(INFO) << log_prefix << "Batch execution failed:"
            << "\nBinary               : " << binary
            << "\nExit code            : " << batch_result.exit_code()
            << "\nFailure              : " << batch_result.failure_description()
            << "\nNumber of inputs     : " << input_vec.size()
            << "\nNumber of inputs read: " << batch_result.num_outputs_read()
            << "\nSuspect input index  : " << suspect_input_idx
            << "\nCrash log            :\n\n";
  for (const auto &log_line :
       absl::StrSplit(absl::StripAsciiWhitespace(batch_result.log()), '\n')) {
    LOG(INFO).NoPrefix() << "CRASH LOG: " << log_line;
  }
  LOG(INFO).NoPrefix() << "\n";

  LOG_IF(INFO, num_crashes_ == env_.max_num_crash_reports)
      << log_prefix
      << "Reached --max_num_crash_reports: further reports will be suppressed";

  // Determine the optimal order of the inputs to try to maximize the chances of
  // finding the reproducer fast.
  // TODO(b/274705740): When the bug is fixed, set `input_idxs_to_try`'s size to
  //  `suspect_input_idx + 1`.
  std::vector<size_t> input_idxs_to_try(input_vec.size() + 1);
  input_idxs_to_try.front() = suspect_input_idx;
  std::iota(input_idxs_to_try.begin() + 1, input_idxs_to_try.end(), 0);
  // Prioritize the presumed crasher by inserting it in front of everything
  // else. However, do keep it at the old location, too, in case the target was
  // primed for a crash by the sequence of inputs that preceded the crasher.

  if (batch_result.failure_description() == kExecutionFailurePerBatchTimeout) {
    LOG(INFO) << log_prefix
              << "Failure applies to entire batch: not executing inputs "
                 "one-by-one, trying to find the reproducer";
    return;
  }

  // Try inputs one-by-one in the determined order.
  LOG(INFO) << log_prefix
            << "Executing inputs one-by-one, trying to find the reproducer";
  for (auto input_idx : input_idxs_to_try) {
    if (EarlyExitRequested()) return;
    const auto &one_input = input_vec[input_idx];
    BatchResult one_input_batch_result;
    if (!user_callbacks_.Execute(binary, {one_input}, one_input_batch_result)) {
      auto hash = Hash(one_input);
      auto crash_dir = env_.MakeCrashReproducerDirPath();
      RemoteMkdir(crash_dir);
      std::string file_path = std::filesystem::path(crash_dir).append(hash);
      LOG(INFO) << log_prefix << "Detected crash-reproducing input:"
                << "\nInput index    : " << input_idx
                << "\nInput bytes    : " << AsString(one_input, /*max_len=*/32)
                << "\nExit code      : " << one_input_batch_result.exit_code()
                << "\nFailure        : "
                << one_input_batch_result.failure_description()
                << "\nSaving input to: " << file_path;
      auto *file = RemoteFileOpen(file_path, "w");  // overwrites existing file.
      CHECK(file != nullptr) << log_prefix << "Failed to open " << file_path;
      RemoteFileAppend(file, one_input);
      RemoteFileClose(file);
      return;
    }
  }

  LOG(INFO) << log_prefix
            << "Crash was not observed when running inputs one-by-one";

  // There will be cases when several inputs collectively cause a crash, but no
  // single input does. Handle this by writing out all inputs from the batch.

  // TODO(bookholt): Check for repro by re-running the whole batch.
  // TODO(ussuri): Consolidate logic for test case reproduction.

  const auto &suspect_input = input_vec[suspect_input_idx];
  // Save inputs to <--workdir>/crash/unreliable_batch-<HASH_OF_SUSPECT_INPUT>.
  auto suspect_hash = Hash(suspect_input);
  auto crash_dir = env_.MakeCrashReproducerDirPath();
  RemoteMkdir(crash_dir);
  std::string save_dir = std::filesystem::path(crash_dir)
                             .append("crashing_batch-")
                             .concat(suspect_hash);
  RemoteMkdir(save_dir);
  LOG(INFO) << log_prefix << "Saving used inputs from batch to: " << save_dir;
  for (int i = 0; i <= suspect_input_idx; ++i) {
    const auto &one_input = input_vec[i];
    auto hash = Hash(one_input);
    std::string file_path = std::filesystem::path(save_dir).append(
        absl::StrFormat("input-%010d-%s", i, hash));
    auto *file = RemoteFileOpen(file_path, "w");
    CHECK(file != nullptr) << log_prefix << "Failed to open " << file_path;
    RemoteFileAppend(file, one_input);
    RemoteFileClose(file);
  }
}

}  // namespace centipede
