// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PERFORMANCE_MANAGER_PUBLIC_DECORATORS_PAGE_LIVE_STATE_DECORATOR_H_
#define COMPONENTS_PERFORMANCE_MANAGER_PUBLIC_DECORATORS_PAGE_LIVE_STATE_DECORATOR_H_

namespace content {
class WebContents;
}  // namespace content

namespace performance_manager {

class PageNode;

// Used to record some live state information about the PageNode.
// All the functions that take a WebContents* as a parameter should only be
// called from the UI thread, the event will be forwarded to the corresponding
// PageNode on the Performance Manager's sequence.
class PageLiveStateDecorator {
 public:
  class Data;

  // This object should only be used via its static methods.
  PageLiveStateDecorator() = delete;
  ~PageLiveStateDecorator() = delete;
  PageLiveStateDecorator(const PageLiveStateDecorator& other) = delete;
  PageLiveStateDecorator& operator=(const PageLiveStateDecorator&) = delete;

  // Must be called when the connected to USB device state changes.
  static void OnIsConnectedToUSBDeviceChanged(content::WebContents* contents,
                                              bool is_connected_to_usb_device);

  // Must be called when the connected to Bluetooth device state changes.
  static void OnIsConnectedToBluetoothDeviceChanged(
      content::WebContents* contents,
      bool is_connected_to_bluetooth_device);

  // Functions that should be called by a MediaStreamCaptureIndicator::Observer.
  static void OnIsCapturingVideoChanged(content::WebContents* contents,
                                        bool is_capturing_video);
  static void OnIsCapturingAudioChanged(content::WebContents* contents,
                                        bool is_capturing_audio);
  static void OnIsBeingMirroredChanged(content::WebContents* contents,
                                       bool is_being_mirrored);
  static void OnIsCapturingDesktopChanged(content::WebContents* contents,
                                          bool is_capturing_desktop);

  // Set the auto discardable property. This indicates whether or not the page
  // can be discarded during an intervention.
  static void SetIsAutoDiscardable(content::WebContents* contents,
                                   bool is_auto_discardable);
};

class PageLiveStateDecorator::Data {
 public:
  Data();
  virtual ~Data();
  Data(const Data& other) = delete;
  Data& operator=(const Data&) = delete;

  virtual bool IsConnectedToUSBDevice() const = 0;
  virtual bool IsConnectedToBluetoothDevice() const = 0;
  virtual bool IsCapturingVideo() const = 0;
  virtual bool IsCapturingAudio() const = 0;
  virtual bool IsBeingMirrored() const = 0;
  virtual bool IsCapturingDesktop() const = 0;
  virtual bool IsAutoDiscardable() const = 0;

  static Data* GetOrCreateForTesting(PageNode* page_node);
};

}  // namespace performance_manager

#endif  // COMPONENTS_PERFORMANCE_MANAGER_PUBLIC_DECORATORS_PAGE_LIVE_STATE_DECORATOR_H_
