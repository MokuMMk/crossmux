#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <array>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraTheme.h"

namespace fui = freeink::ui;

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput, false, true),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(menuItems, hasFootnotes, hasBookmarks);
  buildMenuRowItems();
}

// Populates menuRowItems's labels/actionValue from menuItems. Called once
// here since menuItems (and thus which rows exist) never changes after
// construction; buildScreen() only touches the two rows with a live value.
void EpubReaderMenuActivity::buildMenuRowItems() {
  for (size_t i = 0; i < menuItems.size() && i < MAX_MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i].labelId);
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present()) {
    items.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
#if FREEINK_DEVICE_READPICO
  // 只有 Read Pico 的 E0470A01 实现了错相揭页，所以这一条只在它上面列出。别的板子上
  // HalDisplay::pageTurn() 会返回 false，阅读器退回普通推送 —— 列出这个开关只会让人困惑。
  // / Only the Read Pico panel implements the phase-offset reveal, so the entry is listed
  // only there. Elsewhere HalDisplay::pageTurn() returns false and the reader falls back to
  // a normal push, so showing the switch would only confuse.
  items.push_back({MenuAction::PAGE_TURN_ANIMATION, StrId::STR_PAGE_TURN_ANIMATION});
#endif
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data =
      MenuResult{-1, pendingOrientation,
                 static_cast<uint8_t>(
                     selectedPageTurnOption == 0 ? 0 : std::array<uint8_t, 5>{0, 1, 3, 6, 12}[selectedPageTurnOption])};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const auto selectedAction = menuItems[index].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::NIGHT_MODE) {
    SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

#if FREEINK_DEVICE_READPICO
  if (selectedAction == MenuAction::PAGE_TURN_ANIMATION) {
    // 就地翻转并落盘，和 NIGHT_MODE 一样：效果在下次翻页时可见，菜单本身不显示状态。
    // 代价提示写在 STR_PAGE_TURN_ANIMATION 的翻译里没有意义 —— 动画约 1.1s，普通推送约
    // 0.7s，所以它是"更好看但更慢"的开关。
    // / Flip in place and persist, same shape as NIGHT_MODE: the effect shows on the next
    // turn and the menu does not render the state. The trade is the point of it being a
    // switch at all -- the reveal runs ~1.1 s against ~0.7 s for a plain text push.
    SETTINGS.pageTurnAnimation = SETTINGS.pageTurnAnimation == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
#endif

  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation,
                       static_cast<uint8_t>(selectedPageTurnOption == 0
                                                ? 0
                                                : std::array<uint8_t, 5>{0, 1, 3, 6, 12}[selectedPageTurnOption])});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(true);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // Progress summary where the old sub-header band sat.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), progressLine.c_str(), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // menuRowItems's labels/actionValue were set once in the constructor (see
  // buildMenuRowItems()); only rows with live values need refreshing here.
  for (size_t i = 0; i < menuItems.size(); i++) {
    const auto action = menuItems[i].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      menuRowItems[i].value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      menuRowItems[i].value = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::NIGHT_MODE) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.screenInverted);
    } else if (action == MenuAction::FRONTLIGHT) {
      GUI.setCheckboxRow(menuRowItems[i], Frontlight.isOn());
    }
  }

  fui::ListProps props;
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(menuItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const auto& metrics = uiThemeMetrics(true);
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight};
  GUI.drawHeaderWithStyle(renderer, header, title.c_str(), nullptr, true, true);
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  drawChrome();

  renderUi();

  drawFooter();
  renderer.displayBuffer();
}
