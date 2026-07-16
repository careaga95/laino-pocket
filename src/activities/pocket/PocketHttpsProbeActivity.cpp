#include "PocketHttpsProbeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "PocketHttpsProbeTransport.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PocketHttpsProbeActivity::onEnter() {
  Activity::onEnter();
  presentation.reset();
  requestUpdate();
}

void PocketHttpsProbeActivity::loop() {
  if (presentation.isTesting()) {
    // Complete the e-ink update before the bounded synchronous operation begins.
    requestUpdateAndWait();
    const pocket::HttpsProbeResult result = pocket::runPublicHttpsProbe();
    presentation.complete(result);
    LOG_INF("PKT", "HTTPS result=%s http=%d bytes=%u elapsed=%lu error=%ld", pocket::httpsProbeResultName(result.code),
            result.httpStatus, result.responseBytes, static_cast<unsigned long>(result.elapsedMs),
            static_cast<long>(result.transportError));
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    presentation.reset();
    requestUpdate();
  }
}

void PocketHttpsProbeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int midY = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POCKET_TEST_HTTPS));

  if (presentation.isTesting()) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_HTTPS_TESTING));
  } else {
    const pocket::HttpsProbeResult& result = presentation.result();
    const bool success = result.code == pocket::HttpsProbeResultCode::Success;
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 30,
                              success ? tr(STR_POCKET_HTTPS_SUCCESS) : tr(STR_POCKET_HTTPS_FAILURE), true,
                              EpdFontFamily::BOLD);
    char detail[96];
    pocket::formatHttpsProbeResult(result, detail, sizeof(detail));
    renderer.drawCenteredText(UI_10_FONT_ID, midY, detail, true);
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 30, pocket::HTTPS_PROBE_URL, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
