#include "../src/libwebrtc.h"

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <set>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/fake_network.h"
#include "rtc_base/logging.h"
#include "rtc_base/network.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include "api/task_queue/pending_task_safety_flag.h"
#if __has_include("api/portal_tx_snapshot_v2.h")
#include "api/portal_tx_snapshot_v2.h"
static constexpr bool kPortalSdk = true;
#else
// Ordinary SDK without the portal overlay: no backend portal API, so portal enable refuses.
#include "portal-sdk/portal_tx_snapshot_v2.h"
int webrtc_portal_tx_observe_v2(void*, webrtc_portal_tx_notify_v2, void*, webrtc_portal_tx_snapshot_v2*) { return 0; }
int webrtc_portal_tx_try_send_v2(void*, const void*, int, uint64_t, uint64_t) { return -1; }
int webrtc_portal_tx_activate_v2(void*, uint64_t, uint64_t) { return -1; }
static constexpr bool kPortalSdk = false;
#endif

#if defined(__GNUC__) || defined(__clang__)
#define WEBRTC_EXPORT __attribute__((visibility("default")))
#else
#define WEBRTC_EXPORT
#endif

namespace rtc {
bool HasIPv6Enabled() {
	return false;
}
}

struct libwebrtc_connection;
struct libwebrtc_data_channel;
static constexpr uint64_t kPortalCapabilities = UINT64_C(0x1f);
static constexpr uint64_t kPortalPeerBytes = 65536, kPortalGlobalBytes = 1048576;
static constexpr uint64_t kPortalPeerRecords = 4096, kPortalGlobalRecords = 65536;
// SDK-held ingress before the ring: 32 records per channel; the 1 MiB global cap bounds multi-channel peers.
static constexpr uint32_t kPortalIngressRecords = 32;
static constexpr uint64_t kPortalIngressBytes = 1048576;
static void PortalWake(libwebrtc_context *ctx);
static void PortalRemoveChannel(libwebrtc_data_channel *channel);
static uint64_t PortalBuffered(libwebrtc_context *ctx);
struct stored_ice_server {
	libwebrtc_ice_server_type type = LIBWEBRTC_ICE_SERVER_STUN;
	std::string url;
	std::string username;
	std::string password;
};

class CreateDescriptionObserver;

void LogRtcError(const char* prefix, const webrtc::RTCError& error) {
	std::fprintf(stderr, "webrtc_native_linux: %s: %s\n", prefix, error.message());
}

static bool HasSchemePrefix(const std::string& value) {
	return value.find("://") != std::string::npos;
}

static std::string NormalizeStunUrl(const std::string& server) {
	if(server.empty()) {
		return "";
	}

	if(server.rfind("stun:", 0) == 0 || server.rfind("stuns:", 0) == 0) {
		return server;
	}

	if(HasSchemePrefix(server)) {
		return "";
	}

	return "stun:" + server;
}

static std::string NormalizeTurnUrl(const std::string& server) {
	if(server.empty()) {
		return "";
	}

	if(server.rfind("turn:", 0) == 0 || server.rfind("turns:", 0) == 0) {
		return server;
	}

	if(HasSchemePrefix(server)) {
		return "";
	}

	return "turn:" + server;
}

static std::string ExtractIceUfrag(const std::string& sdp) {
	static constexpr char kPrefix[] = "a=ice-ufrag:";
	const std::size_t start = sdp.find(kPrefix);
	if(start == std::string::npos) {
		return "";
	}

	const std::size_t value_start = start + sizeof(kPrefix) - 1;
	const std::size_t line_end = sdp.find_first_of("\r\n", value_start);
	return sdp.substr(value_start, line_end == std::string::npos ? std::string::npos : line_end - value_start);
}

struct libwebrtc_context {
	lwrtc_callback_function callback = nullptr;
	std::vector<stored_ice_server> ice_servers;

	std::unique_ptr<rtc::Thread> network_thread;
	std::unique_ptr<rtc::Thread> worker_thread;
	std::unique_ptr<rtc::Thread> signaling_thread;
	std::unique_ptr<rtc::NetworkManager> network_manager;
	std::unique_ptr<rtc::BasicPacketSocketFactory> packet_socket_factory;
    bool portal = false;
    std::atomic<bool> portal_snapshot_failed{false};
    std::mutex portal_progress_mutex;
    std::condition_variable portal_progress;
    uint64_t portal_revision = 0;
    void (*portal_wake)(void *) = nullptr;
    void *portal_wake_user = nullptr;
    struct Ledger {
        libwebrtc_data_channel *channel;
        libwebrtc_connection *owner;
        uint64_t bytes = 0;
        webrtc_portal_tx_snapshot_v2 snapshot = {};
        bool bound = false;
        bool activated = false, retire_task = false, activation_failed = false;
        int snapshot_state = 1;
        uint64_t in_progress_bytes = 0, in_progress_records = 0;
    };
    // Stable callback addresses; one record per reserved lifetime until terminal socket destruction.
    std::list<Ledger> portal_ledger; // network-thread only
    std::set<libwebrtc_connection *> portal_connections; // signaling-thread only
    std::shared_ptr<std::atomic<uint64_t>> portal_ingress_bytes = std::make_shared<std::atomic<uint64_t>>(0);
	bool zero_network_environment = false;
	std::unordered_map<std::string, libwebrtc_connection*> zero_network_connections;

	webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
};

struct libwebrtc_data_channel : public webrtc::DataChannelObserver {
	libwebrtc_connection* connection = nullptr;
	void* user_data = nullptr;
	webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
	libwebrtc_data_channel* zero_network_peer = nullptr;
	std::string zero_network_label;
	bool inbound = false;
	bool open_notified = false;
	bool close_notified = false;
	bool zero_network_synthetic = false;
    // Portal: SDK callbacks arrive on the network thread and are posted to signaling here.
    std::shared_ptr<std::atomic<uint32_t>> portal_ingress_records = std::make_shared<std::atomic<uint32_t>>(0);
    webrtc::ScopedTaskSafety portal_safety;

	libwebrtc_data_channel(libwebrtc_connection* parent,
	                       webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel,
	                       bool is_inbound);
	~libwebrtc_data_channel() override;

	bool IsOkToCallOnTheNetworkThread() override;
	void OnStateChange() override;
	void OnMessage(const webrtc::DataBuffer& buffer) override;
	void HandleState(webrtc::DataChannelInterface::DataState state);
	void Deliver(const uint8_t* data, size_t size);
	void OnBufferedAmountChange(uint64_t) override;
	void NotifySyntheticOpen();
	void NotifySyntheticClose();
};

struct libwebrtc_connection : public webrtc::PeerConnectionObserver {
	libwebrtc_context* ctx = nullptr;
	void* user_data = nullptr;
	webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
	webrtc::scoped_refptr<webrtc::DataChannelInterface> bootstrap_channel;
	libwebrtc_connection* zero_network_peer = nullptr;
	std::string local_ufrag;
	std::string remote_ufrag;
    bool portal_closing = false;
    unsigned portal_observers = 0; // SDP observer lifetime, signaling-thread only
    std::vector<std::unique_ptr<libwebrtc_data_channel>> portal_channels;
	bool disconnected_notified = false;
	bool destroy_notified = false;
	bool established_notified = false;

	~libwebrtc_connection() override = default;

	void NotifyError(const char* message) {
		if(ctx && ctx->callback) {
			ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_ERROR, user_data, (void*)message, message ? (int)std::char_traits<char>::length(message) : 0);
		}
	}

	void NotifyDestroyed() {
        if (ctx && ctx->portal) { portal_closing = true; PortalWake(ctx); return; }
		if(destroy_notified) {
			return;
		}
		destroy_notified = true;
		if(ctx && ctx->callback) {
			ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_DESTROY, user_data, nullptr, 0);
		}
	}

	void NotifyDisconnected() {
		if(disconnected_notified) {
			return;
		}
		disconnected_notified = true;
		if(ctx && ctx->callback) {
			ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_DISCONNECTED, user_data, nullptr, 0);
		}
	}

	void MaybeRegisterZeroNetworkLocalDescription(const std::string& sdp) {
		if(!ctx || ctx->portal || !ctx->zero_network_environment) {
			return;
		}
		local_ufrag = ExtractIceUfrag(sdp);
		if(local_ufrag.empty()) {
			return;
		}
		ctx->zero_network_connections[local_ufrag] = this;
	}

	void MaybeLinkZeroNetworkPeer() {
		if(!ctx || !ctx->zero_network_environment || remote_ufrag.empty() || zero_network_peer) {
			return;
		}

		auto it = ctx->zero_network_connections.find(remote_ufrag);
		if(it == ctx->zero_network_connections.end() || !it->second || it->second == this) {
			return;
		}

		zero_network_peer = it->second;
		if(!zero_network_peer->zero_network_peer) {
			zero_network_peer->zero_network_peer = this;
		}
	}

	void MaybeNotifyZeroNetworkEstablished() {
		if(!ctx || !ctx->zero_network_environment || established_notified || !zero_network_peer) {
			return;
		}
		established_notified = true;
		if(ctx->callback) {
			ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_ESTABLISHED, user_data, nullptr, 0);
		}
	}

	void CloseFromError(const char* message) {
		NotifyError(message);
		if(connection) {
			connection->Close();
		} else {
			NotifyDisconnected();
			NotifyDestroyed();
		}
	}

	void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
		if(new_state == webrtc::PeerConnectionInterface::SignalingState::kClosed) {
			NotifyDisconnected();
			NotifyDestroyed();
		}
	}

	void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
        if (ctx->portal && (!channel || portal_closing || !portal_channels.empty() ||
            channel->ordered() || !channel->maxRetransmitsOpt().has_value() || *channel->maxRetransmitsOpt() != 0 ||
            channel->maxPacketLifeTime().has_value())) {
            if (channel) channel->Close();
            CloseFromError("incompatible portal data-channel policy"); return;
        }
		auto* wrapper = new libwebrtc_data_channel(this, channel, true);
        // Reserve pending OPEN_ACK before the SDK schedules OnTransportReady after this callback.
        if (ctx->portal && PortalBuffered(ctx) == UINT64_MAX) {
            CloseFromError("portal initial control ownership unavailable"); return;
        }
		if(channel && channel->state() == webrtc::DataChannelInterface::kOpen) {
			wrapper->OnStateChange();
		}
	}

	void OnRenegotiationNeeded() override {}

	void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
		std::fprintf(stderr, "webrtc_native_linux: ice-connection-state=%d\n", static_cast<int>(new_state));
		switch(new_state) {
			case webrtc::PeerConnectionInterface::kIceConnectionConnected:
			case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
				if(!established_notified && ctx && ctx->callback) {
					established_notified = true;
					ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_ESTABLISHED, user_data, nullptr, 0);
				}
				break;
			case webrtc::PeerConnectionInterface::kIceConnectionFailed:
			case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
			case webrtc::PeerConnectionInterface::kIceConnectionClosed:
				NotifyDisconnected();
				if(connection && new_state != webrtc::PeerConnectionInterface::kIceConnectionClosed) {
					connection->Close();
				}
				break;
			default:
				break;
		}
	}

	void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
		std::fprintf(stderr, "webrtc_native_linux: peer-connection-state=%d\n", static_cast<int>(new_state));
	}

	void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
		std::fprintf(stderr, "webrtc_native_linux: ice-gathering-state=%d\n", static_cast<int>(new_state));
		if(!ctx->portal && new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete && connection) {
			const webrtc::SessionDescriptionInterface* desc = connection->local_description();
			if(desc) {
				std::string sdp;
				if(desc->ToString(&sdp)) {
					std::fprintf(stderr, "webrtc_native_linux: local-description-after-gathering=\n%s\n", sdp.c_str());
				}
			}
		}
	}

	void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
		if(!ctx || !ctx->callback || !candidate) {
			return;
		}

		std::string sdp;
		if(!candidate->ToString(&sdp) || sdp.empty()) {
			NotifyError("ice-candidate-to-string");
			return;
		}
		std::fprintf(stderr, "webrtc_native_linux: local-ice-candidate=%s\n", ctx->portal ? "-" : sdp.c_str());
		ctx->callback(ctx, this, nullptr, LWRTC_CALLBACK_ICE_CANDIDATE, user_data, (void*)sdp.c_str(), (int)sdp.size());
	}

	void OnIceCandidateError(const std::string& address,
	                         int port,
	                         const std::string& url,
	                         int error_code,
	                         const std::string& error_text) override {
        if (ctx && ctx->portal) { NotifyError("ice-candidate-error"); return; }
		std::fprintf(stderr,
		             "webrtc_native_linux: ice-candidate-error address=%s port=%d url=%s code=%d text=%s\n",
		             address.c_str(),
		             port,
		             url.c_str(),
		             error_code,
		             error_text.c_str());
	}

	void OnIceConnectionReceivingChange(bool) override {}
	void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>&) override {}
	void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>,
	                const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {}
	void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>) override {}
};

libwebrtc_data_channel::libwebrtc_data_channel(libwebrtc_connection* parent,
                                               webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel,
                                               bool is_inbound)
	: connection(parent)
	, user_data(parent ? parent->user_data : nullptr)
	, channel(std::move(data_channel))
	, inbound(is_inbound) {
    if (connection && connection->ctx->portal) {
        connection->portal_channels.emplace_back(this);
        connection->ctx->network_thread->BlockingCall([&] {
            connection->ctx->portal_ledger.push_back({this, connection});
        });
    }
	if(channel) {
		channel->RegisterObserver(this);
	}
}

libwebrtc_data_channel::~libwebrtc_data_channel() {
    if (connection && connection->ctx && connection->ctx->portal) PortalRemoveChannel(this);
	if(channel) {
		channel->UnregisterObserver();
	}
}

void libwebrtc_data_channel::NotifySyntheticOpen() {
	if(!connection || !connection->ctx || !connection->ctx->callback || open_notified) {
		return;
	}

	open_notified = true;
	connection->ctx->callback(
		connection->ctx,
		connection,
		this,
		inbound ? LWRTC_CALLBACK_CHANNEL_ACCEPTED : LWRTC_CALLBACK_CHANNEL_CONNECTED,
		user_data,
		zero_network_label.empty() ? nullptr : (void*)zero_network_label.c_str(),
		(int)zero_network_label.size());
}

void libwebrtc_data_channel::NotifySyntheticClose() {
	if(!connection || !connection->ctx || !connection->ctx->callback || close_notified) {
		return;
	}

	close_notified = true;
	connection->ctx->callback(
		connection->ctx,
		connection,
		this,
		LWRTC_CALLBACK_CHANNEL_CLOSED,
		user_data,
		zero_network_label.empty() ? nullptr : (void*)zero_network_label.c_str(),
		(int)zero_network_label.size());
}

bool libwebrtc_data_channel::IsOkToCallOnTheNetworkThread() { return connection && connection->ctx->portal; }

void libwebrtc_data_channel::OnStateChange() {
    if (channel && connection && connection->ctx->portal && connection->ctx->network_thread->IsCurrent()) {
        connection->ctx->signaling_thread->PostTask(webrtc::SafeTask(portal_safety.flag(),
            [this, state = channel->state()] { HandleState(state); }));
        return;
    }
    if (channel) HandleState(channel->state());
}

void libwebrtc_data_channel::HandleState(webrtc::DataChannelInterface::DataState state) {
	if(!connection || !connection->ctx || !connection->ctx->callback || !channel) {
		return;
	}

	std::fprintf(stderr,
	             "webrtc_native_linux: data-channel-state label=%s state=%d inbound=%d\n",
	             connection->ctx->portal ? "-" : channel->label().c_str(),
	             static_cast<int>(state),
	             inbound ? 1 : 0);

	switch(state) {
		case webrtc::DataChannelInterface::kOpen: {
			if(open_notified) {
				return;
			}
			open_notified = true;
			const std::string label = channel->label();
			connection->ctx->callback(
				connection->ctx,
				connection,
				this,
				inbound ? LWRTC_CALLBACK_CHANNEL_ACCEPTED : LWRTC_CALLBACK_CHANNEL_CONNECTED,
				user_data,
				label.empty() ? nullptr : (void*)label.c_str(),
				(int)label.size());
			break;
		}
		case webrtc::DataChannelInterface::kClosed: {
			if(close_notified) {
				return;
			}
			close_notified = true;
            if (connection->ctx->portal) connection->portal_closing = true;
			const std::string label = channel->label();
			connection->ctx->callback(
				connection->ctx,
				connection,
				this,
				LWRTC_CALLBACK_CHANNEL_CLOSED,
				user_data,
				label.empty() ? nullptr : (void*)label.c_str(),
				(int)label.size());
			if (!connection->ctx->portal) delete this;
			break;
		}
		default:
			break;
	}
}

// Holds one SDK message's ingress reservation until delivery to the ring or task cancellation.
struct PortalIngressHold {
    std::shared_ptr<std::atomic<uint32_t>> records;
    std::shared_ptr<std::atomic<uint64_t>> bytes;
    uint64_t size;
    PortalIngressHold(std::shared_ptr<std::atomic<uint32_t>> r, std::shared_ptr<std::atomic<uint64_t>> b, uint64_t n)
        : records(std::move(r)), bytes(std::move(b)), size(n) { ++*records; *bytes += size; }
    PortalIngressHold(PortalIngressHold &&) = default;
    ~PortalIngressHold() { if (records) { --*records; *bytes -= size; } }
};

void libwebrtc_data_channel::OnMessage(const webrtc::DataBuffer& buffer) {
    auto *ctx = connection ? connection->ctx : nullptr;
    if (!ctx || !ctx->portal || !ctx->network_thread->IsCurrent()) return Deliver(buffer.data.cdata(), buffer.size());
    // Network thread: no humblenet locks. Only reservations on this thread add, so the check cannot race.
    const size_t size = buffer.size(), kept = size >= 1 && size <= 1400 ? size : 0;
    // ponytail: uncounted drop; ingress_pressure lives in libhumblenet, counting needs an lwrtc_portal_api field.
    if (*portal_ingress_records >= kPortalIngressRecords || *ctx->portal_ingress_bytes + kept > kPortalIngressBytes) return;
    ctx->signaling_thread->PostTask(webrtc::SafeTask(portal_safety.flag(),
        [this, hold = PortalIngressHold(portal_ingress_records, ctx->portal_ingress_bytes, kept),
         data = kept ? buffer.data : rtc::CopyOnWriteBuffer(), size] { Deliver(data.cdata(), size); }));
}

void libwebrtc_data_channel::Deliver(const uint8_t* data, size_t size) {
    if (connection && connection->ctx->portal && (size < 1 || size > 1400)) {
        connection->ctx->callback(connection->ctx, connection, this, LWRTC_CALLBACK_CHANNEL_RECEIVE,
            user_data, nullptr, size > 1400 ? 1401 : 0);
        return;
    }
	if(!connection || !connection->ctx || !connection->ctx->callback) {
		return;
	}

	if(connection->ctx->callback(
		connection->ctx,
		connection,
		this,
		LWRTC_CALLBACK_CHANNEL_RECEIVE,
		user_data,
		(void*)data,
		(int)size)) {
        if (connection->ctx->portal) {
            connection->portal_closing = true;
            connection->NotifyDisconnected(); PortalWake(connection->ctx);
        } else if (channel) channel->Close();
	}
}

class SetLocalDescriptionObserver : public webrtc::SetLocalDescriptionObserverInterface {
public:
	explicit SetLocalDescriptionObserver(libwebrtc_connection* connection)
		: connection_(connection) { if (connection_->ctx->portal) ++connection_->portal_observers; }
    ~SetLocalDescriptionObserver() override { if (connection_->ctx->portal) { --connection_->portal_observers; PortalWake(connection_->ctx); } }

	void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
		if(!error.ok()) {
			LogRtcError("set-local-description", error);
			if(connection_) {
				connection_->CloseFromError("set-local-description");
			}
		}
	}

private:
	libwebrtc_connection* connection_;
};

class CreateDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
	CreateDescriptionObserver(libwebrtc_connection* connection, webrtc::SdpType expected_type)
		: connection_(connection)
		, expected_type_(expected_type) { if (connection_->ctx->portal) ++connection_->portal_observers; }
    ~CreateDescriptionObserver() override { if (connection_->ctx->portal) { --connection_->portal_observers; PortalWake(connection_->ctx); } }

	void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
		if(!connection_ || !connection_->connection || !connection_->ctx || !connection_->ctx->callback || !desc) {
			delete desc;
			return;
		}

		if(desc->GetType() != expected_type_) {
			delete desc;
			connection_->CloseFromError("unexpected-local-description-type");
			return;
		}

		std::string sdp;
		if(!desc->ToString(&sdp) || sdp.empty()) {
			delete desc;
			connection_->CloseFromError("local-description-to-string");
			return;
		}

		std::unique_ptr<webrtc::SessionDescriptionInterface> owned_desc(desc);
		connection_->connection->SetLocalDescription(
			std::move(owned_desc),
			rtc::make_ref_counted<SetLocalDescriptionObserver>(connection_));
		connection_->MaybeRegisterZeroNetworkLocalDescription(sdp);

		connection_->ctx->callback(
			connection_->ctx,
			connection_,
			nullptr,
			LWRTC_CALLBACK_LOCAL_DESCRIPTION,
			connection_->user_data,
			(void*)sdp.c_str(),
			(int)sdp.size());
	}

	void OnFailure(webrtc::RTCError error) override {
		LogRtcError("create-description", error);
		if(connection_) {
			connection_->CloseFromError("create-description");
		}
	}

private:
	libwebrtc_connection* connection_;
	webrtc::SdpType expected_type_;
};

class SetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
	enum class Action {
		kSetOffer,
		kSetAnswer
	};

	SetRemoteDescriptionObserver(libwebrtc_connection* connection, Action action)
		: connection_(connection)
		, action_(action) { if (connection_->ctx->portal) ++connection_->portal_observers; }
    ~SetRemoteDescriptionObserver() override { if (connection_->ctx->portal) { --connection_->portal_observers; PortalWake(connection_->ctx); } }

	void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
		if(!error.ok()) {
			LogRtcError("set-remote-description", error);
			if(connection_) {
				connection_->CloseFromError("set-remote-description");
			}
			return;
		}

		if(!connection_ || !connection_->connection || (connection_->ctx->portal && connection_->portal_closing)) {
			return;
		}

		if(action_ == Action::kSetOffer) {
			connection_->connection->CreateAnswer(
				rtc::make_ref_counted<CreateDescriptionObserver>(connection_, webrtc::SdpType::kAnswer).get(),
				webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
		} else {
			connection_->MaybeLinkZeroNetworkPeer();
			connection_->MaybeNotifyZeroNetworkEstablished();
			if(connection_->zero_network_peer) {
				connection_->zero_network_peer->MaybeNotifyZeroNetworkEstablished();
			}
		}
	}

private:
	libwebrtc_connection* connection_;
	Action action_;
};

webrtc::PeerConnectionInterface::RTCConfiguration BuildRtcConfig(const libwebrtc_context* ctx) {
	webrtc::PeerConnectionInterface::RTCConfiguration config;
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

	for(const stored_ice_server& server : ctx->ice_servers) {
		const std::string raw_url = server.url;
		const std::string url = server.type == LIBWEBRTC_ICE_SERVER_TURN
			? NormalizeTurnUrl(raw_url)
			: NormalizeStunUrl(raw_url);
		if(url.empty()) {
			continue;
		}

		webrtc::PeerConnectionInterface::IceServer ice_server;
		ice_server.uri = url;
		ice_server.urls.push_back(ice_server.uri);
		if(server.type == LIBWEBRTC_ICE_SERVER_TURN) {
			if(!server.username.empty()) {
				ice_server.username = server.username;
			}
			if(!server.password.empty()) {
				ice_server.password = server.password;
			}
		}
		config.servers.push_back(ice_server);
	}

	return config;
}

static void PortalWake(libwebrtc_context *ctx) {
    { std::lock_guard<std::mutex> lock(ctx->portal_progress_mutex); ++ctx->portal_revision; }
    ctx->portal_progress.notify_all();
    if (ctx->portal_wake) ctx->portal_wake(ctx->portal_wake_user);
}
static void PortalRemoveChannel(libwebrtc_data_channel *channel) {
    auto *ctx = channel->connection->ctx;
    ctx->network_thread->BlockingCall([&] {
        auto &ledger = ctx->portal_ledger;
        for (auto it = ledger.begin(); it != ledger.end(); ++it) {
            if (it->channel != channel) continue;
            if (it->bound) { it->channel = nullptr; break; }
            ledger.erase(it); break;
        }
    });
    PortalWake(ctx);
}
void libwebrtc_data_channel::OnBufferedAmountChange(uint64_t) {
    if (connection && connection->ctx->portal) PortalWake(connection->ctx);
}
static void PortalSnapshot(void *user, const webrtc_portal_tx_snapshot_v2 *snapshot, int state) {
    auto *record = static_cast<libwebrtc_context::Ledger *>(user);
    record->snapshot_state = snapshot && (state == 1 || state == 2) ? state : 0;
    if (record->snapshot_state) {
        record->snapshot = *snapshot;
        record->bytes = snapshot->owned_payload_bytes;
    }
    if (!record->snapshot_state) record->owner->ctx->portal_snapshot_failed.store(true);
    // SDK network callback: store absolute state and wake only; no SDK reentry or destruction.
    PortalWake(record->owner->ctx);
}
static bool PortalPotential(libwebrtc_context *ctx, uint64_t &bytes, uint64_t &records) {
    bytes = records = 0;
    for (const auto &r : ctx->portal_ledger) {
        if (!r.snapshot_state || r.snapshot.owned_payload_bytes > r.snapshot.reservation_bytes ||
            r.snapshot.owned_records > r.snapshot.reservation_records ||
            r.snapshot.reservation_bytes > UINT64_MAX - r.in_progress_bytes ||
            r.snapshot.reservation_records > UINT64_MAX - r.in_progress_records) return false;
        const auto b = (r.activated ? r.snapshot.reservation_bytes : r.snapshot.owned_payload_bytes) + r.in_progress_bytes;
        const auto n = (r.activated ? r.snapshot.reservation_records : r.snapshot.owned_records) + r.in_progress_records;
        if (b > UINT64_MAX - bytes || n > UINT64_MAX - records) return false;
        bytes += b; records += n;
    }
    return true;
}
static void PortalActivate(libwebrtc_context *ctx, libwebrtc_context::Ledger &r) {
    if (!r.bound || r.activated || r.activation_failed || !r.channel || !r.channel->channel) return;
    uint64_t used_bytes, used_records;
    const auto &s = r.snapshot;
    if (!PortalPotential(ctx, used_bytes, used_records) || s.reservation_bytes < s.owned_payload_bytes ||
        s.reservation_records < s.owned_records) {
        ctx->portal_snapshot_failed.store(true); return;
    }
    const auto added_bytes = s.reservation_bytes - s.owned_payload_bytes;
    const auto added_records = s.reservation_records - s.owned_records;
    int result = 0;
    if (s.reservation_bytes <= kPortalPeerBytes && s.reservation_records <= kPortalPeerRecords &&
        added_bytes <= kPortalGlobalBytes && added_records <= kPortalGlobalRecords &&
        used_bytes <= kPortalGlobalBytes - added_bytes && used_records <= kPortalGlobalRecords - added_records) {
        const auto bytes = std::min(kPortalPeerBytes, s.owned_payload_bytes + kPortalGlobalBytes - used_bytes);
        const auto records = std::min(kPortalPeerRecords, s.owned_records + kPortalGlobalRecords - used_records);
        r.in_progress_bytes = added_bytes; r.in_progress_records = added_records;
        result = webrtc_portal_tx_activate_v2(r.channel->channel.get(), bytes, records);
        r.in_progress_bytes = r.in_progress_records = 0;
        if (result == 1 && r.snapshot_state == 1 && s.reservation_bytes <= bytes &&
            s.reservation_records <= records) { r.activated = true; return; }
    }
    // Keep this exact callback context alive until the queued scoped close has run.
    r.activation_failed = r.retire_task = true;
    auto *record = &r;
    ctx->signaling_thread->PostTask([ctx, record] {
        record->owner->CloseFromError("portal initial control reservation refused");
        ctx->network_thread->BlockingCall([record] { record->retire_task = false; });
        PortalWake(ctx);
    });
}
static uint64_t PortalRefresh(libwebrtc_context *ctx) {
    if (ctx->portal_snapshot_failed.load()) return UINT64_MAX;
    uint64_t total = 0;
    for (auto &r : ctx->portal_ledger) {
        if (!r.bound && r.channel && r.channel->channel) {
            webrtc_portal_tx_snapshot_v2 snapshot = {};
            if (!webrtc_portal_tx_observe_v2(r.channel->channel.get(), PortalSnapshot, &r, &snapshot)) {
                ctx->portal_snapshot_failed.store(true);
                return UINT64_MAX;
            }
            r.bound = true; r.snapshot_state = 1; r.snapshot = snapshot;
            r.bytes = snapshot.owned_payload_bytes;
        }
        PortalActivate(ctx, r);
        if (!r.snapshot_state || UINT64_MAX - total < r.bytes) {
            ctx->portal_snapshot_failed.store(true); return UINT64_MAX;
        }
        total += r.bytes;
    }
    return total;
}
static int PortalEnable(libwebrtc_context *ctx, void (*wake)(void *), void *user) {
    if (!ctx || ctx->portal || !wake) return 0;
    ctx->portal_wake = wake; ctx->portal_wake_user = user; ctx->portal = true;
    return 1;
}
static int PortalWrite(libwebrtc_data_channel *wrapper, const void *data, int length) {
    if (!wrapper || !data || length < 1 || length > 1400) return -1;
    auto *ctx = wrapper->connection->ctx;
    if (!ctx->portal || wrapper->zero_network_synthetic) return -1;
    // scoped ref survives a synchronous channel close; wrapper stays owned until explicit reclaim.
    auto channel = wrapper->channel;
    if (!channel) return -1;
    return ctx->network_thread->BlockingCall([&] {
        if (channel->state() != webrtc::DataChannelInterface::kOpen) return -1;
        const auto total = PortalRefresh(ctx);
        auto it = std::find_if(ctx->portal_ledger.begin(), ctx->portal_ledger.end(), [&](const auto &r) { return r.channel == wrapper; });
        if (it == ctx->portal_ledger.end()) return -1;
        if (total == UINT64_MAX || !it->bound || it->snapshot_state != 1) return -1;
        if (!it->activated) return it->activation_failed ? -1 : 0;
        // SDK proof: a queued original prepays its complete fragment family, including restart.
        const uint64_t added_bytes = 2u * static_cast<uint64_t>(length);
        const uint64_t added_records = static_cast<uint64_t>(length) + 1;
        uint64_t used_bytes, used_records;
        if (!PortalPotential(ctx, used_bytes, used_records)) {
            ctx->portal_snapshot_failed.store(true); return -1;
        }
        if (it->in_progress_bytes || it->in_progress_records ||
            it->snapshot.reservation_bytes > kPortalPeerBytes - added_bytes ||
            it->snapshot.reservation_records > kPortalPeerRecords - added_records ||
            used_bytes > kPortalGlobalBytes - added_bytes || used_records > kPortalGlobalRecords - added_records) return 0;
        const auto byte_ceiling = std::min(kPortalPeerBytes, it->snapshot.reservation_bytes + kPortalGlobalBytes - used_bytes);
        const auto record_ceiling = std::min(kPortalPeerRecords, it->snapshot.reservation_records + kPortalGlobalRecords - used_records);
        it->in_progress_bytes = added_bytes; it->in_progress_records = added_records;
        const int result = webrtc_portal_tx_try_send_v2(channel.get(), data, length, byte_ceiling, record_ceiling);
        // A synchronous snapshot may already include this send; hold the transaction through return.
        it->in_progress_bytes = it->in_progress_records = 0;
        if (it->snapshot_state != 1 || it->snapshot.reservation_bytes > byte_ceiling ||
            it->snapshot.reservation_records > record_ceiling) return -1;
        return result;
    });
}
static int PortalReclaim(libwebrtc_connection *connection) {
    if (!connection || !connection->ctx->portal) return 0;
    auto *ctx = connection->ctx;
    return ctx->signaling_thread->BlockingCall([&] {
        if (!connection->portal_closing) return 0;
        if (connection->connection) {
            connection->connection->Close();
            connection->bootstrap_channel = nullptr;
            connection->connection = nullptr;
        }
        connection->portal_channels.clear();
        if (!ctx->network_thread->BlockingCall([&] {
            for (const auto &r : ctx->portal_ledger)
                if (r.owner == connection && (r.retire_task || (r.bound && r.snapshot_state != 2))) return false;
            ctx->portal_ledger.remove_if([&](const auto &r) { return r.owner == connection; });
            return true;
        })) return 0;
        if (connection->portal_observers) return 0;
        ctx->portal_connections.erase(connection);
        connection->destroy_notified = true;
        ctx->callback(ctx, connection, nullptr, LWRTC_CALLBACK_DESTROY, connection->user_data, nullptr, 0);
        delete connection;
        return 1;
    });
}
static void PortalDrainContext(libwebrtc_context *ctx) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // Reuse exact close/reclaim while SDP callbacks and their owning threads can still run.
    while (true) {
        uint64_t revision;
        { std::lock_guard<std::mutex> lock(ctx->portal_progress_mutex); revision = ctx->portal_revision; }
        auto pending = ctx->signaling_thread->BlockingCall([&] {
            return std::vector<libwebrtc_connection *>(ctx->portal_connections.begin(), ctx->portal_connections.end());
        });
        if (pending.empty()) return;
        for (auto *connection : pending) {
            libwebrtc_close_connection(connection);
            PortalReclaim(connection);
        }
        if (ctx->signaling_thread->BlockingCall([&] { return ctx->portal_connections.empty(); })) return;
        std::unique_lock<std::mutex> lock(ctx->portal_progress_mutex);
        if (!ctx->portal_progress.wait_until(lock, deadline, [&] { return revision != ctx->portal_revision; })) {
            // Never destroy a context beneath live callback references, even at shutdown.
            std::fprintf(stderr, "portal shutdown failed: backend callback retirement deadline exceeded\n");
            std::abort();
        }
    }
}
static uint64_t PortalBuffered(libwebrtc_context *ctx) {
    if (!ctx || !ctx->portal) return 0;
    return ctx->network_thread->BlockingCall([&] { return PortalRefresh(ctx); });
}
extern "C" {

WEBRTC_EXPORT struct libwebrtc_context* libwebrtc_create_context(lwrtc_callback_function callback) {
	if(!callback) {
		return nullptr;
	}

	rtc::InitializeSSL();

	std::unique_ptr<libwebrtc_context> ctx(new libwebrtc_context());
	ctx->callback = callback;
	ctx->network_thread = rtc::Thread::CreateWithSocketServer();
	ctx->worker_thread = rtc::Thread::Create();
	ctx->signaling_thread = rtc::Thread::Create();

	if(!ctx->network_thread || !ctx->worker_thread || !ctx->signaling_thread) {
		return nullptr;
	}

	ctx->network_thread->Start();
	ctx->worker_thread->Start();
	ctx->signaling_thread->Start();
	ctx->network_manager = std::make_unique<rtc::BasicNetworkManager>(
		ctx->network_thread->socketserver(),
		nullptr);
	ctx->packet_socket_factory = std::make_unique<rtc::BasicPacketSocketFactory>(
		ctx->network_thread->socketserver());

	if(!ctx->network_manager || !ctx->packet_socket_factory) {
		std::fprintf(stderr, "webrtc_native_linux: failed to create network primitives\n");
		return nullptr;
	}

	ctx->network_thread->BlockingCall([&ctx] {
		ctx->network_manager->StartUpdating();
		std::vector<const rtc::Network*> networks = ctx->network_manager->GetNetworks();
		std::fprintf(stderr, "webrtc_native_linux: detected-networks=%zu\n", networks.size());
		if(networks.empty()) {
			ctx->zero_network_environment = true;
			std::fprintf(stderr, "webrtc_native_linux: using zero-network allocator fallback\n");
		}
	});

	webrtc::PeerConnectionFactoryDependencies dependencies;
	dependencies.network_thread = ctx->network_thread.get();
	dependencies.worker_thread = ctx->worker_thread.get();
	dependencies.signaling_thread = ctx->signaling_thread.get();

	ctx->factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

	if(!ctx->factory) {
		std::fprintf(stderr, "webrtc_native_linux: failed to create PeerConnectionFactory\n");
		return nullptr;
	}

	return ctx.release();
}

WEBRTC_EXPORT void libwebrtc_destroy_context(struct libwebrtc_context* ctx) {
	if(!ctx) {
		return;
	}

    if (ctx->portal) PortalDrainContext(ctx);
	ctx->factory = nullptr;
	if(ctx->signaling_thread) {
		ctx->signaling_thread->Stop();
	}
	if(ctx->worker_thread) {
		ctx->worker_thread->Stop();
	}
	if(ctx->network_thread) {
		ctx->network_thread->Stop();
	}
	delete ctx;
}

WEBRTC_EXPORT void libwebrtc_set_ice_servers(struct libwebrtc_context* ctx, const struct libwebrtc_ice_server* servers, int count) {
	if(!ctx) {
		return;
	}

	ctx->ice_servers.clear();
	for(int i = 0; i < count; ++i) {
		if(servers[i].url) {
			stored_ice_server server;
			server.type = servers[i].type;
			server.url = servers[i].url;
			server.username = servers[i].username ? servers[i].username : "";
			server.password = servers[i].password ? servers[i].password : "";
			ctx->ice_servers.push_back(std::move(server));
		}
	}
}

WEBRTC_EXPORT struct libwebrtc_connection* libwebrtc_create_connection_extended(struct libwebrtc_context* ctx, void* user_data) {
	if(!ctx || !ctx->factory || !ctx->signaling_thread) {
		return nullptr;
	}

	std::unique_ptr<libwebrtc_connection> connection(new libwebrtc_connection());
	connection->ctx = ctx;
	connection->user_data = user_data;

	bool create_ok = false;
	ctx->signaling_thread->BlockingCall([&] {
        if (ctx->portal && (ctx->portal_connections.size() >= 160 || ctx->portal_snapshot_failed.load())) return;
		webrtc::PeerConnectionDependencies dependencies(connection.get());
		auto allocator = std::make_unique<cricket::BasicPortAllocator>(
			ctx->network_manager.get(),
			ctx->packet_socket_factory.get());
		allocator->set_allow_tcp_listen(true);
		uint32_t allocator_flags =
			cricket::PORTALLOCATOR_ENABLE_SHARED_SOCKET |
			cricket::PORTALLOCATOR_ENABLE_ANY_ADDRESS_PORTS;
		if(ctx->zero_network_environment) {
			allocator_flags |= cricket::PORTALLOCATOR_DISABLE_ADAPTER_ENUMERATION;
		} else {
			allocator->SetNetworkIgnoreMask(0);
		}
		allocator->set_flags(allocator_flags);
		dependencies.allocator = std::move(allocator);

		auto error_or_peer_connection =
			ctx->factory->CreatePeerConnectionOrError(BuildRtcConfig(ctx), std::move(dependencies));
		if(!error_or_peer_connection.ok()) {
			LogRtcError("create-peer-connection", error_or_peer_connection.error());
			return;
		}

		connection->connection = std::move(error_or_peer_connection.value());
		create_ok = true;
        if (ctx->portal) ctx->portal_connections.insert(connection.get());
	});

	if(!create_ok || !connection->connection) {
		return nullptr;
	}
	return connection.release();
}

WEBRTC_EXPORT struct libwebrtc_data_channel* libwebrtc_create_channel(struct libwebrtc_connection* connection, const char* name) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread) {
		return nullptr;
	}

	if(!connection->ctx->portal && connection->ctx->zero_network_environment && connection->zero_network_peer) {
		auto* outbound = new libwebrtc_data_channel(connection, nullptr, false);
		auto* inbound = new libwebrtc_data_channel(connection->zero_network_peer, nullptr, true);
		const std::string label = name ? name : "default";
		outbound->zero_network_synthetic = true;
		outbound->zero_network_label = label;
		outbound->zero_network_peer = inbound;
		inbound->zero_network_synthetic = true;
		inbound->zero_network_label = label;
		inbound->zero_network_peer = outbound;
		connection->ctx->signaling_thread->PostTask([outbound, inbound] {
			outbound->NotifySyntheticOpen();
			inbound->NotifySyntheticOpen();
		});
		return outbound;
	}

	webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
	connection->ctx->signaling_thread->BlockingCall([&] {
		if(connection->bootstrap_channel) {
			channel = connection->bootstrap_channel;
			connection->bootstrap_channel = nullptr;
		} else {
			webrtc::DataChannelInit config;
			config.ordered = false;
			config.reliable = false;
			config.maxRetransmits = 0;
			auto error_or_channel = connection->connection->CreateDataChannelOrError(name ? name : "default", &config);
			if(!error_or_channel.ok()) {
				LogRtcError("create-data-channel", error_or_channel.error());
				connection->CloseFromError("create-data-channel");
				return;
			}
			channel = std::move(error_or_channel.value());
		}
	});

	if(!channel) {
		connection->CloseFromError("create-data-channel");
		return nullptr;
	}

	auto* wrapper = new libwebrtc_data_channel(connection, channel, false);
	if(channel->state() == webrtc::DataChannelInterface::kOpen) {
		wrapper->OnStateChange();
	}
	return wrapper;
}

WEBRTC_EXPORT int libwebrtc_create_offer(struct libwebrtc_connection* connection) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread) {
		return 0;
	}

	int ok = 0;
	connection->ctx->signaling_thread->BlockingCall([&] {
		webrtc::DataChannelInit config;
		config.ordered = false;
		config.reliable = false;
		config.maxRetransmits = 0;
		auto error_or_channel = connection->connection->CreateDataChannelOrError("default", &config);
		if(!error_or_channel.ok()) {
			LogRtcError("create-bootstrap-data-channel", error_or_channel.error());
			connection->CloseFromError("create-bootstrap-data-channel");
			return;
		}
		connection->bootstrap_channel = std::move(error_or_channel.value());
		if(!connection->bootstrap_channel) {
			connection->CloseFromError("create-bootstrap-data-channel");
			return;
		}

		connection->connection->CreateOffer(
			rtc::make_ref_counted<CreateDescriptionObserver>(connection, webrtc::SdpType::kOffer).get(),
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
		ok = 1;
	});
	return ok;
}

WEBRTC_EXPORT int libwebrtc_set_offer(struct libwebrtc_connection* connection, const char* sdp) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread || !sdp) {
		return 0;
	}

	int ok = 0;
	connection->ctx->signaling_thread->BlockingCall([&] {
		connection->remote_ufrag = ExtractIceUfrag(sdp);
		webrtc::SdpParseError error;
		std::unique_ptr<webrtc::SessionDescriptionInterface> description =
			webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);
		if(!description) {
			std::fprintf(stderr, "webrtc_native_linux: set-offer parse error at '%s': %s\n", connection->ctx->portal ? "-" : error.line.c_str(), connection->ctx->portal ? "-" : error.description.c_str());
			return;
		}

		connection->connection->SetRemoteDescription(
			std::move(description),
			rtc::make_ref_counted<SetRemoteDescriptionObserver>(connection, SetRemoteDescriptionObserver::Action::kSetOffer));
		ok = 1;
	});
	return ok;
}

WEBRTC_EXPORT int libwebrtc_set_answer(struct libwebrtc_connection* connection, const char* sdp) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread || !sdp) {
		return 0;
	}

	int ok = 0;
	connection->ctx->signaling_thread->BlockingCall([&] {
		connection->remote_ufrag = ExtractIceUfrag(sdp);
		webrtc::SdpParseError error;
		std::unique_ptr<webrtc::SessionDescriptionInterface> description =
			webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);
		if(!description) {
			std::fprintf(stderr, "webrtc_native_linux: set-answer parse error at '%s': %s\n", connection->ctx->portal ? "-" : error.line.c_str(), connection->ctx->portal ? "-" : error.description.c_str());
			return;
		}

		connection->connection->SetRemoteDescription(
			std::move(description),
			rtc::make_ref_counted<SetRemoteDescriptionObserver>(connection, SetRemoteDescriptionObserver::Action::kSetAnswer));
		ok = 1;
	});
	return ok;
}

WEBRTC_EXPORT int libwebrtc_add_ice_candidate(struct libwebrtc_connection* connection, const char* candidate) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread || !candidate) {
		return 0;
	}

	int ok = 0;
	connection->ctx->signaling_thread->BlockingCall([&] {
		webrtc::SdpParseError error;
		std::unique_ptr<webrtc::IceCandidateInterface> ice(
			webrtc::CreateIceCandidate("0", 0, candidate, &error));
		if(!ice) {
			std::fprintf(stderr, "webrtc_native_linux: add-ice-candidate parse error at '%s': %s\n", connection->ctx->portal ? "-" : error.line.c_str(), connection->ctx->portal ? "-" : error.description.c_str());
			return;
		}

		const bool added = connection->connection->AddIceCandidate(ice.get());
		std::fprintf(stderr, "webrtc_native_linux: add-ice-candidate added=%d candidate=%s\n", added ? 1 : 0, connection->ctx->portal ? "-" : candidate);
		if(added) {
			ok = 1;
		}
	});
	return ok;
}

WEBRTC_EXPORT int libwebrtc_write(struct libwebrtc_data_channel* channel, const void* data, int len) {
    if (channel && channel->connection->ctx->portal) {
        const int result = PortalWrite(channel, data, len);
        return result > 0 ? len : result;
    }
	if(!channel || !data || len < 0) {
		return -1;
	}

	if(channel->zero_network_synthetic) {
		if(channel->close_notified || !channel->zero_network_peer || !channel->zero_network_peer->connection ||
		   !channel->zero_network_peer->connection->ctx || !channel->zero_network_peer->connection->ctx->callback) {
			return -1;
		}

		if(channel->zero_network_peer->connection->ctx->callback(
			channel->zero_network_peer->connection->ctx,
			channel->zero_network_peer->connection,
			channel->zero_network_peer,
			LWRTC_CALLBACK_CHANNEL_RECEIVE,
			channel->zero_network_peer->user_data,
			const_cast<void*>(data),
			len)) {
			channel->zero_network_peer->NotifySyntheticClose();
			channel->NotifySyntheticClose();
		}
		return len;
	}

	if(!channel->channel || channel->channel->state() != webrtc::DataChannelInterface::kOpen) {
		return -1;
	}

	rtc::CopyOnWriteBuffer buffer(static_cast<const uint8_t*>(data), (size_t)len);
	return channel->channel->Send(webrtc::DataBuffer(buffer, true)) ? len : -1;
}

WEBRTC_EXPORT void libwebrtc_close_channel(struct libwebrtc_data_channel* channel) {
	if(!channel) {
		return;
	}

	if(channel->zero_network_synthetic) {
		channel->NotifySyntheticClose();
		if(channel->zero_network_peer) {
			channel->zero_network_peer->NotifySyntheticClose();
		}
		return;
	}

	if(!channel->channel) {
		return;
	}

	channel->channel->Close();
}

WEBRTC_EXPORT void libwebrtc_close_connection(struct libwebrtc_connection* connection) {
	if(!connection || !connection->connection || !connection->ctx || !connection->ctx->signaling_thread) {
		return;
	}

	connection->ctx->signaling_thread->BlockingCall([&] {
        if (connection->ctx->portal) connection->portal_closing = true;
		connection->connection->Close();
        if (connection->ctx->portal) PortalWake(connection->ctx);
	});
}

WEBRTC_EXPORT const struct lwrtc_portal_api *libwebrtc_portal_api() {
    static const struct lwrtc_portal_api api = {1, kPortalCapabilities, PortalEnable, PortalWrite, PortalReclaim, PortalBuffered};
    return kPortalSdk ? &api : nullptr;
}

}
