// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.share.qrcode.scan_tab;

import android.content.Context;
import android.hardware.Camera.PreviewCallback;
import android.view.View;
import android.view.ViewGroup.LayoutParams;
import android.widget.FrameLayout;

/**
 * Manages the Android View representing the QrCode scan panel.
 */
class QrCodeScanView {
    private final Context mContext;
    private final FrameLayout mView;
    private final PreviewCallback mCameraCallback;

    private boolean mHasCameraPermission;
    private boolean mIsOnForeground;
    private CameraPreview mCameraPreview;

    /**
     * The QrCodeScanView constructor.
     * @param context The context to use for user permissions.
     * @param cameraCallback The callback to processing camera preview.
     */
    public QrCodeScanView(Context context, PreviewCallback cameraCallback) {
        mContext = context;
        mCameraCallback = cameraCallback;

        mView = new FrameLayout(context);
        mView.setLayoutParams(
                new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
    }

    public View getView() {
        return mView;
    }

    /**
     * Sets camera if possible.
     * @param hasCameraPermission Indicates whether camera permissions wertr granted.
     */
    public void cameraPermissionsChanged(Boolean hasCameraPermission) {
        if (mHasCameraPermission != hasCameraPermission) {
            mHasCameraPermission = hasCameraPermission;
            setCameraPreview();
        }
    }

    /**
     * Applies changes necessary to camera preview.
     * @param isOnForeground Indicates whether this component UI is current on foreground.
     */
    public void onForegroundChanged(Boolean isOnForeground) {
        mIsOnForeground = isOnForeground;
        updateCameraPreviewState();
    }

    /** Creates and sets the camera preview. */
    private void setCameraPreview() {
        mView.removeAllViews();
        if (mCameraPreview != null) {
            mCameraPreview.stopCamera();
            mCameraPreview = null;
        }

        if (mHasCameraPermission) {
            mCameraPreview = new CameraPreview(mContext, mCameraCallback);
            mView.addView(mCameraPreview);

            updateCameraPreviewState();
        }
    }

    /** Starts or stops camera if necessary. */
    private void updateCameraPreviewState() {
        if (mCameraPreview == null) return;

        if (mIsOnForeground) {
            mCameraPreview.startCamera();
        } else {
            mCameraPreview.stopCamera();
        }
    }
}
