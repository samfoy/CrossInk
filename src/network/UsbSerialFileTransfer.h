#pragma once

namespace UsbSerialFileTransfer {

enum class ProcessResult {
  None,
  ScreenshotRequested,
  ButtonRequested,
};

// When process() returns ButtonRequested, this holds the requested logical
// button index (matches MappedInputManager::Button's enum order). Valid only
// on the ButtonRequested return; the caller maps it to a tap injection.
extern int lastRequestedButton;

ProcessResult process(bool fileTransferAllowed);

}  // namespace UsbSerialFileTransfer
