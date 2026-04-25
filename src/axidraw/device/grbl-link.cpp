// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-link.h"

#include "grbl-client.h"
#include "serial-port.h"
#include "tcp-port.h"

#include <utility>

namespace Inkscape::Axidraw {

GrblLink::GrblLink() = default;
GrblLink::~GrblLink() = default;
GrblLink::GrblLink(GrblLink &&) noexcept = default;
GrblLink &GrblLink::operator=(GrblLink &&) noexcept = default;

void GrblLink::set_serial(std::unique_ptr<SerialPort> port)
{
    close();
    _serial = std::move(port);
}

void GrblLink::set_tcp(std::unique_ptr<TcpPort> port)
{
    close();
    _tcp = std::move(port);
}

GrblLink::Kind GrblLink::kind() const noexcept
{
    if (_serial) {
        return Kind::serial;
    }
    if (_tcp) {
        return Kind::tcp;
    }
    return Kind::none;
}

bool GrblLink::is_open() const
{
    return (_serial && _serial->is_open()) || (_tcp && _tcp->is_open());
}

SerialPort *GrblLink::serial_port() const noexcept
{
    return _serial.get();
}

bool GrblLink::write_bytes(void const *data, std::size_t len)
{
    if (_serial && _serial->is_open()) {
        return _serial->write_bytes(data, len);
    }
    if (_tcp && _tcp->is_open()) {
        return _tcp->write_bytes(data, len);
    }
    return false;
}

bool GrblLink::write_line(std::string const &line)
{
    if (_serial && _serial->is_open()) {
        return _serial->write_line(line);
    }
    if (_tcp && _tcp->is_open()) {
        return _tcp->write_line(line);
    }
    return false;
}

bool GrblLink::read_line(std::string &out, int timeout_ms)
{
    if (_serial && _serial->is_open()) {
        return _serial->read_line(out, timeout_ms);
    }
    if (_tcp && _tcp->is_open()) {
        return _tcp->read_line(out, timeout_ms);
    }
    return false;
}

bool GrblLink::send_line_wait_ok(std::string const &line, std::string &err_out)
{
    if (_serial && _serial->is_open()) {
        return grbl_send_line(*_serial, line, err_out);
    }
    if (_tcp && _tcp->is_open()) {
        return grbl_send_line(*_tcp, line, err_out);
    }
    err_out = "not connected";
    return false;
}

void GrblLink::purge_io()
{
    if (_serial && _serial->is_open()) {
        _serial->purge_io();
    } else if (_tcp && _tcp->is_open()) {
        _tcp->purge_io();
    }
}

void GrblLink::close()
{
    if (_serial) {
        _serial->close();
        _serial.reset();
    }
    if (_tcp) {
        _tcp->close();
        _tcp.reset();
    }
}

} // namespace Inkscape::Axidraw
