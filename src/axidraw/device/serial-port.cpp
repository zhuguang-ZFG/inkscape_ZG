// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Minimal RS-232 access for GRBL controllers (Windows + POSIX).
 */

#include "serial-port.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace Inkscape::Axidraw {

#ifdef _WIN32
static HANDLE as_handle(void *p) { return static_cast<HANDLE>(p); }
#endif

SerialPort::SerialPort()
#ifdef _WIN32
    : _handle(nullptr)
#else
    : _fd(-1)
#endif
{
}

SerialPort::~SerialPort()
{
    close();
}

bool SerialPort::is_open() const
{
#ifdef _WIN32
    return _handle != nullptr;
#else
    return _fd >= 0;
#endif
}

bool SerialPort::last_open_timed_out() const
{
    return _last_open_timed_out;
}

bool SerialPort::last_open_access_denied() const
{
    return _last_open_access_denied;
}

void SerialPort::close()
{
#ifdef _WIN32
    if (_handle) {
        CloseHandle(as_handle(_handle));
        _handle = nullptr;
    }
#else
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
#endif
    rxbuf_.clear();
}

#ifndef _WIN32
static bool posix_apply_baud(termios &tio, int baud)
{
    speed_t s;
    switch (baud) {
    case 9600:
        s = B9600;
        break;
    case 19200:
        s = B19200;
        break;
    case 38400:
        s = B38400;
        break;
    case 57600:
        s = B57600;
        break;
    case 115200:
        s = B115200;
        break;
    case 230400:
        s = B230400;
        break;
    default:
        return false;
    }
    if (cfsetispeed(&tio, s) != 0 || cfsetospeed(&tio, s) != 0) {
        return false;
    }
    return true;
}
#endif

bool SerialPort::open(std::string device, int baud_rate, int open_timeout_ms)
{
    close();
    _last_open_timed_out = false;
    _last_open_access_denied = false;
    if (device.empty() || baud_rate <= 0) {
        return false;
    }

#ifdef _WIN32
    // Allow COM10+ and exclusive access.
    if (device.rfind("\\\\.\\", 0) != 0) {
        if (device.size() >= 3 && device.compare(0, 3, "COM") == 0) {
            device.insert(0, "\\\\.\\");
        }
    }

    auto open_handle = [&]() -> HANDLE {
        if (open_timeout_ms < 0) {
            return CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }

        struct AsyncOpenState {
            std::mutex mutex;
            std::condition_variable cv;
            std::atomic<bool> abandon{false};
            bool done = false;
            HANDLE handle = INVALID_HANDLE_VALUE;
            DWORD error = ERROR_SUCCESS;
        };

        auto state = std::make_shared<AsyncOpenState>();
        std::thread worker([state, device] {
            HANDLE h =
                CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD const err = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (state->abandon.load(std::memory_order_acquire) && h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->handle = h;
                state->error = err;
                state->done = true;
            }
            state->cv.notify_one();
        });

        bool completed = false;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            completed = state->cv.wait_for(lock, std::chrono::milliseconds(open_timeout_ms),
                                           [&] { return state->done; });
        }

        if (!completed) {
            _last_open_timed_out = true;
            state->abandon.store(true, std::memory_order_release);
            CancelSynchronousIo(reinterpret_cast<HANDLE>(worker.native_handle()));
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                completed = state->cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return state->done; });
            }
        }

        if (completed) {
            worker.join();
            _last_open_access_denied = state->handle == INVALID_HANDLE_VALUE && state->error == ERROR_ACCESS_DENIED;
        } else {
            worker.detach();
        }

        return _last_open_timed_out ? INVALID_HANDLE_VALUE : state->handle;
    };

    HANDLE h = open_handle();
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }

    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }

    if (!SetupComm(h, 4096, 4096)) {
        CloseHandle(h);
        return false;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    _handle = h;
    return true;
#else
    int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return false;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return false;
    }

    cfmakeraw(&tio);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= CS8 | CREAD | CLOCAL;

    if (!posix_apply_baud(tio, baud_rate)) {
        ::close(fd);
        return false;
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return false;
    }

    tcflush(fd, TCIOFLUSH);
    _fd = fd;
    return true;
#endif
}

void SerialPort::purge_io()
{
#ifdef _WIN32
    if (_handle) {
        PurgeComm(as_handle(_handle), PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
#else
    if (_fd >= 0) {
        tcflush(_fd, TCIOFLUSH);
    }
#endif
    rxbuf_.clear();
}

bool SerialPort::write_bytes(void const *data, size_t len)
{
    if (!is_open() || !data || len == 0) {
        return false;
    }
#ifdef _WIN32
    DWORD written = 0;
    return WriteFile(as_handle(_handle), data, static_cast<DWORD>(len), &written, nullptr) && written == len;
#else
    auto const *p = static_cast<char const *>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(_fd, p + off, len - off);
        if (n <= 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
#endif
}

bool SerialPort::write_line(std::string_view line)
{
    std::string buf(line);
    if (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
        // already terminated
    } else {
        buf.append("\r\n");
    }
    return write_bytes(buf.data(), buf.size());
}

size_t SerialPort::read_chunk(char *buf, size_t cap, int timeout_ms)
{
    if (!is_open() || cap == 0) {
        return 0;
    }
#ifdef _WIN32
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = 0;
    t.ReadTotalTimeoutConstant = timeout_ms < 0 ? 0 : static_cast<DWORD>(timeout_ms);
    SetCommTimeouts(as_handle(_handle), &t);

    DWORD got = 0;
    if (!ReadFile(as_handle(_handle), buf, static_cast<DWORD>(cap), &got, nullptr)) {
        return 0;
    }
    return got;
#else
    pollfd pfd{};
    pfd.fd = _fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return 0;
    }
    ssize_t n = ::read(_fd, buf, cap);
    if (n <= 0) {
        return 0;
    }
    return static_cast<size_t>(n);
#endif
}

bool SerialPort::read_line(std::string &out, int timeout_ms)
{
    using clock = std::chrono::steady_clock;
    auto const deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto const nl = rxbuf_.find('\n');
        if (nl != std::string::npos) {
            out.assign(rxbuf_, 0, nl);
            rxbuf_.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') {
                out.pop_back();
            }
            return true;
        }

        int slice = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count());
        if (slice <= 0) {
            return false;
        }
        slice = std::min(slice, 50);

        char tmp[256];
        size_t n = read_chunk(tmp, sizeof(tmp), slice);
        if (n > 0) {
            rxbuf_.append(tmp, n);
        } else if (clock::now() >= deadline) {
            return false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

} // namespace Inkscape::Axidraw
