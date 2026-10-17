#ifndef SRC_NET_CLIENT_PREDICTION_H_
#define SRC_NET_CLIENT_PREDICTION_H_

#include <array>
#include <cstdint>

#include "src/net/protocol.h"
#include "src/net/replicate.h"

namespace godot::net {

class Entity;

// A client's prediction of its own entity: each command is simulated as
// soon as it is made, rather than a round trip later when the server's
// state for it arrives.
//
// The commands and the states they produced are kept until the server
// acknowledges them. When the server's state for an acknowledged command
// differs from the prediction, the entity is put back to the server's
// state and the commands since are simulated again (without effects); the
// view eases across the difference.
//
// It also keeps the server's queue of this client's commands short but
// never empty: the server simulates one command per tick, so the queue is
// latency, and an empty queue makes the server guess. When commands arrive
// too late, the server repeats the last one for their sequence numbers:
// the client skips ahead in sequence numbers, so its next commands arrive
// in time. When the queue stays longer than needed, the client skips a
// tick's command (see SkipThisTick).
class ClientPrediction {
 public:
  ClientPrediction();
  ~ClientPrediction();

  // Whether to make no command this tick, to shorten the server's queue.
  bool SkipThisTick();
  // Simulate the entity under a new command (quantized), and record both.
  void Predict(Entity* entity, const TimedCommand& command, float dt);
  // The latest commands, for the server.
  InputMessage MakeInputMessage() const;
  // A snapshot: the server's state for the entity after command ack (0:
  // none of ours yet), and how many ticks early our commands arrive there
  // (Snapshot::input_buffer).
  void Reconcile(Entity* entity,
                 uint32_t ack,
                 int input_buffer,
                 const Box& server_state,
                 float dt);

  // Counts for the log.
  uint32_t predicted_count() const { return predicted_count_; }
  uint32_t correction_count() const { return correction_count_; }

 private:
  // Commands kept for replay: two seconds at 60 ticks.
  static constexpr uint32_t kHistorySize = 128;
  // Ticks early our commands should arrive at the server: slack for
  // packets arriving unevenly.
  static constexpr int kTargetBuffer = 2;
  // A queue longer than kTargetBuffer all through this many snapshots is
  // shortened.
  static constexpr int kBufferWindow = 120;

  struct Entry {
    uint32_t sequence = 0;  // 0: empty
    TimedCommand command;
    // The state after the command.
    Box state;
  };

  // Start lengthening or shortening the server's queue, once the last
  // change has shown up in it.
  void AdjustRate(uint32_t ack, int input_buffer);
  Entry* Find(uint32_t sequence);
  const Entry* Find(uint32_t sequence) const;

  std::array<Entry, kHistorySize> history_;
  // The last command made.
  uint32_t sequence_ = 0;
  uint32_t last_ack_ = 0;

  // Ticks still to skip.
  int skipped_ticks_ = 0;
  // Until the server acknowledges this command, its queue doesn't show the
  // last change yet.
  uint32_t adjusted_at_ = 0;
  bool adjusting_ = false;
  // The shortest queue in this window of snapshots.
  int min_buffer_ = 0;
  int window_count_ = 0;

  uint32_t predicted_count_ = 0;
  uint32_t correction_count_ = 0;
};

}  // namespace godot::net

#endif  // SRC_NET_CLIENT_PREDICTION_H_
