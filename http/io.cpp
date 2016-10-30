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

#include "stdafx.h"
#include "io.hpp"

#pragma comment(lib, "ws2_32")

namespace
{
	static const ULONG_PTR KEY_RUN = 0;
	static const ULONG_PTR KEY_SHUTDOWN = 1;
}

io::service::service()
{
	WSADATA data;

	int err = WSAStartup(WINSOCK_VERSION, &data);
	if (err != 0) throw winsock_error(err);

	m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (!m_iocp) throw win32_error();
}

io::service::~service()
{
	CloseHandle(m_iocp);
}

bool io::service::run()
{
	DWORD transferred;
	ULONG_PTR completion_key;
	OVERLAPPED *overlapped;

	if (!GetQueuedCompletionStatus(m_iocp, &transferred, &completion_key, &overlapped, INFINITE) && !overlapped)
		throw win32_error();

	if (completion_key == KEY_SHUTDOWN)
		return false;
	
	static_cast<op_context*>(overlapped)->completion_func();
	return true;
}

bool io::service::run_many()
{
	OVERLAPPED_ENTRY entries[16];
	ULONG removed;

	if (!GetQueuedCompletionStatusEx(m_iocp, entries, 16, &removed, INFINITE, FALSE))
		throw win32_error();

	bool keep_running = true;

	for (ULONG i = 0; i < removed; ++i)
	{
		if (entries[i].lpCompletionKey == KEY_RUN)
			static_cast<op_context*>(entries[i].lpOverlapped)->completion_func();
		else
			keep_running = false;

	}

	return keep_running;
}

void io::service::shutdown()
{
	if (!PostQueuedCompletionStatus(m_iocp, 0, KEY_SHUTDOWN, nullptr))
		throw win32_error();
}

void io::op_context::check_error() const
{
	if (m_error != ERROR_SUCCESS)
		throw winsock_error(m_error);
}

void io::socket_context::completion_func()
{
	m_error = WSAGetOverlappedResult(m_sock, this, &m_transferred, FALSE, &m_flags) ? ERROR_SUCCESS : WSAGetLastError();
}

io::socket::socket(service &svc, int family, int type, int protocol)
{
	m_sock = WSASocket(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_sock == INVALID_SOCKET) throw winsock_error();

	if (!CreateIoCompletionPort((HANDLE) m_sock, svc.handle(), KEY_RUN, 0) || !SetFileCompletionNotificationModes((HANDLE) m_sock, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
	{
		DWORD err = GetLastError();
		closesocket(m_sock);

		throw win32_error(err);
	}
}

void io::socket::close()
{
	if (m_sock != INVALID_SOCKET)
		closesocket(m_sock);

	m_sock = INVALID_SOCKET;
}

void io::socket::set_option(int level, int name, void const *val, int val_len)
{
	if (!try_set_option(level, name, val, val_len))
		throw winsock_error();
}

bool io::socket::try_set_option(int level, int name, void const *val, int val_len)
{
	return ::setsockopt(m_sock, level, name, (char const*) val, val_len) == 0;
}

void io::socket::bind(sockaddr const *name, int name_len)
{
	if (::bind(m_sock, name, name_len))
		throw winsock_error();
}

bool io::socket::accept_and_receive(socket &sock, void *buffer, int receive_len, int local_len, int remote_len, socket_context &ctx)
{
	LPFN_ACCEPTEX fn;
	DWORD retbytes;
	GUID input = WSAID_ACCEPTEX;

	if (WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &input, sizeof(input), &fn, sizeof(fn), &retbytes, nullptr, nullptr))
	{
		ctx.m_error = WSAGetLastError();
		ctx.m_transferred = 0;
		ctx.m_flags = 0;
		return true;
	}

	if (fn(m_sock, sock.m_sock, buffer, receive_len, local_len, remote_len, &ctx.m_transferred, ctx.init(m_sock)))
	{
		ctx.m_error = ERROR_SUCCESS;
		ctx.m_flags = 0;
		return true;
	}

	DWORD err = WSAGetLastError();

	if (err == ERROR_IO_PENDING)
	{
		return false;
	}

	ctx.m_error = err;
	ctx.m_flags = 0;
	return true;
}

bool io::socket::connect(sockaddr const *name, int name_len, connect_context &ctx)
{
	return connect_and_send(name, name_len, nullptr, 0, ctx);
}

bool io::socket::connect_and_send(sockaddr const *name, int name_len, void const *buffer, int buffer_len, connect_context &ctx)
{
	LPFN_CONNECTEX fn;
	DWORD retbytes;
	GUID input = WSAID_CONNECTEX;

	if (WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &input, sizeof(input), &fn, sizeof(fn), &retbytes, nullptr, nullptr))
	{
		ctx.m_error = WSAGetLastError();
		ctx.m_transferred = 0;
		ctx.m_flags = 0;
		return true;
	}

	if (fn(m_sock, name, name_len, (void*)buffer, buffer_len, &ctx.m_transferred, ctx.init(m_sock)))
	{
		ctx.m_error = ERROR_SUCCESS;
		ctx.m_flags = 0;
		return true;
	}

	DWORD err = WSAGetLastError();

	if (err == ERROR_IO_PENDING)
	{
		return false;
	}

	ctx.m_error = err;
	ctx.m_flags = 0;
	return true;
}

void io::connect_context::completion_func()
{
	socket_context::completion_func();

	if (!has_error())
	{
		if (setsockopt(m_sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0))
			throw winsock_error();
	}
}

bool io::socket::send(void const *buffer, int buffer_len, DWORD flags, socket_context &ctx)
{
	WSABUF buf;
	buf.buf = (char*) buffer;
	buf.len = buffer_len;

	return send(&buf, 1, flags, ctx);
}

bool io::socket::send(WSABUF const *buffers, int buffer_count, DWORD flags, socket_context &ctx)
{
	if (!WSASend(m_sock, (WSABUF*) buffers, buffer_count, &ctx.m_transferred, flags, ctx.init(m_sock), nullptr))
	{
		ctx.m_error = ERROR_SUCCESS;
		ctx.m_flags = 0;
		return true;
	}

	DWORD err = WSAGetLastError();

	if (err == ERROR_IO_PENDING)
	{
		return false;
	}

	ctx.m_error = err;
	ctx.m_flags = 0;
	return true;
}

bool io::socket::receive(void *buffer, int buffer_len, DWORD flags, socket_context &ctx)
{
	WSABUF buf;
	buf.buf = (char*) buffer;
	buf.len = buffer_len;

	return receive(&buf, 1, flags, ctx);
}

bool io::socket::receive(WSABUF const *buffers, int buffer_count, DWORD flags, socket_context &ctx)
{
	if (!WSARecv(m_sock, (WSABUF*) buffers, buffer_count, &ctx.m_transferred, &flags, ctx.init(m_sock), nullptr))
	{
		ctx.m_error = ERROR_SUCCESS;
		ctx.m_flags = flags;
		return true;
	}

	DWORD err = WSAGetLastError();

	if (err == ERROR_IO_PENDING)
	{
		return false;
	}

	ctx.m_error = err;
	ctx.m_flags = 0;
	return true;
}

void io::socket::shutdown(int how)
{
	if (::shutdown(m_sock, how))
	{
		throw winsock_error();
	}
}

bool io::socket::disconnect(bool reuse, socket_context &ctx)
{
	LPFN_DISCONNECTEX fn;
	DWORD retbytes;
	GUID input = WSAID_DISCONNECTEX;

	if (WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &input, sizeof(input), &fn, sizeof(fn), &retbytes, nullptr, nullptr))
	{
		ctx.m_error = WSAGetLastError();
		ctx.m_transferred = 0;
		ctx.m_flags = 0;
		return true;
	}

	if (fn(m_sock, ctx.init(m_sock), reuse ? TF_REUSE_SOCKET : 0, 0))
	{
		ctx.m_error = ERROR_SUCCESS;
		ctx.m_flags = 0;
		return true;
	}

	DWORD err = WSAGetLastError();

	if (err == ERROR_IO_PENDING)
	{
		return false;
	}

	ctx.m_error = err;
	ctx.m_flags = 0;
	return true;
}

void CALLBACK io::resolve_context::lookup_completion(DWORD error, DWORD, OVERLAPPED *overlapped)
{
	resolve_context *ctx = (resolve_context*) overlapped;

	if (error == ERROR_SUCCESS && ctx->m_rawres)
	{
		ctx->m_result = std::shared_ptr<ADDRINFOEX>(ctx->m_rawres, [](ADDRINFOEXW *x) { FreeAddrInfoExW(x); });
		ctx->m_rawres = nullptr;
	}

	ctx->m_error = error;
	PostQueuedCompletionStatus(ctx->m_iocp, 0, KEY_RUN, ctx);
}

namespace io
{
	struct resolve_helpers
	{
		static bool resolve(service &svc, wchar_t const *hostname, wchar_t const *servicename, ADDRINFOEXW const *hints, resolve_context &ctx)
		{
			ctx.m_iocp = svc.handle();

			int err = GetAddrInfoExW(hostname, servicename, NS_ALL, nullptr, hints, &ctx.m_rawres, nullptr, ctx.init(), resolve_context::lookup_completion, nullptr);

			if (!err)
			{
				ctx.m_error = ERROR_SUCCESS;
				return true;
			}

			if (err == ERROR_IO_PENDING)
			{
				return false;
			}

			ctx.m_error = err;
			return true;
		}
	};
}

bool io::resolve(service &svc, wchar_t const *hostname, wchar_t const *servicename, ADDRINFOEXW const *hints, resolve_context &ctx)
{
	return resolve_helpers::resolve(svc, hostname, servicename, hints, ctx);
}


io::resolve_and_connect_context::resolve_and_connect_context()
	: resolve_ctx(this)
	, connect_ctx(this)
{
}

void io::resolve_and_connect_context::step()
{
	switch (state)
	{
	case on_start:
		state = on_resolve;
		if (!resolve(*svc, hostname, servicename, nullptr, resolve_ctx))
		{
			sync = false;
			return;
		}

	case on_resolve:
		if (resolve_ctx.has_error())
		{
			m_error = resolve_ctx.status();
			m_transferred = 0;
			m_flags = 0;
			if (!sync) this->completion_func();
			return;
		}

		// loop through each result, creating a socket for that address type.

		for (iter = resolve_ctx.result().get(); iter; iter = iter->ai_next)
		{
			*sock = socket{ *svc, iter->ai_family, iter->ai_socktype, iter->ai_protocol };

			sockaddr_storage storage;
			storage = {};
			storage.ss_family = iter->ai_family;

			sock->bind((sockaddr*) &storage, iter->ai_addrlen);

			state = on_connect;
			if (!sock->connect_and_send(iter->ai_addr, iter->ai_addrlen, buffer, buffer_len, connect_ctx))
			{
				sync = false;
				return;
			}

		case on_connect:
			if (connect_ctx.has_error())
			{
				sock->close();

				if (!iter->ai_next)
				{
					// no more results to loop through, return this error.
					m_error = connect_ctx.status();
					m_transferred = connect_ctx.transferred();
					m_flags = connect_ctx.flags();
					if(!sync) this->completion_func();
					return;
				}
				else
				{
					// otherwise, just move on to the next.
					continue;
				}
			}

			// success!

			m_error = ERROR_SUCCESS;
			m_transferred = connect_ctx.transferred();
			m_flags = connect_ctx.flags();
			if (!sync) this->completion_func();
			return;
		}
	}
}

void io::resolve_and_connect_context::completion_func()
{
}

namespace io
{
	struct connect_and_send_helpers
	{
		static bool connect_and_send(service &svc, socket &sock, wchar_t const *hostname, wchar_t const *servicename, void const *buffer, int buffer_len, resolve_and_connect_context &ctx)
		{
			ctx.svc = &svc;
			ctx.sock = &sock;
			ctx.hostname = hostname;
			ctx.servicename = servicename;
			ctx.buffer = buffer;
			ctx.buffer_len = buffer_len;
			ctx.state = resolve_and_connect_context::on_start;
			ctx.sync = true;
			ctx.step();
			return ctx.sync;
		}
	};
}

bool io::connect_and_send(service &svc, socket &sock, wchar_t const *hostname, wchar_t const *servicename, resolve_and_connect_context &ctx)
{
	return connect_and_send(svc, sock, hostname, servicename, nullptr, 0, ctx);
}

bool io::connect_and_send(service &svc, socket &sock, wchar_t const *hostname, wchar_t const *servicename, void const *buffer, int buffer_len, resolve_and_connect_context &ctx)
{
	return connect_and_send_helpers::connect_and_send(svc, sock, hostname, servicename, buffer, buffer_len, ctx);
}
