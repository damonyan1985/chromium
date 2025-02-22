// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/performance_manager/performance_manager_registry_impl.h"

#include "base/stl_util.h"
#include "components/performance_manager/performance_manager_tab_helper.h"
#include "components/performance_manager/public/performance_manager.h"
#include "components/performance_manager/public/performance_manager_main_thread_observer.h"
#include "content/public/browser/render_process_host.h"

namespace performance_manager {

namespace {
PerformanceManagerRegistryImpl* g_instance = nullptr;
}  // namespace

PerformanceManagerRegistryImpl::PerformanceManagerRegistryImpl() {
  DCHECK(!g_instance);
  g_instance = this;

  // The registry should be created after the PerformanceManager.
  DCHECK(PerformanceManager::IsAvailable());
}

PerformanceManagerRegistryImpl::~PerformanceManagerRegistryImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // TearDown() should have been invoked to reset |g_instance| and clear
  // |web_contents_| and |render_process_user_data_| prior to destroying the
  // registry.
  DCHECK(!g_instance);
  DCHECK(web_contents_.empty());
  DCHECK(render_process_hosts_.empty());
}

// static
PerformanceManagerRegistryImpl* PerformanceManagerRegistryImpl::GetInstance() {
  return g_instance;
}

void PerformanceManagerRegistryImpl::AddObserver(
    PerformanceManagerMainThreadObserver* observer) {
  observers_.AddObserver(observer);
}

void PerformanceManagerRegistryImpl::RemoveObserver(
    PerformanceManagerMainThreadObserver* observer) {
  observers_.RemoveObserver(observer);
}

void PerformanceManagerRegistryImpl::CreatePageNodeForWebContents(
    content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto result = web_contents_.insert(web_contents);
  if (result.second) {
    // Create a PerformanceManagerTabHelper if |web_contents| doesn't already
    // have one. Support for multiple calls to CreatePageNodeForWebContents()
    // with the same WebContents is required for Devtools -- see comment in
    // header file.
    PerformanceManagerTabHelper::CreateForWebContents(web_contents);
    PerformanceManagerTabHelper* tab_helper =
        PerformanceManagerTabHelper::FromWebContents(web_contents);
    DCHECK(tab_helper);
    tab_helper->SetDestructionObserver(this);

    for (auto& observer : observers_)
      observer.OnPageNodeCreatedForWebContents(web_contents);
  }
}

void PerformanceManagerRegistryImpl::CreateProcessNodeForRenderProcessHost(
    content::RenderProcessHost* render_process_host) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto result = render_process_hosts_.insert(render_process_host);
  if (result.second) {
    // Create a RenderProcessUserData if |render_process_host| doesn't already
    // have one.
    RenderProcessUserData* user_data =
        RenderProcessUserData::CreateForRenderProcessHost(render_process_host);
    user_data->SetDestructionObserver(this);
  }
}

void PerformanceManagerRegistryImpl::TearDown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK_EQ(g_instance, this);
  g_instance = nullptr;

  // The registry should be torn down before the PerformanceManager.
  DCHECK(PerformanceManager::IsAvailable());

  for (auto* web_contents : web_contents_) {
    PerformanceManagerTabHelper* tab_helper =
        PerformanceManagerTabHelper::FromWebContents(web_contents);
    DCHECK(tab_helper);
    // Clear the destruction observer to avoid a nested notification.
    tab_helper->SetDestructionObserver(nullptr);
    // Destroy the tab helper.
    tab_helper->TearDown();
    web_contents->RemoveUserData(PerformanceManagerTabHelper::UserDataKey());
  }
  web_contents_.clear();

  for (auto* render_process_host : render_process_hosts_) {
    RenderProcessUserData* user_data =
        RenderProcessUserData::GetForRenderProcessHost(render_process_host);
    DCHECK(user_data);
    // Clear the destruction observer to avoid a nested notification.
    user_data->SetDestructionObserver(nullptr);
    // Destroy the user data.
    render_process_host->RemoveUserData(RenderProcessUserData::UserDataKey());
  }
  render_process_hosts_.clear();
}

void PerformanceManagerRegistryImpl::OnPerformanceManagerTabHelperDestroying(
    content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const size_t num_removed = web_contents_.erase(web_contents);
  DCHECK_EQ(1U, num_removed);
}

void PerformanceManagerRegistryImpl::OnRenderProcessUserDataDestroying(
    content::RenderProcessHost* render_process_host) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const size_t num_removed = render_process_hosts_.erase(render_process_host);
  DCHECK_EQ(1U, num_removed);
}

}  // namespace performance_manager
