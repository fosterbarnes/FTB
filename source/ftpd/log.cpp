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

// FTB: ImGui log drawing replaced by a display line store that FTB's UI reads each frame.

#include "log.h"

#include "platform.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <ranges>
#include <vector>

namespace
{
/// \brief Maximum number of log messages to keep
constexpr auto MAX_LOGS = 250;

/// \brief Maximum number of display lines to keep
constexpr auto MAX_LINES = 500;

/// \brief Display line length cap: the top screen shows about 100 characters, and every drawn
/// glyph costs GPU command buffer space
constexpr std::size_t MAX_LINE_LENGTH = 128;

/// \brief Log message
struct Message
{
	/// \brief Parameterized constructor
	/// \param level_ Log level
	/// \param message_ Log message
	Message (LogLevel const level_, std::string message_)
	    : level (level_), message (std::move (message_))
	{
	}

	/// \brief Log level
	LogLevel level;
	/// \brief Log message
	std::string message;
};

/// \brief Log messages (full text, for log upload)
std::vector<Message> s_messages;

/// \brief Display lines
std::deque<Message> s_lines;

/// \brief Number of display lines ever added
std::uint64_t s_lineSerial = 0;

/// \brief Log lock
platform::Mutex s_lock;

/// \brief Store a message; caller holds s_lock
/// \param level_ Log level
/// \param message_ Log message
void store (LogLevel const level_, std::string message_)
{
	auto text = std::string_view (message_);
	while (!text.empty ())
	{
		auto const end = text.find ('\n');
		auto line      = text.substr (0, end);
		text           = end == std::string_view::npos ? std::string_view () : text.substr (end + 1);

		if (!line.empty () && line.back () == '\r')
			line.remove_suffix (1);

		if (line.size () > MAX_LINE_LENGTH)
		{
			// cut on a UTF-8 lead byte so the renderer never sees half a character
			auto cut = MAX_LINE_LENGTH;
			while (cut > 0 && (static_cast<unsigned char> (line[cut]) & 0xC0) == 0x80)
				--cut;
			line = line.substr (0, cut);
		}

		s_lines.emplace_back (level_, std::string (line));
		++s_lineSerial;
	}

	while (s_lines.size () > MAX_LINES)
		s_lines.pop_front ();

	s_messages.emplace_back (level_, std::move (message_));
	if (s_messages.size () > MAX_LOGS)
		s_messages.erase (std::begin (s_messages));
}
}

std::string getLog ()
{
	auto const lock = std::scoped_lock (s_lock);

	if (s_messages.empty ())
		return {};

	std::vector<Message const *> stack;
	stack.reserve (s_messages.size ());

	std::size_t size = 0;
	for (auto const &msg : s_messages | std::views::reverse)
	{
		if (size + msg.message.size () > 1024 * 1024)
			break;

		size += msg.message.size ();
		stack.emplace_back (&msg);
	}

	std::string log;
	log.reserve (size);

	for (auto const &msg : stack | std::views::reverse)
		log += msg->message;

	return log;
}

void clearLog ()
{
	auto const lock = std::scoped_lock (s_lock);
	s_messages.clear ();
	s_lines.clear ();
}

std::uint64_t readLog (std::size_t const first_,
    std::size_t const count_,
    std::size_t &total_,
    std::function<void (LogLevel, std::string_view)> const &fn_)
{
	auto const lock = std::scoped_lock (s_lock);

	total_ = s_lines.size ();
	for (auto i = first_; i < total_ && i - first_ < count_; ++i)
		fn_ (s_lines[i].level, s_lines[i].message);

	return s_lineSerial;
}

void debug (char const *const fmt_, ...)
{
#ifndef NDEBUG
	va_list ap;

	va_start (ap, fmt_);
	addLog (DEBUG, fmt_, ap);
	va_end (ap);
#else
	(void)fmt_;
#endif
}

void info (char const *const fmt_, ...)
{
	va_list ap;

	va_start (ap, fmt_);
	addLog (INFO, fmt_, ap);
	va_end (ap);
}

void error (char const *const fmt_, ...)
{
	va_list ap;

	va_start (ap, fmt_);
	addLog (ERROR, fmt_, ap);
	va_end (ap);
}

void command (char const *const fmt_, ...)
{
	va_list ap;

	va_start (ap, fmt_);
	addLog (COMMAND, fmt_, ap);
	va_end (ap);
}

void response (char const *const fmt_, ...)
{
	va_list ap;

	va_start (ap, fmt_);
	addLog (RESPONSE, fmt_, ap);
	va_end (ap);
}

void addLog (LogLevel const level_, char const *const fmt_, va_list ap_)
{
#ifdef NDEBUG
	if (level_ == DEBUG)
		return;
#endif

	thread_local static char buffer[1024];

	std::vsnprintf (buffer, sizeof (buffer), fmt_, ap_);

	auto const lock = std::scoped_lock (s_lock);
	store (level_, buffer);
}

void addLog (LogLevel const level_, std::string_view const message_)
{
#ifdef NDEBUG
	if (level_ == DEBUG)
		return;
#endif

	auto msg = std::string (message_);
	for (auto &c : msg)
	{
		// replace nul-characters with ? to avoid truncation
		if (c == '\0')
			c = '?';
	}

	auto const lock = std::scoped_lock (s_lock);
	store (level_, std::move (msg));
}
