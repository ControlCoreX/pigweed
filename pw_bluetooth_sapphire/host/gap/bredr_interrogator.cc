// Copyright 2023 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_interrogator.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"

namespace bt::gap {

BrEdrInterrogator::BrEdrInterrogator(Peer::WeakPtr peer,
                                     hci_spec::ConnectionHandle handle,
                                     hci::CommandChannel::WeakPtr cmd_channel)
    : peer_(std::move(peer)),
      peer_id_(peer_->identifier()),
      handle_(handle),
      cmd_runner_(std::move(cmd_channel)),
      weak_self_(this) {
  PW_CHECK(peer_.is_alive());
}

void BrEdrInterrogator::Start(ResultCallback callback) {
  callback_ = std::move(callback);

  if (!peer_.is_alive() || !peer_->bredr()) {
    Complete(ToResult(HostError::kFailed));
    return;
  }

  if (!peer_->name()) {
    QueueRemoteNameRequest();
  }

  if (!peer_->version()) {
    QueueReadRemoteVersionInformation();
  }

  if (!peer_->features().HasPage(0)) {
    QueueReadRemoteFeatures();
  } else if (peer_->features().HasBit(
                 /*page=*/0, hci_spec::LMPFeature::kExtendedFeatures)) {
    QueueReadRemoteExtendedFeatures(/*page=*/1);
  }

  if (!cmd_runner_.HasQueuedCommands()) {
    Complete(fit::ok());
    return;
  }

  cmd_runner_.RunCommands([this](hci::Result<> result) { Complete(result); });
}

void BrEdrInterrogator::Cancel() {
  if (!cmd_runner_.IsReady()) {
    cmd_runner_.Cancel();
  }
}

void BrEdrInterrogator::Complete(hci::Result<> result) {
  if (!callback_) {
    return;
  }

  auto self = weak_self_.GetWeakPtr();

  // callback may destroy this object
  callback_(result);

  if (self.is_alive() && !cmd_runner_.IsReady()) {
    cmd_runner_.Cancel();
  }
}

void BrEdrInterrogator::QueueRemoteNameRequest() {
  pw::bluetooth::emboss::PageScanRepetitionMode mode =
      pw::bluetooth::emboss::PageScanRepetitionMode::R0_;
  if (peer_->bredr()->page_scan_repetition_mode()) {
    mode = *peer_->bredr()->page_scan_repetition_mode();
  }

  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::RemoteNameRequestCommandWriter>(
      hci_spec::kRemoteNameRequest);
  auto packet_view = packet.view_t();
  packet_view.bd_addr().CopyFrom(peer_->address().value().view());
  packet_view.page_scan_repetition_mode().Write(mode);
  if (peer_->bredr()->clock_offset()) {
    packet_view.clock_offset().valid().Write(true);
    const uint16_t offset = peer_->bredr()->clock_offset().value();
    packet_view.clock_offset().clock_offset().Write(offset);
  }

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (HCI_IS_ERROR(event, WARN, "gap-bredr", "remote name request failed")) {
      return;
    }
    bt_log(TRACE,
           "gap-bredr",
           "name request complete (peer id: %s)",
           bt_str(peer_id_));

    auto params =
        event.view<pw::bluetooth::emboss::RemoteNameRequestCompleteEventView>();
    emboss::support::ReadOnlyContiguousBuffer name =
        params.remote_name().BackingStorage();
    const unsigned char* name_end = std::find(name.begin(), name.end(), '\0');
    std::string name_string(reinterpret_cast<const char*>(name.begin()),
                            reinterpret_cast<const char*>(name_end));
    peer_->RegisterName(std::move(name_string),
                        Peer::NameSource::kNameDiscoveryProcedure);
  };

  bt_log(TRACE,
         "gap-bredr",
         "sending name request (peer id: %s)",
         bt_str(peer_->identifier()));
  cmd_runner_.QueueCommand(std::move(packet),
                           std::move(cmd_cb),
                           /*wait=*/false,
                           hci_spec::kRemoteNameRequestCompleteEventCode,
                           {hci_spec::kInquiry});
}

void BrEdrInterrogator::QueueReadRemoteFeatures() {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadRemoteSupportedFeaturesCommandWriter>(
      hci_spec::kReadRemoteSupportedFeatures);
  packet.view_t().connection_handle().Write(handle_);

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (HCI_IS_ERROR(event,
                     WARN,
                     "gap-bredr",
                     "read remote supported features failed")) {
      return;
    }
    bt_log(TRACE,
           "gap-bredr",
           "remote features request complete (peer id: %s)",
           bt_str(peer_id_));
    auto view = event.view<
        pw::bluetooth::emboss::ReadRemoteSupportedFeaturesCompleteEventView>();
    peer_->SetFeaturePage(0, view.lmp_features().BackingStorage().ReadUInt());

    if (peer_->features().HasBit(/*page=*/0,
                                 hci_spec::LMPFeature::kExtendedFeatures)) {
      peer_->set_last_page_number(1);
      QueueReadRemoteExtendedFeatures(/*page=*/1);
    }
  };

  bt_log(TRACE,
         "gap-bredr",
         "asking for supported features (peer id: %s)",
         bt_str(peer_id_));
  cmd_runner_.QueueCommand(
      std::move(packet),
      std::move(cmd_cb),
      /*wait=*/false,
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
}

void BrEdrInterrogator::QueueReadRemoteExtendedFeatures(uint8_t page) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadRemoteExtendedFeaturesCommandWriter>(
      hci_spec::kReadRemoteExtendedFeatures);
  auto params = packet.view_t();
  params.connection_handle().Write(handle_);
  params.page_number().Write(page);

  auto cmd_cb = [this, page](const hci::EventPacket& event) {
    if (HCI_IS_ERROR(event,
                     WARN,
                     "gap-bredr",
                     "read remote extended features failed (peer id: %s)",
                     bt_str(peer_id_))) {
      return;
    }
    auto view = event.view<
        pw::bluetooth::emboss::ReadRemoteExtendedFeaturesCompleteEventView>();

    bt_log(TRACE,
           "gap-bredr",
           "got extended features page %u, max page %u (requested page: %u, "
           "peer id: %s)",
           view.page_number().Read(),
           view.max_page_number().Read(),
           page,
           bt_str(peer_id_));

    peer_->SetFeaturePage(view.page_number().Read(),
                          view.lmp_features().BackingStorage().ReadUInt());

    if (view.page_number().Read() != page) {
      bt_log(INFO,
             "gap-bredr",
             "requested page %u and got page %u, giving up (peer: %s)",
             page,
             view.page_number().Read(),
             bt_str(peer_id_));
      peer_->set_last_page_number(0);
      return;
    }

    // NOTE: last page number will be capped at 2
    peer_->set_last_page_number(view.max_page_number().Read());

    if (page < peer_->features().last_page_number()) {
      QueueReadRemoteExtendedFeatures(page + 1);
    }
  };

  bt_log(TRACE,
         "gap-bredr",
         "requesting extended features page %u (peer id: %s)",
         page,
         bt_str(peer_id_));
  cmd_runner_.QueueCommand(
      std::move(packet),
      std::move(cmd_cb),
      /*wait=*/false,
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode);
}

void BrEdrInterrogator::QueueReadRemoteVersionInformation() {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadRemoteVersionInfoCommandWriter>(
      hci_spec::kReadRemoteVersionInfo);
  packet.view_t().connection_handle().Write(handle_);

  auto cmd_cb = [this](const hci::EventPacket& event) {
    if (HCI_IS_ERROR(event, WARN, "gap", "read remote version info failed")) {
      return;
    }
    PW_DCHECK(event.event_code() ==
              hci_spec::kReadRemoteVersionInfoCompleteEventCode);
    bt_log(TRACE,
           "gap",
           "read remote version info completed (peer id: %s)",
           bt_str(peer_id_));
    auto view = event.view<
        pw::bluetooth::emboss::ReadRemoteVersionInfoCompleteEventView>();
    peer_->set_version(view.version().Read(),
                       view.company_identifier().Read(),
                       view.subversion().Read());
  };

  bt_log(
      TRACE, "gap", "asking for version info (peer id: %s)", bt_str(peer_id_));
  cmd_runner_.QueueCommand(std::move(packet),
                           std::move(cmd_cb),
                           /*wait=*/false,
                           hci_spec::kReadRemoteVersionInfoCompleteEventCode);
}

}  // namespace bt::gap
