//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include <limits>
#include <string>
#include <unordered_map>

#include "db/column_family.h"
#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/convenience.h"
#include "util/options_helper.h"
#include "util/random.h"
#include "util/sync_point.h"
#include "util/testutil.h"

namespace rocksdb {

class DBOptionsTest : public DBTestBase {
 public:
  DBOptionsTest() : DBTestBase("/db_options_test") {}

#ifndef ROCKSDB_LITE
  std::unordered_map<std::string, std::string> GetMutableCFOptionsMap(
      const ColumnFamilyOptions& options) {
    std::string options_str;
    GetStringFromColumnFamilyOptions(&options_str, options);
    std::unordered_map<std::string, std::string> options_map;
    StringToMap(options_str, &options_map);
    std::unordered_map<std::string, std::string> mutable_map;
    for (const auto opt : cf_options_type_info) {
      if (opt.second.is_mutable &&
          opt.second.verification != OptionVerificationType::kDeprecated) {
        mutable_map[opt.first] = options_map[opt.first];
      }
    }
    return mutable_map;
  }

  std::unordered_map<std::string, std::string> GetRandomizedMutableCFOptionsMap(
      Random* rnd) {
    Options options;
    ImmutableDBOptions db_options(options);
    test::RandomInitCFOptions(&options, rnd);
    auto sanitized_options = SanitizeOptions(db_options, nullptr, options);
    auto opt_map = GetMutableCFOptionsMap(sanitized_options);
    delete options.compaction_filter;
    return opt_map;
  }
#endif  // ROCKSDB_LITE
};

// RocksDB lite don't support dynamic options.
#ifndef ROCKSDB_LITE

TEST_F(DBOptionsTest, GetLatestOptions) {
  // GetOptions should be able to get latest option changed by SetOptions.
  Options options;
  options.create_if_missing = true;
  Random rnd(228);
  Reopen(options);
  CreateColumnFamilies({"foo"}, options);
  ReopenWithColumnFamilies({"default", "foo"}, options);
  auto options_default = GetRandomizedMutableCFOptionsMap(&rnd);
  auto options_foo = GetRandomizedMutableCFOptionsMap(&rnd);
  ASSERT_OK(dbfull()->SetOptions(handles_[0], options_default));
  ASSERT_OK(dbfull()->SetOptions(handles_[1], options_foo));
  ASSERT_EQ(options_default,
            GetMutableCFOptionsMap(dbfull()->GetOptions(handles_[0])));
  ASSERT_EQ(options_foo,
            GetMutableCFOptionsMap(dbfull()->GetOptions(handles_[1])));
}

TEST_F(DBOptionsTest, EnableAutoCompactionAndTriggerStall) {
  const std::string kValue(1024, 'v');
  for (int method_type = 0; method_type < 2; method_type++) {
    for (int option_type = 0; option_type < 4; option_type++) {
      Options options;
      options.create_if_missing = true;
      options.disable_auto_compactions = true;
      options.write_buffer_size = 1024 * 1024 * 10;
      options.compression = CompressionType::kNoCompression;
      options.level0_file_num_compaction_trigger = 1;
      options.level0_stop_writes_trigger = std::numeric_limits<int>::max();
      options.level0_slowdown_writes_trigger = std::numeric_limits<int>::max();
      options.hard_pending_compaction_bytes_limit =
          std::numeric_limits<uint64_t>::max();
      options.soft_pending_compaction_bytes_limit =
          std::numeric_limits<uint64_t>::max();

      DestroyAndReopen(options);
      int i = 0;
      for (; i < 1024; i++) {
        Put(Key(i), kValue);
      }
      Flush();
      for (; i < 1024 * 2; i++) {
        Put(Key(i), kValue);
      }
      Flush();
      dbfull()->TEST_WaitForFlushMemTable();
      ASSERT_EQ(2, NumTableFilesAtLevel(0));
      uint64_t l0_size = SizeAtLevel(0);

      switch (option_type) {
        case 0:
          // test with level0_stop_writes_trigger
          options.level0_stop_writes_trigger = 2;
          options.level0_slowdown_writes_trigger = 2;
          break;
        case 1:
          options.level0_slowdown_writes_trigger = 2;
          break;
        case 2:
          options.hard_pending_compaction_bytes_limit = l0_size;
          options.soft_pending_compaction_bytes_limit = l0_size;
          break;
        case 3:
          options.soft_pending_compaction_bytes_limit = l0_size;
          break;
      }
      Reopen(options);
      dbfull()->TEST_WaitForCompact();
      ASSERT_FALSE(dbfull()->TEST_write_controler().IsStopped());
      ASSERT_FALSE(dbfull()->TEST_write_controler().NeedsDelay());

      SyncPoint::GetInstance()->LoadDependency(
          {{"DBOptionsTest::EnableAutoCompactionAndTriggerStall:1",
            "BackgroundCallCompaction:0"},
           {"DBImpl::BackgroundCompaction():BeforePickCompaction",
            "DBOptionsTest::EnableAutoCompactionAndTriggerStall:2"},
           {"DBOptionsTest::EnableAutoCompactionAndTriggerStall:3",
            "DBImpl::BackgroundCompaction():AfterPickCompaction"}});
      // Block background compaction.
      SyncPoint::GetInstance()->EnableProcessing();

      switch (method_type) {
        case 0:
          ASSERT_OK(
              dbfull()->SetOptions({{"disable_auto_compactions", "false"}}));
          break;
        case 1:
          ASSERT_OK(dbfull()->EnableAutoCompaction(
              {dbfull()->DefaultColumnFamily()}));
          break;
      }
      TEST_SYNC_POINT("DBOptionsTest::EnableAutoCompactionAndTriggerStall:1");
      // Wait for stall condition recalculate.
      TEST_SYNC_POINT("DBOptionsTest::EnableAutoCompactionAndTriggerStall:2");

      switch (option_type) {
        case 0:
          ASSERT_TRUE(dbfull()->TEST_write_controler().IsStopped());
          break;
        case 1:
          ASSERT_FALSE(dbfull()->TEST_write_controler().IsStopped());
          ASSERT_TRUE(dbfull()->TEST_write_controler().NeedsDelay());
          break;
        case 2:
          ASSERT_TRUE(dbfull()->TEST_write_controler().IsStopped());
          break;
        case 3:
          ASSERT_FALSE(dbfull()->TEST_write_controler().IsStopped());
          ASSERT_TRUE(dbfull()->TEST_write_controler().NeedsDelay());
          break;
      }
      TEST_SYNC_POINT("DBOptionsTest::EnableAutoCompactionAndTriggerStall:3");

      // Background compaction executed.
      dbfull()->TEST_WaitForCompact();
      ASSERT_FALSE(dbfull()->TEST_write_controler().IsStopped());
      ASSERT_FALSE(dbfull()->TEST_write_controler().NeedsDelay());
    }
  }
}

#endif  // ROCKSDB_LITE

}  // namespace rocksdb

int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
