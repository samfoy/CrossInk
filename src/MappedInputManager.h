#pragma once

#include <HalGPIO.h>

#include <array>

// Synthetic input injection. Compiled for the simulator (smoke tests drive the
// UI headlessly) AND when the xteink-rig dev flag -DCROSSINK_RIG_INPUT is set,
// which lets the tethered dev rig drive real hardware over serial (CMD:BTN:*).
// When neither is defined this is a no-op and device builds are byte-identical.
#if defined(SIMULATOR) || defined(CROSSINK_RIG_INPUT)
#define CROSSINK_INPUT_INJECTION 1
#endif

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  static constexpr size_t BUTTON_COUNT = static_cast<size_t>(Button::PageForward) + 1;

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  // Enable/disable reader-specific front button mapping.
  // Call with true in reader activity onEnter(), false in onExit().
  void setReaderMode(bool enabled) { readerMode = enabled; }
  void setPowerAsConfirmInReaderMode(bool enabled) { powerAsConfirmInReaderMode = enabled; }

  void update() const { gpio.update(); }
  void suppressNextBackRelease() { suppressBackRelease = true; }
  void suppressNextConfirmRelease() { suppressConfirmRelease = true; }
  void suppressNextPowerRelease() { suppressPowerRelease = true; }
  void suppressNextPowerConfirmRelease() { suppressPowerConfirmRelease = true; }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;
  // Returns the raw front button index that was released this frame (or -1 if none).
  int getReleasedFrontButton() const;
  bool isFrontButtonPressed(uint8_t buttonIndex) const;

#ifdef CROSSINK_INPUT_INJECTION
  void simulatorInjectPress(Button button);
  void simulatorInjectRelease(Button button);
  void simulatorClearInputFrame();

  // Hardware-rig tap driver (used by the serial CMD:BTN:* path). Unlike the
  // simulator smoke test — which drives press/release/clear explicitly frame by
  // frame — the rig has no per-frame script, so a queued tap is auto-sequenced
  // by advanceInjectedInput(): frame N clears the previous frame + presses,
  // frame N+1 releases, frame N+2 clears the release. Returns false if the queue
  // is full. Safe to call from the serial handler (main loop context).
  bool queueInjectedTap(Button button);
  // Call once at the top of each main-loop iteration, BEFORE reading inputs, so
  // an injected tap surfaces exactly like a real press/release edge.
  void advanceInjectedInput();
#endif

 private:
  HalGPIO& gpio;
  bool readerMode = false;
  bool powerAsConfirmInReaderMode = false;
  mutable bool suppressBackRelease = false;
  mutable bool suppressConfirmRelease = false;
  mutable bool suppressPowerRelease = false;
  mutable bool suppressPowerConfirmRelease = false;
#ifdef CROSSINK_INPUT_INJECTION
  std::array<bool, BUTTON_COUNT> simulatorPressed{};
  std::array<bool, BUTTON_COUNT> simulatorReleased{};
  std::array<bool, BUTTON_COUNT> simulatorHeld{};
  std::array<unsigned long, BUTTON_COUNT> simulatorPressStart{};

  // Auto-sequenced tap queue for the hardware rig (see queueInjectedTap()).
  static constexpr size_t INJECT_QUEUE_CAPACITY = 8;
  std::array<Button, INJECT_QUEUE_CAPACITY> injectQueue{};
  size_t injectQueueHead = 0;
  size_t injectQueueCount = 0;
  // 0 = idle, 1 = press frame pending release, 2 = release frame pending clear.
  uint8_t injectPhase = 0;
  Button injectActive = Button::Confirm;
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool shouldUsePowerAsConfirmFallback() const;
  bool shouldMirrorPowerAsConfirmHold() const;
};
