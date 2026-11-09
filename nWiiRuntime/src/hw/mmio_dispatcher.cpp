#include "runtime/hw/mmio_dispatcher.h"
#include <iostream>
#include <algorithm>

namespace nwii::runtime {

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb), nullptr, nullptr});
}

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb,
                                     MMIORegionRead16Callback read16_cb, MMIORegionWrite16Callback write16_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb),
                         std::move(read16_cb), std::move(write16_cb)});
}

uint32_t MMIODispatcher::read32(uint32_t addr) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.read_cb) return region.read_cb(addr);
            return 0;
        }
    }
    // Fallback: Mirror 0xCD00xxxx or 0x0D00xxxx to 0xCC00xxxx
    if ((addr & 0xFF000000) == 0xCD000000 || (addr & 0xFF000000) == 0x0D000000) {
        uint32_t mirrored = (addr & 0x00FFFFFF) | 0xCC000000;
        for (const auto& region : m_regions) {
            if (mirrored >= region.start_addr && mirrored <= region.end_addr) {
                if (region.read_cb) return region.read_cb(mirrored);
                return 0;
            }
        }
    }
    if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[MMIO] Unhandled read32 at 0x" << std::hex << addr << std::dec << std::endl;
    return 0;
}

void MMIODispatcher::write32(uint32_t addr, uint32_t val) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.write_cb) {
                region.write_cb(addr, val);
            }
            return;
        }
    }
    // Fallback: Mirror 0xCD00xxxx or 0x0D00xxxx to 0xCC00xxxx
    if ((addr & 0xFF000000) == 0xCD000000 || (addr & 0xFF000000) == 0x0D000000) {
        uint32_t mirrored = (addr & 0x00FFFFFF) | 0xCC000000;
        for (const auto& region : m_regions) {
            if (mirrored >= region.start_addr && mirrored <= region.end_addr) {
                if (region.write_cb) region.write_cb(mirrored, val);
                return;
            }
        }
    }
    if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[MMIO] Unhandled write32 at 0x" << std::hex << addr << " val=0x" << val << std::dec << std::endl;
}

uint16_t MMIODispatcher::read16(uint32_t addr) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.read16_cb) return region.read16_cb(addr);
            if (region.read_cb) return (uint16_t)region.read_cb(addr);
            return 0;
        }
    }
    if ((addr & 0xFF000000) == 0xCD000000 || (addr & 0xFF000000) == 0x0D000000) {
        uint32_t mirrored = (addr & 0x00FFFFFF) | 0xCC000000;
        for (const auto& region : m_regions) {
            if (mirrored >= region.start_addr && mirrored <= region.end_addr) {
                if (region.read16_cb) return region.read16_cb(mirrored);
                if (region.read_cb) return (uint16_t)region.read_cb(mirrored);
                return 0;
            }
        }
    }
    if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[MMIO] Unhandled read16 at 0x" << std::hex << addr << std::dec << std::endl;
    return 0;
}

void MMIODispatcher::write16(uint32_t addr, uint16_t val) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.write16_cb) region.write16_cb(addr, val);
            else if (region.write_cb) region.write_cb(addr, val);
            return;
        }
    }
    if ((addr & 0xFF000000) == 0xCD000000 || (addr & 0xFF000000) == 0x0D000000) {
        uint32_t mirrored = (addr & 0x00FFFFFF) | 0xCC000000;
        for (const auto& region : m_regions) {
            if (mirrored >= region.start_addr && mirrored <= region.end_addr) {
                if (region.write16_cb) region.write16_cb(mirrored, val);
                else if (region.write_cb) region.write_cb(mirrored, val);
                return;
            }
        }
    }
    if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[MMIO] Unhandled write16 at 0x" << std::hex << addr << " val=0x" << val << std::dec << std::endl;
}

void MMIODispatcher::clear() {
    m_regions.clear();
}

} 
