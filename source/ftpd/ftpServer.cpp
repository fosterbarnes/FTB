// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// Copyright (C) 2026 Michael Theall
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

#include "ftpServer.h"

#include "fs.h"
#include "ftpConfig.h"
#include "ftpSession.h"
#include "log.h"
#include "mdns.h"
#include "platform.h"
#include "sockAddr.h"
#include "socket.h"

#include <jansson.h>

#include <sys/statvfs.h>
using statvfs_t = struct statvfs;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
using namespace std::chrono_literals;

#define LOCKED(x)                                                                                  \
	do                                                                                             \
	{                                                                                              \
		auto const lock = std::scoped_lock (m_lock);                                               \
		x;                                                                                         \
	} while (0)

namespace
{
/// \brief Application start time
auto const s_startTime = std::time (nullptr);

/// \brief Timezone offset in seconds (only used on 3DS)
int s_tzOffset = 0;

/// \brief Mutex for s_freeSpace
platform::Mutex s_lock;

/// \brief Free space string
std::string s_freeSpace;

std::size_t curlCallback (void *const contents_,
    std::size_t const size_,
    std::size_t const count_,
    void *const user_)
{
	auto const total = size_ * count_;
	auto const start = static_cast<char *> (contents_);
	auto const end   = start + total;

	auto &result = *static_cast<std::string *> (user_);
	result.insert (std::end (result), start, end);

	return total;
}
}

///////////////////////////////////////////////////////////////////////////
FtpServer::~FtpServer ()
{
	m_quit = true;

	m_thread.join ();

	if (m_uploadLogCurl)
	{
		curl_multi_remove_handle (m_uploadLogCurlM, m_uploadLogCurl);
		curl_easy_cleanup (m_uploadLogCurl);
		curl_mime_free (m_uploadLogMime);
	}

	if (m_uploadLogCurlM)
		curl_multi_cleanup (m_uploadLogCurlM);
}

FtpServer::FtpServer (UniqueFtpConfig config_) : m_config (std::move (config_))
{
	mdns::setHostname (m_config->hostname ());

	s64 tzOffsetMinutes;
	if (R_SUCCEEDED (svcGetSystemInfo (&tzOffsetMinutes, 0x10000, 0x103)))
		s_tzOffset = tzOffsetMinutes * 60;

	m_thread = platform::Thread (std::bind (&FtpServer::threadFunc, this));
}

UniqueFtpServer FtpServer::create ()
{
	updateFreeSpace ();

	auto config = FtpConfig::load (FTPDCONFIG);

	return UniqueFtpServer (new FtpServer (std::move (config)));
}

std::string FtpServer::address ()
{
	auto const lock = std::scoped_lock (m_lock);
	return m_socket ? m_name : std::string ();
}

std::vector<FtpSession::Status> FtpServer::sessions ()
{
	auto const lock = std::scoped_lock (m_lock);

	std::vector<FtpSession::Status> result;
	result.reserve (m_sessions.size ());
	for (auto &session : m_sessions)
		result.emplace_back (session->status ());

	return result;
}

FtpServer::Settings FtpServer::settings ()
{
	auto const lock = m_config->lockGuard ();
	return {m_config->user (),
	    m_config->pass (),
	    m_config->hostname (),
	    m_config->port (),
	    m_config->deflateLevel (),
	    m_config->getMTime ()};
}

FtpServer::Settings FtpServer::defaultSettings ()
{
	auto const defaults = FtpConfig::create ();
	return {defaults->user (),
	    defaults->pass (),
	    defaults->hostname (),
	    defaults->port (),
	    defaults->deflateLevel (),
	    defaults->getMTime ()};
}

bool FtpServer::applySettings (Settings const &settings_, bool const save_)
{
	bool saved = true;

	{
		auto const lock = m_config->lockGuard ();

		m_config->setUser (settings_.user);
		m_config->setPass (settings_.pass);
		m_config->setHostname (settings_.hostname);
		m_config->setPort (settings_.port);
		m_config->setDeflateLevel (settings_.deflateLevel);
		m_config->setGetMTime (settings_.getMTime);

		if (save_ && !m_config->save (FTPDCONFIG))
		{
			error ("Failed to save config\n");
			saved = false;
		}
	}

	// FTB: upstream dropped m_socket and set the mDNS name here on the UI thread while the server
	// thread was using both; loop () now does it
	m_restart = true;

	return saved;
}

bool FtpServer::uploadLog ()
{
	auto const lock = std::scoped_lock (m_lock);

	if (!m_uploadLogCurlM)
		m_uploadLogCurlM = curl_multi_init ();

	if (!m_uploadLogCurlM || m_uploadLogCurl.load (std::memory_order_relaxed))
		return false;

	m_uploadLogData = getLog ();

	auto const handle = curl_easy_init ();
	if (!handle)
		return false;

	// write result into string
	m_uploadLogResult.clear ();
	curl_easy_setopt (handle, CURLOPT_WRITEFUNCTION, &curlCallback);
	curl_easy_setopt (handle, CURLOPT_WRITEDATA, &m_uploadLogResult);

	// FTB ships no CA bundle; core/http.c skips peer verification the same way
	curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 0L);

	// set headers
	static char contentType[]       = "Content-Type: text/plain";
	static curl_slist const headers = {contentType, nullptr};
	curl_easy_setopt (handle, CURLOPT_URL, "https://pastie.io/documents");
	curl_easy_setopt (handle, CURLOPT_HTTPHEADER, &headers);

	// set form data
	auto const mime = curl_mime_init (handle);
	auto const part = curl_mime_addpart (mime);
	curl_mime_name (part, "data");
	curl_mime_data (part, m_uploadLogData.data (), m_uploadLogData.size ());
	curl_easy_setopt (handle, CURLOPT_MIMEPOST, mime);

	// add to multi handle
	curl_multi_add_handle (m_uploadLogCurlM, handle);

	// signal network thread to process
	m_uploadLogMime = mime;
	m_uploadLogCurl.store (handle, std::memory_order_relaxed);

	info ("Uploading log\n");
	return true;
}

std::string FtpServer::getFreeSpace ()
{
	auto const lock = std::scoped_lock (s_lock);
	return s_freeSpace;
}

void FtpServer::updateFreeSpace ()
{
	statvfs_t st = {};
	if (::statvfs ("sdmc:/", &st) != 0)
		return;

	auto freeSpace = fs::printSize (static_cast<std::uint64_t> (st.f_bsize) * st.f_bfree);

	auto const lock = std::scoped_lock (s_lock);
	if (freeSpace != s_freeSpace)
		s_freeSpace = std::move (freeSpace);
}

std::time_t FtpServer::startTime ()
{
	return s_startTime;
}

int FtpServer::tzOffset ()
{
	return s_tzOffset;
}

void FtpServer::handleNetworkFound ()
{
	SockAddr addr;
	if (!platform::networkAddress (addr))
		return;

	std::uint16_t port;

	{
		auto const lock = m_config->lockGuard ();
		port            = m_config->port ();
	}

	addr.setPort (port);

	auto socket = Socket::create (Socket::eStream);
	if (!socket)
		return;

	if (port != 0 && !socket->setReuseAddress (true))
		return;

	if (!socket->bind (addr))
		return;

	if (!socket->listen (10))
		return;

	auto const &sockName = socket->sockName ();
	auto const name      = sockName.name ();

	std::string fullName (std::strlen (name) + 3 + 5, '\0');
	fullName.resize (std::sprintf (fullName.data (), "[%s]:%u", name, sockName.port ()));

	info ("Started server at %s\n", fullName.c_str ());

	LOCKED (m_name = std::move (fullName); m_socket = std::move (socket));

	socket = mdns::createSocket ();
	if (!socket)
		return;

	LOCKED (m_mdnsSocket = std::move (socket));
}

void FtpServer::handleNetworkLost ()
{
	{
		// destroy sessions
		std::vector<UniqueFtpSession> sessions;
		LOCKED (sessions = std::move (m_sessions));
	}

	{
		UniqueSocket sock;

		// destroy command socket
		LOCKED (sock = std::move (m_socket));

		// destroy mDNS socket
		LOCKED (sock = std::move (m_mdnsSocket));
	}

	info ("Stopped server at %s\n", m_name.c_str ());
}

void FtpServer::loop ()
{
	if (m_restart.exchange (false))
	{
		{
			auto const lock = m_config->lockGuard ();
			mdns::setHostname (m_config->hostname ());
		}

		// FTB: drop sessions like handleNetworkLost; they kept the login they were authorized with
		std::vector<UniqueFtpSession> sessions;
		LOCKED (sessions = std::move (m_sessions));

		UniqueSocket socket;
		LOCKED (socket = std::move (m_socket));
	}

	if (!m_socket)
	{
		if (platform::networkVisible ())
			handleNetworkFound ();
	}

	{
		auto const lock = std::scoped_lock (m_lock);
		if (m_uploadLogCurl.load (std::memory_order_relaxed))
		{
			int busy      = 0;
			auto const mc = curl_multi_perform (m_uploadLogCurlM, &busy);
			if (mc != CURLM_OK)
			{
				error ("curl_multi_perform: %d %s\n", mc, curl_multi_strerror (mc));

				curl_multi_remove_handle (m_uploadLogCurlM, m_uploadLogCurl);
				curl_easy_cleanup (m_uploadLogCurl);
				curl_mime_free (m_uploadLogMime);
				m_uploadLogMime = nullptr;
				m_uploadLogCurl.store (nullptr, std::memory_order_relaxed);
			}
			else
			{
				int count      = 0;
				auto const msg = curl_multi_info_read (m_uploadLogCurlM, &count);
				if (msg && msg->msg == CURLMSG_DONE && msg->easy_handle == m_uploadLogCurl)
				{
					if (msg->data.result != CURLE_OK)
						info ("cURL finished with status %d\n", msg->data.result);

					json_error_t err;
					auto const root = json_loads (m_uploadLogResult.c_str (), 0, &err);
					if (json_is_object (root))
					{
						auto const key = json_object_get (root, "key");
						if (json_is_string (key))
							info (
							    "Log uploaded to https://pastie.io/%s\n", json_string_value (key));
					}
					else
						error ("Failed to upload log\n");

					json_decref (root);

					curl_multi_remove_handle (m_uploadLogCurlM, m_uploadLogCurl);
					curl_easy_cleanup (m_uploadLogCurl);
					curl_mime_free (m_uploadLogMime);
					m_uploadLogMime = nullptr;
					m_uploadLogCurl.store (nullptr, std::memory_order_relaxed);
				}
			}
		}
	}

	// poll listen socket
	if (m_socket)
	{
		Socket::PollInfo info{*m_socket, POLLIN, 0};
		auto const rc = Socket::poll (&info, 1, 0ms);
		if (rc < 0)
		{
			handleNetworkLost ();
			return;
		}

		if (rc > 0 && (info.revents & POLLIN))
		{
			auto socket = m_socket->accept ();
			if (socket)
			{
				auto session = FtpSession::create (*m_config, std::move (socket));
				LOCKED (m_sessions.emplace_back (std::move (session)));
			}
			else
			{
				handleNetworkLost ();
				return;
			}
		}
	}

	// poll mDNS socket
	if (m_socket && m_mdnsSocket)
		mdns::handleSocket (m_mdnsSocket.get (), m_socket->sockName ());

	{
		std::vector<UniqueFtpSession> deadSessions;
		{
			// remove dead sessions
			auto const lock = std::scoped_lock (m_lock);
			auto it         = std::begin (m_sessions);
			while (it != std::end (m_sessions))
			{
				auto &session = *it;
				if (session->dead ())
				{
					deadSessions.emplace_back (std::move (session));
					it = m_sessions.erase (it);
				}
				else
					++it;
			}
		}
	}

	// poll sessions
	if (!m_sessions.empty ())
	{
		if (!FtpSession::poll (m_sessions))
			handleNetworkLost ();
	}
	// avoid busy polling in background thread
	else
		platform::Thread::sleep (16ms);
}

void FtpServer::threadFunc ()
{
	while (!m_quit)
		loop ();
}
