#include "runtime/devices.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include "runtime/config.h"

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

static static std::string host_save_dir();

void vnand_init() {
    if (g_vnand_init) return;
    g_vnand_init = true;
    g_vnand_files.push_back({"/shared2/sys/SYSCONF", make_sysconf()});
    g_vnand_files.push_back({"/title/00000001/00000002/data/setting.txt", make_setting_txt()});
    g_vnand_files.push_back({"/title/00000001/00000002/data/play_rec.dat", std::vector<uint8_t>(0x20, 0)});
    g_vnand_files.push_back({"/shared2/test2/dvderror.dat", std::vector<uint8_t>(4, 0)});

    // The game saves to /title/00010000/<low title id>/data/slot_N.sav. Create
    // the host-backed directory up front so ISFS_GetAttr sees the title data
    // directory and the game does not fall back to /tmp + root staging.
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(host_save_dir()) /
            "title/00010000/534f4a45/data",
        ec);
}

// Host-backed files for writable guest paths (/tmp/slot_N.sav etc.). The
// synthetic NAND above stays read-only; anything under /tmp is persisted to
// ~/.nwii/saves/<gameid>/... so saves survive a relaunch.
static constexpr int HOST_FD_BASE = 40;
static constexpr int HOST_FD_MAX = 8;
struct HostHandle {
    FILE *f = nullptr;
    std::string path;
    bool open = false;
};
static HostHandle g_host_handles[HOST_FD_MAX];

static std::string host_save_dir() {
    std::string root;
    if (const char *e = std::getenv("NWII_NAND_DIR"))
        root = e;
    else {
        const char *home = std::getenv("HOME");
        root = std::string(home ? home : ".") + "/.nwii/saves/" +
               nwii::runtime::Config::get().game_id;
    }
    return root;
}

static std::string host_path_for(const std::string &guest) {
    std::string rel = guest;
    while (!rel.empty() && rel.front() == '/')
        rel.erase(rel.begin());
    // Refuse path traversal.
    if (rel.find("..") != std::string::npos)
        return {};
    return host_save_dir() + "/" + rel;
}

static std::string guest_path_at(CPUContext &ctx, uint32_t addr, int maxlen = 64) {
    std::string out;
    for (int i = 0; i < maxlen; ++i) {
        char c = (char)ctx.mmu.read8(addr + (uint32_t)i);
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

static bool synthetic_exists(const std::string &path) {
    for (const auto &f : g_vnand_files)
        if (f.path == path) return true;
    return false;
}

static bool create_host_file(const std::string &guest, bool truncate) {
    const std::string hp = host_path_for(guest);
    if (hp.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(hp).parent_path(), ec);
    if (truncate) {
        FILE *f = std::fopen(hp.c_str(), "wb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }
    FILE *f = std::fopen(hp.c_str(), "ab");
    if (!f) return false;
    std::fclose(f);
    return true;
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
                        if (std::getenv("NWII_DEBUG"))
                            std::cout << "[FS] vnand open " << path
                                      << " fd=" << (VNAND_FD_BASE + s) << std::endl;
                        return VNAND_FD_BASE + s;
                    }
                }
                return -106; 
            }
        }
        if (path == "/dev/fs")
            return IPC_OK; 

        // Host-backed /tmp files (save slots). Reads of a missing file create
        // an empty one so the game sees a valid, empty slot; later writes
        // persist directly to the host.
        if (path.rfind("/tmp/", 0) == 0) {
            const std::string hp = host_path_for(path);
            if (!hp.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(hp).parent_path(), ec);
                for (int s = 0; s < HOST_FD_MAX; ++s) {
                    if (!g_host_handles[s].open) {
                        FILE *f = std::fopen(hp.c_str(), "r+b");
                        if (!f)
                            f = std::fopen(hp.c_str(), "w+b");
                        if (!f) break;
                        g_host_handles[s] = {f, path, true};
                        if (std::getenv("NWII_DEBUG"))
                            std::cout << "[FS] host open " << path << " -> "
                                      << hp << " fd=" << (HOST_FD_BASE + s)
                                      << std::endl;
                        return HOST_FD_BASE + s;
                    }
                }
            }
        }

        // Host-backed non-synthetic paths (e.g. /title/.../data/state.dat)
        // once ISFS_CreateFile has created them.
        {
            const std::string hp = host_path_for(path);
            if (!hp.empty() && std::filesystem::exists(hp)) {
                for (int s = 0; s < HOST_FD_MAX; ++s) {
                    if (!g_host_handles[s].open) {
                        FILE *f = std::fopen(hp.c_str(), "r+b");
                        if (!f) break;
                        g_host_handles[s] = {f, path, true};
                        if (std::getenv("NWII_DEBUG"))
                            std::cout << "[FS] host open " << path << " -> " << hp
                                      << " fd=" << (HOST_FD_BASE + s) << std::endl;
                        return HOST_FD_BASE + s;
                    }
                }
            }
        }

        if (std::getenv("NWII_DEBUG")) std::cout << "[FS] open: no such file " << path
                                                << " mode=0x" << std::hex << mode << std::dec << std::endl;
        return IPC_ENOENT; 
    }
    
    int32_t close(CPUContext& ctx, uint32_t fd) override {
        int slot = fd - VNAND_FD_BASE;
        if (slot >= 0 && slot < VNAND_FD_MAX && g_vnand_handles[slot].open) {
            g_vnand_handles[slot].open = false;
            return IPC_OK;
        }
        int hs = (int)fd - HOST_FD_BASE;
        if (hs >= 0 && hs < HOST_FD_MAX && g_host_handles[hs].open) {
            std::fflush(g_host_handles[hs].f);
            std::fclose(g_host_handles[hs].f);
            g_host_handles[hs] = {};
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
        int hs = (int)fd - HOST_FD_BASE;
        if (hs >= 0 && hs < HOST_FD_MAX && g_host_handles[hs].open) {
            auto &hh = g_host_handles[hs];
            std::vector<uint8_t> tmp(len);
            const size_t got = std::fread(tmp.data(), 1, len, hh.f);
            uint32_t virt_ptr = buf;
            if (virt_ptr < 0x01800000 || (virt_ptr >= 0x10000000 && virt_ptr < 0x14000000))
                virt_ptr |= 0x80000000;
            if (virt_ptr >= 0x80000000 && virt_ptr < 0x94000000)
                for (size_t i = 0; i < got; ++i)
                    ctx.mmu.write8(virt_ptr + (uint32_t)i, tmp[i]);
            return (int32_t)got;
        }
        return 0; 
    }
    
    int32_t write(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        if (std::getenv("NWII_DEBUG"))
            std::cout << "[FS] write fd=" << fd << " len=" << len << std::endl;
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
        int hs = (int)fd - HOST_FD_BASE;
        if (hs >= 0 && hs < HOST_FD_MAX && g_host_handles[hs].open) {
            auto &hh = g_host_handles[hs];
            uint32_t virt_ptr = buf;
            if (virt_ptr < 0x01800000 || (virt_ptr >= 0x10000000 && virt_ptr < 0x14000000))
                virt_ptr |= 0x80000000;
            std::vector<uint8_t> tmp(len);
            if (virt_ptr >= 0x80000000 && virt_ptr < 0x94000000)
                for (uint32_t i = 0; i < len; ++i)
                    tmp[i] = ctx.mmu.read8(virt_ptr + i);
            const long off = std::ftell(hh.f);
            const size_t put = std::fwrite(tmp.data(), 1, len, hh.f);
            std::fflush(hh.f);
            if (std::getenv("NWII_DEBUG"))
                std::cout << "[FS] host write " << hh.path << " off=" << off
                          << " len=" << len << std::endl;
            return (int32_t)put;
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
        int hs = (int)fd - HOST_FD_BASE;
        if (hs >= 0 && hs < HOST_FD_MAX && g_host_handles[hs].open) {
            int whence_f = (whence == 0) ? SEEK_SET : (whence == 1) ? SEEK_CUR : SEEK_END;
            std::fseek(g_host_handles[hs].f, offset, whence_f);
            return (int32_t)std::ftell(g_host_handles[hs].f);
        }
        return IPC_OK;
    }

    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        if (std::getenv("NWII_DEBUG")) std::cout << "[FS] ioctl fd=" << req.fd << " cmd=0x" << std::hex << req.ioctl_cmd
                  << std::dec << " in=" << req.in_buf << "/" << req.in_size
                  << " out=" << req.out_buf << "/" << req.out_size << std::endl;
        if (req.ioctl_cmd == 0x03) { // CreateDir (owner/group + 0x40 name + perms)
            std::string path = guest_path_at(ctx, req.in_buf + 6);
            const std::string hp = host_path_for(path);
            if (!hp.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(hp, ec);
            }
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] CreateDir " << path << std::endl;
            return IPC_OK;
        }
        if (req.ioctl_cmd == 0x06) { // GetAttr
            std::string path = guest_path_at(ctx, req.in_buf);
            bool ok = synthetic_exists(path) || (!host_path_for(path).empty() &&
                                                std::filesystem::exists(host_path_for(path)));
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] GetAttr " << path << " -> " << (ok ? "OK" : "ENOENT") << std::endl;
            return ok ? IPC_OK : IPC_ENOENT;
        }
        if (req.ioctl_cmd == 0x07) { // Delete
            std::string path = guest_path_at(ctx, req.in_buf);
            const std::string hp = host_path_for(path);
            if (!hp.empty()) { std::error_code ec; std::filesystem::remove(hp, ec); }
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] Delete " << path << std::endl;
            return IPC_OK;
        }
        if (req.ioctl_cmd == 0x08) { // Rename
            std::string a = guest_path_at(ctx, req.in_buf);
            std::string b = guest_path_at(ctx, req.in_buf + 64);
            const std::string ha = host_path_for(a), hb = host_path_for(b);
            if (!ha.empty() && !hb.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(hb).parent_path(), ec);
                std::filesystem::rename(ha, hb, ec);
            }
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] Rename " << a << " -> " << b << std::endl;
            return IPC_OK;
        }
        if (req.ioctl_cmd == 0x09) { // CreateFile
            std::string path = guest_path_at(ctx, req.in_buf + 6);
            bool ok = create_host_file(path, false);
            if (std::getenv("NWII_DEBUG")) std::cout << "[FS] CreateFile " << path << " -> " << (ok ? "OK" : "FAIL") << std::endl;
            return ok ? IPC_OK : IPC_ENOENT;
        }
        if (req.ioctl_cmd == 0x0B) { // GetFileStats (size, seek)
            uint32_t size = 0, pos = 0;
            bool found = false;
            int hs = (int)req.fd - HOST_FD_BASE;
            if (hs >= 0 && hs < HOST_FD_MAX && g_host_handles[hs].open) {
                FILE *f = g_host_handles[hs].f;
                long cur = std::ftell(f);
                std::fseek(f, 0, SEEK_END);
                size = (uint32_t)std::ftell(f);
                std::fseek(f, cur, SEEK_SET);
                pos = (uint32_t)cur;
                found = true;
            }
            int vs = (int)req.fd - VNAND_FD_BASE;
            if (!found && vs >= 0 && vs < VNAND_FD_MAX && g_vnand_handles[vs].open) {
                auto &h = g_vnand_handles[vs];
                size = (uint32_t)g_vnand_files[h.file_idx].data.size();
                pos = h.pos;
                found = true;
            }
            if (std::getenv("NWII_DEBUG"))
                std::cout << "[FS] GetFileStats fd=" << req.fd
                          << " out=" << req.out_buf << "/" << req.out_size
                          << " found=" << found << " size=" << size
                          << " pos=" << pos << std::endl;
            if (found && req.out_buf && req.out_size >= 8) {
                ctx.mmu.write32(req.out_buf + 0, size);
                ctx.mmu.write32(req.out_buf + 4, pos);
            }
            return IPC_OK;
        }
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
