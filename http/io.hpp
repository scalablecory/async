/*
	Experimental Async I/O
	Copyright (C) 2016 Cory Nelson

	This program is free software: you can redistribute it and/or modify
	it under the terms of version 3 of the GNU General Public License as
	published by the Free Software Foundation.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <exception>
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <MSWSock.h>

namespace io
{

struct async_op
{
	virtual void step() = 0;
};

class win32_error : public std::exception
{
public:
	inline win32_error(DWORD err = GetLastError()) : m_err(err) { }

	inline DWORD error() const { return m_err; }
	inline char const* what() const { return "win32 error."; }

private:
	DWORD const m_err;
};

struct winsock_error : win32_error
{
	inline winsock_error(int err = WSAGetLastError()) : win32_error(err) {}
};

class op_context : public OVERLAPPED
{
public:
	void check_error() const;
	inline bool has_error() const { return status() != ERROR_SUCCESS; }

	inline DWORD status() const { return m_error; }

	inline OVERLAPPED* init() { *((OVERLAPPED*)this) = OVERLAPPED(); return this; }
	inline virtual void completion_func() { }

protected:
	DWORD m_error;
};

class socket;

class socket_context : public op_context
{
public:
	inline DWORD transferred() const { return m_transferred; }
	inline DWORD flags() const { return m_flags; }

	void completion_func();
	inline OVERLAPPED* init(SOCKET sock) { m_sock = sock; return op_context::init(); }

protected:
	SOCKET m_sock;
	DWORD m_transferred, m_flags;

	friend socket;
};

struct connect_context : socket_context
{
	void completion_func();
};

class resolve_context : public op_context
{
public:
	inline std::shared_ptr<ADDRINFOEXW> const& result() const { return m_result; }

private:
	HANDLE m_iocp;
	ADDRINFOEXW *m_rawres;
	std::shared_ptr<ADDRINFOEXW> m_result;

	static void CALLBACK lookup_completion(DWORD error, DWORD, OVERLAPPED *overlapped);

	friend struct resolve_helpers;
};

class service
{
public:
	service();
	~service();

	bool run();
	bool run_many();

	void shutdown();

	inline HANDLE handle() { return m_iocp; }

private:
	HANDLE m_iocp;
};

class socket
{
public:
	socket(service &svc, int family, int type, int protocol);
	void close();

	void set_option(int level, int name, void const *val, int val_len);
	bool try_set_option(int level, int name, void const *val, int val_len);

	void bind(sockaddr const *name, int name_len);

	bool accept_and_receive(socket &sock, void *buffer, int receive_len, int local_len, int remote_len, socket_context &ctx);

	bool connect(sockaddr const *name, int name_len, connect_context &ctx);
	bool connect_and_send(sockaddr const *name, int name_len, void const *buffer, int buffer_len, connect_context &ctx);

	bool send(void const *buffer, int buffer_len, DWORD flags, socket_context &ctx);
	bool send(WSABUF const *buffers, int buffer_count, DWORD flags, socket_context &ctx);

	bool receive(void *buffer, int buffer_len, DWORD flags, socket_context &ctx);
	bool receive(WSABUF const *buffers, int buffer_count, DWORD flags, socket_context &ctx);

	void shutdown(int how);
	bool disconnect(bool reuse, socket_context &ctx);

	inline socket() : m_sock(INVALID_SOCKET) {}
	inline ~socket() { close(); }

	inline socket(socket &&sock) { m_sock = sock.m_sock; sock.m_sock = INVALID_SOCKET; }
	inline socket& operator=(socket &&sock) { close(); m_sock = sock.m_sock; sock.m_sock = INVALID_SOCKET; return *this; }

private:
	SOCKET m_sock;
};

template<typename BaseContext>
class async_context : public BaseContext
{
public:
	inline async_context(async_op *op) { m_op = op; }

	inline virtual void completion_func()
	{
		BaseContext::completion_func();
		m_op->step();
	}

private:
	async_op *m_op;
};

class resolve_and_connect_context : public socket_context , public async_op
{
public:
	resolve_and_connect_context();
	void step();
	void completion_func();

private:
	enum { on_start, on_resolve, on_connect } state;
	async_context<resolve_context> resolve_ctx;
	async_context<connect_context> connect_ctx;

	bool sync;
	ADDRINFOEXW const *iter;
	service *svc;
	socket *sock;
	wchar_t const *hostname, *servicename;
	void const *buffer;
	int buffer_len;

	friend struct connect_and_send_helpers;
};

bool resolve(service &svc, wchar_t const *hostname, wchar_t const *servicename, ADDRINFOEXW const *hints, resolve_context &ctx);
bool connect_and_send(service &svc, socket &sock, wchar_t const *hostname, wchar_t const *servicename, resolve_and_connect_context &ctx);
bool connect_and_send(service &svc, socket &sock, wchar_t const *hostname, wchar_t const *servicename, void const *buffer, int buffer_len, resolve_and_connect_context &ctx);


}
