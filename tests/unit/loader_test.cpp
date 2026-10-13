// Loader tests. ELF images are built byte-by-byte here from the ELF32 spec
// layout (System V gABI), so the test does not depend on the cross toolchain
// or on the loader's own structure definitions.
#include "common/loader.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using rvx::LoadError;
using rvx::Memory;

namespace {

struct Seg {
  uint32_t paddr;
  std::vector<uint8_t> data;
  uint32_t memsz;
};
struct Sym {
  std::string name;
  uint32_t value;
  uint8_t info;  // (bind << 4) | type
};

class ElfBuilder {
 public:
  uint32_t entry = 0x80000000;
  std::vector<Seg> segs;
  std::vector<Sym> syms;

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> f(52, 0);
    const uint32_t phoff = 52;
    f.resize(phoff + 32 * segs.size(), 0);
    // Segment payloads.
    std::vector<uint32_t> seg_off;
    for (const auto& s : segs) {
      seg_off.push_back(static_cast<uint32_t>(f.size()));
      f.insert(f.end(), s.data.begin(), s.data.end());
    }
    // String table: leading NUL, then names.
    std::vector<uint32_t> name_off;
    const uint32_t strtab_off = static_cast<uint32_t>(f.size());
    f.push_back(0);
    for (const auto& s : syms) {
      name_off.push_back(static_cast<uint32_t>(f.size()) - strtab_off);
      f.insert(f.end(), s.name.begin(), s.name.end());
      f.push_back(0);
    }
    const uint32_t strtab_size = static_cast<uint32_t>(f.size()) - strtab_off;
    while (f.size() % 4) f.push_back(0);
    // Symbol table: entry 0 is the null symbol.
    const uint32_t symtab_off = static_cast<uint32_t>(f.size());
    f.resize(f.size() + 16, 0);
    for (size_t k = 0; k < syms.size(); ++k) {
      const size_t o = f.size();
      f.resize(o + 16, 0);
      put32(f, o + 0, name_off[k]);
      put32(f, o + 4, syms[k].value);
      f[o + 12] = syms[k].info;
      put16(f, o + 14, 1);  // some defined section index
    }
    const uint32_t symtab_size = static_cast<uint32_t>(f.size()) - symtab_off;
    // Section headers: [0] null, [1] symtab (link -> 2), [2] strtab.
    const uint32_t shoff = static_cast<uint32_t>(f.size());
    f.resize(f.size() + 3 * 40, 0);
    put32(f, shoff + 40 + 4, 2);  // SHT_SYMTAB
    put32(f, shoff + 40 + 16, symtab_off);
    put32(f, shoff + 40 + 20, symtab_size);
    put32(f, shoff + 40 + 24, 2);  // sh_link
    put32(f, shoff + 40 + 36, 16);  // sh_entsize
    put32(f, shoff + 80 + 4, 3);  // SHT_STRTAB
    put32(f, shoff + 80 + 16, strtab_off);
    put32(f, shoff + 80 + 20, strtab_size);

    // ELF header.
    const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(f.data(), ident, 16);
    put16(f, 16, 2);    // ET_EXEC
    put16(f, 18, 243);  // EM_RISCV
    put32(f, 20, 1);
    put32(f, 24, entry);
    put32(f, 28, phoff);
    put32(f, 32, shoff);
    put16(f, 40, 52);
    put16(f, 42, 32);
    put16(f, 44, static_cast<uint16_t>(segs.size()));
    put16(f, 46, 40);
    put16(f, 48, 3);
    // Program headers.
    for (size_t k = 0; k < segs.size(); ++k) {
      const size_t o = phoff + 32 * k;
      put32(f, o + 0, 1);  // PT_LOAD
      put32(f, o + 4, seg_off[k]);
      put32(f, o + 8, segs[k].paddr);
      put32(f, o + 12, segs[k].paddr);
      put32(f, o + 16, static_cast<uint32_t>(segs[k].data.size()));
      put32(f, o + 20, segs[k].memsz);
    }
    return f;
  }

  static void put16(std::vector<uint8_t>& f, size_t o, uint16_t v) {
    f[o] = static_cast<uint8_t>(v);
    f[o + 1] = static_cast<uint8_t>(v >> 8);
  }
  static void put32(std::vector<uint8_t>& f, size_t o, uint32_t v) {
    for (int b = 0; b < 4; ++b) f[o + b] = static_cast<uint8_t>(v >> (8 * b));
  }
};

ElfBuilder typical() {
  ElfBuilder b;
  b.entry = 0x80000040;
  b.segs.push_back({0x80000000, {0x13, 0x00, 0x00, 0x00, 0xEF, 0x00, 0x00, 0x00}, 8});
  // .data with 4 initialised bytes and 12 bytes of BSS.
  b.segs.push_back({0x80001000, {0xAA, 0xBB, 0xCC, 0xDD}, 16});
  b.syms.push_back({"local_label", 0x80000010, 0x00});  // STB_LOCAL
  b.syms.push_back({"tohost", 0x80001000, 0x10});       // STB_GLOBAL
  return b;
}

}  // namespace

TEST(ElfLoader, LoadsSegmentsEntryAndSymbols) {
  Memory mem;
  auto img = typical().build();
  auto r = rvx::load_elf(img, mem);
  EXPECT_EQ(r.entry, 0x80000040u);
  EXPECT_EQ(mem.load(0x80000000, 4), 0x00000013u);
  EXPECT_EQ(mem.load(0x80000004, 4), 0x000000EFu);
  EXPECT_EQ(mem.load(0x80001000, 4), 0xDDCCBBAAu);
  ASSERT_TRUE(r.symbol("tohost"));
  EXPECT_EQ(*r.symbol("tohost"), 0x80001000u);
  EXPECT_EQ(*r.symbol("local_label"), 0x80000010u);
  EXPECT_FALSE(r.symbol("nonexistent"));
}

TEST(ElfLoader, BssIsZeroFilledEvenOverExistingData) {
  Memory mem;
  for (uint32_t a = 0x80001000; a < 0x80001010; a += 4) mem.store(a, 4, 0xFFFFFFFF);
  auto img = typical().build();
  rvx::load_elf(img, mem);
  for (uint32_t a = 0x80001004; a < 0x80001010; a += 4) EXPECT_EQ(mem.load(a, 4), 0u) << a;
  EXPECT_EQ(mem.load(0x80001010, 4), 0u);  // never written, still zero
}

TEST(ElfLoader, RejectsBadMagic) {
  Memory mem;
  auto img = typical().build();
  img[1] = 'X';
  EXPECT_THROW(rvx::load_elf(img, mem), LoadError);
}

TEST(ElfLoader, RejectsElf64) {
  Memory mem;
  auto img = typical().build();
  img[4] = 2;
  EXPECT_THROW(rvx::load_elf(img, mem), LoadError);
}

TEST(ElfLoader, RejectsBigEndian) {
  Memory mem;
  auto img = typical().build();
  img[5] = 2;
  EXPECT_THROW(rvx::load_elf(img, mem), LoadError);
}

TEST(ElfLoader, RejectsNonRiscv) {
  Memory mem;
  auto img = typical().build();
  img[18] = 62;  // EM_X86_64
  img[19] = 0;
  EXPECT_THROW(rvx::load_elf(img, mem), LoadError);
}

TEST(ElfLoader, RejectsTruncatedFile) {
  Memory mem;
  auto img = typical().build();
  for (size_t len : {size_t{0}, size_t{10}, size_t{52}, size_t{80}}) {
    std::vector<uint8_t> cut(img.begin(), img.begin() + static_cast<long>(len));
    EXPECT_THROW(rvx::load_elf(cut, mem), LoadError) << len;
  }
}

TEST(ElfLoader, RejectsSegmentOutsideFile) {
  Memory mem;
  auto img = typical().build();
  ElfBuilder::put32(img, 52 + 4, 0xFFFFFFF0);  // p_offset of segment 0
  EXPECT_THROW(rvx::load_elf(img, mem), LoadError);
}

TEST(ElfLoader, RejectsFileszLargerThanMemsz) {
  Memory mem;
  auto b = typical();
  b.segs[0].memsz = 4;
  EXPECT_THROW(rvx::load_elf(b.build(), mem), LoadError);
}

TEST(FlatLoader, LoadsWholeFileAtBase) {
  const auto path = std::filesystem::temp_directory_path() / "rvx_flat_loader_test.bin";
  {
    std::ofstream os(path, std::ios::binary);
    const char bytes[] = {0x13, 0x00, 0x00, 0x00, 0x6F, 0x00, 0x00, 0x00};
    os.write(bytes, sizeof bytes);
  }
  Memory mem;
  auto r = rvx::load_flat_file(path.string(), 0x1000, mem);
  EXPECT_EQ(r.entry, 0x1000u);
  EXPECT_EQ(mem.load(0x1000, 4), 0x13u);
  EXPECT_EQ(mem.load(0x1004, 4), 0x6Fu);
  std::filesystem::remove(path);
}

TEST(FileLoad, MissingFileThrows) {
  Memory mem;
  EXPECT_THROW(rvx::load_elf_file("/nonexistent/definitely_not_here.elf", mem), LoadError);
}
