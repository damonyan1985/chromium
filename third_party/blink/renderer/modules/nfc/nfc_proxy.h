// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_NFC_NFC_PROXY_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_NFC_NFC_PROXY_H_

#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/nfc.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/page/page_visibility_observer.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"

namespace blink {

class Document;
class NDEFScanOptions;
class NDEFReader;
class NDEFWriter;

// This is a proxy class used by NDEFWriter(s) and NDEFReader(s) to connect
// to implementation of device::mojom::blink::NFC interface.
class MODULES_EXPORT NFCProxy final : public GarbageCollected<NFCProxy>,
                                      public PageVisibilityObserver,
                                      public Supplement<Document>,
                                      public device::mojom::blink::NFCClient {
  USING_GARBAGE_COLLECTED_MIXIN(NFCProxy);
  USING_PRE_FINALIZER(NFCProxy, Dispose);

 public:
  static const char kSupplementName[];
  static NFCProxy* From(Document&);

  explicit NFCProxy(Document&);
  ~NFCProxy() override;

  void Dispose();

  void Trace(blink::Visitor*) override;

  // There is no matching RemoveWriter() method because writers are
  // automatically removed from the weak hash set when they are garbage
  // collected.
  void AddWriter(NDEFWriter*);

  void StartReading(NDEFReader*,
                    const NDEFScanOptions*,
                    device::mojom::blink::NFC::WatchCallback);
  void StopReading(NDEFReader*);
  bool IsReading(const NDEFReader*);
  void Push(device::mojom::blink::NDEFMessagePtr,
            device::mojom::blink::NDEFPushOptionsPtr,
            device::mojom::blink::NFC::PushCallback);
  void CancelPush(const String&, device::mojom::blink::NFC::CancelPushCallback);

 private:
  // Implementation of device::mojom::blink::NFCClient.
  void OnWatch(const Vector<uint32_t>&,
               const String&,
               device::mojom::blink::NDEFMessagePtr) override;
  void OnError(device::mojom::blink::NDEFErrorType) override;

  void OnReaderRegistered(NDEFReader*,
                          uint32_t watch_id,
                          device::mojom::blink::NFC::WatchCallback,
                          device::mojom::blink::NDEFErrorPtr);

  // Implementation of PageVisibilityObserver.
  void PageVisibilityChanged() override;

  void EnsureMojoConnection();

  // This could only happen when the embedder does not implement NFC interface.
  void OnMojoConnectionError();

  // Identifies watch requests tied to a given Mojo connection of NFC interface,
  // i.e. |nfc_|. Incremented each time a watch request is made.
  uint32_t next_watch_id_ = 1;

  // The <NDEFReader, WatchId> map keeps all readers that have started reading.
  // The watch id comes from |next_watch_id_|.
  using ReaderMap = HeapHashMap<WeakMember<NDEFReader>, uint32_t>;
  ReaderMap readers_;

  using WriterSet = HeapHashSet<WeakMember<NDEFWriter>>;
  WriterSet writers_;

  mojo::Remote<device::mojom::blink::NFC> nfc_remote_;
  mojo::Receiver<device::mojom::blink::NFCClient> client_receiver_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_NFC_NFC_PROXY_H_
