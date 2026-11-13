#include "runtime/devices.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

namespace nwii::runtime::devices {

struct VNandFile {
    std::string path;
    std::vector<uint8_t> data;
};

static std::vector<VNandFile> g_vnand_files;
static bool g_vnand_init = false;

// Build a spec-correct /shared2/sys/SYSCONF (Wiibrew: 4-byte "SCv0" magic,
// BE item count, one BE offset per item plus a past-the-end offset, entries of
// type/name/data, padded to 0x4000 and closed by "SCed"). The previous file
// had a malformed header and a single bogus entry, so every settings lookup by
// the game failed. Defaults follow Dolphin's InsertDefaultEntries, plus an
// explicit stereo IPL.SND.
static std::vector<uint8_t> make_sysconf() {
    enum Type : uint8_t {
        BigArray = 1, SmallArray = 2, Byte = 3, Short = 4,
        Long = 5, LongLong = 6, ByteBool = 7
    };
    struct Item { uint8_t type; const char *name; std::vector<uint8_t> data; };
    std::vector<Item> items;
    auto add = [&](uint8_t type, const char *name, std::vector<uint8_t> data) {
        items.push_back({type, name, std::move(data)});
    };
    auto bytes = [](std::initializer_list<uint8_t> b) {
        return std::vector<uint8_t>(b);
    };

    add(BigArray, "BT.DINF", std::vector<uint8_t>(0x461, 0));
    add(BigArray, "BT.CDIF", std::vector<uint8_t>(0x205, 0));
    add(Long, "BT.SENS", bytes({0, 0, 0, 3}));
    add(Byte, "BT.BAR", bytes({1}));
    add(Byte, "BT.SPKV", bytes({0x58}));
    add(Byte, "BT.MOT", bytes({1}));

    std::vector<uint8_t> nick(22, 0);
    for (int i = 0; i < 7; ++i) nick[(size_t)i * 2 + 1] = (uint8_t)"dolphin"[i];
    nick[21] = 7;
    add(SmallArray, "IPL.NIK", std::move(nick));
    add(Byte, "IPL.LNG", bytes({1}));

    std::vector<uint8_t> sadr(0x1008, 0);
    sadr[0] = 0x6c;
    add(BigArray, "IPL.SADR", std::move(sadr));

    auto pc = std::vector<uint8_t>(0x4A, 0);
    pc[1] = 0x04; pc[2] = 0x14;
    add(SmallArray, "IPL.PC", std::move(pc));

    add(Long, "IPL.CB", bytes({0, 0, 0, 0}));
    add(Byte, "IPL.AR", bytes({0}));       // 4:3
    add(Byte, "IPL.SSV", bytes({1}));
    add(ByteBool, "IPL.CD", bytes({0}));
    add(ByteBool, "IPL.CD2", bytes({0}));
    add(ByteBool, "IPL.EULA", bytes({1}));
    add(Byte, "IPL.UPT", bytes({2}));
    add(Byte, "IPL.PGS", bytes({0}));
    add(Byte, "IPL.E60", bytes({1}));
    add(Byte, "IPL.DH", bytes({0}));
    add(Long, "IPL.INC", bytes({0, 0, 0, 8}));
    add(Long, "IPL.FRC", bytes({0, 0, 0, 0x28}));
    add(SmallArray, "IPL.IDL", bytes({0, 1}));
    add(Byte, "IPL.SND", bytes({1}));      // stereo

    add(Long, "NET.WCFG", bytes({0, 0, 0, 1}));
    add(Long, "NET.CTPC", bytes({0, 0, 0, 0}));
    add(Byte, "WWW.RST", bytes({0}));
    add(ByteBool, "MPLS.MOVIE", bytes({1}));

    std::vector<uint8_t> sc;
    auto put8 = [&](uint8_t v) { sc.push_back(v); };
    auto put16 = [&](uint16_t v) { put8(v >> 8); put8(v & 0xFF); };

    sc.insert(sc.end(), {'S', 'C', 'v', '0'});
    put16((uint16_t)items.size());
    const size_t entries_begin = sc.size() + 2 * (items.size() + 1);

    std::vector<uint8_t> entries;
    for (const auto &it : items) {
        put16((uint16_t)(entries_begin + entries.size()));
        const size_t name_len = std::strlen(it.name);
        entries.push_back((uint8_t)((it.type << 5) | (name_len - 1)));
        entries.insert(entries.end(), it.name, it.name + name_len);
        if (it.type == BigArray || it.type == SmallArray) {
            const size_t n = it.data.size();
            if (it.type == BigArray) {
                entries.push_back((uint8_t)((n - 1) >> 8));
                entries.push_back((uint8_t)((n - 1) & 0xFF));
            } else {
                entries.push_back((uint8_t)(n - 1));
            }
        }
        entries.insert(entries.end(), it.data.begin(), it.data.end());
    }
    put16((uint16_t)(entries_begin + entries.size()));
    sc.insert(sc.end(), entries.begin(), entries.end());
    sc.resize(0x4000, 0);
    sc[0x3FFC] = 'S'; sc[0x3FFD] = 'C'; sc[0x3FFE] = 'e'; sc[0x3FFF] = 'd';
    return sc;
}

static std::vector<uint8_t> make_setting_txt() {
    const char* txt =
        "AREA=USA\r\n"
        "MODEL=RVL-001\r\n"
        "DVD=0\r\n"
        "MPCH=0x7FFE\r\n"
        "CODE=0\r\n"
        "SERNO=123456789\r\n"
        "VIDEO=NTSC\r\n"
        "GAME=US\r\n";
    return std::vector<uint8_t>(txt, txt + strlen(txt));
}

static void vnand_init() {
    if (g_vnand_init) return;
    g_vnand_init = true;
    g_vnand_files.push_back({"/shared2/sys/SYSCONF", make_sysconf()});
    g_vnand_files.push_back({"/title/00000001/00000002/data/setting.txt", make_setting_txt()});
    g_vnand_files.push_back({"/title/00000001/00000002/data/play_rec.dat", std::vector<uint8_t>(0x20, 0)});
    g_vnand_files.push_back({"/shared2/test2/dvderror.dat", std::vector<uint8_t>(4, 0)});
}

static constexpr int VNAND_FD_BASE = 20;
static constexpr int VNAND_FD_MAX  = 8;
struct VNandHandle { int file_idx = -1; uint32_t pos = 0; bool open = false; };
static VNandHandle g_vnand_handles[VNAND_FD_MAX];

class FSDevice : public IDevice {
public:
    const char* get_name() const override { return "/dev/fs"; }
    bool matches_path(const std::string& path) const override {

        if (path == "/dev/fs") return true;
        if (path.find("/dev/") == 0) return false; 
        return path.find("/") == 0;
    }
    
    int32_t open(CPUContext& ctx, const std::string& path, uint32_t mode) override {
        vnand_init();
        for (int i = 0; i < (int)g_vnand_files.size(); ++i) {
            if (g_vnand_files[i].path == path) {
                for (int s = 0; s < VNAND_FD_MAX; ++s) {
                    if (!g_vnand_handles[s].open) {
                        g_vnand_handles[s] = {i, 0, true};
                        return VNAND_FD_BASE + s;
                    }
                }
                return -106; 
            }
        }
        if (path == "/dev/fs")
            return IPC_OK; 
        if (std::getenv("NWII_DEBUG")) std::cout << "[FS] open: no such file " << path << std::endl;
        return IPC_ENOENT; 
    }
    
    int32_t close(CPUContext& ctx, uint32_t fd) override {
        int slot = fd - VNAND_FD_BASE;
        if (slot >= 0 && slot < VNAND_FD_MAX && g_vnand_handles[slot].open) {
            g_vnand_handles[slot].open = false;
            return IPC_OK;
        }
        return IPC_OK;
    }
    
    int32_t read(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        int slot = fd - VNAND_FD_BASE;
        if (slot >= 0 && slot < VNAND_FD_MAX && g_vnand_handles[slot].open) {
            auto& h = g_vnand_handles[slot];
            auto& f = g_vnand_files[h.file_idx];
            uint32_t avail = f.data.size() - h.pos;
            uint32_t to_read = std::min(len, avail);

            uint32_t virt_ptr = buf;
            if (virt_ptr < 0x01800000 || (virt_ptr >= 0x10000000 && virt_ptr < 0x14000000))
                virt_ptr |= 0x80000000;
                
            if (virt_ptr >= 0x80000000 && virt_ptr < 0x94000000 && to_read > 0) {
                for (uint32_t i = 0; i < to_read; ++i) {
                    ctx.mmu.write8(virt_ptr + i, f.data[h.pos + i]);
                }
            }
            h.pos += to_read;
            return to_read;
        }
        return 0; 
    }
    
    int32_t write(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        int slot = fd - VNAND_FD_BASE;
        if (slot >= 0 && slot < VNAND_FD_MAX && g_vnand_handles[slot].open) {
            auto& h = g_vnand_handles[slot];
            auto& f = g_vnand_files[h.file_idx];
            if (h.pos + len > f.data.size())
                f.data.resize(h.pos + len, 0);

            uint32_t virt_ptr = buf;
            if (virt_ptr < 0x01800000 || (virt_ptr >= 0x10000000 && virt_ptr < 0x14000000))
                virt_ptr |= 0x80000000;

            if (virt_ptr >= 0x80000000 && virt_ptr < 0x94000000 && len > 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    f.data[h.pos + i] = ctx.mmu.read8(virt_ptr + i);
                }
            }
            h.pos += len;
            return len;
        }
        return 0;
    }

    int32_t seek(CPUContext& ctx, uint32_t fd, int32_t offset, int32_t whence) override {
        int slot = fd - VNAND_FD_BASE;
        if (slot >= 0 && slot < VNAND_FD_MAX && g_vnand_handles[slot].open) {
            auto& h = g_vnand_handles[slot];
            auto& f = g_vnand_files[h.file_idx];
            if (whence == 0) h.pos = offset; 
            else if (whence == 1) h.pos += offset; 
            else if (whence == 2) h.pos = f.data.size() + offset; 
            return h.pos;
        }
        return IPC_OK;
    }

    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        if (std::getenv("NWII_DEBUG")) std::cout << "[FS] ioctl fd=" << req.fd << " cmd=0x" << std::hex << req.ioctl_cmd << std::dec << std::endl;
        if (req.ioctl_cmd == 0x02) { // ISFS_IOCTL_GETSTATS
            if (req.out_buf && req.out_size >= 28) {
                ctx.mmu.write32(req.out_buf + 0, 16384);  // Cluster size (16KB)
                ctx.mmu.write32(req.out_buf + 4, 20000);  // Free clusters
                ctx.mmu.write32(req.out_buf + 8, 1000);   // Used clusters
                ctx.mmu.write32(req.out_buf + 12, 0);     // Bad clusters
                ctx.mmu.write32(req.out_buf + 16, 0);     // Reserved clusters
                ctx.mmu.write32(req.out_buf + 20, 5000);  // Free inodes
                ctx.mmu.write32(req.out_buf + 24, 100);   // Used inodes
                return IPC_OK;
            }
        }
        return IPC_OK;
    }

    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        if (std::getenv("NWII_DEBUG")) std::cout << "[FS] ioctlv fd=" << req.fd << " cmd=0x" << std::hex << req.ioctl_cmd 
                  << " vecs=" << req.ioctlv_vecs.size() << std::dec << std::endl;
        if (req.ioctl_cmd == 0x0C) { // ISFS_IOCTL_GETUSAGE
            std::string path;
            if (!req.ioctlv_vecs.empty()) {
                uint32_t p = req.ioctlv_vecs[0].addr;
                for (int i = 0; i < 64; ++i) {
                    char c = (char)ctx.mmu.read8(p + i);
                    if (!c) break;
                    path += c;
                }
            }
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] ISFS_GetUsage path="" << path << """ << std::endl;
            if (req.ioctlv_vecs.size() > 1 && req.ioctlv_vecs[1].addr) {
                ctx.mmu.write32(req.ioctlv_vecs[1].addr, 500); // 500 clusters used
            }
            if (req.ioctlv_vecs.size() > 2 && req.ioctlv_vecs[2].addr) {
                ctx.mmu.write32(req.ioctlv_vecs[2].addr, 10); // 10 inodes used
            }
            return IPC_OK;
        } else if (req.ioctl_cmd == 0x02) { // ISFS_IOCTL_GETSTATS
            if (req.ioctlv_vecs.size() > req.arg_cnt_in) {
                uint32_t p = req.ioctlv_vecs[req.arg_cnt_in].addr;
                ctx.mmu.write32(p + 0, 16384);  // Cluster size (16KB)
                ctx.mmu.write32(p + 4, 20000);  // Free clusters
                ctx.mmu.write32(p + 8, 1000);   // Used clusters
                ctx.mmu.write32(p + 12, 0);     // Bad clusters
                ctx.mmu.write32(p + 16, 0);     // Reserved clusters
                ctx.mmu.write32(p + 20, 5000);  // Free inodes
                ctx.mmu.write32(p + 24, 100);   // Used inodes
                return IPC_OK;
            }
        }
        return IPC_OK;
    }
};

std::unique_ptr<IDevice> create_fs_device() {
    return std::make_unique<FSDevice>();
}

} 
