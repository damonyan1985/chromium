// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/components/smbfs/smbfs_host.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/logging.h"
#include "base/macros.h"
#include "chromeos/disks/disk_mount_manager.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace smbfs {
namespace {

class SmbFsDelegateImpl : public mojom::SmbFsDelegate {
 public:
  SmbFsDelegateImpl(
      mojo::PendingReceiver<mojom::SmbFsDelegate> pending_receiver,
      base::OnceClosure disconnect_callback)
      : receiver_(this, std::move(pending_receiver)) {
    receiver_.set_disconnect_handler(std::move(disconnect_callback));
  }

  ~SmbFsDelegateImpl() override = default;

 private:
  mojo::Receiver<mojom::SmbFsDelegate> receiver_;

  DISALLOW_COPY_AND_ASSIGN(SmbFsDelegateImpl);
};

}  // namespace

SmbFsHost::Delegate::~Delegate() = default;

SmbFsHost::SmbFsHost(
    const base::FilePath& mount_path,
    Delegate* delegate,
    chromeos::disks::DiskMountManager* disk_mount_manager,
    mojo::Remote<mojom::SmbFs> smbfs_remote,
    mojo::PendingReceiver<mojom::SmbFsDelegate> delegate_receiver)
    : mount_path_(mount_path),
      delegate_(delegate),
      disk_mount_manager_(disk_mount_manager),
      smbfs_(std::move(smbfs_remote)),
      delegate_impl_(std::make_unique<SmbFsDelegateImpl>(
          std::move(delegate_receiver),
          base::BindOnce(&SmbFsHost::OnDisconnect, base::Unretained(this)))) {
  DCHECK(!mount_path.empty());
  DCHECK(delegate);

  smbfs_.set_disconnect_handler(
      base::BindOnce(&SmbFsHost::OnDisconnect, base::Unretained(this)));
}

SmbFsHost::~SmbFsHost() {
  disk_mount_manager_->UnmountPath(mount_path_.value(), base::DoNothing());
}

void SmbFsHost::OnDisconnect() {
  // Ensure only one disconnection event occurs.
  smbfs_.reset();
  delegate_impl_.reset();

  // This may delete us.
  delegate_->OnDisconnected();
}

}  // namespace smbfs
