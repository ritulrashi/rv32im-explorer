#include "iss/trace.h"

#include <gtest/gtest.h>

#include <sstream>

using rvx::MemKind;
using rvx::RetireRecord;

TEST(Trace, AluInstruction) {
  RetireRecord r{.seq = 7, .pc = 0x80000004, .insn = 0x00b50533, .rd = 10, .rd_val = 0x2a};
  EXPECT_EQ(rvx::format_retire(r),
            "7 pc=80000004 insn=00b50533 rd=x10 val=0000002a mem=- addr=- data=- sz=-");
}

TEST(Trace, SingleDigitRegisterIsZeroPadded) {
  RetireRecord r{.seq = 0, .pc = 0, .insn = 0x00100093, .rd = 1, .rd_val = 1};
  EXPECT_EQ(rvx::format_retire(r),
            "0 pc=00000000 insn=00100093 rd=x01 val=00000001 mem=- addr=- data=- sz=-");
}

TEST(Trace, NoRegisterWrite) {
  RetireRecord r{.seq = 3, .pc = 0x10, .insn = 0xfe000ee3};
  EXPECT_EQ(rvx::format_retire(r),
            "3 pc=00000010 insn=fe000ee3 rd=- val=- mem=- addr=- data=- sz=-");
}

TEST(Trace, Load) {
  RetireRecord r{.seq = 42, .pc = 0x80000108, .insn = 0x0005a603, .rd = 12, .rd_val = 0xffffff80,
                 .mem = MemKind::Read, .mem_addr = 0x80002000, .mem_data = 0x80, .mem_size = 1};
  EXPECT_EQ(rvx::format_retire(r),
            "42 pc=80000108 insn=0005a603 rd=x12 val=ffffff80 mem=R addr=80002000 data=00000080 sz=1");
}

TEST(Trace, Store) {
  RetireRecord r{.seq = 41, .pc = 0x80000104, .insn = 0x00c5a023, .mem = MemKind::Write,
                 .mem_addr = 0x80002000, .mem_data = 0x2a, .mem_size = 4};
  EXPECT_EQ(rvx::format_retire(r),
            "41 pc=80000104 insn=00c5a023 rd=- val=- mem=W addr=80002000 data=0000002a sz=4");
}

TEST(Trace, WriterEmitsOneLinePerRecord) {
  std::ostringstream os;
  rvx::TraceWriter w(os);
  w.write(RetireRecord{.seq = 0});
  w.write(RetireRecord{.seq = 1});
  EXPECT_EQ(os.str(),
            "0 pc=00000000 insn=00000000 rd=- val=- mem=- addr=- data=- sz=-\n"
            "1 pc=00000000 insn=00000000 rd=- val=- mem=- addr=- data=- sz=-\n");
}
