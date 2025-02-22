// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROME_BROWSER_INTERFACE_BINDERS_H_
#define CHROME_BROWSER_CHROME_BROWSER_INTERFACE_BINDERS_H_

#include "services/service_manager/public/cpp/binder_map.h"

namespace content {

class RenderFrameHost;
}  // namespace content

namespace chrome {
namespace internal {

// The mechanism implemented by the PopulateChrome*FrameBinders() functions
// below will replace interface registries and binders used for handling
// InterfaceProvider's GetInterface() calls (see crbug.com/718652).

// PopulateChromeFrameBinders() registers BrowserInterfaceBroker's
// GetInterface() handler callbacks for chrome-specific document-scoped
// interfaces.
void PopulateChromeFrameBinders(
    service_manager::BinderMapWithContext<content::RenderFrameHost*>* map);

// PopulateChromeWebUIFrameBinders() registers BrowserInterfaceBroker's
// GetInterface() handler callbacks for chrome-specific document-scoped
// interfaces used from WebUI pages (e.g. chrome://bluetooth-internals).
void PopulateChromeWebUIFrameBinders(
    service_manager::BinderMapWithContext<content::RenderFrameHost*>* map);

}  // namespace internal
}  // namespace chrome

#endif  // CHROME_BROWSER_CHROME_BROWSER_INTERFACE_BINDERS_H_
