// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PERFORMANCE_MANAGER_PERFORMANCE_MANAGER_REGISTRY_IMPL_H_
#define COMPONENTS_PERFORMANCE_MANAGER_PERFORMANCE_MANAGER_REGISTRY_IMPL_H_

#include "base/containers/flat_set.h"
#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "components/performance_manager/embedder/performance_manager_registry.h"
#include "components/performance_manager/performance_manager_tab_helper.h"
#include "components/performance_manager/render_process_user_data.h"

namespace content {
class RenderProcessHost;
class WebContents;
}  // namespace content

namespace performance_manager {

class PerformanceManagerMainThreadObserver;

class PerformanceManagerRegistryImpl
    : public PerformanceManagerRegistry,
      public PerformanceManagerTabHelper::DestructionObserver,
      public RenderProcessUserData::DestructionObserver {
 public:
  PerformanceManagerRegistryImpl();
  ~PerformanceManagerRegistryImpl() override;

  PerformanceManagerRegistryImpl(const PerformanceManagerRegistryImpl&) =
      delete;
  void operator=(const PerformanceManagerRegistryImpl&) = delete;

  // Returns the only instance of PerformanceManagerRegistryImpl living in this
  // process, or nullptr if there is none.
  static PerformanceManagerRegistryImpl* GetInstance();

  // Adds / removes an observer that is notified when a PageNode is created on
  // the main thread.
  void AddObserver(PerformanceManagerMainThreadObserver* observer);
  void RemoveObserver(PerformanceManagerMainThreadObserver* observer);

  // PerformanceManagerRegistry:
  void CreatePageNodeForWebContents(
      content::WebContents* web_contents) override;
  void CreateProcessNodeForRenderProcessHost(
      content::RenderProcessHost* render_process_host) override;
  void TearDown() override;

  // PerformanceManagerTabHelper::DestructionObserver:
  void OnPerformanceManagerTabHelperDestroying(
      content::WebContents* web_contents) override;

  // RenderProcessUserData::DestructionObserver:
  void OnRenderProcessUserDataDestroying(
      content::RenderProcessHost* render_process_host) override;

 private:
  SEQUENCE_CHECKER(sequence_checker_);

  // Tracks WebContents and RenderProcessHost for which we have created user
  // data. Used to destroy all user data when the registry is destroyed.
  base::flat_set<content::WebContents*> web_contents_;
  base::flat_set<content::RenderProcessHost*> render_process_hosts_;

  base::ObserverList<PerformanceManagerMainThreadObserver> observers_;
};

}  // namespace performance_manager

#endif  // COMPONENTS_PERFORMANCE_MANAGER_PERFORMANCE_MANAGER_REGISTRY_IMPL_H_
