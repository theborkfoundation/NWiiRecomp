// Round 5 device regressions: DI output-buffer bounds, disc reads across a
// region gap, and virtual-NAND writes whose end offset would wrap in 32 bits.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/devices.h"
#include "runtime/hw/hw.h"
#include "runtime/ios_device.h"

namespace nwii::runtime {
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;
} // namespace nwii::runtime

using nwii::runtime::CPUContext;
using nwii::runtime::IpcRequest;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void put_be32(std::vector<uint8_t> &data, size_t off, uint32_t value) {
  data[off + 0] = (uint8_t)(value >> 24);
  data[off + 1] = (uint8_t)(value >> 16);
  data[off + 2] = (uint8_t)(value >> 8);
  data[off + 3] = (uint8_t)value;
}

static void write_file(const std::filesystem::path &path,
                       const std::vector<uint8_t> &data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(data.data()),
            (std::streamsize)data.size());
}

// Minimal extracted-disc layout: boot.bin, fst.bin and one file at 0x2440.
// A read at 0x2430 therefore crosses a gap before valid data.
static void make_disc_fixture(const std::filesystem::path &root) {
  namespace fs = std::filesystem;
  fs::create_directories(root / "sys");
  fs::create_directories(root / "files");

  std::vector<uint8_t> boot(0x440, 0);
  put_be32(boot, 0x18, 0x5D1C9EA3u); // Wii disc marker
  put_be32(boot, 0x424, 0x1000u >> 2); // FST offset
  put_be32(boot, 0x428, 0);            // use the actual FST size
  write_file(root / "sys" / "boot.bin", boot);

  std::vector<uint8_t> fst(30, 0);
  fst[0] = 1;        // root directory
  put_be32(fst, 4, 0); // root name offset
  put_be32(fst, 8, 2); // two entries
  fst[12] = 0;         // file "a.bin"
  fst[13] = fst[14] = fst[15] = 0;
  put_be32(fst, 16, 0x2440u >> 2);
  put_be32(fst, 20, 16);
  std::memcpy(&fst[24], "a.bin", 6);
  write_file(root / "sys" / "fst.bin", fst);

  write_file(root / "files" / "a.bin", std::vector<uint8_t>(16, 0x5A));
}

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "nwii_device_bounds_test";
  fs::remove_all(root);
  fs::create_directories(root);
  make_disc_fixture(root / "disc");
  const std::string nand_dir = (root / "nand").string();
  setenv("NWII_NAND_DIR", nand_dir.c_str(), 1);

  auto &config = nwii::runtime::Config::get();
  config.game_dir = (root / "disc").string();
  config.game_id = "SOJE41";

  CPUContext ctx;
  nwii::runtime::g_ctx_ptr = &ctx;
  nwii::runtime::g_mmu = &ctx.mmu;

  // I-027: a disc read that spans a gap must keep the data after the gap.
  auto &mmio = nwii::runtime::MMIODispatcher::get();
  nwii::runtime::hw::register_di(mmio);
  const uint32_t dma_dst = 0x80020000;
  mmio.write32(0xCC006008, 0xA8u << 24);        // read command
  mmio.write32(0xCC00600C, 0x2430u >> 2);       // disc offset, in words
  mmio.write32(0xCC006014, dma_dst & 0x01FFFFFFu);
  mmio.write32(0xCC006018, 32);
  mmio.write32(0xCC00601C, 1);                  // start
  for (uint32_t i = 0; i < 16; ++i)
    CHECK(ctx.mmu.read8(dma_dst + i) == 0);
  for (uint32_t i = 0; i < 16; ++i)
    CHECK(ctx.mmu.read8(dma_dst + 16 + i) == 0x5A);

  // I-026: the embedded length must not exceed the declared output buffer.
  auto di = nwii::runtime::devices::create_di_device();
  const uint32_t in = 0x80030000;
  const uint32_t out = 0x80040000;
  ctx.mmu.write32(in + 0, 0x71u << 24);
  ctx.mmu.write32(in + 4, 64);           // requested length
  ctx.mmu.write32(in + 8, 0x2440u >> 2); // disc offset, in words
  for (uint32_t i = 0; i < 64; ++i)
    ctx.mmu.write8(out + i, 0xAA);

  IpcRequest req{};
  req.ioctl_cmd = 0x71;
  req.in_buf = in;
  req.in_size = 12;
  req.out_buf = out;
  req.out_size = 4;
  CHECK(di->ioctl(ctx, req) == 1);
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(ctx.mmu.read8(out + i) == 0x5A);
  for (uint32_t i = 4; i < 64; ++i)
    CHECK(ctx.mmu.read8(out + i) == 0xAA);

  // A destination span that leaves RAM is rejected instead of running past the
  // end of the backing memory.
  const uint32_t edge = 0x817FFFFC; // last four bytes of 24 MiB MEM1
  for (uint32_t i = 0; i < 4; ++i)
    ctx.mmu.write8(edge + i, 0xAA);
  req.out_buf = edge;
  req.out_size = 64;
  ctx.mmu.write32(in + 4, 64);
  CHECK(di->ioctl(ctx, req) == nwii::runtime::IPC_EINVAL);
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(ctx.mmu.read8(edge + i) == 0xAA);

  // I-028: position + length that wraps in 32 bits must not resize/index.
  auto fsdev = nwii::runtime::devices::create_fs_device();
  const int fd = fsdev->open(ctx, "/shared2/test2/dvderror.dat", 0);
  CHECK(fd >= 20);
  CHECK(fsdev->seek(ctx, fd, 0x7FFFFFFF, 0) == 0x7FFFFFFF);
  const uint32_t guest_buf = 0x80050000;
  for (uint32_t i = 0; i < 4; ++i)
    ctx.mmu.write8(guest_buf + i, (uint8_t)(i + 1));
  CHECK(fsdev->write(ctx, fd, guest_buf, 0x80000002u) == -101); // ISFS_EINVAL
  // The same descriptor still works normally afterwards.
  CHECK(fsdev->seek(ctx, fd, 0, 0) == 0);
  CHECK(fsdev->write(ctx, fd, guest_buf, 2) == 2);
  CHECK(fsdev->seek(ctx, fd, 0, 0) == 0);
  CHECK(fsdev->read(ctx, fd, guest_buf, 2) == 2);
  CHECK(ctx.mmu.read8(guest_buf) == 1);
  CHECK(ctx.mmu.read8(guest_buf + 1) == 2);
  fsdev->close(ctx, fd);

  fs::remove_all(root);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("device_bounds: all checks passed\n");
  return 0;
}
