// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include "../../terminal/parser/OutputStateMachineEngine.hpp"
#include "TerminalDispatch.hpp"
#include "../../inc/unicode.hpp"
#include "../../inc/DefaultSettings.h"
#include "../../inc/argb.h"
#include "../../types/inc/utils.hpp"

#include <winrt/Microsoft.Terminal.TerminalControl.h>

using namespace winrt::Microsoft::Terminal::TerminalControl;
using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

static std::wstring _KeyEventsToText(std::deque<std::unique_ptr<IInputEvent>>& inEventsToWrite)
{
    std::wstring wstr = L"";
    for (const auto& ev : inEventsToWrite)
    {
        if (ev->EventType() == InputEventType::KeyEvent)
        {
            const auto& k = static_cast<KeyEvent&>(*ev);
            const auto wch = k.GetCharData();
            wstr += wch;
        }
    }
    return wstr;
}

#pragma warning(suppress : 26455) // default constructor is throwing, too much effort to rearrange at this time.
Terminal::Terminal() :
    _mutableViewport{ Viewport::Empty() },
    _title{},
    _colorTable{},
    _defaultFg{ RGB(255, 255, 255) },
    _defaultBg{ ARGB(0, 0, 0, 0) },
    _screenReversed{ false },
    _pfnWriteInput{ nullptr },
    _scrollOffset{ 0 },
    _snapOnInput{ true },
    _altGrAliasing{ true },
    _blockSelection{ false },
    _selection{ std::nullopt }
{
    auto dispatch = std::make_unique<TerminalDispatch>(*this);
    auto engine = std::make_unique<OutputStateMachineEngine>(std::move(dispatch));

    _stateMachine = std::make_unique<StateMachine>(std::move(engine));

    auto passAlongInput = [&](std::deque<std::unique_ptr<IInputEvent>>& inEventsToWrite) {
        if (!_pfnWriteInput)
        {
            return;
        }
        std::wstring wstr = _KeyEventsToText(inEventsToWrite);
        _pfnWriteInput(wstr);
    };

    _terminalInput = std::make_unique<TerminalInput>(passAlongInput);

    _InitializeColorTable();
}

void Terminal::Create(COORD viewportSize, SHORT scrollbackLines, IRenderTarget& renderTarget)
{
    _mutableViewport = Viewport::FromDimensions({ 0, 0 }, viewportSize);
    _scrollbackLines = scrollbackLines;
    const COORD bufferSize{ viewportSize.X,
                            Utils::ClampToShortMax(viewportSize.Y + scrollbackLines, 1) };
    const TextAttribute attr{};
    const UINT cursorSize = 12;
    _buffer = std::make_unique<TextBuffer>(bufferSize, attr, cursorSize, renderTarget);
}

// Method Description:
// - Initializes the Terminal from the given set of settings.
// Arguments:
// - settings: the set of CoreSettings we need to use to initialize the terminal
// - renderTarget: A render target the terminal can use for paint invalidation.
void Terminal::CreateFromSettings(ICoreSettings settings,
                                  IRenderTarget& renderTarget)
{
    const COORD viewportSize{ Utils::ClampToShortMax(settings.InitialCols(), 1),
                              Utils::ClampToShortMax(settings.InitialRows(), 1) };

    // TODO:MSFT:20642297 - Support infinite scrollback here, if HistorySize is -1
    Create(viewportSize, Utils::ClampToShortMax(settings.HistorySize(), 0), renderTarget);

    UpdateSettings(settings);
}

// Method Description:
// - Update our internal properties to match the new values in the provided
//   CoreSettings object.
// Arguments:
// - settings: an ICoreSettings with new settings values for us to use.
void Terminal::UpdateSettings(ICoreSettings settings)
{
    _defaultFg = settings.DefaultForeground();
    _defaultBg = settings.DefaultBackground();

    CursorType cursorShape = CursorType::VerticalBar;
    switch (settings.CursorShape())
    {
    case CursorStyle::Underscore:
        cursorShape = CursorType::Underscore;
        break;
    case CursorStyle::FilledBox:
        cursorShape = CursorType::FullBox;
        break;
    case CursorStyle::EmptyBox:
        cursorShape = CursorType::EmptyBox;
        break;
    case CursorStyle::Vintage:
        cursorShape = CursorType::Legacy;
        break;
    default:
    case CursorStyle::Bar:
        cursorShape = CursorType::VerticalBar;
        break;
    }

    if (_buffer)
    {
        _buffer->GetCursor().SetStyle(settings.CursorHeight(),
                                      settings.CursorColor(),
                                      cursorShape);
    }

    for (int i = 0; i < 16; i++)
    {
        _colorTable.at(i) = settings.GetColorTableEntry(i);
    }

    _snapOnInput = settings.SnapOnInput();
    _altGrAliasing = settings.AltGrAliasing();
    _wordDelimiters = settings.WordDelimiters();
    _suppressApplicationTitle = settings.SuppressApplicationTitle();
    _startingTitle = settings.StartingTitle();

    _terminalInput->ForceDisableWin32InputMode(settings.ForceVTInput());

    if (settings.TabColor() == nullptr)
    {
        _tabColor = std::nullopt;
    }
    else
    {
        _tabColor = til::color(settings.TabColor().Value() | 0xff000000);
    }
    if (_pfnTabColorChanged)
    {
        _pfnTabColorChanged(_tabColor);
    }

    // TODO:MSFT:21327402 - if HistorySize has changed, resize the buffer so we
    // have a smaller scrollback. We should do this carefully - if the new buffer
    // size is smaller than where the mutable viewport currently is, we'll want
    // to make sure to rotate the buffer contents upwards, so the mutable viewport
    // remains at the bottom of the buffer.
}

// Method Description:
// - Resize the terminal as the result of some user interaction.
// Arguments:
// - viewportSize: the new size of the viewport, in chars
// Return Value:
// - S_OK if we successfully resized the terminal, S_FALSE if there was
//      nothing to do (the viewportSize is the same as our current size), or an
//      appropriate HRESULT for failing to resize.
[[nodiscard]] HRESULT Terminal::UserResize(const COORD viewportSize) noexcept
{
    const auto oldDimensions = _mutableViewport.Dimensions();
    if (viewportSize == oldDimensions)
    {
        return S_FALSE;
    }

    const auto dx = ::base::ClampSub(viewportSize.X, oldDimensions.X);

    const auto oldTop = _mutableViewport.Top();

    const short newBufferHeight = ::base::ClampAdd(viewportSize.Y, _scrollbackLines);

    COORD bufferSize{ viewportSize.X, newBufferHeight };

    // Save cursor's relative height versus the viewport
    const short sCursorHeightInViewportBefore = ::base::ClampSub(_buffer->GetCursor().GetPosition().Y, _mutableViewport.Top());

    // This will be used to determine where the viewport should be in the new buffer.
    const short oldViewportTop = _mutableViewport.Top();
    short newViewportTop = oldViewportTop;
    short newVisibleTop = ::base::saturated_cast<short>(_VisibleStartIndex());

    // If the original buffer had _no_ scroll offset, then we should be at the
    // bottom in the new buffer as well. Track that case now.
    const bool originalOffsetWasZero = _scrollOffset == 0;

    // skip any drawing updates that might occur until we swap _buffer with the new buffer or if we exit early.
    _buffer->GetCursor().StartDeferDrawing();
    // we're capturing _buffer by reference here because when we exit, we want to EndDefer on the current active buffer.
    auto endDefer = wil::scope_exit([&]() noexcept { _buffer->GetCursor().EndDeferDrawing(); });

    // First allocate a new text buffer to take the place of the current one.
    std::unique_ptr<TextBuffer> newTextBuffer;
    try
    {
        newTextBuffer = std::make_unique<TextBuffer>(bufferSize,
                                                     _buffer->GetCurrentAttributes(),
                                                     0, // temporarily set size to 0 so it won't render.
                                                     _buffer->GetRenderTarget());

        newTextBuffer->GetCursor().StartDeferDrawing();

        // Build a PositionInformation to track the position of both the top of
        // the mutable viewport and the top of the visible viewport in the new
        // buffer.
        // * the new value of mutableViewportTop will be used to figure out
        //   where we should place the mutable viewport in the new buffer. This
        //   requires a bit of trickiness to remain consistent with conpty's
        //   buffer (as seen below).
        // * the new value of visibleViewportTop will be used to calculate the
        //   new scrollOffset in the new buffer, so that the visible lines on
        //   the screen remain roughly the same.
        TextBuffer::PositionInformation oldRows{ 0 };
        oldRows.mutableViewportTop = oldViewportTop;
        oldRows.visibleViewportTop = newVisibleTop;

        const std::optional<short> oldViewStart{ oldViewportTop };
        RETURN_IF_FAILED(TextBuffer::Reflow(*_buffer.get(),
                                            *newTextBuffer.get(),
                                            _mutableViewport,
                                            { oldRows }));

        newViewportTop = oldRows.mutableViewportTop;
        newVisibleTop = oldRows.visibleViewportTop;
    }
    CATCH_RETURN();

    // Conpty resizes a little oddly - if the height decreased, and there were
    // blank lines at the bottom, those lines will get trimmed. If there's not
    // blank lines, then the top will get "shifted down", moving the top line
    // into scrollback. See GH#3490 for more details.
    //
    // If the final position in the buffer is on the bottom row of the new
    // viewport, then we're going to need to move the top down. Otherwise, move
    // the bottom up.
    //
    // There are also important things to consider with line wrapping.
    // * If a line in scrollback wrapped that didn't previously, we'll need to
    //   make sure to have the new viewport down another line. This will cause
    //   our top to move down.
    // * If a line _in the viewport_ wrapped that didn't previously, then the
    //   conpty buffer will also have that wrapped line, and will move the
    //   cursor & text down a line in response. This causes our bottom to move
    //   down.
    //
    // We're going to use a combo of both these things to calculate where the
    // new viewport should be. To keep in sync with conpty, we'll need to make
    // sure that any lines that entered the scrollback _stay in scrollback_. We
    // do that by taking the max of
    // * Where the old top line in the viewport exists in the new buffer (as
    //   calculated by TextBuffer::Reflow)
    // * Where the bottom of the text in the new buffer is (and using that to
    //   calculate another proposed top location).

    const COORD newCursorPos = newTextBuffer->GetCursor().GetPosition();
#pragma warning(push)
#pragma warning(disable : 26496) // cpp core checks wants this const, but it's assigned immediately below...
    COORD newLastChar = newCursorPos;
    try
    {
        newLastChar = newTextBuffer->GetLastNonSpaceCharacter();
    }
    CATCH_LOG();
#pragma warning(pop)

    const auto maxRow = std::max(newLastChar.Y, newCursorPos.Y);

    const short proposedTopFromLastLine = ::base::ClampAdd(::base::ClampSub(maxRow, viewportSize.Y), 1);
    const short proposedTopFromScrollback = newViewportTop;

    short proposedTop = std::max(proposedTopFromLastLine,
                                 proposedTopFromScrollback);

    // If we're using the new location of the old top line to place the
    // viewport, we might need to make an adjustment to it.
    //
    // We're using the last cell of the line to calculate where the top line is
    // in the new buffer. If that line wrapped, then all the lines below it
    // shifted down in the buffer. If there's space for all those lines in the
    // conpty buffer, then the originally unwrapped top line will _still_ be in
    // the buffer. In that case, don't stick to the _end_ of the old top line,
    // instead stick to the _start_, which is one line up.
    //
    // We can know if there's space in the conpty buffer by checking if the
    // maxRow (the highest row we've written text to) is above the viewport from
    // this proposed top position.
    if (proposedTop == proposedTopFromScrollback)
    {
        const auto proposedViewFromTop = Viewport::FromDimensions({ 0, proposedTopFromScrollback }, viewportSize);
        if (maxRow < proposedViewFromTop.BottomInclusive())
        {
            if (dx < 0 && proposedTop > 0)
            {
                try
                {
                    auto row = newTextBuffer->GetRowByOffset(::base::ClampSub(proposedTop, 1));
                    if (row.GetCharRow().WasWrapForced())
                    {
                        proposedTop--;
                    }
                }
                CATCH_LOG();
            }
        }
    }

    // If the new bottom would be higher than the last row of text, then we
    // definitely want to use the last row of text to determine where the
    // viewport should be.
    const auto proposedViewFromTop = Viewport::FromDimensions({ 0, proposedTopFromScrollback }, viewportSize);
    if (maxRow > proposedViewFromTop.BottomInclusive())
    {
        proposedTop = proposedTopFromLastLine;
    }

    // Make sure the proposed viewport is within the bounds of the buffer.
    // First make sure the top is >=0
    proposedTop = std::max(static_cast<short>(0), proposedTop);

    // If the new bottom would be below the bottom of the buffer, then slide the
    // top up so that we'll still fit within the buffer.
    const auto newView = Viewport::FromDimensions({ 0, proposedTop }, viewportSize);
    const auto proposedBottom = newView.BottomExclusive();
    if (proposedBottom > bufferSize.Y)
    {
        proposedTop = ::base::ClampSub(proposedTop, ::base::ClampSub(proposedBottom, bufferSize.Y));
    }

    _mutableViewport = Viewport::FromDimensions({ 0, proposedTop }, viewportSize);

    _buffer.swap(newTextBuffer);

    // GH#3494: Maintain scrollbar position during resize
    // Make sure that we don't scroll past the mutableViewport at the bottom of the buffer
    newVisibleTop = std::min(newVisibleTop, _mutableViewport.Top());
    // Make sure we don't scroll past the top of the scrollback
    newVisibleTop = std::max<short>(newVisibleTop, 0);

    // If the old scrolloffset was 0, then we weren't scrolled back at all
    // before, and shouldn't be now either.
    _scrollOffset = originalOffsetWasZero ? 0 : static_cast<int>(::base::ClampSub(_mutableViewport.Top(), newVisibleTop));

    // GH#5029 - make sure to InvalidateAll here, so that we'll paint the entire visible viewport.
    try
    {
        _buffer->GetRenderTarget().TriggerRedrawAll();
    }
    CATCH_LOG();
    _NotifyScrollEvent();

    return S_OK;
}

void Terminal::Write(std::wstring_view stringView)
{
    auto lock = LockForWriting();

    _stateMachine->ProcessString(stringView);
}

// Method Description:
// - Attempts to snap to the bottom of the buffer, if SnapOnInput is true. Does
//   nothing if SnapOnInput is set to false, or we're already at the bottom of
//   the buffer.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Terminal::TrySnapOnInput()
{
    if (_snapOnInput && _scrollOffset != 0)
    {
        auto lock = LockForWriting();
        _scrollOffset = 0;
        _NotifyScrollEvent();
    }
}

// Routine Description:
// - Relays if we are tracking mouse input
// Parameters:
// - <none>
// Return value:
// - true, if we are tracking mouse input. False, otherwise
bool Terminal::IsTrackingMouseInput() const noexcept
{
    return _terminalInput->IsTrackingMouseInput();
}

// Method Description:
// - Send this particular (non-character) key event to the terminal.
// - The terminal will translate the key and the modifiers pressed into the
//   appropriate VT sequence for that key chord. If we do translate the key,
//   we'll return true. In that case, the event should NOT be processed any further.
// - Character events (e.g. WM_CHAR) are generally the best way to properly receive
//   keyboard input on Windows though, as the OS is suited best at handling the
//   translation of the current keyboard layout, dead keys, etc.
//   As a result of this false is returned for all key events that contain characters.
//   SendCharEvent may then be called with the data obtained from a character event.
// - As a special case we'll always handle VK_TAB key events.
//   This must be done due to TermControl::_KeyDownHandler (one of the callers)
//   always marking tab key events as handled, causing no character event to be raised.
// Arguments:
// - vkey: The vkey of the last pressed key.
// - scanCode: The scan code of the last pressed key.
// - states: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
// - keyDown: If true, the key was pressed, otherwise the key was released.
// Return Value:
// - true if we translated the key event, and it should not be processed any further.
// - false if we did not translate the key, and it should be processed into a character.
bool Terminal::SendKeyEvent(const WORD vkey,
                            const WORD scanCode,
                            const ControlKeyStates states,
                            const bool keyDown)
{
    // GH#6423 - don't snap on this key if the key that was pressed was a
    // modifier key. We'll wait for a real keystroke to snap to the bottom.
    // GH#6481 - Additionally, make sure the key was actually pressed. This
    // check will make sure we behave the same as before GH#6309
    if (!KeyEvent::IsModifierKey(vkey) && keyDown)
    {
        TrySnapOnInput();
    }

    _StoreKeyEvent(vkey, scanCode);

    // As a Terminal we're mostly interested in getting key events from physical hardware (mouse & keyboard).
    // We're thus ignoring events whose values are outside the valid range and unlikely to be generated by the current keyboard.
    // It's very likely that a proper followup character event will be sent to us.
    // This prominently happens using AutoHotKey's keyboard remapping feature,
    // which sends input events whose vkey is 0xff and scanCode is 0.
    // We need to check for this early, as _CharacterFromKeyEvent() always returns 0 for such invalid values,
    // making us believe that this is an actual non-character input, while it usually isn't.
    // GH#7064
    if (vkey == 0 || vkey >= 0xff || scanCode == 0)
    {
        return false;
    }

    const auto isAltOnlyPressed = states.IsAltPressed() && !states.IsCtrlPressed();
    const auto isSuppressedAltGrAlias = !_altGrAliasing && states.IsAltPressed() && states.IsCtrlPressed();

    // DON'T manually handle Alt+Space - the system will use this to bring up
    // the system menu for restore, min/maximize, size, move, close.
    // (This doesn't apply to Ctrl+Alt+Space.)
    if (isAltOnlyPressed && vkey == VK_SPACE)
    {
        return false;
    }

    // By default Windows treats Ctrl+Alt as an alias for AltGr.
    // When the altGrAliasing setting is set to false, this behaviour should be disabled.
    //
    // Whenever possible _CharacterFromKeyEvent() will return a valid character.
    // For instance both Ctrl+Alt+Q as well as AltGr+Q return @ on a German keyboard.
    //
    // We can achieve the altGrAliasing functionality by skipping the call to _CharacterFromKeyEvent,
    // as TerminalInput::HandleKey will then fall back to using the vkey which
    // is the underlying ASCII character (e.g. A-Z) on the keyboard in our case.
    // See GH#5525/GH#6211 for more details
    const auto ch = isSuppressedAltGrAlias ? UNICODE_NULL : _CharacterFromKeyEvent(vkey, scanCode, states);

    // Delegate it to the character event handler if this key event can be
    // mapped to one (see method description above). For Alt+key combinations
    // we'll not receive another character event for some reason though.
    // -> Don't delegate the event if this is a Alt+key combination.
    //
    // As a special case we'll furthermore always handle VK_TAB
    // key events here instead of in Terminal::SendCharEvent.
    // See the method description for more information.
    if (!isAltOnlyPressed && vkey != VK_TAB && ch != UNICODE_NULL)
    {
        return false;
    }

    KeyEvent keyEv{ keyDown, 1, vkey, scanCode, ch, states.Value() };
    return _terminalInput->HandleKey(&keyEv);
}

// Method Description:
// - Send this particular mouse event to the terminal. The terminal will translate
//   the button and the modifiers pressed into the appropriate VT sequence for that
//   mouse event. If we do translate the key, we'll return true. In that case, the
//   event should NOT be processed any further. If we return false, the event
//   was NOT translated, and we should instead use the event normally
// Arguments:
// - viewportPos: the position of the mouse event relative to the viewport origin.
// - uiButton: the WM mouse button event code
// - states: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
// - wheelDelta: the amount that the scroll wheel changed (should be 0 unless button is a WM_MOUSE*WHEEL)
// Return Value:
// - true if we translated the key event, and it should not be processed any further.
// - false if we did not translate the key, and it should be processed into a character.
bool Terminal::SendMouseEvent(const COORD viewportPos, const unsigned int uiButton, const ControlKeyStates states, const short wheelDelta, const TerminalInput::MouseButtonState state)
{
    // GH#6401: VT applications should be able to receive mouse events from outside the
    // terminal buffer. This is likely to happen when the user drags the cursor offscreen.
    // We shouldn't throw away perfectly good events when they're offscreen, so we just
    // clamp them to be within the range [(0, 0), (W, H)].
#pragma warning(suppress : 26496) // analysis can't tell we're assigning through a reference below
    auto clampedPos{ viewportPos };
    _mutableViewport.ToOrigin().Clamp(clampedPos);
    return _terminalInput->HandleMouse(clampedPos, uiButton, GET_KEYSTATE_WPARAM(states.Value()), wheelDelta, state);
}

// Method Description:
// - Send this particular character to the terminal.
// - This method is the counterpart to SendKeyEvent and behaves almost identical.
//   The difference is the focus on sending characters to the terminal,
//   whereas SendKeyEvent handles the sending of keys like the arrow keys.
// Arguments:
// - ch: The UTF-16 code unit to be sent.
// - scanCode: The scan code of the last pressed key. Can be left 0.
// - states: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
// Return Value:
// - true if we translated the character event, and it should not be processed any further.
// - false otherwise.
bool Terminal::SendCharEvent(const wchar_t ch, const WORD scanCode, const ControlKeyStates states)
{
    // DON'T manually handle Alt+Space - the system will use this to bring up
    // the system menu for restore, min/maximize, size, move, close.
    if (ch == L' ' && states.IsAltPressed() && !states.IsCtrlPressed())
    {
        return false;
    }

    auto vkey = _TakeVirtualKeyFromLastKeyEvent(scanCode);
    if (vkey == 0 && scanCode != 0)
    {
        vkey = _VirtualKeyFromScanCode(scanCode);
    }
    if (vkey == 0)
    {
        vkey = _VirtualKeyFromCharacter(ch);
    }

    // Unfortunately, the UI doesn't give us both a character down and a
    // character up event, only a character received event. So fake sending both
    // to the terminal input translator. Unless it's in win32-input-mode, it'll
    // ignore the keyup.
    KeyEvent keyDown{ true, 1, vkey, scanCode, ch, states.Value() };
    KeyEvent keyUp{ false, 1, vkey, scanCode, ch, states.Value() };
    const auto handledDown = _terminalInput->HandleKey(&keyDown);
    const auto handledUp = _terminalInput->HandleKey(&keyUp);
    return handledDown || handledUp;
}

// Method Description:
// - Returns the keyboard's scan code for the given virtual key code.
// Arguments:
// - vkey: The virtual key code.
// Return Value:
// - The keyboard's scan code.
WORD Terminal::_ScanCodeFromVirtualKey(const WORD vkey) noexcept
{
    return LOWORD(MapVirtualKeyW(vkey, MAPVK_VK_TO_VSC));
}

// Method Description:
// - Returns the virtual key code for the given keyboard's scan code.
// Arguments:
// - scanCode: The keyboard's scan code.
// Return Value:
// - The virtual key code. 0 if no mapping can be found.
WORD Terminal::_VirtualKeyFromScanCode(const WORD scanCode) noexcept
{
    return LOWORD(MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK));
}

// Method Description:
// - Returns any virtual key code that produces the given character.
// Arguments:
// - scanCode: The keyboard's scan code.
// Return Value:
// - The virtual key code. 0 if no mapping can be found.
WORD Terminal::_VirtualKeyFromCharacter(const wchar_t ch) noexcept
{
    const auto vkey = LOWORD(VkKeyScanW(ch));
    return vkey == -1 ? 0 : vkey;
}

// Method Description:
// - Translates the specified virtual key code and keyboard state to the corresponding character.
// Arguments:
// - vkey: The virtual key code that initiated this keyboard event.
// - scanCode: The scan code that initiated this keyboard event.
// - states: The current keyboard state.
// Return Value:
// - The character that would result from this virtual key code and keyboard state.
wchar_t Terminal::_CharacterFromKeyEvent(const WORD vkey, const WORD scanCode, const ControlKeyStates states) noexcept
try
{
    const auto sc = scanCode != 0 ? scanCode : _ScanCodeFromVirtualKey(vkey);

    // We might want to use GetKeyboardState() instead of building our own keyState.
    // The question is whether that's necessary though. For now it seems to work fine as it is.
    std::array<BYTE, 256> keyState = {};
    keyState.at(VK_SHIFT) = states.IsShiftPressed() ? 0x80 : 0;
    keyState.at(VK_CONTROL) = states.IsCtrlPressed() ? 0x80 : 0;
    keyState.at(VK_MENU) = states.IsAltPressed() ? 0x80 : 0;

    // For the following use of ToUnicodeEx() please look here:
    //   https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicodeex

    // Technically ToUnicodeEx() can produce arbitrarily long sequences of diacritics etc.
    // Since we only handle the case of a single UTF-16 code point, we can set the buffer size to 2 though.
    std::array<wchar_t, 2> buffer;

    // wFlags:
    // * If bit 0 is set, a menu is active.
    //   If this flag is not specified ToUnicodeEx will send us character events on certain Alt+Key combinations (e.g. Alt+Arrow-Up).
    // * If bit 2 is set, keyboard state is not changed (Windows 10, version 1607 and newer)
    const auto result = ToUnicodeEx(vkey, sc, keyState.data(), buffer.data(), gsl::narrow_cast<int>(buffer.size()), 0b101, nullptr);

    // TODO:GH#2853 We're only handling single UTF-16 code points right now, since that's the only thing KeyEvent supports.
    return result == 1 || result == -1 ? buffer.at(0) : 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return UNICODE_INVALID;
}

// Method Description:
// - It's possible for a single scan code on a keyboard to
//   produce different key codes depending on the keyboard state.
//   MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK) will always chose one of the
//   possibilities no matter what though and thus can't be used in SendCharEvent.
// - This method stores the key code from a key event (SendKeyEvent).
//   If the key event contains character data, handling of the event will be
//   denied, in order to delegate the work to the character event handler.
// - The character event handler (SendCharEvent) will now pick up
//   the stored key code to restore the full key event data.
// Arguments:
// - vkey: The virtual key code.
// - scanCode: The scan code.
void Terminal::_StoreKeyEvent(const WORD vkey, const WORD scanCode)
{
    _lastKeyEventCodes.emplace(KeyEventCodes{ vkey, scanCode });
}

// Method Description:
// - This method acts as a counterpart to _StoreKeyEvent and extracts a stored
//   key code. As a safety measure it'll ensure that the given scan code
//   matches the stored scan code from the previous key event.
// - See _StoreKeyEvent for more information.
// Arguments:
// - scanCode: The scan code.
// Return Value:
// - The key code matching the given scan code. Otherwise 0.
WORD Terminal::_TakeVirtualKeyFromLastKeyEvent(const WORD scanCode) noexcept
{
    const auto codes = _lastKeyEventCodes.value_or(KeyEventCodes{});
    _lastKeyEventCodes.reset();
    return codes.ScanCode == scanCode ? codes.VirtualKey : 0;
}

// Method Description:
// - Acquire a read lock on the terminal.
// Return Value:
// - a shared_lock which can be used to unlock the terminal. The shared_lock
//      will release this lock when it's destructed.
[[nodiscard]] std::shared_lock<std::shared_mutex> Terminal::LockForReading()
{
    return std::shared_lock<std::shared_mutex>(_readWriteLock);
}

// Method Description:
// - Acquire a write lock on the terminal.
// Return Value:
// - a unique_lock which can be used to unlock the terminal. The unique_lock
//      will release this lock when it's destructed.
[[nodiscard]] std::unique_lock<std::shared_mutex> Terminal::LockForWriting()
{
    return std::unique_lock<std::shared_mutex>(_readWriteLock);
}

Viewport Terminal::_GetMutableViewport() const noexcept
{
    return _mutableViewport;
}

short Terminal::GetBufferHeight() const noexcept
{
    return _mutableViewport.BottomExclusive();
}

// ViewStartIndex is also the length of the scrollback
int Terminal::ViewStartIndex() const noexcept
{
    return _mutableViewport.Top();
}

int Terminal::ViewEndIndex() const noexcept
{
    return _mutableViewport.BottomInclusive();
}

// _VisibleStartIndex is the first visible line of the buffer
int Terminal::_VisibleStartIndex() const noexcept
{
    return std::max(0, ViewStartIndex() - _scrollOffset);
}

int Terminal::_VisibleEndIndex() const noexcept
{
    return std::max(0, ViewEndIndex() - _scrollOffset);
}

Viewport Terminal::_GetVisibleViewport() const noexcept
{
    const COORD origin{ 0, gsl::narrow<short>(_VisibleStartIndex()) };
    return Viewport::FromDimensions(origin,
                                    _mutableViewport.Dimensions());
}

// Writes a string of text to the buffer, then moves the cursor (and viewport)
//      in accordance with the written text.
// This method is our proverbial `WriteCharsLegacy`, and great care should be made to
//      keep it minimal and orderly, lest it become WriteCharsLegacy2ElectricBoogaloo
// TODO: MSFT 21006766
//       This needs to become stream logic on the buffer itself sooner rather than later
//       because it's otherwise impossible to avoid the Electric Boogaloo-ness here.
//       I had to make a bunch of hacks to get Japanese and emoji to work-ish.
void Terminal::_WriteBuffer(const std::wstring_view& stringView)
{
    auto& cursor = _buffer->GetCursor();

    // Defer the cursor drawing while we are iterating the string, for a better performance.
    // We can not waste time displaying a cursor event when we know more text is coming right behind it.
    cursor.StartDeferDrawing();

    for (size_t i = 0; i < stringView.size(); i++)
    {
        const auto wch = stringView.at(i);
        const COORD cursorPosBefore = cursor.GetPosition();
        COORD proposedCursorPosition = cursorPosBefore;

        // TODO: MSFT 21006766
        // This is not great but I need it demoable. Fix by making a buffer stream writer.
        //
        // If wch is a surrogate character we need to read 2 code units
        // from the stringView to form a single code point.
        const auto isSurrogate = wch >= 0xD800 && wch <= 0xDFFF;
        const auto view = stringView.substr(i, isSurrogate ? 2 : 1);
        const OutputCellIterator it{ view, _buffer->GetCurrentAttributes() };
        const auto end = _buffer->Write(it);
        const auto cellDistance = end.GetCellDistance(it);
        const auto inputDistance = end.GetInputDistance(it);

        if (inputDistance > 0)
        {
            // If "wch" was a surrogate character, we just consumed 2 code units above.
            // -> Increment "i" by 1 in that case and thus by 2 in total in this iteration.
            proposedCursorPosition.X += gsl::narrow<SHORT>(cellDistance);
            i += inputDistance - 1;
        }
        else
        {
            // If _WriteBuffer() is called with a consecutive string longer than the viewport/buffer width
            // the call to _buffer->Write() will refuse to write anything on the current line.
            // GetInputDistance() thus returns 0, which would in turn cause i to be
            // decremented by 1 below and force the outer loop to loop forever.
            // This if() basically behaves as if "\r\n" had been encountered above and retries the write.
            // With well behaving shells during normal operation this safeguard should normally not be encountered.
            proposedCursorPosition.X = 0;
            proposedCursorPosition.Y++;

            // Try the character again.
            i--;

            // If we write the last cell of the row here, TextBuffer::Write will
            // mark this line as wrapped for us. If the next character we
            // process is a newline, the Terminal::CursorLineFeed will unmark
            // this line as wrapped.

            // TODO: GH#780 - This should really be a _deferred_ newline. If
            // the next character to come in is a newline or a cursor
            // movement or anything, then we should _not_ wrap this line
            // here.
        }

        _AdjustCursorPosition(proposedCursorPosition);
    }

    cursor.EndDeferDrawing();
}

void Terminal::_AdjustCursorPosition(const COORD proposedPosition)
{
#pragma warning(suppress : 26496) // cpp core checks wants this const but it's modified below.
    auto proposedCursorPosition = proposedPosition;
    auto& cursor = _buffer->GetCursor();
    const Viewport bufferSize = _buffer->GetSize();

    // If we're about to scroll past the bottom of the buffer, instead cycle the
    // buffer.
    SHORT rowsPushedOffTopOfBuffer = 0;
    const auto newRows = std::max(0, proposedCursorPosition.Y - bufferSize.Height() + 1);
    if (proposedCursorPosition.Y >= bufferSize.Height())
    {
        for (auto dy = 0; dy < newRows; dy++)
        {
            _buffer->IncrementCircularBuffer();
            proposedCursorPosition.Y--;
            rowsPushedOffTopOfBuffer++;
        }
    }

    // Update Cursor Position
    cursor.SetPosition(proposedCursorPosition);

    // Move the viewport down if the cursor moved below the viewport.
    bool updatedViewport = false;
    const auto scrollAmount = std::max(0, proposedCursorPosition.Y - _mutableViewport.BottomInclusive());
    if (scrollAmount > 0)
    {
        const auto newViewTop = std::max(0, proposedCursorPosition.Y - (_mutableViewport.Height() - 1));
        if (newViewTop != _mutableViewport.Top())
        {
            _mutableViewport = Viewport::FromDimensions({ 0, gsl::narrow<short>(newViewTop) },
                                                        _mutableViewport.Dimensions());
            updatedViewport = true;
        }
    }

    if (updatedViewport)
    {
        // scroll if...
        //   - no selection is active
        //   - viewport is already at the bottom
        const bool scrollToOutput = !IsSelectionActive() && _scrollOffset == 0;

        _scrollOffset = scrollToOutput ? 0 : _scrollOffset + scrollAmount + newRows;

        _NotifyScrollEvent();
    }

    if (rowsPushedOffTopOfBuffer != 0)
    {
        // We have to report the delta here because we might have circled the text buffer.
        // That didn't change the viewport and therefore the TriggerScroll(void)
        // method can't detect the delta on its own.
        COORD delta{ 0, -rowsPushedOffTopOfBuffer };
        _buffer->GetRenderTarget().TriggerScroll(&delta);
    }

    _NotifyTerminalCursorPositionChanged();
}

void Terminal::UserScrollViewport(const int viewTop)
{
    // we're going to modify state here that the renderer could be reading.
    auto lock = LockForWriting();

    const auto clampedNewTop = std::max(0, viewTop);
    const auto realTop = ViewStartIndex();
    const auto newDelta = realTop - clampedNewTop;
    // if viewTop > realTop, we want the offset to be 0.

    _scrollOffset = std::max(0, newDelta);

    // We can use the void variant of TriggerScroll here because
    // we adjusted the viewport so it can detect the difference
    // from the previous frame drawn.
    _buffer->GetRenderTarget().TriggerScroll();
}

int Terminal::GetScrollOffset() noexcept
{
    return _VisibleStartIndex();
}

void Terminal::_NotifyScrollEvent() noexcept
try
{
    if (_pfnScrollPositionChanged)
    {
        const auto visible = _GetVisibleViewport();
        const auto top = visible.Top();
        const auto height = visible.Height();
        const auto bottom = this->GetBufferHeight();
        _pfnScrollPositionChanged(top, height, bottom);
    }
}
CATCH_LOG()

void Terminal::_NotifyTerminalCursorPositionChanged() noexcept
{
    if (_pfnCursorPositionChanged)
    {
        try
        {
            _pfnCursorPositionChanged();
        }
        CATCH_LOG();
    }
}

void Terminal::SetWriteInputCallback(std::function<void(std::wstring&)> pfn) noexcept
{
    _pfnWriteInput.swap(pfn);
}

void Terminal::SetTitleChangedCallback(std::function<void(const std::wstring_view&)> pfn) noexcept
{
    _pfnTitleChanged.swap(pfn);
}

void Terminal::SetTabColorChangedCallback(std::function<void(const std::optional<til::color>)> pfn) noexcept
{
    _pfnTabColorChanged.swap(pfn);
}

void Terminal::SetCopyToClipboardCallback(std::function<void(const std::wstring_view&)> pfn) noexcept
{
    _pfnCopyToClipboard.swap(pfn);
}

void Terminal::SetScrollPositionChangedCallback(std::function<void(const int, const int, const int)> pfn) noexcept
{
    _pfnScrollPositionChanged.swap(pfn);
}

void Terminal::SetCursorPositionChangedCallback(std::function<void()> pfn) noexcept
{
    _pfnCursorPositionChanged.swap(pfn);
}

// Method Description:
// - Allows setting a callback for when the background color is changed
// Arguments:
// - pfn: a function callback that takes a uint32 (DWORD COLORREF) color in the format 0x00BBGGRR
void Terminal::SetBackgroundCallback(std::function<void(const COLORREF)> pfn) noexcept
{
    _pfnBackgroundColorChanged.swap(pfn);
}

void Terminal::_InitializeColorTable()
try
{
    const gsl::span<COLORREF> tableView = { _colorTable.data(), _colorTable.size() };
    // First set up the basic 256 colors
    Utils::Initialize256ColorTable(tableView);
    // Then use fill the first 16 values with the Campbell scheme
    Utils::InitializeCampbellColorTable(tableView);
    // Then make sure all the values have an alpha of 255
    Utils::SetColorTableAlpha(tableView, 0xff);
}
CATCH_LOG()

// Method Description:
// - Sets the cursor to be currently on. On/Off is tracked independently of
//   cursor visibility (hidden/visible). On/off is controlled by the cursor
//   blinker. Visibility is usually controlled by the client application. If the
//   cursor is hidden, then the cursor will remain hidden. If the cursor is
//   Visible, then it will immediately become visible.
// Arguments:
// - isVisible: whether the cursor should be visible
void Terminal::SetCursorOn(const bool isOn)
{
    auto lock = LockForWriting();
    _buffer->GetCursor().SetIsOn(isOn);
}

bool Terminal::IsCursorBlinkingAllowed() const noexcept
{
    const auto& cursor = _buffer->GetCursor();
    return cursor.IsBlinkingAllowed();
}

const std::optional<til::color> Terminal::GetTabColor() const noexcept
{
    return _tabColor;
}
