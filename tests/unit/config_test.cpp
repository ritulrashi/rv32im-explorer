#include "pipeline/config.h"

#include <gtest/gtest.h>

#include "common/kvfile.h"

using rvx::ConfigError;
using rvx::pipe::Config;

TEST(KvFile, ParsesKeysValuesAndComments) {
  auto kv = rvx::parse_kv("# header\n  a.b = 1  \n\nc = hello # trailing\n", "test");
  ASSERT_EQ(kv.size(), 2u);
  EXPECT_EQ(kv.at("a.b"), "1");
  EXPECT_EQ(kv.at("c"), "hello");
}

TEST(KvFile, RejectsMalformedLines) {
  EXPECT_THROW(rvx::parse_kv("no equals sign\n", "t"), ConfigError);
  EXPECT_THROW(rvx::parse_kv(" = 3\n", "t"), ConfigError);
  EXPECT_THROW(rvx::parse_kv("a = 1\na = 2\n", "t"), ConfigError);  // duplicate
}

TEST(KvFile, ParsesOverride) {
  auto [k, v] = rvx::parse_override("bpred.type=gshare");
  EXPECT_EQ(k, "bpred.type");
  EXPECT_EQ(v, "gshare");
  EXPECT_THROW(rvx::parse_override("nokey"), ConfigError);
}

TEST(MachineConfig, DefaultsAreThePhase2Baseline) {
  Config c;
  EXPECT_EQ(c.mul_latency, 3u);
  EXPECT_EQ(c.div_latency, 33u);
  EXPECT_EQ(c.bpred.type, rvx::bpred::Type::StaticNT);
  EXPECT_EQ(c.bpred.btb_entries, 0u);
  EXPECT_FALSE(c.icache.enabled);
  EXPECT_FALSE(c.dcache.enabled);
  EXPECT_NO_THROW(rvx::pipe::validate(c));
}

TEST(MachineConfig, AppliesEverySetting) {
  Config c;
  rvx::pipe::apply_setting(c, "core.mul_latency", "5");
  rvx::pipe::apply_setting(c, "bpred.type", "gshare");
  rvx::pipe::apply_setting(c, "bpred.entries", "1024");
  rvx::pipe::apply_setting(c, "bpred.history_bits", "10");
  rvx::pipe::apply_setting(c, "btb.entries", "64");
  rvx::pipe::apply_setting(c, "icache.enabled", "true");
  rvx::pipe::apply_setting(c, "dcache.line_bytes", "64");
  rvx::pipe::apply_setting(c, "dram.row_miss_cycles", "55");
  EXPECT_EQ(c.mul_latency, 5u);
  EXPECT_EQ(c.bpred.type, rvx::bpred::Type::Gshare);
  EXPECT_EQ(c.bpred.entries, 1024u);
  EXPECT_EQ(c.bpred.history_bits, 10u);
  EXPECT_EQ(c.bpred.btb_entries, 64u);
  EXPECT_TRUE(c.icache.enabled);
  EXPECT_EQ(c.dcache.line_bytes, 64u);
  EXPECT_EQ(c.dram.row_miss_cycles, 55u);
}

TEST(MachineConfig, RejectsUnknownKeysAndBadValues) {
  Config c;
  EXPECT_THROW(rvx::pipe::apply_setting(c, "icache.sise_bytes", "4096"), ConfigError);  // typo
  EXPECT_THROW(rvx::pipe::apply_setting(c, "bpred.type", "tage"), ConfigError);
  EXPECT_THROW(rvx::pipe::apply_setting(c, "icache.enabled", "yes"), ConfigError);
  EXPECT_THROW(rvx::pipe::apply_setting(c, "core.mul_latency", "-1"), ConfigError);
  EXPECT_THROW(rvx::pipe::apply_setting(c, "core.mul_latency", "3x"), ConfigError);
}

TEST(MachineConfig, ValidationRejectsImpossibleMachines) {
  auto bad = [](auto mutate) {
    Config c;
    mutate(c);
    EXPECT_THROW(rvx::pipe::validate(c), ConfigError);
  };
  bad([](Config& c) { c.mul_latency = 0; });
  bad([](Config& c) { c.bpred.entries = 500; });                 // not a power of two
  bad([](Config& c) { c.bpred.type = rvx::bpred::Type::Gshare; c.bpred.entries = 16; c.bpred.history_bits = 5; });
  bad([](Config& c) { c.bpred.btb_entries = 3; });
  bad([](Config& c) { c.icache.enabled = true; c.icache.line_bytes = 24; });
  bad([](Config& c) { c.icache.enabled = true; c.icache.line_bytes = 2; });   // smaller than a word
  bad([](Config& c) { c.dcache.enabled = true; c.dcache.size_bytes = 64; c.dcache.assoc = 4; c.dcache.line_bytes = 32; });
  bad([](Config& c) { c.dcache.enabled = true; c.dcache.size_bytes = 96; });  // 96/(2*32) sets, not pow2
  bad([](Config& c) { c.dram.banks = 3; });
  bad([](Config& c) { c.icache.enabled = true; c.icache.line_bytes = 4; c.dram.bus_bytes_per_cycle = 8; });
}

TEST(MachineConfig, DescribeRoundTrips) {
  Config c;
  c.bpred.type = rvx::bpred::Type::Bimodal;
  c.bpred.btb_entries = 16;
  c.icache.enabled = true;
  c.dram.banks = 8;
  const std::string text = rvx::pipe::describe(c);
  Config d;
  for (const auto& [k, v] : rvx::parse_kv(text, "describe")) rvx::pipe::apply_setting(d, k, v);
  EXPECT_EQ(rvx::pipe::describe(d), text);
  EXPECT_NE(text.find("bpred.type = bimodal"), std::string::npos);
}
