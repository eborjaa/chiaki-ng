// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GUI_H
#define CHIAKI_GUI_H

#include <glad.h>
#include <GLFW/glfw3.h>
#include "nanovg.h"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <map>
#include <thread>
#include <fmt/format.h>

#include <borealis.hpp>
#include "discoverymanager.h"
#include "host.h"
#include "io.h"
#include "settings.h"
#include "switch.h"
#include "views/enter_pin_view.h"
#include "views/ps_remote_play.h"

class HostInterface;

// Runs HostInterface::Tick on the UI thread (session callbacks come from
// chiaki's own threads and must not tear the session down themselves).
class HostTickTask : public brls::RepeatingTask
{
	private:
		HostInterface *host_interface;

	public:
		HostTickTask(HostInterface *hi) : brls::RepeatingTask(250), host_interface(hi) {}
		void run(retro_time_t currentTime) override;
};

class HostInterface : public brls::List
{
	private:
		IO *io;
		Host *host;
		Settings *settings;
		ChiakiLog *log = nullptr;

		// connection state machine, driven by Tick() on the UI thread.
		// chiaki/session callbacks only set atomics; the UI thread acts on them.
		enum class ConnState { Idle, Preparing, Starting, Streaming };
		ConnState conn_state = ConnState::Idle;
		bool stream_view_shown = false;
		HostTickTask *tick_task = nullptr;

		// "Connecting..." dialog
		brls::Dialog *connecting_dialog = nullptr;
		brls::Label *connecting_label = nullptr;
		std::mutex status_mutex;
		std::string status_text;
		int status_dots = 0;
		uint64_t status_dots_ms = 0;

		// preparation worker (network check, reachability probe, wake-up)
		std::thread prep_thread;
		std::atomic<bool> prep_done{false};
		std::atomic<bool> prep_ok{false};
		std::atomic<bool> cancel_requested{false};
		std::string prep_error;

		// session events (set from chiaki threads)
		std::atomic<bool> pending_connected{false};
		std::atomic<bool> pending_close{false};
		std::atomic<int> pending_reason{0};
		std::atomic<bool> pending_register_connect{false};
		// set once the stream view is really gone; the dialog is shown on the
		// next Tick so the view stack is never changed twice in one tick
		std::string pending_disconnect_msg;

		// the PS5 keeps a dropped session for a few seconds, so an immediate
		// reconnect is answered with "already in use"; retry instead of failing
		bool retry_active = false;
		uint64_t retry_at_ms = 0;
		uint64_t retry_deadline_ms = 0;
		static const uint64_t RETRY_DELAY_MS = 3000;
		static const uint64_t RETRY_WINDOW_MS = 30000;
		static bool RetryableQuit(ChiakiQuitReason reason);

		// stall watchdog: chiaki keeps a dead session alive after Wi-Fi loss
		static const uint64_t STALL_TIMEOUT_MS = 5000;
		uint64_t watch_start_ms = 0;
		uint64_t last_net_check_ms = 0;
		std::string watchdog_reason;

		void SetStatus(const std::string &text);
		void PrepareWorker();
		void StartSessionAfterPrep();
		void HandleSessionClosed();
		void Watchdog(uint64_t now);
		void CloseConnectingDialog(std::function<void(void)> then = nullptr);
		void ShowError(const std::string &title, const std::string &message);
		void ShowDisconnected(const std::string &message);
		static std::string QuitReasonMessage(ChiakiQuitReason reason, bool was_streaming);

	public:
		void Tick();
		HostInterface(Host *host);
		~HostInterface();

		static void Register(Host *host, std::function<void()> success_cb = nullptr);
		void Register();
		void Connect(brls::View *view);
		// pops the stream view, frees the session and only then runs `then`:
		// borealis animates (and deletes) the view asynchronously, so touching
		// the session or pushing another view in the same tick frees a view that
		// is still being drawn (crash on disconnect).
		void Disconnect(std::function<void()> then = nullptr);
		void Stream();
		void EnterPin(bool isError);
		void CloseStream(ChiakiQuitEvent *quit);
};

class MainApplication
{
	private:
		Settings *settings;
		ChiakiLog *log;
		DiscoveryManager *discoverymanager;
		IO *io;
		brls::TabFrame *rootFrame;
		std::map<Host *, HostInterface *> host_menuitems;

		bool BuildConfigurationMenu(brls::List *, Host *host = nullptr);
		void BuildAddHostConfigurationMenu(brls::List *);

	public:
		MainApplication(DiscoveryManager *discoverymanager);
		~MainApplication();
		bool Load();
};

#endif // CHIAKI_GUI_H
