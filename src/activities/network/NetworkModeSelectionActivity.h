#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"

/**
 * File-transfer modes, in the order the list shows them.
 *
 * activateIndex() casts a row index STRAIGHT to NetworkMode, so this order and
 * the three parallel row tables in the .cpp (label / description / icon) are
 * one invariant, not two lists that happen to agree: a mode may only be added
 * here together with a row at the same index, and MENU_ITEM_COUNT must equal
 * the number of rows. The static_asserts below (here and in the .cpp) fail the
 * build the moment that correspondence is broken.
 *
 * USB_DRIVE's row exists only on USB-MSC boards (FREEINK_CAP_USB_MSC), AIRDROP
 * only on the readpico (FREEINK_DEVICE_READPICO). Both optional entries are
 * ordered so that each board's own optional mode lands on its last row:
 * readpico has no USB MSC, so its index 3 is AIRDROP rather than USB_DRIVE.
 * Boards with neither option see exactly the three rows they saw before.
 */
enum class NetworkMode {
  JOIN_NETWORK,
  CONNECT_CALIBRE,
  CREATE_HOTSPOT,
#if FREEINK_DEVICE_READPICO
  AIRDROP,  // readpico-only; occupies readpico's last row
#endif
  USB_DRIVE,  // USB-MSC boards only, and only they have a row for it
};

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "USB Drive" - Hand the SD card to a computer (USB-MSC boards)
 * - "AirDrop" - Receive files from an iPhone, iPad or Mac (readpico)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Rows: the three shared modes plus this board's optional one. Keep in step
  // with the enum above -- the assert right below is what enforces it.
#if FREEINK_CAP_USB_MSC
  static constexpr int MENU_ITEM_COUNT = 4;  // JOIN, CALIBRE, HOTSPOT, USB_DRIVE
#elif FREEINK_DEVICE_READPICO
  static constexpr int MENU_ITEM_COUNT = 4;  // JOIN, CALIBRE, HOTSPOT, AIRDROP
#else
  static constexpr int MENU_ITEM_COUNT = 3;  // JOIN, CALIBRE, HOTSPOT
#endif

  // The last row must resolve to the optional mode that row belongs to;
  // nothing else can catch an enum entry added without its row (or the
  // reverse), because a short row table silently zero-fills instead.
  static_assert(
#if FREEINK_CAP_USB_MSC
      static_cast<int>(NetworkMode::USB_DRIVE) == MENU_ITEM_COUNT - 1,
#elif FREEINK_DEVICE_READPICO
      static_cast<int>(NetworkMode::AIRDROP) == MENU_ITEM_COUNT - 1,
#else
      static_cast<int>(NetworkMode::CREATE_HOTSPOT) == MENU_ITEM_COUNT - 1,
#endif
      "NetworkMode order must match the row tables in NetworkModeSelectionActivity.cpp");

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Row storage: entirely static (label/subtitle/icon never change), so it's
  // built once in the constructor instead of every buildScreen() call, into
  // fixed-capacity storage that avoids any heap allocation for the row list.
  freeink::ui::ListItem rowItems_[MENU_ITEM_COUNT]{};
};
