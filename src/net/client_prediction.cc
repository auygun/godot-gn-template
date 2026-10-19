#include "src/net/client_prediction.h"

#include <algorithm>

#include "src/net/entity.h"
#include "src/net/view_smoother.h"

namespace godot::net {

ClientPrediction::ClientPrediction() = default;

ClientPrediction::~ClientPrediction() = default;

bool ClientPrediction::SkipThisTick() {
  skipped_last_tick_ = skipped_ticks_ > 0;
  if (skipped_last_tick_) {
    skipped_ticks_--;
  }
  return skipped_last_tick_;
}

void ClientPrediction::Predict(Entity* entity,
                               const TimedCommand& command,
                               float dt) {
  sequence_++;
  // 0 means none.
  if (sequence_ == 0) {
    sequence_++;
  }
  entity->SimulateBox(command.command,
                      {dt, SimulationMode::kPredicted, command.view_tick});
  Entry& entry = history_[sequence_ % kHistorySize];
  entry.sequence = sequence_;
  entry.command = command;
  entity->CaptureBox(&entry.state);
  predicted_count_++;
}

InputMessage ClientPrediction::MakeInputMessage() const {
  InputMessage message;
  message.sequence = sequence_;
  // The latest consecutive commands (sequence numbers skipped to catch up
  // have none), oldest first.
  int count = 0;
  while (count < kMaxInputCommands && Find(sequence_ - count)) {
    count++;
  }
  for (int i = count - 1; i >= 0; i--) {
    message.commands.push_back(Find(sequence_ - i)->command);
  }
  return message;
}

void ClientPrediction::Reconcile(Entity* entity,
                                 uint32_t ack,
                                 int input_buffer,
                                 const Box& server_state,
                                 float dt,
                                 ViewSmoother* smoother) {
  AdjustRate(ack, input_buffer);
  const Entry* acked = ack != 0 ? Find(ack) : nullptr;
  // What only the server changes (health, scores) is taken whenever it
  // differs, even for a command already reconciled.
  bool server_fields_same =
      !acked || SameServerFields(acked->state, server_state);
  // The state for an acknowledged command doesn't change otherwise
  // (snapshots that arrive out of order are dropped before this).
  if (ack != 0 && ack == last_ack_ && server_fields_same) {
    return;
  }
  last_ack_ = ack;

  bool matches = acked && PredictionMatches(acked->state, server_state);
  if (matches && server_fields_same) {
    return;
  }
  // A command the server repeated for a sequence number we skipped (see
  // AdjustRate) was never predicted: not a misprediction.
  if (acked && !matches) {
    correction_count_++;
  }

  // Back to the server's state, then forward through the commands it
  // hasn't simulated yet.
  Box predicted;
  entity->CaptureBox(&predicted);
  entity->RestoreBox(server_state);
  uint32_t first = ack + 1;
  if (sequence_ - ack >= kHistorySize) {
    first = sequence_ - kHistorySize + 1;
  }
  // The corrected state before the last tick's command, for the view (the
  // state the last tick started from).
  Box before_last;
  bool replayed_last = false;
  for (uint32_t s = first; s != sequence_ + 1; s++) {
    Entry* entry = Find(s);
    if (!entry) {
      continue;
    }
    if (s == sequence_) {
      entity->CaptureBox(&before_last);
      replayed_last = true;
    }
    entity->SimulateBox(entry->command.command, {dt, SimulationMode::kReplay,
                                                 entry->command.view_tick});
    entity->CaptureBox(&entry->state);
  }
  Box corrected;
  entity->CaptureBox(&corrected);
  if (!SameTeleportFields(predicted, corrected)) {
    smoother->Teleported(corrected);
    entity->OnTeleported();
    return;
  }
  if (skipped_last_tick_) {
    // The last tick simulated nothing: it ended where it started.
    smoother->Corrected(&corrected, corrected);
  } else {
    // Unless the server has simulated the last command too: the tick
    // before is as predicted.
    smoother->Corrected(replayed_last ? &before_last : nullptr, corrected);
  }
}

void ClientPrediction::AdjustRate(uint32_t ack, int input_buffer) {
  if (adjusting_) {
    if (SequenceNewer(adjusted_at_, ack)) {
      return;
    }
    adjusting_ = false;
    window_count_ = 0;
  }

  if (input_buffer < 1) {
    // Our commands arrive late, or just in time: number the next ones as
    // far ahead as they should arrive. The server repeats the last command
    // for the numbers skipped.
    sequence_ += static_cast<uint32_t>(kTargetBuffer - input_buffer);
  } else {
    min_buffer_ =
        window_count_ == 0 ? input_buffer : std::min(min_buffer_, input_buffer);
    if (++window_count_ < kBufferWindow) {
      return;
    }
    window_count_ = 0;
    if (min_buffer_ <= kTargetBuffer) {
      return;
    }
    skipped_ticks_ = min_buffer_ - kTargetBuffer;
  }
  // Either change shows up when the server gets to our next command.
  adjusting_ = true;
  adjusted_at_ = sequence_ + 1;
}

ClientPrediction::Entry* ClientPrediction::Find(uint32_t sequence) {
  Entry& entry = history_[sequence % kHistorySize];
  return sequence != 0 && entry.sequence == sequence ? &entry : nullptr;
}

const ClientPrediction::Entry* ClientPrediction::Find(uint32_t sequence) const {
  const Entry& entry = history_[sequence % kHistorySize];
  return sequence != 0 && entry.sequence == sequence ? &entry : nullptr;
}

}  // namespace godot::net
