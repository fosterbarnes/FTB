// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// Copyright (C) 2024 Michael Theall
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// FTB: upstream's ImGui draw and menus became status, settings, and log upload calls for FTB's UI.

#pragma once

#include "ftpConfig.h"
#include "ftpSession.h"
#include "platform.h"
#include "socket.h"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FtpServer;
using UniqueFtpServer = std::unique_ptr<FtpServer>;

/// \brief FTP server
class FtpServer
{
public:
	/// \brief Values edited on the settings screen
	struct Settings
	{
		std::string user;
		std::string pass;
		std::string hostname;
		std::uint16_t port;
		int deflateLevel;
		bool getMTime;
	};

	~FtpServer ();

	/// \brief Create server
	static UniqueFtpServer create ();

	/// \brief Listen address, empty while waiting for wifi
	std::string address ();

	/// \brief Snapshot every session; call once per frame
	std::vector<FtpSession::Status> sessions ();

	/// \brief Current settings
	Settings settings ();

	/// \brief Default settings
	static Settings defaultSettings ();

	/// \brief Apply settings and restart the listen socket; open sessions stay connected
	/// \param settings_ Settings to apply
	/// \param save_ Whether to also write the config file
	/// \returns false if saving failed
	bool applySettings (Settings const &settings_, bool save_);

	/// \brief Upload the log to pastie.io; the result is written to the log
	/// \returns false if an upload is already running
	bool uploadLog ();

	/// \brief Get free space
	static std::string getFreeSpace ();

	/// \brief Update free space
	static void updateFreeSpace ();

	/// \brief Server start time
	static std::time_t startTime ();

	/// \brief Get timezone offset in seconds (only used on 3DS)
	static int tzOffset ();

private:
	/// \brief Paramterized constructor
	/// \param config_ FTP config
	FtpServer (UniqueFtpConfig config_);

	/// \brief Handle when network is found
	void handleNetworkFound ();

	/// \brief Handle when network is lost
	void handleNetworkLost ();

	/// \brief Server loop
	void loop ();

	/// \brief Thread entry point
	void threadFunc ();

	/// \brief Thread
	platform::Thread m_thread;

	/// \brief Mutex
	platform::Mutex m_lock;

	/// \brief Config
	UniqueFtpConfig m_config;

	/// \brief Listen socket
	UniqueSocket m_socket;

	/// \brief mDNS socket
	UniqueSocket m_mdnsSocket;

	/// \brief Listen address
	std::string m_name;

	/// \brief Sessions
	std::vector<UniqueFtpSession> m_sessions;

	/// \brief Whether thread should quit
	std::atomic_bool m_quit = false;

	/// \brief Whether the server thread should reopen the listen socket (settings changed)
	std::atomic_bool m_restart = false;

	/// \brief Log upload cURL context
	CURLM *m_uploadLogCurlM = nullptr;
	/// \brief Log upload mime context
	curl_mime *m_uploadLogMime = nullptr;
	/// \brief Log upload cURL context
	std::atomic<CURL *> m_uploadLogCurl = nullptr;

	/// \brief Log upload data
	std::string m_uploadLogData;
	/// \brief Log upload result
	std::string m_uploadLogResult;
};
