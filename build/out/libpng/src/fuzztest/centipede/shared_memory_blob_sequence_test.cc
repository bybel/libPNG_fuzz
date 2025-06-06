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

#include "./centipede/shared_memory_blob_sequence.h"

#include <unistd.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "gtest/gtest.h"

namespace centipede {

std::string ShmemName() {
  std::ostringstream oss;
  oss << "/shared_memory_blob_sequence_test-" << getpid() << "-"
      << std::this_thread::get_id();
  return oss.str();
}

// Helper: Blob to std::vector.
static std::vector<uint8_t> Vec(SharedMemoryBlobSequence::Blob blob) {
  return {blob.data, blob.data + blob.size};
}

// Helper: std::vector to Blob.
static SharedMemoryBlobSequence::Blob Blob(const std::vector<uint8_t> &vec,
                                           uint64_t tag = 1) {
  return {tag, vec.size(), vec.data()};
}

TEST(SharedMemoryBlobSequence, ParentChild) {
  std::vector<uint8_t> kTestData1 = {1, 2, 3};
  std::vector<uint8_t> kTestData2 = {4, 5, 6, 7};
  std::vector<uint8_t> kTestData3 = {8, 9};
  std::vector<uint8_t> kTestData4 = {'a', 'b', 'c', 'd', 'e'};

  // Creating a child w/o first creating a parent should crash.
  EXPECT_DEATH(
      SharedMemoryBlobSequence child_with_no_parent(ShmemName().c_str()),
      "shm_open\\(\\) failed");

  SharedMemoryBlobSequence parent(ShmemName().c_str(), 1000);
  // Parent writes data.
  EXPECT_TRUE(parent.Write(Blob(kTestData1, 123)));
  EXPECT_TRUE(parent.Write(Blob(kTestData2, 456)));

  // Child created.
  SharedMemoryBlobSequence child(ShmemName().c_str());
  // Child reads data.
  auto blob1 = child.Read();
  EXPECT_EQ(kTestData1, Vec(blob1));
  EXPECT_EQ(blob1.tag, 123);
  auto blob2 = child.Read();
  EXPECT_EQ(kTestData2, Vec(blob2));
  EXPECT_EQ(blob2.tag, 456);
  EXPECT_FALSE(child.Read().IsValid());

  // Child writes data.
  child.Reset();
  EXPECT_TRUE(child.Write(Blob(kTestData3)));
  EXPECT_TRUE(child.Write(Blob(kTestData4)));

  // Parent reads data.
  parent.Reset();
  EXPECT_EQ(kTestData3, Vec(parent.Read()));
  EXPECT_EQ(kTestData4, Vec(parent.Read()));
  EXPECT_FALSE(parent.Read().IsValid());
}

TEST(SharedMemoryBlobSequence, CheckForResourceLeaks) {
  const int kNumIters = 1 << 17;  // Some large number of iterations.
  const int kBlobSize = 1 << 30;  // Some large blob size.
  // Create and destroy lots of parent/child blob pairs.
  for (int iter = 0; iter < kNumIters; iter++) {
    SharedMemoryBlobSequence parent(ShmemName().c_str(), kBlobSize);
    parent.Write(Blob({1, 2, 3}));
    SharedMemoryBlobSequence child(ShmemName().c_str());
    EXPECT_EQ(child.Read().size, 3);
  }
  // Create a parent blob, then create and destroy lots of child blobs.
  SharedMemoryBlobSequence parent(ShmemName().c_str(), kBlobSize);
  parent.Write(Blob({1, 2, 3, 4}));
  for (int iter = 0; iter < kNumIters; iter++) {
    SharedMemoryBlobSequence child(ShmemName().c_str());
    EXPECT_EQ(child.Read().size, 4);
  }
}

// Tests that Read-after-Write or Write-after-Read w/o Reset crashes.
TEST(SharedMemoryBlobSequence, ReadVsWriteWithoutReset) {
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 1000);
  blobseq.Write(Blob({1, 2, 3}));
  EXPECT_DEATH(blobseq.Read(), "Had writes after reset");
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 3);
  EXPECT_DEATH(blobseq.Write(Blob({1, 2, 3, 4})), "Had reads after reset");
  blobseq.Reset();
  blobseq.Write(Blob({1, 2, 3, 4}));
}

// Check cases when SharedMemoryBlobSequence is nearly full.
TEST(SharedMemoryBlobSequence, WriteToFullSequence) {
  // Can't create SharedMemoryBlobSequence with sizes < 8.
  EXPECT_DEATH(SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 7),
               "Size too small");

  // Allocate a blob sequence with 28 bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 28);

  // 17 bytes: 8 bytes size, 8 bytes tag, 1 byte payload.
  EXPECT_TRUE(blobseq.Write(Blob({1})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 1);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 20 bytes: 4-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(Blob({1, 2, 3, 4})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 4);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 23 bytes: 7-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(Blob({1, 2, 3, 4, 5, 6, 7})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 7);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 28 bytes: 12-byte payload.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(Blob({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 12);
  EXPECT_FALSE(blobseq.Read().IsValid());

  // 13-byte payload - there is not enough space (for 13+8 bytes).
  blobseq.Reset();
  EXPECT_FALSE(
      blobseq.Write(Blob({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 12);  // State remained the same.

  // 1-, and 2- byte payloads. The last one fails.
  blobseq.Reset();
  EXPECT_TRUE(blobseq.Write(Blob({1})));
  EXPECT_FALSE(blobseq.Write(Blob({1, 2})));
  blobseq.Reset();
  EXPECT_EQ(blobseq.Read().size, 1);
  EXPECT_FALSE(blobseq.Read().IsValid());
}

// Test Write-Reset-Write-Read scenario.
TEST(SharedMemoryBlobSequence, WriteAfterReset) {
  // Allocate a blob sequence with 28 bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 100);
  const std::vector<uint8_t> kFirstWriteData(/*count=*/64, /*value=*/255);
  EXPECT_TRUE(blobseq.Write(Blob(kFirstWriteData)));
  blobseq.Reset();  // The data in shmem is unchanged.
  const std::vector<uint8_t> kSecondWriteData{42, 43};
  EXPECT_TRUE(blobseq.Write(Blob(kSecondWriteData)));
  blobseq.Reset();  // The data in shmem is unchanged.
  auto blob1 = blobseq.Read();
  EXPECT_TRUE(blob1.IsValid());
  EXPECT_EQ(Vec(blob1), kSecondWriteData);
  auto blob2 = blobseq.Read();  // must be invalid.
  EXPECT_FALSE(blob2.IsValid());
}

// Test ReleaseSharedMemory and NumBytesUsed.
TEST(SharedMemoryBlobSequence, ReleaseSharedMemory) {
  // Allocate a blob sequence with 1M bytes of storage.
  SharedMemoryBlobSequence blobseq(ShmemName().c_str(), 1 << 20);
  EXPECT_EQ(blobseq.NumBytesUsed(), 0);
  EXPECT_TRUE(blobseq.Write(Blob({1, 2, 3, 4})));
  EXPECT_GT(blobseq.NumBytesUsed(), 5);
  blobseq.ReleaseSharedMemory();
  EXPECT_EQ(blobseq.NumBytesUsed(), 0);
  EXPECT_TRUE(blobseq.Write(Blob({1, 2, 3, 4})));
  EXPECT_GT(blobseq.NumBytesUsed(), 5);
}

}  // namespace centipede
