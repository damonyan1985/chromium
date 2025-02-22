// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.image_editor;

import android.support.v7.app.AppCompatActivity;

/**
 * Interface to interact with the image editor dialog.
 */
public interface ImageEditorDialogCoordinator {
    public void launchEditor(AppCompatActivity activity);
}
