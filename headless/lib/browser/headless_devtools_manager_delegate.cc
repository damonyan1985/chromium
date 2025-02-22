// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/headless_devtools_manager_delegate.h"

#include "build/build_config.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "headless/grit/headless_lib_resources.h"
#include "headless/lib/browser/headless_browser_context_impl.h"
#include "headless/lib/browser/headless_browser_impl.h"
#include "headless/lib/browser/headless_web_contents_impl.h"
#include "headless/lib/browser/protocol/headless_devtools_session.h"
#include "ui/base/resource/resource_bundle.h"

namespace headless {

HeadlessDevToolsManagerDelegate::HeadlessDevToolsManagerDelegate(
    base::WeakPtr<HeadlessBrowserImpl> browser)
    : browser_(std::move(browser)) {}

HeadlessDevToolsManagerDelegate::~HeadlessDevToolsManagerDelegate() = default;

void HeadlessDevToolsManagerDelegate::HandleCommand(
    content::DevToolsAgentHost* agent_host,
    content::DevToolsAgentHostClient* client,
    const std::string& method,
    base::span<const uint8_t> message,
    NotHandledCallback callback) {
  DCHECK(sessions_.find(client) != sessions_.end());
  sessions_[client]->HandleCommand(method, message, std::move(callback));
}

scoped_refptr<content::DevToolsAgentHost>
HeadlessDevToolsManagerDelegate::CreateNewTarget(const GURL& url) {
  if (!browser_)
    return nullptr;

  HeadlessBrowserContext* context = browser_->GetDefaultBrowserContext();
  HeadlessWebContentsImpl* web_contents_impl = HeadlessWebContentsImpl::From(
      context->CreateWebContentsBuilder()
          .SetInitialURL(url)
          .SetWindowSize(browser_->options()->window_size)
          .Build());
  return content::DevToolsAgentHost::GetOrCreateFor(
      web_contents_impl->web_contents());
}

std::string HeadlessDevToolsManagerDelegate::GetDiscoveryPageHTML() {
  return ui::ResourceBundle::GetSharedInstance()
      .GetRawDataResource(IDR_HEADLESS_LIB_DEVTOOLS_DISCOVERY_PAGE)
      .as_string();
}

bool HeadlessDevToolsManagerDelegate::HasBundledFrontendResources() {
  return true;
}

void HeadlessDevToolsManagerDelegate::ClientAttached(
    content::DevToolsAgentHost* agent_host,
    content::DevToolsAgentHostClient* client) {
  DCHECK(sessions_.find(client) == sessions_.end());
  sessions_[client] = std::make_unique<protocol::HeadlessDevToolsSession>(
      browser_, agent_host, client);
}

void HeadlessDevToolsManagerDelegate::ClientDetached(
    content::DevToolsAgentHost* agent_host,
    content::DevToolsAgentHostClient* client) {
  sessions_.erase(client);
}

std::vector<content::BrowserContext*>
HeadlessDevToolsManagerDelegate::GetBrowserContexts() {
  std::vector<content::BrowserContext*> contexts;
  for (auto* context : browser_->GetAllBrowserContexts()) {
    if (context != browser_->GetDefaultBrowserContext())
      contexts.push_back(HeadlessBrowserContextImpl::From(context));
  }
  return contexts;
}
content::BrowserContext*
HeadlessDevToolsManagerDelegate::GetDefaultBrowserContext() {
  return HeadlessBrowserContextImpl::From(browser_->GetDefaultBrowserContext());
}

content::BrowserContext*
HeadlessDevToolsManagerDelegate::CreateBrowserContext() {
  auto builder = browser_->CreateBrowserContextBuilder();
  builder.SetIncognitoMode(true);
  HeadlessBrowserContext* browser_context = builder.Build();
  return HeadlessBrowserContextImpl::From(browser_context);
}

void HeadlessDevToolsManagerDelegate::DisposeBrowserContext(
    content::BrowserContext* browser_context,
    DisposeCallback callback) {
  HeadlessBrowserContextImpl* context =
      HeadlessBrowserContextImpl::From(browser_context);
  std::vector<HeadlessWebContents*> web_contents = context->GetAllWebContents();
  while (!web_contents.empty()) {
    for (auto* wc : web_contents)
      wc->Close();
    // Since HeadlessWebContents::Close spawns a nested run loop to await
    // closing, new web_contents could be opened. We need to re-query pages and
    // close them too.
    web_contents = context->GetAllWebContents();
  }
  context->Close();
  std::move(callback).Run(true, "");
}

}  // namespace headless
