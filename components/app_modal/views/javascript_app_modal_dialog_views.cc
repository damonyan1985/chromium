// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/app_modal/views/javascript_app_modal_dialog_views.h"

#include "base/strings/utf_string_conversions.h"
#include "components/app_modal/javascript_app_modal_dialog.h"
#include "components/strings/grit/components_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/controls/message_box_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/widget/widget.h"

namespace app_modal {

////////////////////////////////////////////////////////////////////////////////
// JavaScriptAppModalDialogViews, public:

JavaScriptAppModalDialogViews::JavaScriptAppModalDialogViews(
    JavaScriptAppModalDialog* parent)
    : parent_(parent) {
  int options = views::MessageBoxView::DETECT_DIRECTIONALITY;
  if (parent->javascript_dialog_type() ==
      content::JAVASCRIPT_DIALOG_TYPE_PROMPT)
    options |= views::MessageBoxView::HAS_PROMPT_FIELD;

  views::MessageBoxView::InitParams params(parent->message_text());
  params.options = options;
  params.default_prompt = parent->default_prompt_text();
  message_box_view_ = new views::MessageBoxView(params);
  DCHECK(message_box_view_);

  message_box_view_->AddAccelerator(
      ui::Accelerator(ui::VKEY_C, ui::EF_CONTROL_DOWN));
  if (parent->display_suppress_checkbox()) {
    message_box_view_->SetCheckBoxLabel(
        l10n_util::GetStringUTF16(IDS_JAVASCRIPT_MESSAGEBOX_SUPPRESS_OPTION));
  }

  DialogDelegate::set_buttons(
      parent_->javascript_dialog_type() == content::JAVASCRIPT_DIALOG_TYPE_ALERT
          ? ui::DIALOG_BUTTON_OK
          : (ui::DIALOG_BUTTON_OK | ui::DIALOG_BUTTON_CANCEL));

  if (parent_->is_before_unload_dialog()) {
    DialogDelegate::set_button_label(
        ui::DIALOG_BUTTON_OK,
        l10n_util::GetStringUTF16(
            parent_->is_reload()
                ? IDS_BEFORERELOAD_MESSAGEBOX_OK_BUTTON_LABEL
                : IDS_BEFOREUNLOAD_MESSAGEBOX_OK_BUTTON_LABEL));
  }
}

JavaScriptAppModalDialogViews::~JavaScriptAppModalDialogViews() {
}

////////////////////////////////////////////////////////////////////////////////
// JavaScriptAppModalDialogViews, NativeAppModalDialog implementation:

void JavaScriptAppModalDialogViews::ShowAppModalDialog() {
  GetWidget()->Show();
}

void JavaScriptAppModalDialogViews::ActivateAppModalDialog() {
  GetWidget()->Show();
  GetWidget()->Activate();
}

void JavaScriptAppModalDialogViews::CloseAppModalDialog() {
  GetWidget()->Close();
}

void JavaScriptAppModalDialogViews::AcceptAppModalDialog() {
  AcceptDialog();
}

void JavaScriptAppModalDialogViews::CancelAppModalDialog() {
  CancelDialog();
}

bool JavaScriptAppModalDialogViews::IsShowing() const {
  return GetWidget()->IsVisible();
}

//////////////////////////////////////////////////////////////////////////////
// JavaScriptAppModalDialogViews, views::DialogDelegate implementation:

base::string16 JavaScriptAppModalDialogViews::GetWindowTitle() const {
  return parent_->title();
}

void JavaScriptAppModalDialogViews::DeleteDelegate() {
  delete this;
}

bool JavaScriptAppModalDialogViews::Cancel() {
  parent_->OnCancel(message_box_view_->IsCheckBoxSelected());
  return true;
}

bool JavaScriptAppModalDialogViews::Accept() {
  parent_->OnAccept(message_box_view_->GetInputText(),
                    message_box_view_->IsCheckBoxSelected());
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// JavaScriptAppModalDialogViews, views::WidgetDelegate implementation:

ui::ModalType JavaScriptAppModalDialogViews::GetModalType() const {
  return ui::MODAL_TYPE_SYSTEM;
}

views::View* JavaScriptAppModalDialogViews::GetContentsView() {
  return message_box_view_;
}

views::View* JavaScriptAppModalDialogViews::GetInitiallyFocusedView() {
  if (message_box_view_->text_box())
    return message_box_view_->text_box();
  return views::DialogDelegate::GetInitiallyFocusedView();
}

bool JavaScriptAppModalDialogViews::ShouldShowCloseButton() const {
  return false;
}

void JavaScriptAppModalDialogViews::WindowClosing() {
  parent_->OnClose();
}

views::Widget* JavaScriptAppModalDialogViews::GetWidget() {
  return message_box_view_->GetWidget();
}

const views::Widget* JavaScriptAppModalDialogViews::GetWidget() const {
  return message_box_view_->GetWidget();
}

}  // namespace app_modal
