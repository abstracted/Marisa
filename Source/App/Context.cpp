/*
    This file is part of Marisa.
    Copyright (C) 2018-2019 ReimuNotMoe

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "Context.hpp"
#include "App.hpp"
#include "../Utilities/Utilities.hpp"
#include "../Server/Session.hpp"


using namespace Marisa::Application;
using namespace Marisa::Utilities;
using namespace Marisa::Server;

const char ModuleName[] = "AppContext";

bool Context::determine_hp_state() {
	if (state & FLAG_NOT_STREAMED)
		return http_parser->finished();
	else
		return http_parser->headers_finished();
}

void Context::process_request_data(uint8_t *__buf, size_t __len) {
#ifdef DEBUG
	LogD("%s[0x%016" PRIxPTR "]:\tSpinning state machine\n", ModuleName, (uintptr_t)this);
#endif

	if (!determine_hp_state()) { // Parse operation in progress
		auto &mhs = app.config.http.max_header_size;
		if (mhs < session->total_read_size) {
			LogW("%s[0x%016" PRIxPTR "]:\tHTTP header size exceeded: %zu/%zu\n", ModuleName, (uintptr_t) this, mhs, session->total_read_size);
			return; // Return and teardown
		}

		try {
			http_parser->parse((const char *) __buf, __len);
		} catch (std::exception &e) { // HTTP parser exception, return HTTP 500
			LogW("%s[0x%016" PRIxPTR "]:\tHTTP parse error: %s\n", ModuleName, (uintptr_t) this,
			     e.what());
			return; // Return and teardown
		}

#ifdef DEBUG
		LogD("%s[0x%016" PRIxPTR "]:\tbody size: %zu\n", ModuleName, (uintptr_t) this, http_parser->body().size());
#endif

	}

	if (!determine_hp_state()) { // Check again
		state |= STATE_WANT_READ;
		return; // Return and request more data
	}

	// Parse done
	if (!route) {
		auto &url = http_parser->url();
		auto &rms = app.route_mapping;

		auto it_h_conn = http_parser->headers_lowercase().find("connection");

		if (it_h_conn != http_parser->headers_lowercase().end()) {
			if (it_h_conn->second == "keep-alive") { // Check for 'Connection: keep-alive'
#ifdef DEBUG
				LogD("%s[0x%016" PRIxPTR "]:\tkeep-alive enabled\n", ModuleName, (uintptr_t) this);
#endif
				state |= FLAG_KEEPALIVE;
				auto &kc = http_generator->keepalive_config;
				kc[0] = app.config.connection.timeout_seconds;
				kc[1] = app.config.http.max_requests_per_conn;
			}
		}

		// Match URL with routes
		std::smatch url_smatch; // Don't let this hell go out of scope
		std::vector<std::string> smbuf;
		for (auto &it : rms) {
			if (std::regex_search(url, url_smatch, it.first)) {
				size_t smpos = 0;
				for (auto &its : url_smatch) {
#ifdef DEBUG
					LogD("%s[0x%016" PRIxPTR "]:\tsmatch[%zu]: %s\n", ModuleName, (uintptr_t) this,
					     smpos, its.str().c_str());
#endif
					smbuf.push_back(its.str());
					smpos++;
				}
				route = std::static_pointer_cast<RouteExposed>(it.second);
			}
		}

		// Found single route
		if (route) {
#ifdef DEBUG
			LogD("%s[0x%016" PRIxPTR "]:\tUsing route %p\n", ModuleName, (uintptr_t) this, route.get());
#endif
		} else { // Not found
			if (app.route_global) { // Found global route
				route = std::static_pointer_cast<RouteExposed>(app.route_global);
#ifdef DEBUG
				LogD("%s[0x%016" PRIxPTR "]:\tUsing global route at %p\n", ModuleName, (uintptr_t) this,
				     route.get());
#endif
			} else { // Global route not defined, just send error page on event loop and tear down
				LogW("%s[0x%016" PRIxPTR "]:\tURL %s not found\n", ModuleName, (uintptr_t) this,
				     url.c_str());
				use_default_status_page(HTTP::Status(404));
				return; // Return and teardown
			}
		}

		if (init_handler_data()) {
			std::swap(smbuf, request->url_smatch);
		} else {
			return; // Return and teardown
		}

		// Read entire request if not streamed
		if (!route->mode_streamed) {
			state |= FLAG_NOT_STREAMED;

			if (!determine_hp_state()) { // Check again
				state |= STATE_WANT_READ;
				return; // Return and request more data
			}
		}
	}

	// Run App
	run_handler();

	// No suitable thread pool found so far
	//  session->instance.app_thread_pool->enqueue([](Context *__ctx, void *__ptr) { container_thread(__ctx, __ptr); }, this, sptr);
	//  session->instance.app_thread_pool->submit(Context::container_thread, this, (void *)sptr);

}

void Context::run_raw_handler() {
	if (app.flags & 0x1000) {
		boost::asio::spawn(session->io_strand, [&, s = std::shared_ptr<Session>(session->my_shared_from_this())](boost::asio::yield_context yield){
			state |= STATE_THREAD_RUNNING;
			yield_context = &yield;

			auto cur_mw = app.raw_mw->New();
			cur_mw->__load_context(static_cast<ContextExposed *>(this));

			if (app.config.app.catch_unhandled_exception) {
				try {
					cur_mw->handler();
					LogD("%s[0x%016" PRIxPTR "]:\tdone running raw handler\n", ModuleName, (uintptr_t) this);
				} catch (std::exception &e) {
					LogE("%s[0x%016" PRIxPTR "]:\tuncaught exception in raw middleware at %p: %s\n",
					     ModuleName, (uintptr_t) this, app.raw_mw.get(),
					     e.what());
				}
			} else {
				cur_mw->handler();
				LogD("%s[0x%016" PRIxPTR "]:\tdone running raw handler\n", ModuleName, (uintptr_t) this);
			}

			session->close_socket();

			state &= ~STATE_THREAD_RUNNING;
		});
	} else { // Run in thread
		state |= STATE_THREAD_RUNNING;

		container = std::thread([&, s = std::shared_ptr<Session>(session->my_shared_from_this())](){
			auto cur_mw = app.raw_mw->New();
			cur_mw->__load_context(static_cast<ContextExposed *>(this));

			if (app.config.app.catch_unhandled_exception) {
				try {
					cur_mw->handler();
					LogD("%s[0x%016" PRIxPTR "]:\tdone running raw handler\n", ModuleName, (uintptr_t) this);
				} catch (std::exception &e) {
					LogE("%s[0x%016" PRIxPTR "]:\tuncaught exception in raw middleware at %p: %s\n",
					     ModuleName, (uintptr_t) this, app.raw_mw.get(),
					     e.what());
				}
			} else {
				try {
					cur_mw->handler();
					LogD("%s[0x%016" PRIxPTR "]:\tdone running raw handler\n", ModuleName, (uintptr_t) this);
				} catch (...) {
					state &= ~STATE_THREAD_RUNNING;
					throw;
				}
			}

			state &= ~STATE_THREAD_RUNNING;
		});
		container.detach();
	}
}

void Context::run_handler() {
	// Route declared async, run it here
	if (route->mode_no_yield) {
		if (app.config.app.catch_unhandled_exception) {
			try {
				next();
			} catch (std::exception &e) {
				auto hpos = handlers->pos_cur_handler;
				LogE("%s[0x%016" PRIxPTR "]:\tuncaught exception in middleware #%zu at %p: %s\n",
				     ModuleName, (uintptr_t) this, hpos, handlers->middleware_list[hpos].get(),
				     e.what());
			}
		} else {
			next();
		}

		response.reset();

		session->reload_context();
	} else if (route->mode_async) {
		boost::asio::spawn(session->io_strand, [this, s = std::shared_ptr<Session>(session->my_shared_from_this())](boost::asio::yield_context yield){
			state |= STATE_THREAD_RUNNING;
			yield_context = response->yield_context = &yield;

			if (app.config.app.catch_unhandled_exception) {
				try {
					next();
				} catch (std::exception &e) {
					auto hpos = handlers->pos_cur_handler;
					LogE("%s[0x%016" PRIxPTR "]:\tuncaught exception in middleware #%zu at %p: %s\n",
					     ModuleName, (uintptr_t) this, hpos, handlers->middleware_list[hpos].get(),
					     e.what());
				}
			} else {
				next();
			}

			response.reset();
			state &= ~STATE_THREAD_RUNNING;

			session->reload_context();
		});
	} else { // Run in thread
		std::shared_ptr<Session> *sptr = new std::shared_ptr<Session>(session->my_shared_from_this());
		state |= STATE_THREAD_RUNNING;
		container = std::thread(&ContextExposed::container_thread, this, sptr);
		container.detach();
	}
}


void Context::use_default_status_page(const HTTP::Status &__status) {
	ResponseContext rsp_ctx(this);
	rsp_ctx.status = __status;

	auto page = http_status_page_v(__status);
	rsp_ctx.headers["Content-Length"] = std::to_string(page.size());


	session->inline_async_write(std::move(Buffer(std::move(http_generator->generate_all(rsp_ctx)))));
	session->inline_async_write(std::move(Buffer(std::move(page))));
}

void Context::container_thread(Context *__ctx, void *__session_sptr) {
	auto *sptr = (std::shared_ptr<Session> *)__session_sptr;

#ifdef DEBUG
	LogD("%s[0x%016" PRIxPTR "]:\tcontainer_thread: started\n", ModuleName, (uintptr_t)__ctx);
#endif

	if (__ctx->app.config.app.catch_unhandled_exception) {
		try {
			__ctx->next();
		} catch (std::exception &e) {
			auto hpos = __ctx->handlers->pos_cur_handler;
			LogE("%s[0x%016" PRIxPTR "]:\tuncaught exception in middleware #%zu at %p: %s\n", ModuleName,
			     (uintptr_t) __ctx, hpos, __ctx->handlers->middleware_list[hpos].get(), e.what());
		}
	} else {
		__ctx->next();
	}

	__ctx->response.reset();

#ifdef DEBUG
	LogD("%s[0x%016" PRIxPTR "]:\tcontainer_thread: done\n", ModuleName, (uintptr_t)__ctx);
#endif

	__ctx->state &= ~STATE_THREAD_RUNNING;

	__ctx->session->reload_context();

	delete sptr; // This kind of memory management sucks
}

void Context::next() {
	auto &h = *handlers;

#ifdef DEBUG
	LogD("%s[0x%016" PRIxPTR "]:\tnext() called. handlers=%p\n", ModuleName, (uintptr_t)this, &h);
#endif

	if (h.pos_cur_handler > (h.middleware_list.size()-1)) {
		// TODO: Empty warning
	} else {
		auto cur_mw = h.middleware_list[h.pos_cur_handler]->New();

		cur_mw->__load_context(static_cast<ContextExposed *>(this));

#ifdef DEBUG
		LogD("%s[0x%016" PRIxPTR "]:\tcalling middleware #%zu at %p\n", ModuleName, (uintptr_t)this, h.pos_cur_handler, cur_mw.get());
#endif

		h.pos_cur_handler++;
		cur_mw->handler();
	}
}

bool Context::init_handler_data() {
	auto it_rmm = route->routemethods_mapping.find(http_parser->method());

	if (it_rmm != route->routemethods_mapping.end()) {
		auto &rm = static_cast<RouteMethodsExposed &>(it_rmm->second);

		if (rm.middlewares.empty())
			throw std::logic_error("Method defined but no middlewares");

		handlers = std::make_unique<HandlerData>(rm.middlewares);
	} else {
		auto &rm = static_cast<RouteMethodsExposed &>(route->routemethods_default);

		if (rm.middlewares.empty()) {
			use_default_status_page(HTTP::Status(404));
			return false;
		}

		handlers = std::make_unique<HandlerData>(rm.middlewares);
	}

	response = std::make_unique<Response::ResponseContext>(this);
	request = std::make_unique<Request::RequestContext>(this, *http_parser, *(session->conn_ctx));

	return true;
}

Context::Context(AppExposed &__ref_app, boost::asio::io_service& __io_svc, boost::asio::io_service::strand& __io_strand) : app(__ref_app), io_service(__io_svc), io_strand(__io_strand) {
//	this.http_parser = std::unique_ptr<HTTP1::Parser, std::function<void(HTTP1::Parser*)>>(new (memory) HTTP1::Parser(), [](HTTP1::Parser *p){
////		p->~Parser();
//	});
	http_parser = std::make_unique<HTTP1::Parser>();
	http_generator = std::make_unique<HTTP1::Generator>();
}






