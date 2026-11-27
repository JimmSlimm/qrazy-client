// The client application loop. Standalone: host = null (own window, Esc quits). Embedded: host = the CEF host's main window; hostVisible says when the native view is the one showing, hostStop ends the loop.
#pragma once
#include "nc_vk.h"
#include <atomic>
#include <string>

#if defined(NC_WINDOWS)
typedef HWND NativeWindow;
#else
struct SDL_Window; union SDL_Event;
typedef SDL_Window* NativeWindow;
void appOnSdlEvent(const SDL_Event& e);   // the host forwards its keyboard / mouse / focus events (any thread)
bool appCaptureWanted();                  // the native view wants relative mouse mode: the host applies it on its main thread
bool appTextInputWanted();                // a text box is open: the host runs SDL_StartTextInput (else SDL_StopTextInput) on its main thread
#endif

void appSetAssetsDir(const std::string& dir);   // the folder with fonts/ and hud/ (the client's own files); standalone uses the dev paths
void appSetMenuRequest(void (*f)());                                // embedded: called (on the render thread) when the player presses Esc; the host shows the page's menu
void appSetTabCallback(void (*f)(bool down));                      // embedded: Tab went down / up in the native view; the host passes it to the page, which reports the information board
void appSetBoard(const std::string& json);                          // embedded: the page's information board message (any thread): {"held":bool,"snap":{...}}
bool appTakePoseRequest();   // embedded: true once after the page asked for the pose (op "pose"); the host then pushes qrazyNativePose at once
bool appGetPose(double out[14]);   // as of the last frame, false before the first: eye x, y, z, view yaw, pitch (radians, the sim's axes), feet x, y, z, velocity x, y, z, on ground (0/1), crouched (0/1), held weapon
void appClearOps();      // drop ops of an earlier match; call before starting one
bool appTakeStarted();   // true once after the match loaded its map
bool appHudFeed();
bool appAudioFeed();
void appHudReady();
void appSetSettings(const std::string& json);   // embedded: the player's settings from the page (qrazyNativeSettings), any thread
bool appCursor();   // the mouse is free (chat / console open, or a clickable element on screen)
void appSetPageHud(const std::string& json);    // embedded: chat lines and the typing line from the page (qrazyNativeHud), any thread
void appSetSimOut(void (*f)(const char* json));   // embedded: the sim events the page may want for its HUD texts (pickups, deaths, run events ...): {"t": timerMs, "events": [...]}
int appClickElement(int px, int py, int* element, int* index, char* url, int cap);   // a click on a clickable element (framebuffer px): 0 nothing, 1 forward click (element, index) to the page, 2 open the address in url (map-info link)
void appLogMouse(int kind, int px, int py, bool shift);   // the open chat box / console: kind 0 down, 1 move, 2 up (framebuffer px)
void appLogWheel(double notches);                         // +1 = up (towards older lines)
void appSimOp(const std::string& json);   // embedded: an op of the page for the running sim (respawn, mode, cmd ...), any thread; applied before the next tick
bool appTyping();   // a text box (chat / console) is open: the page takes the keyboard
// The core's text field (chat / team chat / console). Text input reaches it from the host window's thread; the keys come from raw input on the render thread.
void appSetJsOut(void (*f)(const char* js));   // scripts for the hidden page: qrazyNativeTextState({open,mode,reason}), qrazyNativeTextSubmit({mode,text}), qrazyNativeTextEscapeReleased()
void appTextUnit(unsigned utf16Unit);          // WM_CHAR (any thread; surrogates combined)
void appTextCommit(const char* utf8);          // an IME result
void appTextPreedit(const char* utf8, unsigned curFrom, unsigned curTo, unsigned ulFrom, unsigned ulTo);   // the IME composition (UTF-8 byte offsets); empty ends it
bool appTextComposing();
bool appCaretRect(float out[4]);               // the caret in framebuffer px (valid while composing), false before the first draw
int appRun(int argc, char** argv, NativeWindow host, const std::atomic<bool>* hostVisible, const std::atomic<bool>* hostStop, std::atomic<int>* hostFrameCap = nullptr);
