#include "src/client_prediction.h"

#include <algorithm>
#include <cmath>

#include "src/fps_character.h"

namespace godot {

namespace {

// Whether a prediction got the server's result. The server runs the same
// code from the same state, so they usually match exactly; the margins
// only allow for float noise.
bool Matches(const PlayerState& predicted, const PlayerState& server) {
  return predicted.position.distance_squared_to(server.position) < 1e-6f &&
         predicted.velocity.distance_squared_to(server.velocity) < 1e-4f &&
         std::abs(predicted.recoil - server.recoil) < 1e-4f &&
         std::abs(predicted.recoil_yaw - server.recoil_yaw) < 1e-4f &&
         std::abs(predicted.landing_offset - server.landing_offset) < 1e-4f &&
         predicted.on_floor == server.on_floor &&
         predicted.shot_count == server.shot_count &&
         predicted.respawn_count == server.respawn_count &&
         predicted.dead == server.dead &&
         predicted.weapon.ammo == server.weapon.ammo &&
         predicted.weapon.reserve_ammo == server.weapon.reserve_ammo &&
         predicted.weapon.reloading == server.weapon.reloading;
}

}  // namespace

ClientPrediction::ClientPrediction() = default;

ClientPrediction::~ClientPrediction() = default;

bool ClientPrediction::SkipThisTick() {
  if (skipped_ticks_ > 0) {
    skipped_ticks_--;
    return true;
  }
  return false;
}

void ClientPrediction::Predict(FpsCharacter* player,
                               const PlayerInput& command,
                               float dt) {
  sequence_++;
  // 0 means none.
  if (sequence_ == 0) {
    sequence_++;
  }
  player->Simulate(command, dt, SimulationMode::kPredicted);
  Entry& entry = history_[sequence_ % kHistorySize];
  entry.sequence = sequence_;
  entry.command = command;
  entry.state = player->CaptureState();
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

void ClientPrediction::Reconcile(FpsCharacter* player,
                                 uint32_t ack,
                                 int input_buffer,
                                 const PlayerState& server_state,
                                 float dt) {
  AdjustRate(ack, input_buffer);
  // The state for an acknowledged command doesn't change (snapshots that
  // arrive out of order are dropped before this).
  if (ack != 0 && ack == last_ack_) {
    return;
  }
  last_ack_ = ack;

  const Entry* acked = ack != 0 ? Find(ack) : nullptr;
  if (acked && Matches(acked->state, server_state)) {
    return;
  }
  // A command the server repeated for a sequence number we skipped (see
  // AdjustRate) was never predicted: not a misprediction.
  if (acked) {
    correction_count_++;
  }

  // Back to the server's state, then forward through the commands it
  // hasn't simulated yet.
  Vector3 predicted_position = player->get_global_position();
  uint8_t predicted_respawn = player->respawn_count();
  player->RestoreState(server_state);
  uint32_t first = ack + 1;
  if (sequence_ - ack >= kHistorySize) {
    first = sequence_ - kHistorySize + 1;
  }
  for (uint32_t s = first; s != sequence_ + 1; s++) {
    Entry* entry = Find(s);
    if (!entry) {
      continue;
    }
    player->Simulate(entry->command, dt, SimulationMode::kReplay);
    entry->state = player->CaptureState();
  }

  if (player->respawn_count() != predicted_respawn) {
    player->OnTeleported();
  } else {
    player->SmoothCorrection(predicted_position -
                             player->get_global_position());
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

}  // namespace godot
