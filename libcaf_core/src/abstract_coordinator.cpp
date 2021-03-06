/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/scheduler/abstract_coordinator.hpp"

#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <condition_variable>

#include "caf/on.hpp"
#include "caf/send.hpp"
#include "caf/spawn.hpp"
#include "caf/anything.hpp"
#include "caf/to_string.hpp"
#include "caf/local_actor.hpp"
#include "caf/scoped_actor.hpp"
#include "caf/system_messages.hpp"

#include "caf/scheduler/coordinator.hpp"

#include "caf/policy/work_stealing.hpp"

#include "caf/detail/logging.hpp"

namespace caf {
namespace scheduler {

/******************************************************************************
 *                     utility and implementation details                     *
 ******************************************************************************/

namespace {

using hrc = std::chrono::high_resolution_clock;

struct delayed_msg {
  actor_addr from;
  channel to;
  message_id mid;
  message msg;
};

inline void deliver(delayed_msg& dm) {
  dm.to->enqueue(dm.from, dm.mid, std::move(dm.msg), nullptr);
}

template <class Map, class... Ts>
inline void insert_dmsg(Map& storage, const duration& d, Ts&&... xs) {
  auto tout = hrc::now();
  tout += d;
  delayed_msg dmsg{std::forward<Ts>(xs)...};
  storage.insert(std::make_pair(std::move(tout), std::move(dmsg)));
}

class timer_actor : public blocking_actor {
 public:
  inline mailbox_element_ptr dequeue() {
    blocking_actor::await_data();
    return next_message();
  }

  bool await_data(const hrc::time_point& tp) {
    if (has_next_message()) {
      return true;
    }
    return mailbox().synchronized_await(m_mtx, m_cv, tp);
  }

  mailbox_element_ptr try_dequeue(const hrc::time_point& tp) {
    if (await_data(tp)) {
      return next_message();
    }
    return mailbox_element_ptr{};
  }

  void act() override {
    trap_exit(true);
    // setup & local variables
    bool received_exit = false;
    mailbox_element_ptr msg_ptr;
    std::multimap<hrc::time_point, delayed_msg> messages;
    // message handling rules
    message_handler mfun{
      [&](const duration& d, actor_addr& from, channel& to,
          message_id mid, message& msg) {
         insert_dmsg(messages, d, std::move(from),
                     std::move(to), mid, std::move(msg));
      },
      [&](const exit_msg&) {
        received_exit = true;
      },
      others >> [&] {
        CAF_LOG_ERROR("unexpected: " << to_string(msg_ptr->msg));
      }
    };
    // loop
    while (!received_exit) {
      while (!msg_ptr) {
        if (messages.empty())
          msg_ptr = dequeue();
        else {
          auto tout = hrc::now();
          // handle timeouts (send messages)
          auto it = messages.begin();
          while (it != messages.end() && (it->first) <= tout) {
            deliver(it->second);
            messages.erase(it);
            it = messages.begin();
          }
          // wait for next message or next timeout
          if (it != messages.end()) {
            msg_ptr = try_dequeue(it->first);
          }
        }
      }
      mfun(msg_ptr->msg);
      msg_ptr.reset();
    }
  }
};

void printer_loop(blocking_actor* self) {
  self->trap_exit(true);
  std::map<actor_addr, std::string> out;
  auto flush_output = [&out](const actor_addr& s) {
    auto i = out.find(s);
    if (i != out.end()) {
      auto& line = i->second;
      if (!line.empty()) {
        std::cout << line << std::flush;
        line.clear();
      }
    }
  };
  auto flush_if_needed = [](std::string& str) {
    if (str.back() == '\n') {
      std::cout << str << std::flush;
      str.clear();
    }
  };
  bool running = true;
  self->receive_while([&] { return running; })(
    on(atom("add"), arg_match) >> [&](std::string& str) {
      auto s = self->current_sender();
      if (str.empty() || s == invalid_actor_addr) {
        return;
      }
      auto i = out.find(s);
      if (i == out.end()) {
        i = out.insert(make_pair(s, std::move(str))).first;
        // monitor actor to flush its output on exit
        self->monitor(s);
      } else {
        i->second += std::move(str);
      }
      flush_if_needed(i->second);
    },
    on(atom("flush")) >> [&] {
      flush_output(self->current_sender());
    },
    [&](const down_msg& dm) {
      flush_output(dm.source);
      out.erase(dm.source);
    },
    [&](const exit_msg&) {
      running = false;
    },
    others >> [&] {
      std::cerr << "*** unexpected: " << to_string(self->current_message())
                << std::endl;
    }
  );
}

} // namespace <anonymous>

/******************************************************************************
 *            implementation of coordinator             *
 ******************************************************************************/

abstract_coordinator::~abstract_coordinator() {
  // nop
}

// creates a default instance
abstract_coordinator* abstract_coordinator::create_singleton() {
  return new coordinator<policy::work_stealing>;
}

void abstract_coordinator::initialize() {
  CAF_LOG_TRACE("");
  // launch utility actors
  m_timer = spawn<timer_actor, hidden + detached + blocking_api>();
  m_printer = spawn<hidden + detached + blocking_api>(printer_loop);
}

void abstract_coordinator::stop_actors() {
  CAF_LOG_TRACE("");
  scoped_actor self{true};
  self->monitor(m_timer);
  self->monitor(m_printer);
  anon_send_exit(m_timer, exit_reason::user_shutdown);
  anon_send_exit(m_printer, exit_reason::user_shutdown);
  int i = 0;
  self->receive_for(i, 2)(
    [](const down_msg&) {
      // nop
    }
  );
}

abstract_coordinator::abstract_coordinator(size_t nw)
    : m_next_worker(0),
      m_num_workers(nw) {
  // nop
}

} // namespace scheduler
} // namespace caf
