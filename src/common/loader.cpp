#include "common/loader.h"

#include <fstream>
#include <iterator>

namespace rvx {
namespace {

// ELF32 constants (System V gABI). Only what we use.
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfDataLsb = 1;
constexpr uint16_t kEmRiscv = 243;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kEhdrSize = 52;
constexpr uint32_t kPhdrSize = 32;
constexpr uint32_t kShdrSize = 40;
constexpr uint32_t kSymSize = 16;
constexpr uint8_t kStbGlobal = 1;

// Bounds-checked little-endian reader over the file image. Fields are read
// by explicit offset rather than by casting the bytes to a struct, so host
// endianness and struct padding never matter.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> img) : img_(img) {}

  void require(uint64_t off, uint64_t len, const char* what) const {
    if (off > img_.size() || len > img_.size() - off) {
      throw LoadError(std::string("ELF: ") + what + " extends past end of file");
    }
  }
  uint8_t u8(uint64_t off) const {
    require(off, 1, "field");
    return img_[off];
  }
  uint16_t u16(uint64_t off) const {
    require(off, 2, "field");
    return static_cast<uint16_t>(img_[off] | (img_[off + 1] << 8));
  }
  uint32_t u32(uint64_t off) const {
    require(off, 4, "field");
    return uint32_t{img_[off]} | (uint32_t{img_[off + 1]} << 8) | (uint32_t{img_[off + 2]} << 16) |
           (uint32_t{img_[off + 3]} << 24);
  }
  std::span<const uint8_t> bytes(uint64_t off, uint64_t len, const char* what) const {
    require(off, len, what);
    return img_.subspan(off, len);
  }
  // NUL-terminated string starting at `off`, which must lie in [lo, hi).
  std::string cstr(uint64_t off, uint64_t hi) const {
    std::string s;
    for (uint64_t k = off; k < hi; ++k) {
      if (img_[k] == 0) return s;
      s.push_back(static_cast<char>(img_[k]));
    }
    throw LoadError("ELF: unterminated symbol name");
  }

 private:
  std::span<const uint8_t> img_;
};

void read_symbols(const Reader& r, uint32_t shoff, uint16_t shnum, LoadResult& out) {
  r.require(shoff, uint64_t{shnum} * kShdrSize, "section header table");
  for (uint16_t s = 0; s < shnum; ++s) {
    const uint64_t sh = shoff + uint64_t{s} * kShdrSize;
    if (r.u32(sh + 4) != kShtSymtab) continue;
    const uint32_t sym_off = r.u32(sh + 16);
    const uint32_t sym_size = r.u32(sh + 20);
    const uint32_t link = r.u32(sh + 24);
    if (link >= shnum) throw LoadError("ELF: symtab sh_link out of range");
    const uint64_t str_sh = shoff + uint64_t{link} * kShdrSize;
    const uint32_t str_off = r.u32(str_sh + 16);
    const uint32_t str_size = r.u32(str_sh + 20);
    r.require(sym_off, sym_size, "symbol table");
    r.require(str_off, str_size, "string table");

    for (uint64_t e = sym_off; e + kSymSize <= uint64_t{sym_off} + sym_size; e += kSymSize) {
      const uint32_t name = r.u32(e + 0);
      if (name == 0) continue;  // unnamed (e.g. the null symbol, section symbols)
      if (name >= str_size) throw LoadError("ELF: symbol name offset out of range");
      const std::string n = r.cstr(uint64_t{str_off} + name, uint64_t{str_off} + str_size);
      const uint32_t value = r.u32(e + 4);
      const bool global = (r.u8(e + 12) >> 4) == kStbGlobal;
      // If a name appears more than once, a global definition wins over locals.
      auto [it, inserted] = out.symbols.emplace(n, value);
      if (!inserted && global) it->second = value;
    }
  }
}

}  // namespace

LoadResult load_elf(std::span<const uint8_t> image, Memory& mem) {
  const Reader r(image);
  r.require(0, kEhdrSize, "ELF header");
  if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F')) {
    throw LoadError("not an ELF file (bad magic)");
  }
  if (image[4] != kElfClass32) throw LoadError("ELF: not a 32-bit ELF");
  if (image[5] != kElfDataLsb) throw LoadError("ELF: not little-endian");
  if (r.u16(18) != kEmRiscv) throw LoadError("ELF: e_machine is not RISC-V");

  LoadResult out;
  out.entry = r.u32(24);
  const uint32_t phoff = r.u32(28);
  const uint32_t shoff = r.u32(32);
  const uint16_t phentsize = r.u16(42);
  const uint16_t phnum = r.u16(44);
  const uint16_t shentsize = r.u16(46);
  const uint16_t shnum = r.u16(48);

  if (phnum == 0) throw LoadError("ELF: no program headers");
  if (phentsize != kPhdrSize) throw LoadError("ELF: unexpected e_phentsize");
  r.require(phoff, uint64_t{phnum} * kPhdrSize, "program header table");

  for (uint16_t p = 0; p < phnum; ++p) {
    const uint64_t ph = phoff + uint64_t{p} * kPhdrSize;
    if (r.u32(ph) != kPtLoad) continue;
    const uint32_t offset = r.u32(ph + 4);
    const uint32_t paddr = r.u32(ph + 12);
    const uint32_t filesz = r.u32(ph + 16);
    const uint32_t memsz = r.u32(ph + 20);
    if (filesz > memsz) throw LoadError("ELF: segment p_filesz > p_memsz");
    mem.write_block(paddr, r.bytes(offset, filesz, "segment"));
    // Bytes between filesz and memsz (e.g. .bss) must read as zero; write the
    // zeros explicitly in case this memory already held something.
    for (uint32_t k = filesz; k < memsz; ++k) mem.store8(paddr + k, 0);
  }

  if (shoff != 0 && shnum != 0) {
    if (shentsize != kShdrSize) throw LoadError("ELF: unexpected e_shentsize");
    read_symbols(r, shoff, shnum, out);
  }
  return out;
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream is(path, std::ios::binary);
  if (!is) throw LoadError("cannot open " + path);
  return {std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>()};
}

LoadResult load_elf_file(const std::string& path, Memory& mem) {
  const auto img = read_file(path);
  return load_elf(img, mem);
}

LoadResult load_flat_file(const std::string& path, uint32_t base, Memory& mem) {
  const auto img = read_file(path);
  mem.write_block(base, img);
  LoadResult out;
  out.entry = base;
  return out;
}

}  // namespace rvx
