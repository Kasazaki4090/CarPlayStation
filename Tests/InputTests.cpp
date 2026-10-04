#include "Core/Common/Types.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Input/InputEvents.h"
#include "Core/Input/MouseTouchTranslator.h"
#include "Core/Input/KeyboardKnobTranslator.h"
#include "Core/Input/MultiTouchInjector.h"
#include "Core/Input/MockInputEventSink.h"
#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <type_traits>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Input;

// Print a value to a stream. Enums are printed as their underlying integer so
// the ASSERT macros can report them without requiring operator<< overloads.
template <typename T>
void PrintValue(std::ostream& os, const T& v) {
    if constexpr (std::is_enum_v<T>) {
        os << static_cast<std::underlying_type_t<T>>(v);
    } else {
        os << v;
    }
}

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "Assertion failed: " #expr " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "Assertion failed: " #a " == " #b " ("; \
            PrintValue(std::cerr, (a)); \
            std::cerr << " != "; \
            PrintValue(std::cerr, (b)); \
            std::cerr << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// Step 2: MouseTouchTranslator — pointer -> touch, right-click -> button,
// wheel -> rotary knob.
// ---------------------------------------------------------------------------
void TestMousePointerToTouch() {
    std::cout << "[RUN] TestMousePointerToTouch" << std::endl;

    MockInputEventSink sink;
    MouseTouchTranslator::Config cfg;
    cfg.windowWidth = 1920;
    cfg.windowHeight = 720;
    MouseTouchTranslator translator(sink, cfg);

    // Left button down at (960, 360) -> normalized (0.5, 0.5).
    PointerEvent down;
    down.type = PointerType::Mouse;
    down.button = PointerButton::Left;
    down.isDown = true;
    down.x = 960.0f;
    down.y = 360.0f;
    down.pointerId = 0;
    translator.OnPointerEvent(down);

    ASSERT_TRUE(translator.IsTouchActive());
    ASSERT_EQ(sink.TouchCount(), 1U);
    auto touches = sink.TouchEvents();
    ASSERT_EQ(touches[0].action, TouchAction::Down);
    ASSERT_TRUE(std::abs(touches[0].point.x_normalized - 0.5f) < 1e-4f);
    ASSERT_TRUE(std::abs(touches[0].point.y_normalized - 0.5f) < 1e-4f);

    // Move while held -> Touch Move.
    PointerEvent move;
    move.type = PointerType::Mouse;
    move.button = PointerButton::Left;
    move.isDown = true;
    move.x = 480.0f;
    move.y = 180.0f;
    move.pointerId = 0;
    translator.OnPointerEvent(move);
    ASSERT_EQ(sink.TouchCount(), 2U);
    touches = sink.TouchEvents();
    ASSERT_EQ(touches[1].action, TouchAction::Move);
    ASSERT_TRUE(std::abs(touches[1].point.x_normalized - 0.25f) < 1e-4f);

    // Left button up -> Touch Up, touch no longer active.
    PointerEvent up;
    up.type = PointerType::Mouse;
    up.button = PointerButton::Left;
    up.isDown = false;
    up.x = 480.0f;
    up.y = 180.0f;
    up.pointerId = 0;
    translator.OnPointerEvent(up);
    ASSERT_TRUE(!translator.IsTouchActive());
    ASSERT_EQ(sink.TouchCount(), 3U);
    touches = sink.TouchEvents();
    ASSERT_EQ(touches[2].action, TouchAction::Up);

    std::cout << "[PASS] TestMousePointerToTouch" << std::endl;
}

void TestMouseRightClickToButton() {
    std::cout << "[RUN] TestMouseRightClickToButton" << std::endl;

    MockInputEventSink sink;
    MouseTouchTranslator::Config cfg;
    cfg.rightClickButton = ButtonType::Back;
    MouseTouchTranslator translator(sink, cfg);

    PointerEvent rightDown;
    rightDown.type = PointerType::Mouse;
    rightDown.button = PointerButton::Right;
    rightDown.isDown = true;
    rightDown.x = 0.0f;
    rightDown.y = 0.0f;
    translator.OnPointerEvent(rightDown);

    PointerEvent rightUp;
    rightUp.type = PointerType::Mouse;
    rightUp.button = PointerButton::Right;
    rightUp.isDown = false;
    rightUp.x = 0.0f;
    rightUp.y = 0.0f;
    translator.OnPointerEvent(rightUp);

    ASSERT_EQ(sink.ButtonCount(), 2U);
    auto buttons = sink.ButtonEvents();
    ASSERT_EQ(buttons[0].button, ButtonType::Back);
    ASSERT_TRUE(buttons[0].isPressed);
    ASSERT_EQ(buttons[1].button, ButtonType::Back);
    ASSERT_TRUE(!buttons[1].isPressed);

    std::cout << "[PASS] TestMouseRightClickToButton" << std::endl;
}

void TestMouseWheelToKnob() {
    std::cout << "[RUN] TestMouseWheelToKnob" << std::endl;

    MockInputEventSink sink;
    MouseTouchTranslator translator(sink);

    // Vertical wheel down (deltaY < 0) -> clockwise rotation.
    WheelEvent wheelDown;
    wheelDown.deltaY = -120;
    translator.OnWheelEvent(wheelDown);
    ASSERT_EQ(sink.KnobCount(), 1U);
    auto knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[0].action, KnobAction::RotateClockwise);

    // Vertical wheel up (deltaY > 0) -> counter-clockwise rotation.
    WheelEvent wheelUp;
    wheelUp.deltaY = 120;
    translator.OnWheelEvent(wheelUp);
    ASSERT_EQ(sink.KnobCount(), 2U);
    knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[1].action, KnobAction::RotateCounterClockwise);

    // Horizontal wheel right (deltaX > 0) -> clockwise.
    WheelEvent wheelRight;
    wheelRight.deltaX = 120;
    translator.OnWheelEvent(wheelRight);
    ASSERT_EQ(sink.KnobCount(), 3U);
    knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[2].action, KnobAction::RotateClockwise);

    // Empty wheel event produces nothing.
    WheelEvent empty;
    translator.OnWheelEvent(empty);
    ASSERT_EQ(sink.KnobCount(), 3U);

    std::cout << "[PASS] TestMouseWheelToKnob" << std::endl;
}

// ---------------------------------------------------------------------------
// Step 3: KeyboardKnobTranslator — 4-way d-pad + WASD, confirm, back, media.
// ---------------------------------------------------------------------------
void TestKeyboardDirectionalKnob() {
    std::cout << "[RUN] TestKeyboardDirectionalKnob" << std::endl;

    MockInputEventSink sink;
    KeyboardKnobTranslator translator(sink);

    // Arrow keys -> directional knob press.
    translator.OnKeyEvent(KeyEvent{Key::Up, true});
    translator.OnKeyEvent(KeyEvent{Key::Down, true});
    translator.OnKeyEvent(KeyEvent{Key::Left, true});
    translator.OnKeyEvent(KeyEvent{Key::Right, true});
    ASSERT_EQ(sink.KnobCount(), 4U);
    auto knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[0].action, KnobAction::PressUp);
    ASSERT_EQ(knobs[1].action, KnobAction::PressDown);
    ASSERT_EQ(knobs[2].action, KnobAction::PressLeft);
    ASSERT_EQ(knobs[3].action, KnobAction::PressRight);

    // WASD alias -> same directional knob press.
    translator.OnKeyEvent(KeyEvent{Key::KeyW, true});
    translator.OnKeyEvent(KeyEvent{Key::KeyA, true});
    translator.OnKeyEvent(KeyEvent{Key::KeyS, true});
    translator.OnKeyEvent(KeyEvent{Key::KeyD, true});
    ASSERT_EQ(sink.KnobCount(), 8U);
    knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[4].action, KnobAction::PressUp);
    ASSERT_EQ(knobs[5].action, KnobAction::PressLeft);
    ASSERT_EQ(knobs[6].action, KnobAction::PressDown);
    ASSERT_EQ(knobs[7].action, KnobAction::PressRight);

    std::cout << "[PASS] TestKeyboardDirectionalKnob" << std::endl;
}

void TestKeyboardConfirmAndBack() {
    std::cout << "[RUN] TestKeyboardConfirmAndBack" << std::endl;

    MockInputEventSink sink;
    KeyboardKnobTranslator translator(sink);

    // Enter -> knob center push (select confirm).
    translator.OnKeyEvent(KeyEvent{Key::Enter, true});
    ASSERT_EQ(sink.KnobCount(), 1U);
    auto knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[0].action, KnobAction::PushDown);

    // Backspace / Escape -> Back button.
    translator.OnKeyEvent(KeyEvent{Key::Backspace, true});
    translator.OnKeyEvent(KeyEvent{Key::Escape, true});
    ASSERT_EQ(sink.ButtonCount(), 2U);
    auto buttons = sink.ButtonEvents();
    ASSERT_EQ(buttons[0].button, ButtonType::Back);
    ASSERT_EQ(buttons[1].button, ButtonType::Back);

    std::cout << "[PASS] TestKeyboardConfirmAndBack" << std::endl;
}

void TestKeyboardMediaAndButtons() {
    std::cout << "[RUN] TestKeyboardMediaAndButtons" << std::endl;

    MockInputEventSink sink;
    KeyboardKnobTranslator translator(sink);

    translator.OnKeyEvent(KeyEvent{Key::PlayPause, true});
    translator.OnKeyEvent(KeyEvent{Key::NextTrack, true});
    translator.OnKeyEvent(KeyEvent{Key::PrevTrack, true});
    translator.OnKeyEvent(KeyEvent{Key::Siri, true});
    translator.OnKeyEvent(KeyEvent{Key::CallAccept, true});
    translator.OnKeyEvent(KeyEvent{Key::CallReject, true});
    translator.OnKeyEvent(KeyEvent{Key::VolumeUp, true});
    translator.OnKeyEvent(KeyEvent{Key::VolumeDown, true});

    ASSERT_EQ(sink.ButtonCount(), 8U);
    auto buttons = sink.ButtonEvents();
    ASSERT_EQ(buttons[0].button, ButtonType::PlayPause);
    ASSERT_EQ(buttons[1].button, ButtonType::NextTrack);
    ASSERT_EQ(buttons[2].button, ButtonType::PrevTrack);
    ASSERT_EQ(buttons[3].button, ButtonType::Siri);
    ASSERT_EQ(buttons[4].button, ButtonType::CallAccept);
    ASSERT_EQ(buttons[5].button, ButtonType::CallReject);
    ASSERT_EQ(buttons[6].button, ButtonType::VolumeUp);
    ASSERT_EQ(buttons[7].button, ButtonType::VolumeDown);

    std::cout << "[PASS] TestKeyboardMediaAndButtons" << std::endl;
}

void TestKeyboardIgnoresKeyUpsAndUnmapped() {
    std::cout << "[RUN] TestKeyboardIgnoresKeyUpsAndUnmapped" << std::endl;

    MockInputEventSink sink;
    KeyboardKnobTranslator translator(sink);

    // Key-ups produce no action.
    translator.OnKeyEvent(KeyEvent{Key::Up, false});
    ASSERT_EQ(sink.KnobCount(), 0U);
    ASSERT_EQ(sink.ButtonCount(), 0U);

    // Unmapped keys (e.g. None) produce no action.
    translator.OnKeyEvent(KeyEvent{Key::None, true});
    ASSERT_EQ(sink.KnobCount(), 0U);
    ASSERT_EQ(sink.ButtonCount(), 0U);

    std::cout << "[PASS] TestKeyboardIgnoresKeyUpsAndUnmapped" << std::endl;
}

// ---------------------------------------------------------------------------
// Step 4: MultiTouchInjector — multi-finger touch passthrough.
// ---------------------------------------------------------------------------
void TestMultiTouchDownMoveUp() {
    std::cout << "[RUN] TestMultiTouchDownMoveUp" << std::endl;

    MockInputEventSink sink;
    MultiTouchInjector::Config cfg;
    cfg.windowWidth = 1920;
    cfg.windowHeight = 720;
    MultiTouchInjector injector(sink, cfg);

    // Frame 1: two fingers appear.
    std::vector<PointerEvent> frame1;
    PointerEvent f1;
    f1.type = PointerType::Touch;
    f1.pointerId = 1;
    f1.x = 480.0f;
    f1.y = 180.0f;
    frame1.push_back(f1);
    PointerEvent f2;
    f2.type = PointerType::Touch;
    f2.pointerId = 2;
    f2.x = 1440.0f;
    f2.y = 540.0f;
    frame1.push_back(f2);
    injector.OnFrame(frame1);

    ASSERT_EQ(injector.ActiveContactCount(), 2U);
    ASSERT_TRUE(injector.IsContactActive(1));
    ASSERT_TRUE(injector.IsContactActive(2));
    ASSERT_EQ(sink.TouchCount(), 2U);
    auto touches = sink.TouchEvents();
    ASSERT_EQ(touches[0].action, TouchAction::Down);
    ASSERT_EQ(touches[0].point.id, 1U);
    ASSERT_EQ(touches[1].action, TouchAction::Down);
    ASSERT_EQ(touches[1].point.id, 2U);

    // Frame 2: finger 1 moves, finger 2 stays.
    std::vector<PointerEvent> frame2;
    PointerEvent f1m;
    f1m.type = PointerType::Touch;
    f1m.pointerId = 1;
    f1m.x = 960.0f;
    f1m.y = 360.0f;
    frame2.push_back(f1m);
    frame2.push_back(f2);
    injector.OnFrame(frame2);

    ASSERT_EQ(sink.TouchCount(), 4U);
    touches = sink.TouchEvents();
    ASSERT_EQ(touches[2].action, TouchAction::Move);
    ASSERT_EQ(touches[2].point.id, 1U);
    ASSERT_TRUE(std::abs(touches[2].point.x_normalized - 0.5f) < 1e-4f);

    // Frame 3: finger 1 lifts (absent), finger 2 still present.
    std::vector<PointerEvent> frame3;
    frame3.push_back(f2);
    injector.OnFrame(frame3);

    ASSERT_EQ(injector.ActiveContactCount(), 1U);
    ASSERT_TRUE(!injector.IsContactActive(1));
    ASSERT_TRUE(injector.IsContactActive(2));
    touches = sink.TouchEvents();
    // A Touch Up for finger 1 must have been emitted (lifted this frame).
    bool foundUpForFinger1 = false;
    for (const auto& t : touches) {
        if (t.action == TouchAction::Up && t.point.id == 1U) {
            foundUpForFinger1 = true;
            break;
        }
    }
    ASSERT_TRUE(foundUpForFinger1);

    std::cout << "[PASS] TestMultiTouchDownMoveUp" << std::endl;
}

void TestMultiTouchCancelAll() {
    std::cout << "[RUN] TestMultiTouchCancelAll" << std::endl;

    MockInputEventSink sink;
    MultiTouchInjector injector(sink);

    std::vector<PointerEvent> frame;
    PointerEvent f1;
    f1.type = PointerType::Touch;
    f1.pointerId = 7;
    f1.x = 100.0f;
    f1.y = 100.0f;
    frame.push_back(f1);
    injector.OnFrame(frame);
    ASSERT_EQ(injector.ActiveContactCount(), 1U);

    injector.CancelAll();
    ASSERT_EQ(injector.ActiveContactCount(), 0U);
    ASSERT_TRUE(!injector.IsContactActive(7));
    auto touches = sink.TouchEvents();
    ASSERT_EQ(touches.back().action, TouchAction::Cancel);
    ASSERT_EQ(touches.back().point.id, 7U);

    std::cout << "[PASS] TestMultiTouchCancelAll" << std::endl;
}

// ---------------------------------------------------------------------------
// Step 5: MockInputEventSink — recording + reset.
// ---------------------------------------------------------------------------
void TestMockSinkRecordingAndReset() {
    std::cout << "[RUN] TestMockSinkRecordingAndReset" << std::endl;

    MockInputEventSink sink;
    ASSERT_EQ(sink.TouchCount(), 0U);
    ASSERT_EQ(sink.KnobCount(), 0U);
    ASSERT_EQ(sink.ButtonCount(), 0U);

    TouchPoint point;
    point.id = 3;
    point.x_normalized = 0.25f;
    point.y_normalized = 0.75f;
    sink.SendTouchEvent(TouchAction::Down, point);
    sink.SendKnobEvent(KnobAction::RotateClockwise, 2);
    sink.SendButtonEvent(ButtonType::Home, true);

    ASSERT_EQ(sink.TouchCount(), 1U);
    ASSERT_EQ(sink.KnobCount(), 1U);
    ASSERT_EQ(sink.ButtonCount(), 1U);

    auto knobs = sink.KnobEvents();
    ASSERT_EQ(knobs[0].action, KnobAction::RotateClockwise);
    ASSERT_EQ(knobs[0].stepCount, 2U);

    sink.Reset();
    ASSERT_EQ(sink.TouchCount(), 0U);
    ASSERT_EQ(sink.KnobCount(), 0U);
    ASSERT_EQ(sink.ButtonCount(), 0U);

    std::cout << "[PASS] TestMockSinkRecordingAndReset" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  CarPlayStation Input Tests (Step 2-5) " << std::endl;
    std::cout << "========================================" << std::endl;

    // Step 2: mouse -> touch / button / knob.
    TestMousePointerToTouch();
    TestMouseRightClickToButton();
    TestMouseWheelToKnob();

    // Step 3: keyboard -> 4-way d-pad / confirm / back / media.
    TestKeyboardDirectionalKnob();
    TestKeyboardConfirmAndBack();
    TestKeyboardMediaAndButtons();
    TestKeyboardIgnoresKeyUpsAndUnmapped();

    // Step 4: multi-touch passthrough.
    TestMultiTouchDownMoveUp();
    TestMultiTouchCancelAll();

    // Step 5: mock sink.
    TestMockSinkRecordingAndReset();

    std::cout << "========================================" << std::endl;
    std::cout << "  ALL INPUT TESTS PASSED!               " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
