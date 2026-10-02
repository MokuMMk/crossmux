#pragma once

#include <BoardConfig.h>

#if FREEINK_DEVICE_READPICO

#include <esp32drop.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * AirDropActivity is the receive side of the file-transfer page on the
 * readpico: it runs the ESP32Drop listener (the AWDL link layer plus the
 * TLS/HTTP application layer), writes every file a sender hands over into
 * /airdrop/ on the SD card, and shows what the listener is doing.
 *
 * RADIO EXCLUSIVITY. AirDrop owns the 2.4 GHz radio while it runs: AWDL parks
 * the interface on channel 6 in promiscuous mode and brings the WiFi driver up
 * itself (esp_wifi_init()), so awdl_begin() fails while an ordinary WiFi
 * session is already initialized. Enter this page only when no WiFi session is
 * running -- the container (CrossPointWebServerActivity) tears WiFi down and
 * reaches the mode list before any session starts, which is where this page is
 * entered from -- and never start WiFi from here. A half-shared radio would not
 * report an error, it would simply stop answering senders.
 *
 * Lifecycle: esp32drop_start() in onEnter(), esp32drop_poll() in loop() and
 * esp32drop_stop() in onExit(), all three on the main task, which is what the
 * library requires: the file callback only ever runs inside poll().
 */
class AirDropActivity final : public Activity, private UiAppHost {
 public:
  AirDropActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AirDrop", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // While the listener is up, neither the idle sleep nor the CPU downclock may
  // take the loop (and with it the poll) away: AWDL's frame timing and the TLS
  // listener are not negotiable about being scheduled.
  bool skipLoopDelay() override { return listening_; }
  bool preventAutoSleep() override { return listening_; }

 private:
  // The library advertises at most AD_NAME_MAX (64) bytes; 32 is plenty for a
  // name plus a MAC suffix and keeps the share-sheet entry short.
  static constexpr size_t DEVICE_NAME_CAPACITY = 32;
  // Shortest useful failure text ("last error" line).
  static constexpr size_t ERROR_CAPACITY = 40;
  static constexpr int16_t PROGRESS_BAR_HEIGHT = 8;

  // Everything the render task may read, published as a whole: the loop task
  // fills the slot the render task is NOT reading and then flips the index, so
  // a repaint can never catch a half-written failure message (the panel would
  // keep showing that text until something else changed). Same hand-off as the
  // shared theme tokens in UiAppHelpers, without the shared-instance pool.
  struct RenderState {
    bool starting = true;
    bool listening = false;
    bool receiving = false;
    uint32_t bytes = 0;
    uint32_t bytesMax = 0;
    uint32_t files = 0;
    char failure[ERROR_CAPACITY] = {};
  };

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen) const;

  // esp32drop_file_cb_t: runs inside esp32drop_poll() on the loop task, and the
  // file's bytes are valid only for the duration of the call.
  static bool onFileTrampoline(void* ctx, const esp32drop_file_t* file);
  bool saveFile(const esp32drop_file_t& file);

  void buildDeviceName();
  // Publish the state the render task should draw; true when it changed.
  bool publishState(const esp32drop_status_t& status);
  static bool sameState(const RenderState& lhs, const RenderState& rhs);

  // The name the sender's share sheet lists.
  char deviceName_[DEVICE_NAME_CAPACITY] = {};
  // True until esp32drop_start() has returned, so the first paint can say so.
  bool starting_ = true;
  // True once the listener reported itself running; drives skipLoopDelay().
  bool listening_ = false;
  // Sticky failure of ours (start error, SD write error). The library's own
  // errmsg is shown when we have nothing more specific to say.
  char error_[ERROR_CAPACITY] = {};
  uint32_t savedFiles_ = 0;

  RenderState slots_[2];
  std::atomic<uint8_t> publishedSlot_{0};
};

#endif  // FREEINK_DEVICE_READPICO
