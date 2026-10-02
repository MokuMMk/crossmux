#include "AirDropActivity.h"

#if FREEINK_DEVICE_READPICO

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#if CROSSPOINT_EMULATED == 0
#include <esp_mac.h>
#endif

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

// Where arriving files land, and the module tag every log/SD call is filed
// under. AirDrop is one folder the user can find again, not a cache directory.
constexpr char kTargetDir[] = "/airdrop";
constexpr char kModuleTag[] = "DROP";

// A cpio basename can be 255 bytes of anything the sender's filesystem held;
// FAT long names hold less, and only some of it is storable at all.
constexpr size_t SAFE_NAME_CAPACITY = 96;
constexpr size_t NAME_STEM_CAPACITY = 96;
constexpr size_t NAME_EXTENSION_CAPACITY = 16;
// The path handed to HalStorage: "/airdrop/" + basename + "-999" + extension.
constexpr size_t TARGET_PATH_CAPACITY = 160;
// Highest number appended before a name is given up on.
constexpr int MAX_NAME_ATTEMPTS = 999;
// Longest extension taken from the sender's name; longer tails are treated as
// part of the stem rather than truncated into something that looks deliberate.
constexpr size_t MAX_EXTENSION_LENGTH = 12;

// "2.4 MB" / "812 KB" / "512 B", integer math only: the render task must not
// pull a floating-point printf or a std::string into a repaint, and this is a
// status line, not a measurement.
void formatBytes(char* out, const size_t capacity, const uint32_t bytes) {
  if (bytes >= 1024u * 1024u) {
    const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 10u) / (1024u * 1024u));
    snprintf(out, capacity, "%u.%u MB", static_cast<unsigned>(tenths / 10u), static_cast<unsigned>(tenths % 10u));
  } else if (bytes >= 1024u) {
    snprintf(out, capacity, "%u KB", static_cast<unsigned>(bytes / 1024u));
  } else {
    snprintf(out, capacity, "%u B", static_cast<unsigned>(bytes));
  }
}

// The extension a missing one is filled in with. The library sniffs the type
// from the bytes, so a file that arrives as "IMG_0001" still lands with a name
// the reader's own file browser can dispatch on.
const char* extensionForType(const esp32drop_file_type_t type) {
  switch (type) {
    case ESP32DROP_FILE_JPEG:
      return ".jpg";
    case ESP32DROP_FILE_PNG:
      return ".png";
    case ESP32DROP_FILE_GIF:
      return ".gif";
    case ESP32DROP_FILE_HEIC:
      return ".heic";
    case ESP32DROP_FILE_PDF:
      return ".pdf";
    case ESP32DROP_FILE_ZIP:
      return ".zip";
    case ESP32DROP_FILE_OTHER:
    case ESP32DROP_FILE_APPLEDOUBLE:
    default:
      return nullptr;  // nothing to invent for an unknown type
  }
}

// Copy the sender's basename into `out`, dropping what FAT cannot store
// (controls, reserved punctuation) and what would turn a name into a path: a
// sender-supplied "../../boot" arrives as "boot". Returns the sanitized length,
// 0 when nothing usable is left.
size_t copySafeName(const char* name, char* out, const size_t capacity) {
  if (out == nullptr || capacity < 2) return 0;
  out[0] = '\0';
  if (name == nullptr) return 0;

  size_t length = 0;
  bool truncated = false;
  for (const char* p = name; *p != '\0'; ++p) {
    if (length + 1 >= capacity) {
      truncated = true;
      break;
    }
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c == 0x7F) continue;
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      continue;
    }
    out[length++] = static_cast<char>(c);
  }
  out[length] = '\0';

  // A cut through a multi-byte UTF-8 sequence (a Chinese filename, say) leaves
  // a name no font or FAT driver can read, so back up over the broken tail.
  if (truncated) {
    size_t start = length;
    while (start > 0 && (static_cast<unsigned char>(out[start - 1]) & 0xC0) == 0x80) --start;
    if (start > 0) {
      const unsigned char lead = static_cast<unsigned char>(out[start - 1]);
      const size_t sequenceLength = lead < 0x80 ? 1u : (lead < 0xE0 ? 2u : (lead < 0xF0 ? 3u : 4u));
      if (start - 1 + sequenceLength > length) length = start - 1;
    }
    out[length] = '\0';
  }

  // FAT does not keep trailing dots or spaces, and a name of only those is not
  // a name.
  while (length > 0 && (out[length - 1] == '.' || out[length - 1] == ' ')) out[--length] = '\0';
  return length;
}

// Split "photo.jpg" into "photo" + ".jpg"; a dotfile (".profile") has no
// extension, it is all stem.
void splitName(const char* name, char* stem, const size_t stemCapacity, char* extension,
               const size_t extensionCapacity) {
  stem[0] = '\0';
  extension[0] = '\0';

  const char* dot = strrchr(name, '.');
  if (dot != nullptr && dot != name && strlen(dot) <= MAX_EXTENSION_LENGTH) {
    const size_t stemLength = static_cast<size_t>(dot - name);
    if (stemLength < stemCapacity) {
      memcpy(stem, name, stemLength);
      stem[stemLength] = '\0';
      snprintf(extension, extensionCapacity, "%s", dot);
      return;
    }
  }
  snprintf(stem, stemCapacity, "%s", name);
}

// Pick the path an arriving file is written to: the sender's own basename
// inside /airdrop/, and when that name is taken, the same name numbered from 2
// upward instead of overwriting what is already there. Returns false when no
// free name was found or the directory cannot be created.
bool buildTargetPath(const esp32drop_file_t& file, char* path, const size_t capacity) {
  // The listener is only useful if files can land, so a missing card fails the
  // file (and shows up as the page's failure line) rather than silently.
  if (!Storage.ensureDirectoryExists(kTargetDir)) return false;

  char name[SAFE_NAME_CAPACITY];
  if (copySafeName(file.name, name, sizeof(name)) == 0) {
    // An entry with no usable basename still has to land somewhere: the
    // transfer's own ordering is all the identity it has.
    snprintf(name, sizeof(name), "airdrop-%u", static_cast<unsigned>(file.index) + 1u);
  }

  char stem[NAME_STEM_CAPACITY];
  char extension[NAME_EXTENSION_CAPACITY];
  splitName(name, stem, sizeof(stem), extension, sizeof(extension));
  if (extension[0] == '\0') {
    if (const char* fromType = extensionForType(file.type)) {
      snprintf(extension, sizeof(extension), "%s", fromType);
    }
  }

  for (int attempt = 1; attempt <= MAX_NAME_ATTEMPTS; ++attempt) {
    const int written = attempt == 1 ? snprintf(path, capacity, "%s/%s%s", kTargetDir, stem, extension)
                                     : snprintf(path, capacity, "%s/%s-%d%s", kTargetDir, stem, attempt, extension);
    if (written <= 0 || static_cast<size_t>(written) >= capacity) return false;
    if (!Storage.exists(path)) return true;
  }
  return false;
}

// One wrapped block in the page body: measure its height from the live theme
// font, reserve it, draw it. Same sizing rules the UITheme uses, so nothing
// here hardcodes a font metric or a panel size.
void drawBodyText(UiAppHost::UiScreen& screen, const char* text, fui::TextStyle style, const int16_t gap) {
  if (text == nullptr) return;
  const fui::Rect body = screen.body();
  if (body.height <= 0 || body.width <= 0) return;
  const int16_t height = fui::measureWrappedText(screen.target(), text, style, body.width).height;
  if (height <= 0) return;
  screen.target().text(screen.takeTop(height, gap), text, style);
}

}  // namespace

void AirDropActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&AirDropActivity::screenTrampoline, this);

  starting_ = true;
  listening_ = false;
  error_[0] = '\0';
  savedFiles_ = 0;
  buildDeviceName();

  // The first paint lands before the radio comes up: AWDL bring-up and the TLS
  // listener take a moment, and a stale screen under a listener that is already
  // advertising is worse than a "Starting..." line.
  requestUpdateAndWait();

  // RADIO EXCLUSIVITY -- read the class comment before touching this. AWDL
  // parks the interface on 2.4 GHz channel 6 in promiscuous mode, and its
  // awdl_begin() calls esp_wifi_init(), which refuses to run while the Arduino
  // WiFi driver is already initialized. The normal path into this page has no
  // WiFi session (the container reaches the mode list before it starts one),
  // but a cancelled Wi-Fi scan can leave the driver up, so release it here.
  // This page must never start a WiFi session of its own.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    LOG_INF("DROP", "Releasing the WiFi driver before AWDL takes the radio");
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }

  // max_receive 0 is the library's own ceiling (6 MiB), lowered by what PSRAM
  // can actually supply. The effective value comes back in status.rx_max and is
  // what the page shows; a transfer over it is refused to the sender with HTTP
  // 413 instead of being silently truncated.
  const esp_err_t err = esp32drop_start(deviceName_, /*max_receive=*/0, &AirDropActivity::onFileTrampoline, this);
  starting_ = false;

  esp32drop_status_t status{};
  if (err != ESP_OK) {
    // Nothing is listening, and the library has no status to report from: keep
    // the reason here so the page can show it.
    snprintf(error_, sizeof(error_), "%s", esp_err_to_name(err));
    LOG_ERR("DROP", "esp32drop_start failed: %s", esp_err_to_name(err));
  } else {
    listening_ = true;
    esp32drop_get_status(&status);
    LOG_INF("DROP", "Listening as \"%s\"", deviceName_);
  }

  if (publishState(status)) requestUpdate();
}

void AirDropActivity::onExit() {
  // Same task as esp32drop_start(): both run on the main task out of
  // ActivityManager::loop, which is what the library demands of start, stop and
  // poll. A stop that times out frees nothing and leaves the library holding
  // the receiver role -- report it, there is no retry left on this page.
  const esp_err_t err = esp32drop_stop();
  if (err != ESP_OK) {
    LOG_ERR("DROP", "esp32drop_stop: %s", esp_err_to_name(err));
  }
  listening_ = false;

  Activity::onExit();
}

void AirDropActivity::loop() {
  // Leaving is the only input this page has. wasPressed(Back) folds in the
  // touch gestures (header tap, left-edge swipe), and onExit() releases the
  // radio.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!listening_) return;

  // The only place a file callback runs, and the only place the received
  // archive is released. Cheap when there is nothing to deliver.
  esp32drop_poll();

  esp32drop_status_t status{};
  esp32drop_get_status(&status);
  if (publishState(status)) requestUpdate();
}

void AirDropActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Chrome first, then the FreeInkUI screen paints the body between the header
  // band and the button hints (setContentMarginFromScreen in buildScreen).
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AIRDROP));

  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AirDropActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<AirDropActivity*>(user)->buildScreen(screen);
}

void AirDropActivity::buildScreen(UiScreen& screen) const {
  // One read of the published slot: this repaint shows one consistent state,
  // never a mix of two.
  const RenderState state = slots_[publishedSlot_.load(std::memory_order_acquire)];

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const fui::ThemeTokens& theme = screen.theme();
  auto captionStyle = theme.smallText;
  captionStyle.maxLines = 2;
  auto stateStyle = theme.bodyText;
  stateStyle.bold = true;
  stateStyle.maxLines = 2;

  char line[128];

  // What the sender sees on its share sheet while this page is listening.
  snprintf(line, sizeof(line), "%s: %s", tr(STR_AIRDROP_DEVICE_NAME), deviceName_);
  drawBodyText(screen, line, captionStyle, theme.spaceSm);

  if (state.starting) {
    drawBodyText(screen, tr(STR_AIRDROP_STARTING), stateStyle, theme.spaceMd);
  } else if (!state.listening) {
    drawBodyText(screen, tr(STR_AIRDROP_FAILED), stateStyle, theme.spaceMd);
  } else if (state.receiving) {
    drawBodyText(screen, tr(STR_AIRDROP_RECEIVING), stateStyle, theme.spaceMd);
  } else {
    drawBodyText(screen, tr(STR_AIRDROP_WAITING), stateStyle, theme.spaceMd);
  }

  // Bytes of the transfer in flight against the ceiling the library actually
  // granted, not the one that was asked for.
  if (state.bytesMax > 0 && (state.receiving || state.bytes > 0)) {
    char received[16];
    char ceiling[16];
    formatBytes(received, sizeof(received), state.bytes);
    formatBytes(ceiling, sizeof(ceiling), state.bytesMax);
    snprintf(line, sizeof(line), "%s / %s", received, ceiling);
    drawBodyText(screen, line, captionStyle, theme.spaceSm);

    const fui::Rect band = screen.takeTop(PROGRESS_BAR_HEIGHT, theme.spaceMd);
    fui::ProgressBarProps bar;
    bar.value = static_cast<int32_t>(state.bytes);
    bar.max = static_cast<int32_t>(state.bytesMax);
    bar.track = fui::Paint::dither(fui::Color::LightGray);
    bar.fill = fui::Paint::solid(fui::Color::Black);
    fui::progressBar(screen.frame(), band, bar);
  }

  snprintf(line, sizeof(line), tr(STR_AIRDROP_RECEIVED_FMT), static_cast<unsigned>(state.files));
  drawBodyText(screen, line, captionStyle, theme.spaceSm);

  if (state.failure[0] != '\0') {
    snprintf(line, sizeof(line), "%s: %s", tr(STR_AIRDROP_LAST_ERROR), state.failure);
    drawBodyText(screen, line, captionStyle, 0);
  }
}

bool AirDropActivity::onFileTrampoline(void* ctx, const esp32drop_file_t* file) {
  auto* self = static_cast<AirDropActivity*>(ctx);
  if (self == nullptr || file == nullptr) return false;
  return self->saveFile(*file);
}

bool AirDropActivity::saveFile(const esp32drop_file_t& file) {
  // macOS sends a "._name" metadata sidecar beside every real file. It is not
  // content the user asked for, so it is accepted (not counted as a refused
  // file) but not written: /airdrop/ holds what was sent, not what came along.
  if (file.type == ESP32DROP_FILE_APPLEDOUBLE) {
    LOG_DBG("DROP", "Skipping the AppleDouble sidecar %s", file.name != nullptr ? file.name : "(unnamed)");
    return true;
  }
  if (file.len > 0 && file.data == nullptr) {
    LOG_ERR("DROP", "Refusing a %u-byte file with no bytes", static_cast<unsigned>(file.len));
    return false;
  }

  char path[TARGET_PATH_CAPACITY];
  if (!buildTargetPath(file, path, sizeof(path))) {
    snprintf(error_, sizeof(error_), "%s", "no free name on the SD card");
    LOG_ERR("DROP", "No writable name for %s", file.name != nullptr ? file.name : "(unnamed)");
    return false;
  }

  HalFile out;
  if (!Storage.openFileForWrite(kModuleTag, path, out)) {
    snprintf(error_, sizeof(error_), "%s", "could not write to the SD card");
    LOG_ERR("DROP", "Could not open %s for writing", path);
    return false;
  }

  // The library keeps the bytes in PSRAM for the length of this call only, so
  // they are written here and now; blocking costs nothing on the air (the AWDL
  // transmit cadence belongs to the library's own task).
  const size_t written = out.write(file.data, file.len);
  out.flush();
  if (written != file.len) {
    // A short write leaves a file that looks like the sender's but is not, so
    // it is removed rather than reported as received.
    LOG_ERR("DROP", "Short write to %s (%u of %u bytes)", path, static_cast<unsigned>(written),
            static_cast<unsigned>(file.len));
    Storage.remove(path);
    snprintf(error_, sizeof(error_), "%s", "could not write to the SD card");
    return false;
  }

  savedFiles_++;
  LOG_INF("DROP", "Saved %s (%u bytes)", path, static_cast<unsigned>(file.len));
  return true;
}

void AirDropActivity::buildDeviceName() {
  // Read the MAC straight from the eFuse: the name is needed before the radio
  // exists, and this is the same call WifiSelectionActivity builds the WiFi
  // hostname from.
  uint8_t mac[6] = {};
#if CROSSPOINT_EMULATED
  const bool haveMac = WiFi.macAddress(mac);
#else
  const bool haveMac = esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;
#endif
  if (haveMac) {
    // Short enough for a share sheet and unique enough to tell two readers in
    // the same room apart.
    snprintf(deviceName_, sizeof(deviceName_), "CrossPoint-%02X%02X", mac[4], mac[5]);
  } else {
    snprintf(deviceName_, sizeof(deviceName_), "CrossPoint");
  }
}

bool AirDropActivity::publishState(const esp32drop_status_t& status) {
  RenderState next;
  next.starting = starting_;
  next.listening = listening_ && status.running;
  next.receiving = status.rx_active;
  next.bytes = status.rx_bytes;
  next.bytesMax = status.rx_max;
  next.files = savedFiles_;
  // Our own reason wins while it exists (a start error, an SD write error); the
  // library's message is the latest of its failures -- a refused file, a
  // truncated archive -- and stays visible as "the last failure".
  snprintf(next.failure, sizeof(next.failure), "%s", error_[0] != '\0' ? error_ : status.errmsg);

  const uint8_t shown = publishedSlot_.load(std::memory_order_relaxed);
  if (sameState(next, slots_[shown])) return false;

  // Write the slot the render task is not reading, then hand it over with one
  // atomic store: a repaint sees either the whole previous state or the whole
  // new one, never a half-written failure message.
  const uint8_t target = static_cast<uint8_t>(shown ^ 1u);
  slots_[target] = next;
  publishedSlot_.store(target, std::memory_order_release);
  return true;
}

bool AirDropActivity::sameState(const RenderState& lhs, const RenderState& rhs) {
  // Field by field: RenderState has padding, so memcmp would report changes
  // that are not changes.
  return lhs.starting == rhs.starting && lhs.listening == rhs.listening && lhs.receiving == rhs.receiving &&
         lhs.bytes == rhs.bytes && lhs.bytesMax == rhs.bytesMax && lhs.files == rhs.files &&
         strncmp(lhs.failure, rhs.failure, sizeof(lhs.failure)) == 0;
}

#endif  // FREEINK_DEVICE_READPICO
