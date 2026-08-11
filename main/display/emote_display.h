#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetAccentColor(const char* color) override;
    virtual void SetAccentRingProgress(int64_t started_at_ms, int64_t ends_at_ms) override;
    virtual void ClearAccentRingProgress() override;
    virtual void ShowConfirmScreen(const char* summary) override;
    virtual void HideConfirmScreen() override;
    virtual void SetPreviewImage(const void* image);

    // Show or hide a named object from the layout, e.g. "clock_label".
    bool SetObjectVisible(const char* name, bool visible);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    bool EnsureConfirmObjects();
    void EnsureArcRefreshTimer();

    emote_handle_t emote_handle_ = nullptr;
    esp_timer_handle_t arc_refresh_timer_ = nullptr;
    bool confirm_objects_created_ = false;
    // While the confirm screen is up, emotion/status/caption updates are
    // dropped: the turn's closing ui_state arrives right after confirm_request
    // and would redraw the eyes and toast on top of the prompt.
    bool confirm_screen_active_ = false;

};

} // namespace emote
