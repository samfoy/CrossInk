#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 9;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME,          StrId::STR_PASSWORD,
                                     StrId::STR_SYNC_SERVER_URL,   StrId::STR_DOCUMENT_MATCHING,
                                     StrId::STR_SEND_METADATA,     StrId::STR_SYNC_BEHAVIOR,
                                     StrId::STR_UPLOAD_READING_STATS, StrId::STR_SIGN_UP,
                                     StrId::STR_AUTHENTICATE};
// Row indices. Named because three separate switches below must agree; an
// off-by-one here silently mislabels or misroutes a row.
constexpr int ROW_UPLOAD_READING_STATS = 6;
constexpr int ROW_SIGN_UP = 7;
constexpr int ROW_AUTHENTICATE = 8;
}  // namespace

KOReaderSettingsActivity::KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("KOReaderSettings", renderer, mappedInput) {}

int KOReaderSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* KOReaderSettingsActivity::headerTitle() const { return tr(STR_KOREADER_SYNC); }

void KOReaderSettingsActivity::activateIndex(const int index) {
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  app.clearTapFlash();
  if (index == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(kb.text, KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (index == 1) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                KOREADER_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
            KOREADER_STORE.saveToFile();
          }
        });
  } else if (index == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOREADER_STORE.setServerUrl(urlToSave);
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (index == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == 4) {
    // Send Metadata - toggle on/off
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == 5) {
    // Sync behavior - toggle between Ask and Smart
    const auto current = KOREADER_STORE.getSyncBehavior();
    const auto newBehavior = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                               : KOReaderSyncBehavior::ASK_EVERY_TIME;
    KOREADER_STORE.setSyncBehavior(newBehavior);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == ROW_UPLOAD_READING_STATS) {
    // Upload Reading Stats (BookOrbit page-stats) - toggle on/off.
    // Lives in CrossPointSettings (not KOREADER_STORE) because it is also
    // exposed in the web settings UI via SettingsList.h.
    SETTINGS.uploadReadingStats = SETTINGS.uploadReadingStats ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (index == ROW_SIGN_UP) {
    // Sign Up - create a new account on the sync server with the entered credentials
    if (!KOREADER_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (index == ROW_AUTHENTICATE) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      const auto username = KOREADER_STORE.getUsername();
      values[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 1) {
      values[i] = KOREADER_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 2) {
      values[i] = KOREADER_STORE.getServerUrl();
      if (values[i].empty()) {
        // Show which server the default actually is, scheme stripped for space
        std::string defaultUrl = KOREADER_STORE.getBaseUrl();
        const auto schemeEnd = defaultUrl.find("://");
        if (schemeEnd != std::string::npos) {
          defaultUrl.erase(0, schemeEnd + 3);
        }
        values[i] = std::string(tr(STR_DEFAULT_VALUE)) + ": " + defaultUrl;
      }
    } else if (i == 3) {
      values[i] = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
    } else if (i == 4) {
      values[i] = KOREADER_STORE.getSendMetadata() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 5) {
      values[i] =
          KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART ? tr(STR_SMART_SYNC) : tr(STR_ASK_EVERY_TIME);
    } else if (i == ROW_UPLOAD_READING_STATS) {
      values[i] = SETTINGS.shouldUploadReadingStats() ? tr(STR_ENABLED) : tr(STR_DISABLED);
    } else {
      values[i] = KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}
