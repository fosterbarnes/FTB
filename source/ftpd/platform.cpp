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

// FTB: derived from ftpd's source/3ds/platform.cpp without graphics, input, or soc:u setup.

#include "platform.h"

#include "log.h"

// FTB: core/task/task.h (C) pulls in dataop.h, whose `delete` member is not valid C++
extern "C" void task_sleep_block ();
extern "C" void task_sleep_unblock ();

#include <arpa/inet.h>

#include <algorithm>
#include <mutex>

namespace
{
/// \brief Thread stack size
constexpr auto STACK_SIZE = 0x8000;

/// \brief Whether ndm:u is initialized
bool s_ndmuActive = false;
/// \brief Whether ndm:u is locked
bool s_ndmuLocked = false;

/// \brief AC event handle
Handle s_acEventHandle = 0;
/// \brief Whether AC event handle was signaled
bool s_acEventSignaled = false;
/// \brief ac:u fence
platform::Mutex s_acuFence;

/// \brief Whether to power backlight
bool s_backlight = true;

/// \brief APT hook cookie
aptHookCookie s_aptHookCookie;

/// \brief Enable backlight
/// \param enable_ Whether to enable backlight
void enableBacklight (bool const enable_)
{
	if (R_FAILED (gspLcdInit ()))
		return;

	(enable_ ? GSPLCD_PowerOnBacklight : GSPLCD_PowerOffBacklight) (GSPLCD_SCREEN_BOTH);

	gspLcdExit ();
}

/// \brief Handle APT cookie
/// \param type_ Hook type
/// \param param_ User param
void handleAPTHook (APT_HookType const type_, void *const param_)
{
	(void)param_;

	switch (type_)
	{
	case APTHOOK_ONSUSPEND:
	case APTHOOK_ONSLEEP:
		// turn on backlight, or you can't see the home menu!
		if (!s_backlight)
			enableBacklight (true);
		break;

	case APTHOOK_ONRESTORE:
	case APTHOOK_ONWAKEUP:
		// restore backlight setting
		enableBacklight (s_backlight);
		break;

	default:
		break;
	}
}

/// \brief Ask AC to connect; signals s_acEventHandle when done
bool requestConnect ()
{
	Result result;
	if (R_FAILED (result = svcCreateEvent (&s_acEventHandle, RESET_STICKY)))
	{
		error ("svcCreateEvent(acEventHandle): result 0x%lx\n", result);
		s_acEventHandle = 0;
		return false;
	}

	acuConfig config;
	if (R_FAILED (result = ACU_CreateDefaultConfig (&config)))
	{
		error ("ACU_CreateDefaultConfig: result 0x%lx\n", result);
		return false;
	}

	if (R_FAILED (result = ACU_SetNetworkArea (&config, 2)))
	{
		error ("ACU_SetNetworkArea: result 0x%lx\n", result);
		return false;
	}

	if (R_FAILED (result = ACU_SetAllowApType (&config, static_cast<u8> (AC_AP_TYPE_ALL))))
	{
		error ("ACU_SetAllowApType: result 0x%lx\n", result);
		return false;
	}

	if (R_FAILED (result = ACU_SetRequestEulaVersion (&config)))
	{
		error ("ACU_SetRequestEulaVersion: result 0x%lx\n", result);
		return false;
	}

	if (R_FAILED (result = ACU_ConnectAsync (&config, s_acEventHandle)))
	{
		error ("ACU_ConnectAsync: result 0x%lx\n", result);
		return false;
	}

	return true;
}

/// \brief Keep wifi up while the lid is closed
void lockNetworkState ()
{
	if (s_ndmuLocked || !s_ndmuActive)
		return;

	Result res;
	if (R_FAILED (res = NDMU_EnterExclusiveState (NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)))
	{
		error ("Failed to enter exclusive NDM state: 0x%lx\n", res);
		return;
	}

	if (R_FAILED (res = NDMU_LockState ()))
	{
		error ("Failed to lock NDM: 0x%lx\n", res);
		NDMU_LeaveExclusiveState ();
		return;
	}

	// FTB: through the shared sleep block, so a data op ending meanwhile cannot re-allow sleep
	task_sleep_block ();
	s_ndmuLocked = true;
	info ("Wifi connected\n");
}
}

void platform::start ()
{
	s_ndmuActive = R_SUCCEEDED (ndmuInit ());
	if (!s_ndmuActive)
		error ("Failed to initialize ndm:u; closing the lid will drop wifi\n");

	aptHook (&s_aptHookCookie, handleAPTHook, nullptr);
}

void platform::stop ()
{
	if (s_ndmuLocked)
	{
		NDMU_UnlockState ();
		NDMU_LeaveExclusiveState ();
		task_sleep_unblock ();
		s_ndmuLocked = false;
	}

	if (s_ndmuActive)
	{
		ndmuExit ();
		s_ndmuActive = false;
	}

	aptUnhook (&s_aptHookCookie);

	// turn backlight back on
	if (!s_backlight)
	{
		s_backlight = true;
		enableBacklight (true);
	}
}

void platform::toggleBacklight ()
{
	s_backlight = !s_backlight;
	enableBacklight (s_backlight);
}

bool platform::networkVisible ()
{
	// serialize ac:u access from multiple threads
	auto const lock = std::scoped_lock (s_acuFence);

	// ask AC to bring wifi up once per process; the connection outlives the FTP screen
	if (!s_acEventSignaled)
	{
		bool done = false;
		if (!s_acEventHandle)
		{
			// a failed request never signals; rely on the system's own wifi state instead
			done = !requestConnect ();
		}
		else
		{
			auto const result = svcWaitSynchronization (s_acEventHandle, 0);
			if (result != MAKERESULT (RL_INFO, RS_STATUSCHANGED, RM_OS, RD_TIMEOUT))
			{
				if (R_FAILED (result))
					error ("svcWaitSynchronization(acEventHandle): result 0x%lx\n", result);
				done = true;
			}
		}

		if (done)
		{
			if (s_acEventHandle)
				svcCloseHandle (s_acEventHandle);
			s_acEventHandle   = 0;
			s_acEventSignaled = true;
		}
	}

	// get wifi status
	std::uint32_t wifi = 0;
	auto const result  = ACU_GetWifiStatus (&wifi);
	if (R_FAILED (result) || !wifi)
	{
		if (R_FAILED (result))
			error ("ACU_GetWifiStatus: result 0x%lx\n", result);

		return false;
	}

	lockNetworkState ();
	return true;
}

bool platform::networkAddress (SockAddr &addr_)
{
	sockaddr_in addr;
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = gethostid ();

	addr_ = addr;
	return true;
}

std::string const &platform::hostname ()
{
	static std::string const hostname = "3ds-ftpd";
	return hostname;
}

///////////////////////////////////////////////////////////////////////////
platform::steady_clock::time_point platform::steady_clock::now () noexcept
{
	return time_point (duration (svcGetSystemTick ()));
}

///////////////////////////////////////////////////////////////////////////
/// \brief Platform thread pimpl
class platform::Thread::privateData_t
{
public:
	privateData_t () = default;

	/// \brief Parameterized constructor
	/// \param func_ Thread entry point
	privateData_t (std::function<void ()> &&func_) : func (std::move (func_))
	{
		// use next-lower priority
		s32 priority = 0x30;
		svcGetThreadPriority (&priority, CUR_THREAD_HANDLE);
		priority = std::clamp<s32> (priority, 0x18, 0x3F - 1) + 1;

		// use appcore
		// FTB: upstream only asserted, which NDEBUG compiles out; the FTP screen shows the log line
		thread = threadCreate (&privateData_t::threadFunc, this, STACK_SIZE, priority, 0, false);
		if (!thread)
			error ("Failed to start the server thread\n");
	}

	/// \brief FTB opens a new server per FTP screen, so each thread is joined and freed here
	~privateData_t ()
	{
		if (!thread)
			return;

		threadJoin (thread, UINT64_MAX);
		threadFree (thread);
	}

	/// \brief Underlying thread entry point
	/// \param arg_ Thread pimpl object
	static void threadFunc (void *const arg_)
	{
		// call passed-in entry point
		auto const t = static_cast<privateData_t *> (arg_);
		t->func ();
	}

	/// \brief Underlying thread
	::Thread thread = nullptr;
	/// \brief Thread entry point
	std::function<void ()> func;
};

///////////////////////////////////////////////////////////////////////////
platform::Thread::~Thread () = default;

platform::Thread::Thread () : m_d (new privateData_t ())
{
}

platform::Thread::Thread (std::function<void ()> &&func_)
    : m_d (new privateData_t (std::move (func_)))
{
}

platform::Thread::Thread (Thread &&that_) : m_d (new privateData_t ())
{
	std::swap (m_d, that_.m_d);
}

platform::Thread &platform::Thread::operator= (Thread &&that_)
{
	std::swap (m_d, that_.m_d);
	return *this;
}

void platform::Thread::join ()
{
	if (m_d->thread)
		threadJoin (m_d->thread, UINT64_MAX);
}

void platform::Thread::sleep (std::chrono::milliseconds const timeout_)
{
	svcSleepThread (std::chrono::nanoseconds (timeout_).count ());
}

///////////////////////////////////////////////////////////////////////////
/// \brief Platform mutex pimpl
class platform::Mutex::privateData_t
{
public:
	/// \brief Underlying mutex
	LightLock mutex;
};

///////////////////////////////////////////////////////////////////////////
platform::Mutex::~Mutex () = default;

platform::Mutex::Mutex () : m_d (new privateData_t ())
{
	LightLock_Init (&m_d->mutex);
}

void platform::Mutex::lock ()
{
	LightLock_Lock (&m_d->mutex);
}

void platform::Mutex::unlock ()
{
	LightLock_Unlock (&m_d->mutex);
}
