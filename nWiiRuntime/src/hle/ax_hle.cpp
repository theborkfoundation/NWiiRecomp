// Minimal AXWii ucode high-level emulation.
//
// The AX library builds a command list in main memory each frame and sends
// 0xBABE<size> followed by the list address to the DSP. The ucode applies the
// per-voice parameter-block update lists and mixes into the output buffer that
// AI/ADMA plays. This implements that path for PCM16/PCM8 voices; ADPCM and
// the auxiliary/surround/compressor paths are still stubs.

#include "runtime/ax_hle.h"
#include "runtime/ax/ax_structs.h"
#include "runtime/cpu_context.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nwii::runtime {

extern MMU *g_mmu;

namespace {

inline uint8_t rd8(uint32_t a) { return g_mmu->read8(a); }
inline uint16_t rd16(uint32_t a) { return g_mmu->read16(a); }
inline uint32_t rd32(uint32_t a) { return g_mmu->read32(a); }
inline void wr16(uint32_t a, uint16_t v) { g_mmu->write16(a, v); }
inline void wr32(uint32_t a, uint32_t v) { g_mmu->write32(a, v); }
inline uint32_t hilo(uint16_t hi, uint16_t lo) {
  return ((uint32_t)hi << 16) | lo;
}
inline int16_t clamp16(int32_t v) {
  return (int16_t)std::clamp<int32_t>(v, -0x8000, 0x7FFF);
}

bool axdbg() {
  static const bool v = std::getenv("NWII_AXDBG") != nullptr;
  return v;
}

void read_pb(uint32_t addr, ax::AXPBWii &pb) {
  uint16_t *p = reinterpret_cast<uint16_t *>(&pb);
  for (size_t i = 0; i < sizeof(pb) / 2; ++i)
    p[i] = rd16(addr + (uint32_t)i * 2);
}

void write_pb(uint32_t addr, const ax::AXPBWii &pb) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(&pb);
  for (size_t i = 0; i < sizeof(pb) / 2; ++i)
    wr16(addr + (uint32_t)i * 2, p[i]);
}

} // namespace

AXHle &AXHle::get() {
  static AXHle s;
  return s;
}

void AXHle::set_mail_sink(void (*sink)(uint16_t)) { mail_sink_ = sink; }

void AXHle::push_mail(uint16_t lo) {
  if (mail_sink_)
    mail_sink_(lo);
}

bool AXHle::handle_mail(uint32_t mail) {
  const uint32_t hi = mail & 0xFFFF0000u;
  const uint16_t lo = (uint16_t)mail;

  switch (state_) {
  case State::CmdListSize:
    if (hi == 0xBABE0000u) {
      cmd_size_ = lo;
      state_ = State::CmdListAddr;
      return true;
    }
    if (hi == 0xCDD10000u) {
      // Task mail while idle: ACK a resume so the library can proceed.
      if (lo == 0)
        push_mail(0x0001);
      state_ = State::CmdListSize;
      return true;
    }
    return false;

  case State::CmdListAddr:
    // Any mail here is the command list address for the size just given.
    copy_cmdlist(mail, cmd_size_);
    run_cmds();
    cmd_size_ = 0;
    state_ = State::NextTask;
    return true;

  case State::NextTask:
    if (hi == 0xCDD10000u) {
      if (lo == 0)
        push_mail(0x0001); // DSP_RESUME
      state_ = State::CmdListSize;
      return true;
    }
    if (hi == 0xBABE0000u) {
      cmd_size_ = lo;
      state_ = State::CmdListAddr;
      return true;
    }
    return false;
  }
  return false;
}

void AXHle::copy_cmdlist(uint32_t addr, uint16_t size) {
  if (size > sizeof(cmd_) / 2)
    size = sizeof(cmd_) / 2;
  for (uint16_t i = 0; i < size; ++i)
    cmd_[i] = rd16(addr + (uint32_t)i * 2);
  if (axdbg())
    std::printf("[AX] cmdlist addr=%08X size=%u\n", addr, size);
}

void AXHle::run_cmds() {
  uint32_t pb_addr = 0;
  uint32_t i = 0;
  bool end = false;
  uint32_t guard = 0;
  while (!end && i < sizeof(cmd_) / 2 && guard++ < 512) {
    const uint16_t cmd = cmd_[i++];
    switch (cmd) {
    case 0x00: { // SETUP
      const uint32_t init = ((uint32_t)cmd_[i] << 16) | cmd_[i + 1];
      i += 2;
      if (axdbg())
        std::printf("[AX] SETUP init=%08X\n", init);
      setup();
      break;
    }
    case 0x01:
    case 0x02: { // ADD/SUB_TO_LR
      const uint32_t a = ((uint32_t)cmd_[i] << 16) | cmd_[i + 1];
      i += 2;
      if (axdbg())
        std::printf("[AX] %s addr=%08X\n", cmd == 1 ? "ADD_TO_LR" : "SUB_TO_LR", a);
      add_to_lr(a, cmd == 2);
      break;
    }
    case 0x03: { // ADD_SUB_TO_LR
      const uint32_t a = ((uint32_t)cmd_[i] << 16) | cmd_[i + 1];
      i += 2;
      if (axdbg())
        std::printf("[AX] ADD_SUB_TO_LR addr=%08X\n", a);
      break;
    }
    case 0x04: { // PROCESS
      pb_addr = ((uint32_t)cmd_[i] << 16) | cmd_[i + 1];
      i += 2;
      if (axdbg())
        std::printf("[AX] PROCESS pb=%08X\n", pb_addr);
      process_pbs(pb_addr);
      break;
    }
    case 0x05:
    case 0x06:
    case 0x07: // MIX_AUX A/B/C
      i += 5;
      break;
    case 0x08:
    case 0x09: // UPL_AUXx_MIX_LRSC
      i += 13;
      break;
    case 0x0A: // COMPRESSOR
      i += 4;
      break;
    case 0x0B:
    case 0x0C: { // OUTPUT / OUTPUT_DPL2
      const uint16_t volume = cmd_[i++];
      const uint32_t surround = ((uint32_t)cmd_[i] << 16) | cmd_[i + 1];
      const uint32_t lr = ((uint32_t)cmd_[i + 2] << 16) | cmd_[i + 3];
      i += 4;
      if (axdbg())
        std::printf("[AX] OUTPUT vol=%04X surround=%08X lr=%08X\n", volume,
                    surround, lr);
      output(lr, surround, volume);
      break;
    }
    case 0x0D: // WM_OUTPUT
      i += 8;
      break;
    case 0x0E: // END
      end = true;
      break;
    default:
      if (axdbg())
        std::printf("[AX] unknown cmd=%02X at %u\n", cmd, i - 1);
      end = true;
      break;
    }
  }
}

void AXHle::setup() {
  std::fill(std::begin(main_l_), std::end(main_l_), 0);
  std::fill(std::begin(main_r_), std::end(main_r_), 0);
  std::fill(std::begin(main_s_), std::end(main_s_), 0);
}

void AXHle::add_to_lr(uint32_t addr, bool neg) {
  for (int i = 0; i < 96; ++i) {
    int32_t v = (int32_t)rd32(addr + (uint32_t)i * 4);
    if (neg)
      v = -v;
    main_l_[i] += v;
    main_r_[i] += v;
  }
}

void AXHle::process_pbs(uint32_t addr) {
  static int dbg_list = 0;
  const bool dbg = axdbg() && dbg_list < 2;
  int voices = 0, running = 0, with_updates = 0, start_updates = 0;
  while (addr) {
    ax::AXPBWii pb;
    read_pb(addr, pb);

    // The AX library starts voices through the per-millisecond update lists
    // instead of writing PB.running directly. Apply them before mixing.
    const uint16_t n0 = pb.updates.num_updates[0];
    const uint16_t n1 = pb.updates.num_updates[1];
    const uint16_t n2 = pb.updates.num_updates[2];
    if (n0 | n1 | n2)
      ++with_updates;
    if (dbg && voices < 4)
      std::printf("[AX] PB %08X run=%u next=%08X upd=%u,%u,%u fmt=%u\n", addr,
                  pb.running, hilo(pb.next_pb_hi, pb.next_pb_lo), n0, n1, n2,
                  pb.audio_addr.sample_format);
    if (n0 | n1 | n2) {
      struct PBUpdate { uint16_t pb_offset, new_value; };
      PBUpdate updates[32];
      const uint32_t ua = hilo(pb.updates.data_hi, pb.updates.data_lo);
      for (int k = 0; k < 32; ++k) {
        updates[k].pb_offset = rd16(ua + (uint32_t)k * 4);
        updates[k].new_value = rd16(ua + (uint32_t)k * 4 + 2);
      }
      const uint16_t counts[3] = {n0, n1, n2};
      uint32_t start = 0;
      for (int ms = 0; ms < 3; ++ms) {
        const uint16_t count = std::min<uint16_t>(counts[ms], 32);
        for (uint16_t k = 0; k < count && start + k < 32; ++k) {
          const uint16_t off = updates[start + k].pb_offset;
          if (off < sizeof(pb) / 2)
            reinterpret_cast<uint16_t *>(&pb)[off] = updates[start + k].new_value;
          if (off == 8 && updates[start + k].new_value == 1)
            ++start_updates;
          if (dbg && voices < 4 && k < 8)
            std::printf("[AX]   upd ms=%d off=%u val=%04X\n", ms, off,
                        updates[start + k].new_value);
        }
        start += counts[ms];
        process_voice(pb, ms * 32, 32);
      }
    } else {
      process_voice(pb, 0, 96);
    }

    ++voices;
    if (pb.running == 1)
      ++running;
    write_pb(addr, pb);
    addr = hilo(pb.next_pb_hi, pb.next_pb_lo);
    if (voices > 200)
      break;
  }
  if (axdbg() && dbg_list < 2) {
    std::printf("[AX] PROCESS voices=%d running=%d with_updates=%d start=%d\n",
                voices, running, with_updates, start_updates);
    ++dbg_list;
  }
}

void AXHle::process_voice(ax::AXPBWii &pb, int offset, int count) {
  if (pb.running != 1)
    return;

  const uint32_t fmt = pb.audio_addr.sample_format;
  uint32_t addr = hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo);
  const uint32_t end = hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo);
  const uint32_t loop = hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo);
  uint32_t ratio = hilo(pb.src.ratio_hi, pb.src.ratio_lo);
  if (ratio == 0)
    ratio = 0x10000;
  uint32_t frac = pb.src.cur_addr_frac;

  const uint32_t mc = ((uint32_t)pb.mixer_control_hi << 16) | pb.mixer_control_lo;
  const bool mix_l = (mc & 0x0001) || (mc & 0x0004);
  const bool mix_r = (mc & 0x0002) || (mc & 0x0004);
  const bool ramp = (mc & 0x0004) != 0;

  int32_t vol = (uint16_t)pb.vol_env.cur_volume;
  const int32_t vdelta = (int16_t)pb.vol_env.cur_volume_delta;
  int32_t ml = mix_l ? (int32_t)pb.mixer.main_left.volume : 0;
  int32_t mr = mix_r ? (int32_t)pb.mixer.main_right.volume : 0;
  int32_t mdl = ramp ? (int16_t)pb.mixer.main_left.volume_delta : 0;
  int32_t mdr = ramp ? (int16_t)pb.mixer.main_right.volume_delta : 0;

  const uint32_t bytes = (fmt == 1) ? 2 : (fmt == 2) ? 1 : 0;
  if (bytes == 0) {
    // ADPCM: not decoded yet.
    static bool warned = false;
    if (!warned && axdbg()) {
      warned = true;
      std::printf("[AX] ADPCM voice unsupported (addr=%08X)\n", addr);
    }
    return;
  }

  for (int i = 0; i < count; ++i) {
    if (end && addr > end) {
      if (pb.audio_addr.looping) {
        addr = loop;
        frac = 0;
      } else {
        pb.running = 0;
        break;
      }
    }
    int32_t s;
    if (fmt == 1)
      s = (int16_t)rd16(addr);
    else
      s = ((int8_t)rd8(addr)) << 8;

    frac += ratio;
    addr += (frac >> 16) * bytes;
    frac &= 0xFFFF;

    s = (s * vol) >> 15;
    vol += vdelta;

    if (mix_l)
      main_l_[offset + i] += (s * ml) >> 15;
    if (mix_r)
      main_r_[offset + i] += (s * mr) >> 15;
    ml += mdl;
    mr += mdr;
  }

  pb.audio_addr.cur_addr_hi = (uint16_t)(addr >> 16);
  pb.audio_addr.cur_addr_lo = (uint16_t)addr;
  pb.src.cur_addr_frac = (uint16_t)frac;
  pb.vol_env.cur_volume = (uint16_t)vol;
  if (mix_l)
    pb.mixer.main_left.volume = (uint16_t)ml;
  if (mix_r)
    pb.mixer.main_right.volume = (uint16_t)mr;
}

void AXHle::output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume) {
  // Global volume ramp over the 96 samples.
  float ramp = last_volume_;
  const float step = ((float)volume - (float)last_volume_) / 96.0f;
  last_volume_ = volume;

  if (surround_addr) {
    for (int i = 0; i < 96; ++i)
      wr32(surround_addr + (uint32_t)i * 4, (uint32_t)main_s_[i]);
  }

  // NWII_AXTONE=1: mix a 440 Hz test tone into the output so the
  // AX -> AI/ADMA -> SDL path can be verified without any guest voices.
  static const bool tone_on = std::getenv("NWII_AXTONE") != nullptr;
  static double tone_phase = 0.0;
  const double tone_step = 2.0 * 3.141592653589793 * 440.0 / 19200.0;

  for (int i = 0; i < 96; ++i) {
    ramp += step;
    const int32_t v = (int32_t)ramp;
    int32_t t = 0;
    if (tone_on) {
      t = (int32_t)(std::sin(tone_phase) * 6000.0);
      tone_phase += tone_step;
      if (tone_phase > 6.283185307179586)
        tone_phase -= 6.283185307179586;
    }
    const int16_t l = clamp16((int32_t)(((int64_t)main_l_[i] * v) >> 15) + t);
    const int16_t r = clamp16((int32_t)(((int64_t)main_r_[i] * v) >> 15) + t);
    if (lr_addr) {
      wr16(lr_addr + (uint32_t)i * 4, (uint16_t)r);
      wr16(lr_addr + (uint32_t)i * 4 + 2, (uint16_t)l);
    }
  }

  // AX ucode signals frame completion with DSP_SYNC (0xDCD10004).
  // Completion mail. DSP_SYNC (4) is what the real AX ucode sends; the value
  // is configurable for experiments because the library's task state machine
  // is sensitive to it.
  static const uint16_t sync_mail = []() {
    const char *e = std::getenv("NWII_AXSYNC");
    return e ? (uint16_t)std::strtoul(e, nullptr, 16) : (uint16_t)0x0002;
  }();
  push_mail(sync_mail);
}

} // namespace nwii::runtime
