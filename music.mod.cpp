// ==WindhawkMod==
// @id              music-widget
// @name            Music Widget
// @description     Windows 11 native-style widget music controller with timeline and media controls.
// @version         1.0
// @author          Justus
// @include         explorer.exe
// @compilerOptions -lole32 -ldwmapi -lgdi32 -luser32 -lwindowsapp -lshcore -lgdiplus
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Music Lounge

A modern Windows 11 taskbar music controller with native DWM styling, timeline seek bar, and media controls.

## ✨ Features
* **Universal Media Support:** Works with any player via GSMTC (Spotify, YouTube, etc).
* **Album Art:** Shows current track cover art.
* **Native Windows 11 Look:** Acrylic blur, rounded corners, and seamless integration.
* **Controls:** Play/Pause, Next, Previous, and timeline seek for supported players.
* **Volume:** Scroll over the panel to adjust system volume.

## ⚠️ Requirements
* **Disable Widgets:** Taskbar Settings → Widgets → Off.
* **Windows 11:** Required for native visuals.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- PanelWidth: 300
  $name: Panel Width
- PanelHeight: 48
  $name: Panel Height
- FontSize: 11
  $name: Font Size
- OffsetX: 12
  $name: X Offset
- OffsetY: 0
  $name: Y Offset
- AutoTheme: true
  $name: Auto Theme
- TextColor: 0xFFFFFF
  $name: Manual Text Color (Hex)
- BgOpacity: 0
  $name: Acrylic Tint Opacity (0-255). Keep 0 for pure glass.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shcore.h> 
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdio>

// WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace Gdiplus;
using namespace std;
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

// --- Constants ---
const WCHAR* FONT_NAME = L"Segoe UI Variable Display"; 

// --- DWM API ---

typedef enum _WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 } WINDOWCOMPOSITIONATTRIB;
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4, 
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;
typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID Data;
    SIZE_T SizeOfData;
} WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// --- Z-Band API ---
enum ZBID {
    ZBID_DEFAULT = 0,
    ZBID_DESKTOP = 1,
    ZBID_UIACCESS = 2,
    ZBID_IMMERSIVE_IHM = 3,
    ZBID_IMMERSIVE_NOTIFICATION = 4,
    ZBID_IMMERSIVE_APPCHROME = 5,
    ZBID_IMMERSIVE_MOGO = 6,
    ZBID_IMMERSIVE_EDGY = 7,
    ZBID_IMMERSIVE_INACTIVEMOBODY = 8,
    ZBID_IMMERSIVE_INACTIVEDOCK = 9,
    ZBID_IMMERSIVE_ACTIVEMOBODY = 10,
    ZBID_IMMERSIVE_ACTIVEDOCK = 11,
    ZBID_IMMERSIVE_BACKGROUND = 12,
    ZBID_IMMERSIVE_SEARCH = 13,
    ZBID_GENUINE_WINDOWS = 14,
    ZBID_IMMERSIVE_RESTRICTED = 15,
    ZBID_SYSTEM_TOOLS = 16,
    ZBID_LOCK = 17,
    ZBID_ABOVELOCK_UX = 18,
};

typedef HWND(WINAPI* pCreateWindowInBand)(
    DWORD dwExStyle,
    LPCWSTR lpClassName,
    LPCWSTR lpWindowName,
    DWORD dwStyle,
    int x,
    int y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam,
    DWORD dwBand
);

typedef BOOL(WINAPI* pSetWindowBand)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);
typedef BOOL(WINAPI* pGetWindowBand)(HWND hWnd, PDWORD pdwBand);

// --- Configurable State ---
struct ModSettings {
    int width = 400;
    int height = 100;
    int fontSize = 14;
    int offsetX = 100;
    int offsetY = 100;
    bool autoTheme = true;
    DWORD manualTextColor = 0xFFFFFFFF; 
    int bgOpacity = 0;   
} g_Settings;

// --- Global State ---
HWND g_hMediaWindow = NULL;
bool g_Running = true; 
int g_HoverState = 0;

// Panel sliding state
bool g_PanelOpen = true;
int g_PanelOffsetX = 0;  // Current animation offset
int g_PanelTargetOffsetX = 0;  // Target animation offset
ULONGLONG g_HoverTimerStart = 0;  // When hover started
bool g_HoverTabZone = false;  // Currently hovering over tab zone
float g_HoverBoldLevel = 0.0f;  // Visual bold effect (0.0 to 1.0)
ULONGLONG g_HoverLastLeftTime = 0;  // When last left hover zone (for resume logic) 

// Data Model
struct MediaState {
    wstring title = L"Waiting for media...";
    wstring artist = L"";
    bool isPlaying = false;
    bool hasMedia = false;
    Bitmap* albumArt = nullptr;
    bool isSpotify = false;
    double position = 0.0;
    double duration = 0.0;
    double smoothPosition = 0.0; // For smooth animation
    ULONGLONG lastUpdateTick = 0; // For time interpolation
    mutex lock;
} g_MediaState;

// Animation
int g_ScrollOffset = 0;
int g_TextWidth = 0;
bool g_IsScrolling = false;
int g_ScrollWait = 60;



// Timeline seek state
bool g_TimelineHover = false;
bool g_TimelineDragging = false;
float g_TimelineDragProgress = 0.0f;

// --- Settings ---
void LoadSettings() {
    g_Settings.width = Wh_GetIntSetting(L"PanelWidth");
    g_Settings.height = Wh_GetIntSetting(L"PanelHeight");
    g_Settings.fontSize = Wh_GetIntSetting(L"FontSize");
    g_Settings.offsetX = Wh_GetIntSetting(L"OffsetX");
    g_Settings.offsetY = Wh_GetIntSetting(L"OffsetY");
    // Force visible defaults if settings are missing or invalid
    if (g_Settings.width < 100) g_Settings.width = 400;
    if (g_Settings.height < 24) g_Settings.height = 100;
    if (g_Settings.offsetX < 0) g_Settings.offsetX = 100;
    if (g_Settings.offsetY < 0) g_Settings.offsetY = 100;
    g_Settings.autoTheme = Wh_GetIntSetting(L"AutoTheme") != 0;
    
    PCWSTR textHex = Wh_GetStringSetting(L"TextColor");
    DWORD textRGB = 0xFFFFFF;
    if (textHex) {
        if (wcslen(textHex) > 0) textRGB = wcstoul(textHex, nullptr, 16);
        Wh_FreeStringSetting(textHex);
    }
    g_Settings.manualTextColor = 0xFF000000 | textRGB;
    
    g_Settings.bgOpacity = Wh_GetIntSetting(L"BgOpacity");
    if (g_Settings.bgOpacity < 0) g_Settings.bgOpacity = 0;
    if (g_Settings.bgOpacity > 255) g_Settings.bgOpacity = 255;

    if (g_Settings.width < 100) g_Settings.width = 300;
    if (g_Settings.height < 24) g_Settings.height = 48;
}

// --- WinRT / GSMTC ---
GlobalSystemMediaTransportControlsSessionManager g_SessionManager = nullptr;

Bitmap* StreamToBitmap(IRandomAccessStreamWithContentType const& stream) {
    if (!stream) {
        OutputDebugStringW(L"[AlbumArt] Stream is null");
        return nullptr;
    }
    
    IStream* nativeStream = nullptr;
    HRESULT hr = CreateStreamOverRandomAccessStream(reinterpret_cast<IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&nativeStream));
    if (FAILED(hr)) {
        OutputDebugStringW(L"[AlbumArt] CreateStreamOverRandomAccessStream failed");
        return nullptr;
    }
    
    if (!nativeStream) {
        OutputDebugStringW(L"[AlbumArt] nativeStream is null");
        return nullptr;
    }
    
    Bitmap* bmp = Bitmap::FromStream(nativeStream, TRUE);  // TRUE = useIcm for better color handling
    nativeStream->Release();
    
    if (!bmp) {
        OutputDebugStringW(L"[AlbumArt] Bitmap::FromStream returned null");
        return nullptr;
    }
    
    if (bmp->GetLastStatus() != Ok) {
        WCHAR dbgMsg[256];
        swprintf_s(dbgMsg, L"[AlbumArt] Bitmap status error: %d", bmp->GetLastStatus());
        OutputDebugStringW(dbgMsg);
        delete bmp;
        return nullptr;
    }
    
    OutputDebugStringW(L"[AlbumArt] Bitmap loaded successfully");
    return bmp;
}

void UpdateMediaInfo() {
    try {
        if (!g_SessionManager) {
            g_SessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        }
        if (!g_SessionManager) return;

        auto session = g_SessionManager.GetCurrentSession();
        if (session) {
            auto props = session.TryGetMediaPropertiesAsync().get();
            auto info = session.GetPlaybackInfo();

            lock_guard<mutex> guard(g_MediaState.lock);
            wstring newTitle = props.Title().c_str();
            wstring newArtist = props.Artist().c_str();
            
            // Update album art if title changed, artist changed, or no art loaded
            bool shouldUpdateArt = (newTitle != g_MediaState.title) || 
                                   (newArtist != g_MediaState.artist) || 
                                   (g_MediaState.albumArt == nullptr);
            
            if (shouldUpdateArt) {
                if (g_MediaState.albumArt) { 
                    delete g_MediaState.albumArt; 
                    g_MediaState.albumArt = nullptr; 
                }
                
                try {
                    auto thumbRef = props.Thumbnail();
                    if (thumbRef) {
                        OutputDebugStringW(L"[AlbumArt] Thumbnail reference available, attempting load...");
                        auto stream = thumbRef.OpenReadAsync().get();
                        Bitmap* newArt = StreamToBitmap(stream);
                        if (newArt) {
                            g_MediaState.albumArt = newArt;
                            OutputDebugStringW(L"[AlbumArt] Successfully loaded album art");
                        } else {
                            OutputDebugStringW(L"[AlbumArt] Failed to convert stream to bitmap");
                        }
                    } else {
                        OutputDebugStringW(L"[AlbumArt] No thumbnail available for current track");
                    }
                } catch (const std::exception& e) {
                    WCHAR dbgMsg[256];
                    swprintf_s(dbgMsg, L"[AlbumArt] Exception loading thumbnail: %hs", e.what());
                    OutputDebugStringW(dbgMsg);
                } catch (...) {
                    OutputDebugStringW(L"[AlbumArt] Unknown exception loading thumbnail");
                }
            }
            
            g_MediaState.title = newTitle;
            g_MediaState.artist = newArtist;
            g_MediaState.isPlaying = (info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
            g_MediaState.hasMedia = true;

            // Debug: log Spotify detection and timeline values
            g_MediaState.isSpotify = false;
            g_MediaState.position = 0.0;
            g_MediaState.duration = 0.0;
            try {
                auto source = session.SourceAppUserModelId();
                WCHAR dbgAppId[256];
                swprintf_s(dbgAppId, L"[SessionAppId] %ls", source.c_str());
                OutputDebugStringW(dbgAppId);
                if (wcsstr(source.c_str(), L"Spotify") != nullptr) {
                    g_MediaState.isSpotify = true;
                    auto timeline = session.GetTimelineProperties();
                    g_MediaState.position = timeline.Position().count() / 10000000.0;
                    g_MediaState.duration = timeline.EndTime().count() / 10000000.0;
                    // If duration is valid, update smoothPosition
                    if (g_MediaState.duration > 0.0) {
                        // If paused or stopped, always set smoothPosition to position
                        auto playbackStatus = info.PlaybackStatus();
                        if (playbackStatus != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                            g_MediaState.smoothPosition = g_MediaState.position;
                        } else {
                            // If seek/jump, update immediately
                            if (abs(g_MediaState.smoothPosition - g_MediaState.position) > 2.0) {
                                g_MediaState.smoothPosition = g_MediaState.position;
                            }
                            // Otherwise, interpolate
                            // (interpolation handled in DrawMediaPanel)
                        }
                        // Update last tick for interpolation
                        g_MediaState.lastUpdateTick = GetTickCount64();
                    } else {
                        g_MediaState.smoothPosition = 0.0;
                        g_MediaState.lastUpdateTick = 0;
                    }
                    WCHAR dbgTimeline[256];
                    swprintf_s(dbgTimeline, L"[SpotifyTimelineRaw] position=%lld duration=%lld", timeline.Position().count(), timeline.EndTime().count());
                    OutputDebugStringW(dbgTimeline);
                }
            } catch (...) {}
            WCHAR dbgMsg[256];
            swprintf_s(dbgMsg, L"[SpotifyUpdate] isSpotify=%d position=%.2f duration=%.2f", g_MediaState.isSpotify ? 1 : 0, g_MediaState.position, g_MediaState.duration);
            OutputDebugStringW(dbgMsg);
        } else {
            lock_guard<mutex> guard(g_MediaState.lock);
            g_MediaState.hasMedia = false;
            g_MediaState.title = L"No Media";
            g_MediaState.artist = L"";
            if (g_MediaState.albumArt) { delete g_MediaState.albumArt; g_MediaState.albumArt = nullptr; }
            g_MediaState.isSpotify = false;
            g_MediaState.position = 0.0;
            g_MediaState.duration = 0.0;
        }
    } catch (...) {
        lock_guard<mutex> guard(g_MediaState.lock);
        g_MediaState.hasMedia = false;
        g_MediaState.isSpotify = false;
        g_MediaState.position = 0.0;
        g_MediaState.duration = 0.0;
    }
}

void SendMediaCommand(int cmd) {
    try {
        if (!g_SessionManager) return;
        auto session = g_SessionManager.GetCurrentSession();
        if (session) {
            if (cmd == 1) session.TrySkipPreviousAsync();
            else if (cmd == 2) session.TryTogglePlayPauseAsync();
            else if (cmd == 3) session.TrySkipNextAsync();
        }
    } catch (...) {}
}

// --- Helper Functions ---
struct TimelineGeometry {
    int barX, barY, barW, barH;
};

TimelineGeometry CalcTimelineGeometry(int startControlX, int panelHeight, int panelWidth, int fontSize) {
    int nX = startControlX + 56;
    int textX = nX + 20;
    int textMaxW = panelWidth - textX - 10;
    
    FontFamily fontFamily(FONT_NAME, nullptr);
    Font font(&fontFamily, (REAL)fontSize, FontStyleBold, UnitPixel);
    RectF layoutRect(0, 0, 2000, 100);
    RectF boundRect;
    Graphics g((HDC)0);
    g.MeasureString(L"A", -1, &font, layoutRect, &boundRect);
    
    float timelineHeight = 10.0f;
    float textY = ((float)panelHeight - boundRect.Height - timelineHeight) / 2.0f;
    int barPadding = 4;
    int barY = (int)(textY + boundRect.Height + barPadding) - 2;
    int barHeight = 7;
    
    return { textX, barY, textMaxW, barHeight };
}

// --- Visuals ---
bool IsSystemLightMode() {
    DWORD value = 0; DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value != 0;
    }
    return false;
}

DWORD GetCurrentTextColor() {
    if (g_Settings.autoTheme) return IsSystemLightMode() ? 0xFF000000 : 0xFFFFFFFF;
    return g_Settings.manualTextColor;
}

void UpdateAppearance(HWND hwnd) {
    // 1. Native Windows 11 Rounding
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));

    // 2. Acrylic Blur
    HMODULE hUser = GetModuleHandle(L"user32.dll");
    if (hUser) {
        auto SetComp = (pSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        if (SetComp) {
            // Calculate tint color based on theme
            DWORD tint = 0; // Default transparent
            if (g_Settings.autoTheme) {
                // Light: Slight white tint, Dark: Slight black tint
                tint = IsSystemLightMode() ? 0x40FFFFFF : 0x40000000;
            } else {
                tint = (g_Settings.bgOpacity << 24) | (0xFFFFFF); // User tint
            }

            ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, tint, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(ACCENT_POLICY) };
            SetComp(hwnd, &data);
        }
    }
}

void DrawMediaPanel(HDC hdc, int width, int height) {
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0)); 

    Color mainColor{GetCurrentTextColor()};
    
    // Lock Data
    MediaState state;
    {
        lock_guard<mutex> guard(g_MediaState.lock);
        state.title = g_MediaState.title;
        state.artist = g_MediaState.artist;
        state.albumArt = g_MediaState.albumArt ? g_MediaState.albumArt->Clone() : nullptr;
        state.hasMedia = g_MediaState.hasMedia;
        state.isPlaying = g_MediaState.isPlaying;
        state.isSpotify = g_MediaState.isSpotify;
        state.position = g_MediaState.position;
        state.duration = g_MediaState.duration;
    }

    // Calculate animation offset
    int separatorX = width - 20;

    // 1. Album Art
    int artSize = height - 12;
    int artX = 6;
    int artY = 6;
    
    if (state.albumArt) {
        graphics.DrawImage(state.albumArt, artX, artY, artSize, artSize);
        delete state.albumArt;
    } else {
        SolidBrush placeBrush{Color(40, 128, 128, 128)};
        graphics.FillRectangle(&placeBrush, artX, artY, artSize, artSize);
    }

    // 2. Controls
    int startControlX = artX + artSize + 12;
    int controlY = height / 2;

    SolidBrush iconBrush{mainColor};
    SolidBrush hoverBrush{Color(255, mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue())};
    SolidBrush activeBg{Color(40, mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue())};

    // Prev
    int pX = startControlX;
    if (g_HoverState == 1) graphics.FillEllipse(&activeBg, pX - 8, controlY - 12, 24, 24);
    Point prevPts[3] = { Point(pX + 8, controlY - 6), Point(pX + 8, controlY + 6), Point(pX, controlY) };
    graphics.FillPolygon(g_HoverState == 1 ? &hoverBrush : &iconBrush, prevPts, 3);
    graphics.FillRectangle(g_HoverState == 1 ? &hoverBrush : &iconBrush, pX, controlY - 6, 2, 12);

    // Play/Pause
    int plX = startControlX + 28;
    if (g_HoverState == 2) graphics.FillEllipse(&activeBg, plX - 8, controlY - 12, 24, 24);
    if (state.isPlaying) {
        graphics.FillRectangle(g_HoverState == 2 ? &hoverBrush : &iconBrush, plX, controlY - 7, 3, 14);
        graphics.FillRectangle(g_HoverState == 2 ? &hoverBrush : &iconBrush, plX + 6, controlY - 7, 3, 14);
    } else {
        Point playPts[3] = { Point(plX, controlY - 8), Point(plX, controlY + 8), Point(plX + 10, controlY) };
        graphics.FillPolygon(g_HoverState == 2 ? &hoverBrush : &iconBrush, playPts, 3);
    }

    // Next
    int nX = startControlX + 56;
    if (g_HoverState == 3) graphics.FillEllipse(&activeBg, nX - 8, controlY - 12, 24, 24);
    Point nextPts[3] = { Point(nX, controlY - 6), Point(nX, controlY + 6), Point(nX + 8, controlY) };
    graphics.FillPolygon(g_HoverState == 3 ? &hoverBrush : &iconBrush, nextPts, 3);
    graphics.FillRectangle(g_HoverState == 3 ? &hoverBrush : &iconBrush, nX + 8, controlY - 6, 2, 12);

    // 3. Vertical Separator Line (20px from right, respecting 10px margin)
    int contentMaxX = separatorX - 10;  // 10px margin to left of separator
    
    // Draw vertical separator line (always visible, position independent)
    // Bold level increases smoothly when hovering
    {
        float lineThickness = 1.0f + (g_HoverBoldLevel * 2.5f);  // 1.0 to 3.5px
        Color sepColor(60 + (int)(60 * g_HoverBoldLevel), mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue());
        Pen sepPen(sepColor, lineThickness);
        graphics.DrawLine(&sepPen, separatorX, 6, separatorX, height - 6);
        
        // Draw music icon in hover area
        int iconX = separatorX + 7;
        int iconY = height / 2;
        Color iconColor(100, 100, 100);  // Grey color
        
        // Draw two musical notes (simplified as circles with stems)
        // First note
        int note1X = iconX - 3;
        int note1Y = iconY + 1;
        SolidBrush noteBrush(iconColor);
        graphics.FillEllipse(&noteBrush, note1X - 2, note1Y, 4, 3);
        Pen noteStem(iconColor, 1.0f);
        graphics.DrawLine(&noteStem, note1X, note1Y - 3, note1X, note1Y);
        
        // Second note (higher)
        int note2X = iconX + 3;
        int note2Y = iconY - 2;
        graphics.FillEllipse(&noteBrush, note2X - 2, note2Y, 4, 3);
        graphics.DrawLine(&noteStem, note2X, note2Y - 3, note2X, note2Y);
        
        // Connecting beam
        graphics.DrawLine(&noteStem, note1X, note1Y - 3, note2X, note2Y - 3);
    }

    // 4. Text
    int textX = nX + 20;
    int textMaxW = contentMaxX - textX;
    if (textMaxW < 50) textMaxW = 50;  // Minimum width

    wstring fullText = state.title;
    if (!state.artist.empty()) fullText += L" • " + state.artist;

    FontFamily fontFamily(FONT_NAME, nullptr);
    Font font(&fontFamily, (REAL)g_Settings.fontSize, FontStyleBold, UnitPixel);
    SolidBrush textBrush{mainColor};

    RectF layoutRect(0, 0, 2000, 100);
    RectF boundRect;
    graphics.MeasureString(fullText.c_str(), -1, &font, layoutRect, &boundRect);
    g_TextWidth = (int)boundRect.Width;

    // Text vertical position: leave space for timeline if Spotify
    float timelineHeight = (state.isSpotify && state.duration > 0.0) ? 10.0f : 0.0f;
    float textY = ((float)height - boundRect.Height - timelineHeight) / 2.0f;

    Region textClip(Rect(textX, 0, textMaxW, height));
    graphics.SetClip(&textClip);

    if (g_TextWidth > textMaxW) {
        g_IsScrolling = true;
        float drawX = (float)(textX - g_ScrollOffset);
        graphics.DrawString(fullText.c_str(), -1, &font, PointF(drawX, textY), &textBrush);
        if (drawX + g_TextWidth < width) {
            graphics.DrawString(fullText.c_str(), -1, &font, PointF(drawX + g_TextWidth + 40, textY), &textBrush);
        }
    } else {
        g_IsScrolling = false;
        g_ScrollOffset = 0;
        graphics.DrawString(fullText.c_str(), -1, &font, PointF((float)textX, textY), &textBrush);
    }

    // 5. Spotify Progression Bar (native look, integrated)
    if (state.isSpotify && state.duration > 0.0) {
        WCHAR dbgMsg[256];
        swprintf_s(dbgMsg, L"[SpotifyTimeline] position=%.2f duration=%.2f progress=%.2f isPlaying=%d", state.position, state.duration, (state.duration > 0.0 ? state.position / state.duration : 0.0), state.isPlaying ? 1 : 0);
        OutputDebugStringW(dbgMsg);

        // Timeline bar geometry
        int barHeight = g_TimelineHover || g_TimelineDragging ? 8 : 5;
        int barPadding = 4;
        int barX = textX;
        int barW = textMaxW;
        int barY = (int)(textY + boundRect.Height + barPadding);

        // Progress calculation
        float progress = (float)(state.position / state.duration);
        if (progress < 0.0f || isnan(progress)) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        float drawProgress = g_TimelineDragging ? g_TimelineDragProgress : progress;

        // Colors: subtle, native, with rounded corners
        Color barBg(32, 0, 0, 0); // subtle dark overlay
        Color barFg(mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue(), 220); // main accent, slightly transparent
        Color barBorder(60, mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue());
        SolidBrush bgBrush(barBg);
        SolidBrush fgBrush(barFg);
        Pen borderPen(barBorder, 1.5f);

        Region fullClip(Rect(0, 0, width, height));
        graphics.SetClip(&fullClip, CombineModeReplace);

        // Draw rounded background bar
        int radius = barHeight / 2;
        GraphicsPath bgPath;
        bgPath.AddArc(barX, barY, barHeight, barHeight, 90, 180);
        bgPath.AddArc(barX + barW - barHeight, barY, barHeight, barHeight, 270, 180);
        bgPath.AddLine(barX + barW - radius, barY + barHeight, barX + radius, barY + barHeight);
        bgPath.CloseFigure();
        graphics.FillPath(&bgBrush, &bgPath);
        graphics.DrawPath(&borderPen, &bgPath);

        // Draw progress (rounded)
        int progW = (int)(barW * drawProgress);
        if (progW > 0) {
            GraphicsPath fgPath;
            if (progW < barHeight) {
                fgPath.AddArc(barX, barY, barHeight, barHeight, 90, 180 * (float)progW / (float)barHeight);
            } else {
                fgPath.AddArc(barX, barY, barHeight, barHeight, 90, 180);
                fgPath.AddArc(barX + progW - barHeight, barY, barHeight, barHeight, 270, 180);
                fgPath.AddLine(barX + progW - radius, barY + barHeight, barX + radius, barY + barHeight);
            }
            fgPath.CloseFigure();
            graphics.FillPath(&fgBrush, &fgPath);
        }

        // Draw seek thumb (circle) if hovered or dragging
        bool showThumb = g_TimelineHover || g_TimelineDragging;
        if (showThumb) {
            int cx = barX + progW;
            int cy = barY + barHeight / 2;
            int thumbRadius = barHeight / 2 + 2; // Smaller thumb
            Color thumbColor(mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue(), 220);
            Color thumbBorder(255, 255, 255, 255);
            SolidBrush thumbBrush(thumbColor);
            Pen thumbPen(thumbBorder, 1.5f);
            graphics.FillEllipse(&thumbBrush, cx - thumbRadius, cy - thumbRadius, thumbRadius * 2, thumbRadius * 2);
            graphics.DrawEllipse(&thumbPen, cx - thumbRadius, cy - thumbRadius, thumbRadius * 2, thumbRadius * 2);
        }

        // Restore text clip for any further drawing
        graphics.SetClip(&textClip);
    }
}

// --- Window Procedure ---
#define IDT_POLL_MEDIA 1001
#define IDT_ANIMATION  1002
#define IDT_HOVER_TIMER 1003
#define APP_WM_CLOSE   WM_APP

LRESULT CALLBACK MediaWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: 
            UpdateAppearance(hwnd); // Apply DWM Rounding + Acrylic
            SetTimer(hwnd, IDT_POLL_MEDIA, 1000, NULL); 
            return 0;

        case WM_ERASEBKGND: 
            return 1;

        case WM_CLOSE:
            return 0;

        case APP_WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_SessionManager = nullptr;
            PostQuitMessage(0);
            return 0;

        case WM_SETTINGCHANGE:
            UpdateAppearance(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_TIMER:
            if (wParam == IDT_POLL_MEDIA) {
                UpdateMediaInfo();
                InvalidateRect(hwnd, NULL, FALSE);
                // Use fast timer for Spotify timeline, slower otherwise
                {
                    lock_guard<mutex> guard(g_MediaState.lock);
                    int timerInterval = 1000;  // Default: 1s
                    if (g_MediaState.isSpotify) {
                        timerInterval = 16;  // 16ms for smooth timeline
                    }
                    SetTimer(hwnd, IDT_POLL_MEDIA, timerInterval, NULL);
                }
            }
            else if (wParam == IDT_HOVER_TIMER) {
                // Update bold level and check timer
                if (g_HoverTabZone) {
                    ULONGLONG elapsed = GetTickCount64() - g_HoverTimerStart;
                    float boldTarget = (float)elapsed / 3000.0f;  // 0.0 to 1.0 over 3 seconds
                    if (boldTarget > 1.0f) boldTarget = 1.0f;
                    g_HoverBoldLevel = boldTarget;
                    
                    if (elapsed >= 3000) {
                        // Trigger panel slide
                        int slideAmount = g_Settings.width - 20;
                        
                        if (g_PanelOpen) {
                            // Slide left (open to closed)
                            g_PanelTargetOffsetX = -slideAmount;
                            g_PanelOpen = false;
                        } else {
                            // Slide right (closed to open)
                            g_PanelTargetOffsetX = 0;
                            g_PanelOpen = true;
                        }
                        
                        // Kill the hover timer since we've triggered the action
                        KillTimer(hwnd, IDT_HOVER_TIMER);
                        g_HoverTabZone = false;
                        g_HoverBoldLevel = 0.0f;
                        
                        // Start animation timer if not already running
                        SetTimer(hwnd, IDT_ANIMATION, 16, NULL);
                        InvalidateRect(hwnd, NULL, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                } else {
                    // Slowly decrease bold level when not hovering
                    ULONGLONG now = GetTickCount64();
                    if (now - g_HoverLastLeftTime < 500) {  // Grace period: 500ms
                        // Still in grace period, decrease bold slowly
                        g_HoverBoldLevel -= 0.05f;  // Decrease over time
                        if (g_HoverBoldLevel < 0.0f) g_HoverBoldLevel = 0.0f;
                        InvalidateRect(hwnd, NULL, FALSE);
                    } else {
                        // Grace period expired, kill the timer
                        KillTimer(hwnd, IDT_HOVER_TIMER);
                        g_HoverBoldLevel = 0.0f;
                    }
                }
            }
            else if (wParam == IDT_ANIMATION) {
                if (g_IsScrolling || g_PanelOffsetX != g_PanelTargetOffsetX) {
                    // Handle text scrolling
                    if (g_IsScrolling) {
                        if (g_ScrollWait > 0) {
                            g_ScrollWait--;
                        } else {
                            g_ScrollOffset++;
                            if (g_ScrollOffset > g_TextWidth + 40) {
                                g_ScrollOffset = 0;
                                g_ScrollWait = 60; 
                            }
                        }
                    }
                    
                    // Handle panel sliding animation
                    if (g_PanelOffsetX != g_PanelTargetOffsetX) {
                        int diff = g_PanelTargetOffsetX - g_PanelOffsetX;
                        int step = (diff > 0) ? 15 : -15;  // 15px per frame
                        
                        if (abs(diff) <= 15) {
                            g_PanelOffsetX = g_PanelTargetOffsetX;
                        } else {
                            g_PanelOffsetX += step;
                        }
                        
                        // Update window position
                        RECT screenRect;
                        SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);
                        int x = g_Settings.offsetX + g_PanelOffsetX;
                        int y = screenRect.bottom - g_Settings.height - g_Settings.offsetY;
                        SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                    
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    KillTimer(hwnd, IDT_ANIMATION); 
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            int artSize = g_Settings.height - 12;
            int startControlX = 6 + artSize + 12;
            int newState = 0;
            bool shouldShowHandCursor = false;
            
            // Tab zone detection (right side of separator line, 20px from right)
            int separatorX = g_Settings.width - 20;
            int tabZoneLeft = separatorX - 5;  // 5px to left of separator for easier targeting
            bool hoveredTabZone = (x >= tabZoneLeft && y >= 6 && y <= g_Settings.height - 6);
            
            // Handle hover timer for tab zone
            if (hoveredTabZone) {
                if (!g_HoverTabZone) {
                    // Entering hover zone
                    ULONGLONG now = GetTickCount64();
                    // Check if recently left (within 500ms grace period)
                    if (g_HoverLastLeftTime > 0 && now - g_HoverLastLeftTime < 500) {
                        // Resume from previous progress
                        ULONGLONG timeSincePreviousStart = now - g_HoverTimerStart;
                        if (timeSincePreviousStart > 3000) {
                            g_HoverTimerStart = now;  // Reset if was too long ago
                        }
                    } else {
                        // Fresh start
                        g_HoverTimerStart = now;
                    }
                    g_HoverTabZone = true;
                    g_HoverLastLeftTime = 0;
                    SetTimer(hwnd, IDT_HOVER_TIMER, 50, NULL);  // Check every 50ms for smooth animation
                }
                shouldShowHandCursor = true;
            } else {
                if (g_HoverTabZone) {
                    // Leaving hover zone, but don't kill the timer yet (grace period)
                    g_HoverTabZone = false;
                    g_HoverLastLeftTime = GetTickCount64();
                    // Timer continues for 500ms to allow smooth unbold animation
                }
            }

            // Timeline bar geometry via helper function
            TimelineGeometry tlGeom = CalcTimelineGeometry(startControlX, g_Settings.height, g_Settings.width, g_Settings.fontSize);
            int barX = tlGeom.barX;
            int barY = tlGeom.barY;
            int barW = tlGeom.barW;
            int barH = tlGeom.barH;
            int circleRadius = 8;

            // Timeline drag/hover logic
            // Don't allow timeline interaction in hover area (right side)
            bool onTimeline = false;
            if (g_MediaState.isSpotify && g_MediaState.duration > 0.0) {
                int rightBoundary = separatorX - 15;  // Exclude hover area
                if (y >= barY - 4 && y <= barY + barH + 8 && x >= barX && x <= barX + barW && x < rightBoundary) {
                    onTimeline = true;
                }
            }

            if (g_TimelineDragging) {
                // Update drag progress
                float rel = (float)(x - barX) / (float)barW;
                if (rel < 0.0f) rel = 0.0f;
                if (rel > 1.0f) rel = 1.0f;
                g_TimelineDragProgress = rel;
                shouldShowHandCursor = true;
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (onTimeline) {
                g_TimelineHover = true;
                shouldShowHandCursor = true;
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                g_TimelineHover = false;
                // Continue to check controls
                if (y > 10 && y < g_Settings.height - 10) {
                    if (x >= startControlX - 10 && x < startControlX + 14) newState = 1;
                    else if (x >= startControlX + 14 && x < startControlX + 42) newState = 2;
                    else if (x >= startControlX + 42 && x < startControlX + 66) newState = 3;
                }
                if (newState > 0) shouldShowHandCursor = true;
                if (newState != g_HoverState) {
                    g_HoverState = newState;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }

            // Update cursor based on hover state
            SetCursor(LoadCursor(NULL, shouldShowHandCursor ? IDC_HAND : IDC_ARROW));

            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_HoverState = 0;
            g_TimelineHover = false;
            g_HoverTabZone = false;
            g_HoverLastLeftTime = GetTickCount64();
            if (!g_TimelineDragging) g_TimelineDragProgress = 0.0f;
            // Reset the timer to normal speed when leaving
            {
                lock_guard<mutex> guard(g_MediaState.lock);
                int timerInterval = 1000;
                if (g_MediaState.isSpotify) {
                    timerInterval = 16;
                }
                SetTimer(hwnd, IDT_POLL_MEDIA, timerInterval, NULL);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            
            // Check if clicking on timeline
            if (g_MediaState.isSpotify && g_MediaState.duration > 0.0) {
                int artSize = g_Settings.height - 12;
                int startControlX = 6 + artSize + 12;
                TimelineGeometry tlGeom = CalcTimelineGeometry(startControlX, g_Settings.height, g_Settings.width, g_Settings.fontSize);
                int circleRadius = 8;
                
                float progress = (float)(g_MediaState.position / g_MediaState.duration);
                if (progress < 0.0f || isnan(progress)) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                
                int cx = tlGeom.barX + (int)(tlGeom.barW * progress);
                int cy = tlGeom.barY + tlGeom.barH / 2;
                int dx = x - cx;
                int dy = y - cy;
                
                // Check if clicking on seek thumb
                if (dx * dx + dy * dy <= circleRadius * circleRadius * 2) {
                    g_TimelineDragging = true;
                    g_TimelineDragProgress = progress;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                
                // Allow clicking anywhere on bar to start drag
                if (y >= tlGeom.barY - 4 && y <= tlGeom.barY + tlGeom.barH + 8 && x >= tlGeom.barX && x <= tlGeom.barX + tlGeom.barW) {
                    float rel = (float)(x - tlGeom.barX) / (float)tlGeom.barW;
                    if (rel < 0.0f) rel = 0.0f;
                    if (rel > 1.0f) rel = 1.0f;
                    g_TimelineDragging = true;
                    g_TimelineDragProgress = rel;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            
            // Don't send command on down, only on up
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_TimelineDragging) {
                // Seek to new time
                if (g_MediaState.isSpotify && g_MediaState.duration > 0.0) {
                    double newTime = g_TimelineDragProgress * g_MediaState.duration;
                    // Seek Spotify
                    try {
                        if (g_SessionManager) {
                            auto session = g_SessionManager.GetCurrentSession();
                            if (session) {
                                auto timeline = session.GetTimelineProperties();
                                auto status = session.GetPlaybackInfo().PlaybackStatus();
                                session.TryChangePlaybackPositionAsync((LONGLONG)(newTime * 10000000)).get();
                                // If paused, update position immediately
                                if (status != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                                    lock_guard<mutex> guard(g_MediaState.lock);
                                    g_MediaState.position = newTime;
                                }
                            }
                        }
                    } catch (...) {}
                }
                g_TimelineDragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // Send control command on button up (not down) to prevent double clicks
            if (g_HoverState > 0) SendMediaCommand(g_HoverState);
            return 0;
        case WM_MOUSEWHEEL: {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            // Reverse scroll: up decreases, down increases
            keybd_event(zDelta > 0 ? VK_VOLUME_DOWN : VK_VOLUME_UP, 0, 0, 0);
            keybd_event(zDelta > 0 ? VK_VOLUME_DOWN : VK_VOLUME_UP, 0, KEYEVENTF_KEYUP, 0);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
            
            DrawMediaPanel(memDC, rc.right, rc.bottom);
            
            if (g_IsScrolling) SetTimer(hwnd, IDT_ANIMATION, 16, NULL);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- Main Thread ---
void MediaThread() {
    winrt::init_apartment();

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = MediaWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("WindhawkMusicLounge_GSMTC");
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

        // Position at bottom-left of screen
        RECT screenRect;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);
        int x = g_Settings.offsetX;
        int y = screenRect.bottom - g_Settings.height - g_Settings.offsetY;
        g_hMediaWindow = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            wc.lpszClassName, TEXT("MusicLounge"),
            WS_POPUP | WS_VISIBLE,
            x, y, g_Settings.width, g_Settings.height,
            NULL, NULL, wc.hInstance, NULL
        );
        ShowWindow(g_hMediaWindow, SW_SHOWNORMAL);
        UpdateWindow(g_hMediaWindow);

    SetLayeredWindowAttributes(g_hMediaWindow, 0, 255, LWA_ALPHA);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClass(wc.lpszClassName, wc.hInstance);
    GdiplusShutdown(gdiplusToken);
    winrt::uninit_apartment();
}

std::thread* g_pMediaThread = nullptr;

// --- CALLBACKS ---
BOOL WhTool_ModInit() {
    LoadSettings(); 
    g_Running = true;
    g_pMediaThread = new std::thread(MediaThread);
    return TRUE;
}

void WhTool_ModUninit() {
    g_Running = false;
    if (g_hMediaWindow) SendMessage(g_hMediaWindow, APP_WM_CLOSE, 0, 0);
    if (g_pMediaThread) {
        if (g_pMediaThread->joinable()) g_pMediaThread->join();
        delete g_pMediaThread;
        g_pMediaThread = nullptr;
    }
}

void WhTool_ModSettingsChanged() {
    LoadSettings();
    if (g_hMediaWindow) {
         SendMessage(g_hMediaWindow, WM_TIMER, IDT_POLL_MEDIA, 0);
         SendMessage(g_hMediaWindow, WM_SETTINGCHANGE, 0, 0); 
    }
}
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    bool isService = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0) {
            isService = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isService) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}