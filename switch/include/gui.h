// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GUI_H
#define CHIAKI_GUI_H

#include <glad.h>
#include <GLFW/glfw3.h>
#include "nanovg.h"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

#include <atomic>
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
		std::atomic<bool> connected{false};

		// session quit handling + auto-reconnect (driven by Tick on the UI thread)
		enum class ReconnectState { Idle, WaitNetwork, WaitWake };
		static const int RECONNECT_MAX_ATTEMPTS = 12;
		static const uint64_t RECONNECT_NETWORK_TIMEOUT_MS = 180000;
		static const uint64_t RECONNECT_WAKE_DELAY_MS = 6000;
		HostTickTask *tick_task = nullptr;
		std::atomic<bool> pending_close{false};
		std::atomic<int> pending_reason{0};
		ReconnectState reconnect_state = ReconnectState::Idle;
		int reconnect_attempt = 0;
		uint64_t reconnect_deadline_ms = 0;
		uint64_t reconnect_next_ms = 0;
		bool reconnect_active = false;
		void HandleSessionClosed();
		void ScheduleReconnect(const char *why);
		void CancelReconnect();

	public:
		void Tick();
		HostInterface(Host *host);
		~HostInterface();

		static void Register(Host *host, std::function<void()> success_cb = nullptr);
		void Register();
		void Wakeup(brls::View *view);
		void Connect(brls::View *view);
		bool ConnectSession();
		void Disconnect();
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
