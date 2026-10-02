#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
// Row tables, one entry per row, in NetworkMode order. activateIndex() casts
// the row index straight to NetworkMode, so the optional entry is guarded
// exactly like the enum and stays in the same position (see the header): the
// readpico's AirDrop row takes index 3, where USB-MSC boards have USB_DRIVE.
// The two are mutually exclusive in this tree (readpico has no USB MSC), which
// the "last row" static_asserts below turn into a build error if that ever
// stops being true.
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#if FREEINK_DEVICE_READPICO
    StrId::STR_AIRDROP,
#elif FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#if FREEINK_DEVICE_READPICO
    StrId::STR_AIRDROP_DESC,
#elif FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#if FREEINK_DEVICE_READPICO
    UIIcon::Transfer,  // app-specific icon: the AirDrop/transfer glyph
#elif FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
};

// The row tables must be indexed by their mode, so each row's key is checked
// against the enumerator it is reached through. This is the other half of the
// header's MENU_ITEM_COUNT assert: that one pins the enum, these pin the
// tables to it, and a row inserted or dropped without its mode fails here.
static_assert(menuItems[static_cast<int>(NetworkMode::JOIN_NETWORK)] == StrId::STR_JOIN_NETWORK,
              "row 0 is not Join a Network");
static_assert(menuItems[static_cast<int>(NetworkMode::CONNECT_CALIBRE)] == StrId::STR_CALIBRE_WIRELESS,
              "row 1 is not Connect to Calibre");
static_assert(menuItems[static_cast<int>(NetworkMode::CREATE_HOTSPOT)] == StrId::STR_CREATE_HOTSPOT,
              "row 2 is not Create Hotspot");
#if FREEINK_DEVICE_READPICO
static_assert(menuItems[static_cast<int>(NetworkMode::AIRDROP)] == StrId::STR_AIRDROP,
              "readpico's last row is not AirDrop");
#endif
#if FREEINK_CAP_USB_MSC
static_assert(menuItems[static_cast<int>(NetworkMode::USB_DRIVE)] == StrId::STR_USB_DRIVE,
              "the USB-MSC board's last row is not USB Drive");
#endif
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return MENU_ITEM_COUNT; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  onModeSelected(static_cast<NetworkMode>(index));
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
