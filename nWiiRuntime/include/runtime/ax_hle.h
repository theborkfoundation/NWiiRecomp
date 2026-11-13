#pragma once

#include <cstdint>

namespace nwii::runtime {
namespace ax {
struct AXPBWii;
}


// Minimal AXWii ucode HLE: command-list mail protocol, PB updates and
// PCM16/PCM8 voice mixing. The guest never starts voices itself; the AX
// library expects the DSP to apply the PB update lists and mix.
class AXHle {
public:
  static AXHle &get();

  // Feed a completed CPU->DSP mail word. Returns true when the AX protocol
  // consumed it (command-list size/address, task mails).
  bool handle_mail(uint32_t mail);

  // Set by the DSP mailbox layer: queue a DSP->CPU mail and raise the DSP
  // interrupt.
  void set_mail_sink(void (*sink)(uint16_t lo));

private:
  AXHle() = default;

  enum class State { NextTask, CmdListSize, CmdListAddr };
  State state_ = State::CmdListSize;
  uint16_t cmd_size_ = 0;
  uint16_t cmd_[1024]{};

  int32_t main_l_[96]{};
  int32_t main_r_[96]{};
  int32_t main_s_[96]{};
  uint16_t last_volume_ = 0;

  void (*mail_sink_)(uint16_t) = nullptr;

  void push_mail(uint16_t lo);
  void copy_cmdlist(uint32_t addr, uint16_t size);
  void run_cmds();

  void setup();
  void add_to_lr(uint32_t addr, bool neg);
  void process_pbs(uint32_t addr);
  void output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume);

  void process_voice(ax::AXPBWii &pb, int offset, int count);
};

} // namespace nwii::runtime
