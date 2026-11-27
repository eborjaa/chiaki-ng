// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "gui.h"
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chrono>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "views/enter_pin_view.h"
#include "views/ps_remote_play.h"



#define SCREEN_W 1280
#define SCREEN_H 720

// TODO
using namespace brls::i18n::literals; // for _i18n

#define DIALOG(dialog, r)                                                       \
	brls::Dialog *d_##dialog = new brls::Dialog(r);                             \
	brls::GenericEvent::Callback cb_##dialog = [d_##dialog](brls::View *view) { \
		d_##dialog->close();                                                    \
	};                                                                          \
	d_##dialog->addButton("Ok", cb_##dialog);                                   \
	d_##dialog->setCancelable(false);                                           \
	d_##dialog->open();                                                         \
	brls::Logger::info("Dialog: {0}", r);

HostInterface::HostInterface(Host *host)
	: host(host)
{
	this->settings = Settings::GetInstance();
	this->log = this->settings->GetLogger();
	this->io = IO::GetInstance();

	// Connect wakes the console up by itself when it is in rest mode
	brls::ListItem *connect = new brls::ListItem("Connect");
	connect->getClickEvent()->subscribe(std::bind(&HostInterface::Connect, this, std::placeholders::_1));
	this->addView(connect);

	// message delimiter
	brls::Label *info = new brls::Label(brls::LabelStyle::REGULAR,
		"Host configuration", true);
	this->addView(info);

	this->host->SetEventConnectedCallback(std::bind(&HostInterface::Stream, this));
	this->host->SetEventQuitCallback(std::bind(&HostInterface::CloseStream, this, std::placeholders::_1));
	// push login pin view onto the stack in callback fn
	this->host->SetEventLoginPinRequestCallback(std::bind(&HostInterface::EnterPin, this, std::placeholders::_1));
	// allow host to update controller state
	this->host->SetEventRumbleCallback(std::bind(&IO::SetRumble, this->io, std::placeholders::_1, std::placeholders::_2));
	this->host->SetReadControllerCallback(std::bind(&IO::UpdateControllerState, this->io, std::placeholders::_1, std::placeholders::_2));

	this->tick_task = new HostTickTask(this);
	this->tick_task->start();
}

HostInterface::~HostInterface()
{
	if(this->tick_task)
	{
		this->tick_task->stop();
		this->tick_task = nullptr; // owned and freed by borealis' task manager
	}
	this->cancel_requested = true;
	if(this->prep_thread.joinable())
		this->prep_thread.join();
	Disconnect();
}

void HostTickTask::run(retro_time_t currentTime)
{
	brls::RepeatingTask::run(currentTime);
	this->host_interface->Tick();
}

static uint64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool SwitchNetworkUp()
{
#ifdef __SWITCH__
	bool up = false;
	if(R_SUCCEEDED(nifmInitialize(NifmServiceType_User)))
	{
		NifmInternetConnectionType type;
		u32 strength = 0;
		NifmInternetConnectionStatus status;
		if(R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)))
			up = status == NifmInternetConnectionStatus_Connected;
		nifmExit();
	}
	return up;
#else
	return true;
#endif
}

// ---------------------------------------------------------------------------
// Reachability probe: the same discovery request the PS app sends. Works over
// the internet too when UDP 9302 (PS5) / 987 (PS4) is forwarded.
enum class ProbeResult { Ready, Standby, NoAnswer, ResolveFail };

static ProbeResult ProbeConsole(const std::string &addr, bool ps5, int timeout_ms, std::string *ip_out,
	const std::atomic<bool> *cancel = nullptr)
{
	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo *res = nullptr;
	if(getaddrinfo(addr.c_str(), nullptr, &hints, &res) != 0 || !res)
		return ProbeResult::ResolveFail;
	struct sockaddr_in sa;
	memcpy(&sa, res->ai_addr, sizeof(sa));
	freeaddrinfo(res);
	sa.sin_port = htons(ps5 ? 9302 : 987);
	if(ip_out)
	{
		char buf[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf));
		*ip_out = buf;
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(fd < 0)
		return ProbeResult::NoAnswer;
	const char *req = ps5
		? "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"
		: "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00020020\n";
	sendto(fd, req, strlen(req), 0, (struct sockaddr *)&sa, sizeof(sa));

	// poll in slices so a Cancel press is noticed within ~100 ms instead of
	// waiting out the whole timeout
	ProbeResult result = ProbeResult::NoAnswer;
	uint64_t deadline = NowMs() + (uint64_t)timeout_ms;
	while(true)
	{
		if(cancel && cancel->load())
			break;
		uint64_t left = deadline > NowMs() ? deadline - NowMs() : 0;
		if(left == 0)
			break;
		struct pollfd pfd = { fd, POLLIN, 0 };
		int slice = left > 100 ? 100 : (int)left;
		if(poll(&pfd, 1, slice) > 0 && (pfd.revents & POLLIN))
		{
			char buf[512] = {0};
			ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
			if(n > 0)
			{
				if(strstr(buf, " 200 "))
					result = ProbeResult::Ready;
				else if(strstr(buf, " 620 "))
					result = ProbeResult::Standby;
			}
			break;
		}
	}
	close(fd);
	return result;
}

// fallback when UDP discovery is filtered: is the Remote Play TCP port open?
static bool TcpPortOpen(const std::string &ip, int port, int timeout_ms,
	const std::atomic<bool> *cancel = nullptr)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
		return false;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	struct sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
	bool ok = false;
	if(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		ok = true;
	else if(errno == EINPROGRESS)
	{
		uint64_t deadline = NowMs() + (uint64_t)timeout_ms;
		while(true)
		{
			if(cancel && cancel->load())
				break;
			uint64_t left = deadline > NowMs() ? deadline - NowMs() : 0;
			if(left == 0)
				break;
			struct pollfd pfd = { fd, POLLOUT, 0 };
			int slice = left > 100 ? 100 : (int)left;
			if(poll(&pfd, 1, slice) > 0)
			{
				int err = 0;
				socklen_t len = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
				ok = err == 0;
				break;
			}
		}
	}
	close(fd);
	return ok;
}

// ---------------------------------------------------------------------------

void HostInterface::SetStatus(const std::string &text)
{
	std::lock_guard<std::mutex> lock(this->status_mutex);
	this->status_text = text;
}

void HostInterface::Connect(brls::View *view)
{
	if(this->conn_state != ConnState::Idle)
		return;

	if(!this->host->HasRPkey())
	{
		// first time: pair with the console (PIN), then connect
		if(!this->host->IsDiscovered())
		{
			ShowError("Can't pair", "The PlayStation must be on the same network as the Switch to pair it the first time.");
			return;
		}
		this->Register();
		return;
	}

	CHIAKI_LOGI(this->log, "Connect pressed for %s (target %s)",
		this->host->GetHostName().c_str(), this->host->GetConnectAddr().c_str());

	this->cancel_requested = false;
	this->retry_active = false;
	this->show_countdown = false;
	this->connect_deadline_ms = NowMs() + CONNECT_WINDOW_MS;
	this->watchdog_reason.clear();
	SetStatus("Checking network");

	// "Connecting..." dialog with a live status line
	this->connecting_label = new brls::Label(brls::LabelStyle::DIALOG, "", true);
	this->connecting_label->setHorizontalAlign(NVG_ALIGN_CENTER);
	this->connecting_dialog = new brls::Dialog(this->connecting_label);
	this->connecting_dialog->addButton("Cancel", [this](brls::View *v) {
		CHIAKI_LOGI(this->log, "Connect cancelled by user");
		this->cancel_requested = true;
		this->retry_active = false;
		this->show_countdown = false;
		if(this->conn_state == ConnState::Starting)
			this->host->StopSession();
		// close immediately; the worker and the session callbacks see the flag
		// and unwind on their own, Tick() puts the state back to Idle
		CloseConnectingDialog();
	});
	this->connecting_dialog->setCancelable(false);
	this->connecting_dialog->open();
	this->status_dots_ms = 0;

	StartPrep();
}

void HostInterface::StartPrep()
{
	this->conn_state = ConnState::Preparing;
	this->prep_done = false;
	this->prep_ok = false;
	this->prep_error.clear();
	if(this->prep_thread.joinable())
		this->prep_thread.join();
	this->prep_thread = std::thread(&HostInterface::PrepareWorker, this);
}

// false when Cancel was pressed meanwhile
bool HostInterface::SleepUnlessCancelled(uint64_t ms)
{
	uint64_t end = NowMs() + ms;
	while(!this->cancel_requested && NowMs() < end)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return !this->cancel_requested;
}

// runs on its own thread: everything here may block
void HostInterface::PrepareWorker()
{
	std::string name = this->host->GetHostName();
	std::string addr = this->host->GetConnectAddr();
	bool ps5 = this->host->IsPS5();
	bool local = this->host->IsDiscovered();
	auto fail = [this](const std::string &msg) {
		this->prep_error = msg;
		this->prep_ok = false;
		this->prep_done = true;
	};

	if(!SwitchNetworkUp())
		return fail("No internet connection.\nThe Switch isn't connected to a network.");
	if(addr.empty())
		return fail("No address for this PlayStation.\nSet \"Remote IP/name\" in its settings.");

	// Keep trying until the connect window closes. Every attempt resolves the
	// name again, so a new home IP is picked up as soon as DDNS has it.
	std::string ip, first_ip;
	ProbeResult probe = ProbeResult::NoAnswer;
	bool ip_changed = false;
	int attempt = 0;
	while(true)
	{
		attempt++;
		uint64_t attempt_start = NowMs();
		if(!SwitchNetworkUp())
			return fail("No internet connection.\nThe Switch isn't connected to a network.");
		if(attempt == 1)
			SetStatus(local ? "Looking for your PS5 on this network" : "Looking for your PS5");

		ip.clear();
		probe = ProbeConsole(addr, ps5, 1500, &ip, &this->cancel_requested);
		if(probe == ProbeResult::NoAnswer && !this->cancel_requested)
			probe = ProbeConsole(addr, ps5, 2000, &ip, &this->cancel_requested);
		if(probe == ProbeResult::NoAnswer && !this->cancel_requested && !ip.empty()
			&& TcpPortOpen(ip, 9295, 3000, &this->cancel_requested))
		{
			// UDP discovery may be filtered while the session port still works
			CHIAKI_LOGI(this->log, "Discovery unanswered but TCP 9295 is open, trying anyway");
			probe = ProbeResult::Ready;
		}
		if(this->cancel_requested)
			return fail("");
		if(!ip.empty())
		{
			if(first_ip.empty())
				first_ip = ip;
			else if(ip != first_ip && !ip_changed)
			{
				ip_changed = true;
				CHIAKI_LOGI(this->log, "%s now resolves to a new address (%s, was %s)",
					addr.c_str(), ip.c_str(), first_ip.c_str());
			}
		}
		CHIAKI_LOGI(this->log, "Probe %d %s (%s): %s", attempt, addr.c_str(), ip.c_str(),
			probe == ProbeResult::Ready ? "ready" : probe == ProbeResult::Standby ? "rest mode" :
			probe == ProbeResult::ResolveFail ? "name not found" : "no answer");

		if(probe == ProbeResult::Ready || probe == ProbeResult::Standby)
			break;

		uint64_t deadline = this->connect_deadline_ms;
		uint64_t next = attempt_start + ATTEMPT_INTERVAL_MS;
		if(next < NowMs())
			next = NowMs();
		if(next >= deadline)
		{
			this->show_countdown = false;
			if(probe == ProbeResult::ResolveFail)
				return fail("Can't find \"" + addr + "\".\n"
					"Check \"Remote IP/name\" in the PS5's settings, or this network's internet connection.");
			return fail("Your PS5 didn't answer.\n"
				"It may be fully turned off (not in rest mode), your home internet may be down,\n"
				"or your home address hasn't updated yet. Try again in a minute.");
		}
		if(probe == ProbeResult::ResolveFail)
			SetStatus("Can't find \"" + addr + "\" yet. Still trying");
		else
			SetStatus("Can't reach your PS5 yet. Your home internet address may have changed.\nStill trying");
		this->show_countdown = true;
		if(!SleepUnlessCancelled(next - NowMs()))
			return fail("");
	}
	this->show_countdown = false;

	if(probe == ProbeResult::Standby)
	{
		SetStatus("Waking your PS5");
		uint64_t deadline = NowMs() + 45000;
		uint64_t next_wake = 0;
		bool awake = false;
		while(!this->cancel_requested && NowMs() < deadline)
		{
			if(NowMs() >= next_wake)
			{
				this->host->Wakeup();
				next_wake = NowMs() + 10000;
			}
			if(ProbeConsole(addr, ps5, 2000, nullptr, &this->cancel_requested) == ProbeResult::Ready)
			{
				awake = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		if(this->cancel_requested)
			return fail("");
		if(!awake)
			return fail("Your PS5 didn't wake up.\nOn the PS5, enable Settings > System > Power Saving > "
				"Features Available in Rest Mode > Enable Turning On PS5 from Network.");
		// Remote Play refuses connections for a moment right after waking
		SetStatus("PS5 is awake, starting Remote Play");
		for(int i = 0; i < 6 && !this->cancel_requested; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	this->prep_ok = true;
	this->prep_done = true;
}

void HostInterface::StartSessionAfterPrep()
{
	SetStatus("Starting Remote Play");
	this->conn_state = ConnState::Starting;
	try
	{
		this->host->InitSession(this->io);
		this->host->StartSession();
	}
	catch(const std::exception &e)
	{
		CHIAKI_LOGE(this->log, "Connect failed: %s", e.what());
		this->host->FiniSession();
		this->conn_state = ConnState::Idle;
		CloseConnectingDialog([this, e]() {
			ShowError("Couldn't connect", std::string("Internal error: ") + e.what());
		});
	}
}

void HostInterface::Tick()
{
	// UI thread
	uint64_t now = NowMs();

	if(this->pending_register_connect.exchange(false))
		Connect(nullptr);

	if(!this->pending_disconnect_msg.empty())
	{
		std::string msg = this->pending_disconnect_msg;
		this->pending_disconnect_msg.clear();
		ShowDisconnected(msg);
	}

	// animate the connecting dialog
	if(this->connecting_label && now - this->status_dots_ms >= 400)
	{
		this->status_dots_ms = now;
		this->status_dots = (this->status_dots + 1) % 4;
		std::string text;
		{
			std::lock_guard<std::mutex> lock(this->status_mutex);
			text = this->status_text;
		}
		std::string dots = std::string(this->status_dots, '.') + std::string(3 - this->status_dots, ' ');
		if(this->show_countdown)
		{
			uint64_t deadline = this->connect_deadline_ms;
			uint64_t left = deadline > now ? (deadline - now + 999) / 1000 : 0;
			dots += fmt::format(" ({}:{:02d} left)", left / 60, left % 60);
		}
		this->connecting_label->setText("Connecting to " + this->host->GetHostName() + "\n\n" + text + dots);
	}

	// session refused (busy / still waking): try again within the same window,
	// running the whole preparation again so the address is looked up afresh
	if(this->retry_active && this->conn_state == ConnState::Idle && now >= this->retry_at_ms)
	{
		this->retry_active = false;
		this->show_countdown = false;
		CHIAKI_LOGI(this->log, "Retrying connection");
		StartPrep();
	}

	if(this->conn_state == ConnState::Preparing && this->prep_done)
	{
		if(this->prep_thread.joinable())
			this->prep_thread.join();
		if(this->cancel_requested || !this->prep_ok)
		{
			this->conn_state = ConnState::Idle;
			std::string err = this->prep_error;
			CloseConnectingDialog([this, err]() {
				if(!err.empty())
					ShowError("Couldn't connect", err);
			});
			if(!err.empty())
				CHIAKI_LOGW(this->log, "Connect aborted: %s", err.c_str());
		}
		else
			StartSessionAfterPrep();
	}

	if(this->pending_connected.exchange(false))
	{
		if(this->cancel_requested)
		{
			// the user cancelled while the session was coming up: do not push
			// the stream view, just tear the attempt down
			CHIAKI_LOGI(this->log, "Connected after cancel, stopping the session");
			this->host->StopSession();
			this->conn_state = ConnState::Idle;
		}
		else if(this->conn_state == ConnState::Starting)
		{
			this->conn_state = ConnState::Streaming;
			this->retry_active = false;
			this->show_countdown = false;
			this->watch_start_ms = 0;
			CloseConnectingDialog([this]() {
				// https://github.com/natinusala/borealis/issues/59
				// disable 60 fps limit
				brls::Application::setMaximumFPS(0);
				brls::Application::pushView(new PSRemotePlay(this->host));
				this->stream_view_shown = true;
			});
		}
	}

	if(this->pending_close.exchange(false))
		HandleSessionClosed();

	Watchdog(now);
}

void HostInterface::Watchdog(uint64_t now)
{
	if(this->conn_state != ConnState::Streaming || !this->watchdog_reason.empty())
		return;
	if(this->watch_start_ms == 0)
		this->watch_start_ms = now;

	uint64_t last_video = IO::LastVideoMs();
	if(last_video < this->watch_start_ms)
		last_video = this->watch_start_ms; // grace period from stream start
	bool stalled = now - last_video > STALL_TIMEOUT_MS;

	bool net_down = false;
	if(now - this->last_net_check_ms >= 1000)
	{
		this->last_net_check_ms = now;
		net_down = !SwitchNetworkUp();
	}

	if(stalled || net_down)
	{
		// chiaki doesn't end the session on its own when the network is gone
		this->watchdog_reason = net_down
			? "The Switch lost its network connection."
			: "The PS5 stopped sending video (connection lost).";
		CHIAKI_LOGW(this->log, "Watchdog: %s Stopping session.", this->watchdog_reason.c_str());
		this->host->StopSession();
	}
}

std::string HostInterface::QuitReasonMessage(ChiakiQuitReason reason, bool was_streaming)
{
	switch(reason)
	{
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN:
			return "Your PS5 didn't answer the Remote Play request.\n"
				"This network may block Remote Play, or the PS5 is still starting up.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
			return "Your PS5 refused the connection.\nIt may still be waking up, try again in a few seconds.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
			return "Remote Play is already in use by another device.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
			return "Remote Play crashed on the PS5.\nRestart the PS5 and try again.";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
			return "This PS5's Remote Play version isn't supported.";
		case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED:
		case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
		case CHIAKI_QUIT_REASON_CTRL_UNKNOWN:
			return was_streaming ? "The connection to your PS5 was lost."
				: "Couldn't open the control connection to your PS5.";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN:
			return was_streaming ? "The connection to your PS5 was lost."
				: "The video stream couldn't be set up.\nThis network may block Remote Play video (UDP).";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED:
			return "Your PS5 ended the Remote Play session.";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
			return "Your PS5 was turned off or went into rest mode.";
		case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
			return "PSN registration failed.";
		default:
			return chiaki_quit_reason_string(reason);
	}
}

bool HostInterface::RetryableQuit(ChiakiQuitReason reason)
{
	// the console keeps the old session for a moment after the link drops and
	// then rejects the next request; waking it takes a moment too
	switch(reason)
	{
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
			return true;
		default:
			return false;
	}
}

void HostInterface::HandleSessionClosed()
{
	ConnState state = this->conn_state;
	ChiakiQuitReason reason = (ChiakiQuitReason)this->pending_reason.load();
	bool cancelled = this->cancel_requested.exchange(false);
	std::string watchdog = this->watchdog_reason;
	this->watchdog_reason.clear();
	this->watch_start_ms = 0;
	this->conn_state = ConnState::Idle;

	brls::Application::setMaximumFPS(60);

	if(state == ConnState::Streaming)
	{
		std::string msg = !watchdog.empty() ? watchdog : QuitReasonMessage(reason, true);
		CHIAKI_LOGI(this->log, "Disconnected: %s", msg.c_str());
		// the stream view is still on screen (and drawn during its fade-out),
		// so the session is freed once borealis has deleted it, and the dialog
		// is shown on the next tick
		Disconnect([this, msg]() { this->pending_disconnect_msg = msg; });
	}
	else if(state == ConnState::Starting)
	{
		uint64_t now = NowMs();
		bool retry = !cancelled && RetryableQuit(reason)
			&& now + ATTEMPT_INTERVAL_MS < this->connect_deadline_ms;
		std::string msg = QuitReasonMessage(reason, false);
		Disconnect();
		if(retry)
		{
			this->retry_at_ms = now + ATTEMPT_INTERVAL_MS;
			this->retry_active = true;
			this->show_countdown = true;
			SetStatus(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE
				? "PS5 is finishing the last session" : "PS5 isn't ready yet");
			CHIAKI_LOGI(this->log, "Session refused (%s), retrying in %llu s",
				chiaki_quit_reason_string(reason), (unsigned long long)(ATTEMPT_INTERVAL_MS / 1000));
			return;
		}
		this->retry_active = false;
		this->show_countdown = false;
		CloseConnectingDialog([this, msg, cancelled]() {
			if(!cancelled)
				ShowError("Couldn't connect", msg);
		});
	}
	else
		Disconnect();
}

void HostInterface::CloseConnectingDialog(std::function<void(void)> then)
{
	if(this->connecting_dialog)
	{
		brls::Dialog *d = this->connecting_dialog;
		this->connecting_dialog = nullptr;
		this->connecting_label = nullptr;
		d->close(then);
	}
	else if(then)
		then();
}

void HostInterface::ShowError(const std::string &title, const std::string &message)
{
	brls::Dialog *d = new brls::Dialog(title + "\n\n" + message);
	d->addButton("OK", [d](brls::View *v) { d->close(); });
	d->setCancelable(true);
	d->open();
}

void HostInterface::ShowDisconnected(const std::string &message)
{
	brls::Dialog *d = new brls::Dialog("Disconnected from " + this->host->GetHostName() + "\n\n" + message);
	d->addButton("Reconnect", [this, d](brls::View *v) {
		d->close([this]() { this->Connect(nullptr); });
	});
	d->addButton("Close", [d](brls::View *v) { d->close(); });
	d->setCancelable(true);
	d->open();
}

void HostInterface::Register(Host *host, std::function<void()> success_cb)
{
	Settings *settings = Settings::GetInstance();
	IO *io = IO::GetInstance();

	// user must provide psn id for registration
	std::string account_id = settings->GetPSNAccountID(host);
	std::string online_id = settings->GetPSNOnlineID(host);
	ChiakiTarget target = host->GetChiakiTarget();

	if(target >= CHIAKI_TARGET_PS4_9 && account_id.length() <= 0)
	{
		// PS4 firmware > 7.0
		DIALOG(upaid, "Undefined PSN Account ID (Please configure a valid psn_account_id)");
		return;
	}
	else if(target < CHIAKI_TARGET_PS4_9 && online_id.length() <= 0)
	{
		// use oline ID for ps4 < 7.0
		DIALOG(upoid, "Undefined PSN Online ID (Please configure a valid psn_online_id)");
		return;
	}

	// add HostConnected function to regist_event_type_finished_success
	auto event_type_finished_success_cb = [settings, success_cb]() {
		// save RP keys
		settings->WriteFile();
		if(success_cb != nullptr)
		{
			// FIXME: may raise a connection refused
			// when the connection is triggered
			// just after the register success
			sleep(2);
			success_cb();
		}
		// decrement block input token number
		brls::Application::unblockInputs();
	};
	host->SetRegistEventTypeFinishedSuccess(event_type_finished_success_cb);

	auto event_type_finished_failed_cb = []() {
		// unlock user inputs
		brls::Application::unblockInputs();
		brls::Application::notify("Registration failed");
	};
	host->SetRegistEventTypeFinishedFailed(event_type_finished_failed_cb);

	// the host is not registered yet
	// use callback to ensure that the message is showed on screen
	// before the Swkbd
	auto pin_input_cb = [host](int pin) {
		// prevent users form messing with the gui
		brls::Application::blockInputs();
		int ret = host->Register(pin);
		if(ret != HOST_REGISTER_OK)
		{
			switch(ret)
			{
				// account not configured
				case HOST_REGISTER_ERROR_SETTING_PSNACCOUNTID:
					brls::Application::notify("No PSN Account ID provided");
					brls::Application::unblockInputs();
					break;
				case HOST_REGISTER_ERROR_SETTING_PSNONLINEID:
					brls::Application::notify("No PSN Online ID provided");
					brls::Application::unblockInputs();
					break;
			}
		}
	};
	// the pin is 8 digit
	bool success = brls::Swkbd::openForNumber(pin_input_cb,
		"Please enter your PlayStation registration PIN code", "8 digits without spaces", 8, "", "", "");
}

void HostInterface::Register()
{
	// connect right after a successful registration; the callback runs on a
	// chiaki thread, so hand it to the UI thread
	HostInterface::Register(this->host, [this]() { this->pending_register_connect = true; });
}

void HostInterface::Disconnect(std::function<void()> then)
{
	if(this->conn_state == ConnState::Streaming)
		this->conn_state = ConnState::Idle;
	this->host->StopSession();
	if(this->stream_view_shown && then)
	{
		this->stream_view_shown = false;
		// popView() deletes the view when the fade ends; freeing the session
		// before that leaves the view drawing freed video buffers
		brls::Application::popView(brls::ViewAnimation::FADE, [this, then]() {
			this->host->FiniSession();
			then();
		});
		return;
	}
	if(this->stream_view_shown)
	{
		// teardown path (destructor): no continuation to capture
		this->stream_view_shown = false;
		brls::Application::popView();
	}
	this->host->FiniSession();
}

void HostInterface::Stream()
{
	// chiaki session thread: the UI thread pushes the stream view
	this->pending_connected = true;
}

void HostInterface::CloseStream(ChiakiQuitEvent *quit)
{
	// chiaki session thread: only record it; Tick() (UI thread) cleans up
	CHIAKI_LOGI(this->log, "Session quit: %s%s%s", chiaki_quit_reason_string(quit->reason),
		quit->reason_str ? " - " : "", quit->reason_str ? quit->reason_str : "");
	this->pending_reason = (int)quit->reason;
	this->pending_close = true;
}

void HostInterface::EnterPin(bool isError) 
{
	// enter pin callback,
	// inputs were blocked in ConnectSession
	brls::Application::unblockInputs();
	// if this is triggered as a result 
	if(isError){
		brls::Application::notify("Wrong PIN");
	}
	
	brls::Application::pushView(new EnterPinView(this->host, isError));
}

MainApplication::MainApplication(DiscoveryManager *discoverymanager)
	: discoverymanager(discoverymanager)
{
	this->settings = Settings::GetInstance();
	this->log = this->settings->GetLogger();
	this->io = IO::GetInstance();
}

MainApplication::~MainApplication()
{
	this->discoverymanager->SetService(false);
	this->io->FreeController();
	this->io->FreeVideo();
}

bool MainApplication::Load()
{
	this->discoverymanager->SetService(true);
	// Init the app
	brls::Logger::setLogLevel(brls::LogLevel::DEBUG);

	brls::i18n::loadTranslations();
	if(!brls::Application::init("Chiaki Remote play"))
	{
		brls::Logger::error("Unable to init Borealis application");
		return false;
	}

	// init chiaki gl after borealis
	// let borealis manage the main screen/window
	// if(!io->InitVideo(0, 0, SCREEN_W, SCREEN_H))
	// {
	// 	brls::Logger::error("Failed to initiate Video");
	// }

	brls::Logger::info("Load SDL/HiD controller");
	if(!io->InitController())
	{
		brls::Logger::error("Failed to initiate Controller");
	}

	// Create a view
	this->rootFrame = new brls::TabFrame();
	this->rootFrame->setTitle("Chiaki-ng: Open Source PlayStation Remote Play Client");
	this->rootFrame->setIcon(BOREALIS_ASSET("icon.png"));

	brls::List *config = new brls::List();
	brls::List *add_host = new brls::List();
	BuildConfigurationMenu(config);
	BuildAddHostConfigurationMenu(add_host);

	std::map<std::string, Host> *hosts = this->settings->GetHostsMap();
	for (auto it = hosts->begin(); it != hosts->end(); ++it)
	{
		if (this->host_menuitems.find(&it->second) == this->host_menuitems.end() &&
		    (it->second.HasRPkey() || it->second.IsDiscovered()))
		{
		HostInterface *new_host = new HostInterface(&it->second);
		this->host_menuitems[&it->second] = new_host;
		BuildConfigurationMenu(new_host, &it->second);
		this->rootFrame->addTab(it->second.GetHostName().c_str(), new_host);
		}
	}

	// Static tabs under the registered targets
	this->rootFrame->addSeparator();
	this->rootFrame->addTab("Configuration", config);
	this->rootFrame->addTab("Add Manual Host", add_host);

	brls::Application::pushView(this->rootFrame);

	while (brls::Application::mainLoop()) {
	}
	
	return true;
}

bool MainApplication::BuildConfigurationMenu(brls::List *ls, Host *host)
{
	std::string psn_account_id_string = this->settings->GetPSNAccountID(host);
	brls::InputListItem *psn_account_id = new brls::InputListItem("PSN Account ID", psn_account_id_string,
		"Account ID in base64 format", "PS5 or PS4 System Software version 7.00 and greater", CHIAKI_PSN_ACCOUNT_ID_SIZE * 2,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);

	auto psn_account_id_cb = [this, host, psn_account_id](brls::View *view) {
		// retrieve, push and save setting
		this->settings->SetPSNAccountID(host, psn_account_id->getValue());
		// write on disk
		this->settings->WriteFile();
	};
	psn_account_id->getClickEvent()->subscribe(psn_account_id_cb);
	ls->addView(psn_account_id);

	
	brls::InputListItem *lookup_account_id = new brls::InputListItem("Lookup Account ID by username",
		"", "PSN Account name", "Only works if the PSN account's privacy settings are set to public!", 16,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);

	auto lookup_account_id_cb = [this, host, lookup_account_id, psn_account_id](brls::View *view) {
		// lookup psn_online
		std::string entered_value = lookup_account_id->getValue();
		this->discoverymanager->makeRequest(entered_value, 
			 [this, host, lookup_account_id, psn_account_id](const std::string& accountId) {
				// retrieve, push and save setting
				CHIAKI_LOGI(this->log, fmt::format("Found account id {}", accountId).c_str());
				this->settings->SetPSNAccountID(host, std::string(accountId));
				// write on disk
				this->settings->WriteFile();
				lookup_account_id->setValue("");
				// set the view of the other field,
				psn_account_id->setValue(std::string(accountId));
			},
			[this, lookup_account_id, entered_value](const std::string& error) {
				CHIAKI_LOGE(this->log, error.c_str());
				lookup_account_id->setValue("");
				DIALOG(upaid, fmt::format("Unable to fetch account id for {}", entered_value));
			}
		);
	};
	lookup_account_id->getClickEvent()->subscribe(lookup_account_id_cb);
	ls->addView(lookup_account_id);


	std::string psn_online_id_string = this->settings->GetPSNOnlineID(host);
	brls::InputListItem *psn_online_id = new brls::InputListItem("PSN Online ID",
		psn_online_id_string, "", "", 16,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);

	auto psn_online_id_cb = [this, host, psn_online_id](brls::View *view) {
		// retrieve, push and save setting
		this->settings->SetPSNOnlineID(host, psn_online_id->getValue());
		// write on disk
		this->settings->WriteFile();
	};
	psn_online_id->getClickEvent()->subscribe(psn_online_id_cb);
	ls->addView(psn_online_id);


	int value;
	ChiakiVideoResolutionPreset resolution_preset = this->settings->GetVideoResolution(host);
	switch(resolution_preset)
	{
		case CHIAKI_VIDEO_RESOLUTION_PRESET_1080p:
			value = 0;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_720p:
			value = 1;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_540p:
			value = 2;
			break;
		case CHIAKI_VIDEO_RESOLUTION_PRESET_360p:
			value = 3;
			break;
	}

	brls::SelectListItem *resolution = new brls::SelectListItem(
		"Resolution", { "1080p (PS5 and PS4 Pro only)", "720p", "540p", "360p" }, value);

	auto resolution_cb = [this, host](int result) {
		ChiakiVideoResolutionPreset value = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		switch(result)
		{
			case 0:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
				break;
			case 1:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
				break;
			case 2:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
				break;
			case 3:
				value = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
				break;
		}
		this->settings->SetVideoResolution(host, value);
		this->settings->WriteFile();
	};
	resolution->getValueSelectedEvent()->subscribe(resolution_cb);
	ls->addView(resolution);

	ChiakiVideoFPSPreset fps_preset = this->settings->GetVideoFPS(host);
	switch(fps_preset)
	{
		case CHIAKI_VIDEO_FPS_PRESET_60:
			value = 0;
			break;
		case CHIAKI_VIDEO_FPS_PRESET_30:
			value = 1;
			break;
	}

	brls::SelectListItem *fps = new brls::SelectListItem(
		"FPS", { "60fps", "30fps" }, value);

	auto fps_cb = [this, host](int result) {
		ChiakiVideoFPSPreset value = CHIAKI_VIDEO_FPS_PRESET_60;
		switch(result)
		{
			case 0:
				value = CHIAKI_VIDEO_FPS_PRESET_60;
				break;
			case 1:
				value = CHIAKI_VIDEO_FPS_PRESET_30;
				break;
		}
		this->settings->SetVideoFPS(host, value);
		this->settings->WriteFile();
	};

	fps->getValueSelectedEvent()->subscribe(fps_cb);
	ls->addView(fps);

	value = this->settings->GetHaptic(host);

	brls::SelectListItem *haptic = new brls::SelectListItem(
		"Haptics", { "Disabled", "Weak", "Strong" }, value);

	auto haptic_cb = [this, host](int result) {
		HapticPreset value = HAPTIC_PRESET_DIABLED;
		switch(result)
		{
			case 0:
				value = HAPTIC_PRESET_DIABLED;
				break;
			case 1:
				value = HAPTIC_PRESET_WEAK;
				break;
			case 2:
				value = HAPTIC_PRESET_STRONG;
				break;
		}
		this->settings->SetHaptic(host, value);
		this->settings->WriteFile();
	};

	haptic->getValueSelectedEvent()->subscribe(haptic_cb);
	ls->addView(haptic);

	if(host != nullptr)
	{	
	    brls::Label *info = new brls::Label(brls::LabelStyle::REGULAR,
		"Host information", true);
	    ls->addView(info);
	
	    std::string host_name_string = this->settings->GetHostName(host);
	    brls::ListItem *host_name = new brls::ListItem("PS Hostname");
	    host_name->setValue(host_name_string.c_str());
	    ls->addView(host_name);
	
	    std::string host_addr_string = settings->GetHostAddr(host);		
		brls::InputListItem *host_addr = new brls::InputListItem("Remote IP/name",
		host_addr_string, "IP address or fqdn", "", 255,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);		
		
	    auto host_addr_cb = [this, host, host_addr](brls::View *view)
		{
			std::string new_addr = host_addr->getValue();	
			this->settings->SetHostAddr(host, new_addr);	
			this->settings->WriteFile();		
			brls::Application::notify("PS Address updated successfully");
	    };
	
	    host_addr->getClickEvent()->subscribe(host_addr_cb);
	    ls->addView(host_addr);

		brls::ListItem *host_regist_state_item = new brls::ListItem("Register Status");
		host_regist_state_item->setValue(!settings->GetHostRPKey(host).empty() ? "registered" : "unregistered");
		ls->addView(host_regist_state_item);
	}

	return true;
}

void MainApplication::BuildAddHostConfigurationMenu(brls::List *add_host)
{
	// create host for wan connection
	// brls::Label* add_host_label = new brls::Label(brls::LabelStyle::REGULAR,
	// 	"Add Host configuration", true);

	brls::InputListItem *display_name = new brls::InputListItem("Display name",
		"default", "configuration name", "", 16,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);

	add_host->addView(display_name);

	brls::InputListItem *address = new brls::InputListItem("Remote IP/name",
		"", "IP address or fqdn", "", 255,
		brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH |
			brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH);

	add_host->addView(address);

	// TODO
	// brls::ListItem* port = new brls::ListItem("Remote session port",  "tcp/udp 9295");
	// brls::ListItem* port = new brls::ListItem("Remote stream port",  "udp 9296");
	// brls::ListItem* port = new brls::ListItem("Remote Senkusha port",  "udp 9297");
	brls::SelectListItem *ps_version = new brls::SelectListItem("Target PlayStation System",
		{ "PS5", "PS4 System Version > 8.00", "7.00 < PS4 System Version < 8.00", "PS4 System Version < 7.00" }); //This may look like a pointless change but it does actually make things a bit clearer for the user
	add_host->addView(ps_version);

	brls::ListItem *register_host = new brls::ListItem("Register");
	auto register_host_cb = [this, display_name, address, ps_version](brls::View *view) {
		bool err = false;
		std::string dn = display_name->getValue();
		std::string addr = address->getValue();
		ChiakiTarget version = CHIAKI_TARGET_PS4_UNKNOWN;

		switch(ps_version->getSelectedValue())
		{
			case 0:
				// ps5 v1
				version = CHIAKI_TARGET_PS5_1;
				break;
			case 1:
				// ps4 v8
				version = CHIAKI_TARGET_PS4_10;
				break;
			case 2:
				// ps4 v7
				version = CHIAKI_TARGET_PS4_9;
				break;
			case 3:
				// ps4 v6
				version = CHIAKI_TARGET_PS4_8;
				break;
		}

		if(dn.length() <= 0)
		{
			brls::Application::notify("No Display name defined");
			err = true;
		}

		if(addr.length() <= 0)
		{
			brls::Application::notify("No Remote address provided");
			err = true;
		}

		if(version <= CHIAKI_TARGET_PS4_UNKNOWN)
		{
			brls::Application::notify("No PlayStation Version provided");
			err = true;
		}

		if(err)
			return;

		Host *host = this->settings->GetOrCreateHost(&dn);
		host->SetHostAddr(addr);
		host->SetChiakiTarget(version);
		HostInterface::Register(host);
	};
	register_host->getClickEvent()->subscribe(register_host_cb);

	add_host->addView(register_host);
}

