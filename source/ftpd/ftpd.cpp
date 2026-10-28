#include "ftpd.h"

#include "ftpServer.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace
{
UniqueFtpServer s_server;

/// \brief Copy with truncation; keepTail_ keeps the end (file names) instead of the start
void copyText (char *const out_, std::size_t const size_, std::string_view text_, bool const keepTail_)
{
	if (size_ == 0)
		return;

	if (text_.size () >= size_)
	{
		if (keepTail_)
		{
			// skip UTF-8 continuation bytes so the copy starts on a whole character
			auto start = text_.size () - (size_ - 1);
			while (start < text_.size () && (static_cast<unsigned char> (text_[start]) & 0xC0) == 0x80)
				++start;
			text_ = text_.substr (start);
		}
		else
		{
			auto end = size_ - 1;
			while (end > 0 && (static_cast<unsigned char> (text_[end]) & 0xC0) == 0x80)
				--end;
			text_ = text_.substr (0, end);
		}
	}

	std::memcpy (out_, text_.data (), text_.size ());
	out_[text_.size ()] = '\0';
}

void toSettings (ftpd_settings *const out_, FtpServer::Settings const &settings_)
{
	copyText (out_->user, sizeof (out_->user), settings_.user, false);
	copyText (out_->pass, sizeof (out_->pass), settings_.pass, false);
	copyText (out_->hostname, sizeof (out_->hostname), settings_.hostname, false);
	out_->port         = settings_.port;
	out_->deflateLevel = settings_.deflateLevel;
	out_->getMTime     = settings_.getMTime;
}
}

void ftpd_start (void)
{
	if (s_server)
		return;

	platform::start ();
	clearLog ();
	s_server = FtpServer::create ();
}

void ftpd_stop (void)
{
	if (!s_server)
		return;

	s_server.reset ();
	platform::stop ();
}

char const *ftpd_version (void)
{
	return STATUS_STRING;
}

void ftpd_toggle_backlight (void)
{
	platform::toggleBacklight ();
}

bool ftpd_address (char *const out, std::size_t const size)
{
	if (!s_server)
		return false;

	// "[ip]:port" brackets only matter for IPv6, which the 3DS lacks
	auto address = s_server->address ();
	if (address.empty ())
		return false;

	std::erase (address, '[');
	std::erase (address, ']');
	copyText (out, size, address, false);
	return true;
}

std::size_t ftpd_sessions (ftpd_session *const out, std::size_t const max)
{
	if (!s_server)
		return 0;

	auto const sessions = s_server->sessions ();
	for (std::size_t i = 0; i < sessions.size () && i < max; ++i)
	{
		auto const &session = sessions[i];
		auto &dst           = out[i];

		copyText (dst.peer, sizeof (dst.peer), session.peer, false);
		copyText (dst.item, sizeof (dst.item), session.item, true);
		dst.state    = static_cast<ftpd_session_state> (session.state);
		dst.position = session.position;
		dst.size     = session.size;
		dst.rate     = session.rate;

		static_assert (FTPD_RATE_HISTORY == std::tuple_size_v<decltype (session.history)>);
		std::copy (std::begin (session.history), std::end (session.history), dst.history);
	}

	return sessions.size ();
}

std::uint64_t ftpd_log_read (std::size_t const first,
    ftpd_log_line *const out,
    std::size_t const max,
    std::size_t *const total)
{
	std::size_t index = 0;
	return readLog (first, max, *total, [&] (LogLevel const level_, std::string_view const text_) {
		out[index].level = static_cast<ftpd_log_level> (level_);
		copyText (out[index].text, sizeof (out[index].text), text_, false);
		++index;
	});
}

void ftpd_get_settings (ftpd_settings *const out)
{
	toSettings (out, s_server ? s_server->settings () : FtpServer::defaultSettings ());
}

void ftpd_get_default_settings (ftpd_settings *const out)
{
	toSettings (out, FtpServer::defaultSettings ());
}

bool ftpd_apply_settings (ftpd_settings const *const settings, bool const save)
{
	if (!s_server)
		return false;

	return s_server->applySettings ({settings->user,
	                                    settings->pass,
	                                    settings->hostname,
	                                    settings->port,
	                                    settings->deflateLevel,
	                                    settings->getMTime},
	    save);
}

bool ftpd_upload_log (void)
{
	return s_server && s_server->uploadLog ();
}
