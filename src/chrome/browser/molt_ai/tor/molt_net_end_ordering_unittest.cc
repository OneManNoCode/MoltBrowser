// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The end of a MoltNet identity against every order its messages can take
// (services/network/molt_ended_identity_policy.h,
// molt_net_identity_end_policy.h). Std-only.
//
// A model of the pipes involved: pages send requests on URLLoaderFactory
// pipes, open WebSockets and preconnects on the network context's pipe, and
// send on their WebSockets' pipes; the browser sends the end on each
// context's ProxyConfigClient pipe. Each pipe is in order, and nothing
// orders one pipe against another, so every step picks at random which
// pipe the network service reads next, which internal step of a request or
// connection runs next, whether a page sends something, and whether an
// answer reaches the browser. The network contexts apply the real
// MoltEndedContextAdmits() and kMoltEndSteps; the browser the real
// kMoltNetEndSteps, MoltNetEndStepMayPrecedeNetwork() and
// MoltNetEndReachedNetwork(). Over thousands of seeded runs:
//   * nothing starts, no stream through Tor opens and nothing is written to
//     Tor for the identity after its context has handled the end;
//   * the identity's pages are stopped only after every context has it;
// and the same model with round 25's behaviour (the refusal and a stop of
// requests, pages ended at once) does write after the end, as does any
// order of kMoltEndSteps that refuses writes too late, so the checks bite.

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "chrome/browser/molt_ai/tor/molt_net_identity_end_policy.h"
#include "services/network/molt_ended_identity_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

using network::kMoltEndSteps;
using network::MoltEndedContextAdmits;
using network::MoltEndStep;
using network::MoltNetworkEntry;

using EndSteps = std::array<MoltEndStep, kMoltEndSteps.size()>;

// How the model's network contexts and browser behave.
struct Behaviour {
  // The network context: the steps it takes at the end, and whether it
  // keeps a flag that refuses what comes after (round 26) or only has the
  // refusal of the proxy configuration and a stop of what is under way
  // (round 25).
  EndSteps steps = kMoltEndSteps;
  bool ended_flag = true;
  // The browser: waits for every context before stopping pages (round 26),
  // or stops them at once (round 25).
  bool wait_for_network = true;
};

Behaviour Round25() {
  Behaviour b;
  b.ended_flag = false;
  b.wait_for_network = false;
  return b;
}

enum class Kind {
  // On a URLLoaderFactory pipe.
  kCreateLoader,
  // On the network context's pipe.
  kCreateWebSocket,
  kPreconnect,
  kPrefetch,
  kResolveHost,
  kLookUpProxy,
  // On a WebSocket's pipe.
  kWebSocketSend,
  // On the ProxyConfigClient pipe.
  kRefuseAll,
  kFlushProxyConfig,
};

struct Message {
  Kind kind;
  int context = 0;
  int target = 0;  // The WebSocket, for kWebSocketSend.
};

struct Pipe {
  std::deque<Message> queue;
  bool closed = false;  // The receiving end is closed: unread messages go.
};

// What reached Tor (or the system's resolver) for the identity, and when.
struct Send {
  int tick;
  int context;
  bool after_end;  // After that context handled the end.
};

class Model {
 public:
  Model(uint32_t seed, Behaviour behaviour, int contexts = 2, int pages = 3)
      : rng_(seed), behaviour_(behaviour) {
    contexts_.resize(contexts);
    for (int c = 0; c < contexts; ++c) {
      for (int p = 0; p < pages; ++p) {
        contexts_[c].factories.emplace_back();
      }
    }
    pages_alive_ = true;
  }

  // Runs `ticks` steps; the browser ends the identity at `end_at`. A context
  // in `hung` never reads its ProxyConfigClient pipe.
  void Run(int ticks, int end_at, std::set<int> hung = {}) {
    hung_ = std::move(hung);
    for (tick_ = 0; tick_ < ticks; ++tick_) {
      if (tick_ == end_at) {
        BrowserEndsIdentity();
      }
      Step();
    }
  }

  // Results.
  std::vector<Send> sends;            // Bytes or streams to Tor, DNS.
  std::vector<Send> starts;           // Requests the contexts started.
  int pages_stopped_at = -1;          // When the browser stopped the pages.
  std::vector<int> context_ended_at;  // When each context handled the end.
  int network_had_end_at = -1;        // When the browser knew.
  bool browser_stepped_early = false;  // A step past kSendToNetwork before.

  bool SentAfterEnd() const {
    return std::any_of(sends.begin(), sends.end(),
                       [](const Send& s) { return s.after_end; });
  }
  bool AnythingAfterEnd() const {
    return SentAfterEnd() ||
           std::any_of(starts.begin(), starts.end(),
                       [](const Send& s) { return s.after_end; });
  }

 private:
  struct Request {
    int stage = 0;  // 0 proxy, 1 stream, 2 write, 3 response.
    bool on_session = false;
  };
  struct Session {
    std::deque<int> frames;  // Queued frames.
    bool draining = false;
  };
  struct WebSocket {
    bool open = false;
    int stage = 0;  // 0 proxy, 1 stream, 2 handshake write, 3 open.
  };
  struct Context {
    std::vector<Pipe> factories;  // One per page.
    Pipe context_pipe;
    Pipe proxy_config;
    std::vector<Pipe> websocket_pipes;
    bool refuse_all = false;  // The proxy configuration.
    bool ended = false;       // The flag (MoltEndStep::kMarkEnded).
    bool tor_refused = false;  // MoltEndStep::kRefuseTorWrites.
    int ended_at = -1;
    std::map<int, Request> requests;
    std::map<int, WebSocket> websockets;
    Session session;
    bool has_session = false;
    int next_id = 0;
  };

  bool Admits(const Context& c, MoltNetworkEntry entry) const {
    return MoltEndedContextAdmits(entry, behaviour_.ended_flag && c.ended);
  }

  int Pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng_); }

  void RecordSend(int context) {
    sends.push_back({tick_, context, contexts_[context].ended_at >= 0});
  }
  void RecordStart(int context) {
    starts.push_back({tick_, context, contexts_[context].ended_at >= 0});
  }

  // A write to Tor, and a new stream through it: the SOCKS layer sees the
  // end only once the session's MoltIdentityEnd is marked
  // (MoltEndStep::kRefuseTorWrites), not the context's flag.
  bool Write(int context) {
    if (!MoltEndedContextAdmits(MoltNetworkEntry::kTorWrite,
                                contexts_[context].tor_refused)) {
      return false;
    }
    RecordSend(context);
    return true;
  }
  bool OpenStream(int context) {
    Context& c = contexts_[context];
    if (c.refuse_all ||
        !MoltEndedContextAdmits(MoltNetworkEntry::kTorStream, c.tor_refused)) {
      return false;
    }
    RecordSend(context);
    return true;
  }

  // ---- The browser ----

  void BrowserEndsIdentity() {
    for (MoltNetEndStep step : kMoltNetEndSteps) {
      switch (step) {
        case MoltNetEndStep::kMarkEnded:
          browser_ended_ = true;
          break;
        case MoltNetEndStep::kSendToNetwork:
          for (Context& c : contexts_) {
            c.proxy_config.queue.push_back({Kind::kRefuseAll});
            c.proxy_config.queue.push_back({Kind::kFlushProxyConfig});
          }
          acks_outstanding_ = static_cast<int>(contexts_.size());
          break;
        case MoltNetEndStep::kWaitForNetwork:
          end_sent_at_ = tick_;
          if (!behaviour_.wait_for_network) {
            BrowserGoesOn();
          }
          return;  // The rest when the network has it.
        default:
          break;
      }
    }
  }

  void BrowserAnswered() {
    --acks_outstanding_;
    MaybeBrowserGoesOn();
  }

  void MaybeBrowserGoesOn() {
    if (network_had_end_at >= 0 || end_sent_at_ < 0) {
      return;
    }
    const bool deadline_passed = tick_ - end_sent_at_ > kDeadlineTicks;
    if (MoltNetEndReachedNetwork(acks_outstanding_, deadline_passed)) {
      network_had_end_at = tick_;
      BrowserGoesOn();
    }
  }

  void BrowserGoesOn() {
    for (MoltNetEndStep step : kMoltNetEndSteps) {
      if (MoltNetEndStepMayPrecedeNetwork(step) ||
          step == MoltNetEndStep::kWaitForNetwork) {
        continue;
      }
      if (network_had_end_at < 0) {
        browser_stepped_early = true;
      }
      if (step == MoltNetEndStep::kStopPages && pages_alive_) {
        // Ends the renderers: nothing more is sent, but what they sent is
        // still in the pipes and is read (mojo delivers it).
        pages_alive_ = false;
        pages_stopped_at = tick_;
      }
    }
  }

  // ---- The pages ----

  void PageSends() {
    const int c = Pick(static_cast<int>(contexts_.size()));
    Context& context = contexts_[c];
    switch (Pick(6)) {
      case 0:
      case 1:
        context.factories[Pick(static_cast<int>(context.factories.size()))]
            .queue.push_back({Kind::kCreateLoader, c});
        break;
      case 2:
        context.context_pipe.queue.push_back({Kind::kCreateWebSocket, c});
        break;
      case 3: {
        const Kind kinds[] = {Kind::kPreconnect, Kind::kPrefetch,
                              Kind::kResolveHost, Kind::kLookUpProxy};
        context.context_pipe.queue.push_back({kinds[Pick(4)], c});
        break;
      }
      default:
        if (!context.websocket_pipes.empty()) {
          const int w = Pick(static_cast<int>(context.websocket_pipes.size()));
          context.websocket_pipes[w].queue.push_back(
              {Kind::kWebSocketSend, c, w});
        }
        break;
    }
  }

  // ---- The network service ----

  void Handle(int c, const Message& m) {
    Context& context = contexts_[c];
    switch (m.kind) {
      case Kind::kCreateLoader:
        if (!Admits(context, MoltNetworkEntry::kURLLoader)) {
          return;
        }
        RecordStart(c);
        context.requests[context.next_id++] = Request{
            0, context.has_session && !context.session.draining && Pick(2)};
        return;
      case Kind::kCreateWebSocket:
        if (!Admits(context, MoltNetworkEntry::kWebSocket)) {
          return;
        }
        RecordStart(c);
        context.websockets[static_cast<int>(context.websocket_pipes.size())] =
            WebSocket{};
        context.websocket_pipes.emplace_back();
        return;
      case Kind::kPreconnect:
      case Kind::kPrefetch:
        if (!Admits(context, m.kind == Kind::kPreconnect
                                 ? MoltNetworkEntry::kPreconnect
                                 : MoltNetworkEntry::kPrefetch)) {
          return;
        }
        RecordStart(c);
        OpenStream(c);
        return;
      case Kind::kResolveHost:
        if (!Admits(context, MoltNetworkEntry::kHostResolution)) {
          return;
        }
        RecordStart(c);
        RecordSend(c);  // A lookup leaves for the system's resolver.
        return;
      case Kind::kLookUpProxy:
        // Answered; sends nothing.
        EXPECT_TRUE(Admits(context, MoltNetworkEntry::kProxyLookup));
        return;
      case Kind::kWebSocketSend: {
        auto it = context.websockets.find(m.target);
        if (it != context.websockets.end() && it->second.open) {
          Write(c);
        }
        return;
      }
      case Kind::kRefuseAll:
        if (context.refuse_all) {
          return;
        }
        context.refuse_all = true;
        ContextEnds(c);
        return;
      case Kind::kFlushProxyConfig:
        // Answered on the same pipe: after the refusal.
        answers_.push_back(c);
        return;
    }
  }

  void ContextEnds(int c) {
    Context& context = contexts_[c];
    context.ended_at = tick_;
    context_ended_at.push_back(tick_);
    if (!behaviour_.ended_flag) {
      // Round 25: requests cancelled (an HTTP/2 stream's RST_STREAM queued
      // first), then connections closed; open WebSockets carry on.
      for (auto& [id, request] : context.requests) {
        if (request.on_session) {
          context.session.frames.push_back(-1);
        }
      }
      context.requests.clear();
      context.session.draining = context.has_session;
      return;
    }
    for (MoltEndStep step : behaviour_.steps) {
      switch (step) {
        case MoltEndStep::kMarkEnded:
          context.ended = true;
          break;
        case MoltEndStep::kRefuseTorWrites:
          context.tor_refused = true;
          break;
        case MoltEndStep::kCloseURLLoaderFactories:
          for (Pipe& pipe : context.factories) {
            pipe.closed = true;
            pipe.queue.clear();
          }
          break;
        case MoltEndStep::kDropWebSockets:
          for (auto& [id, ws] : context.websockets) {
            if (ws.open) {
              Write(c);  // The closing handshake.
            }
          }
          context.websockets.clear();
          for (Pipe& pipe : context.websocket_pipes) {
            pipe.closed = true;
            pipe.queue.clear();
          }
          break;
        case MoltEndStep::kDropWebTransports:
          break;  // None can go through a proxy.
        case MoltEndStep::kCancelRequests:
          for (auto& [id, request] : context.requests) {
            if (request.on_session) {
              // RST_STREAM, queued; the session writes it later, if it can.
              context.session.frames.push_back(-1);
            }
          }
          context.requests.clear();
          break;
        case MoltEndStep::kCloseConnections:
          // Closed sessions still drain what they had queued.
          context.session.draining = context.has_session;
          break;
      }
    }
  }

  // One internal step of something under way in context `c`.
  void Advance(int c) {
    Context& context = contexts_[c];
    std::vector<int> choices;
    if (!context.requests.empty()) {
      choices.push_back(0);
    }
    if (!context.session.frames.empty()) {
      choices.push_back(1);
    }
    if (!context.websockets.empty()) {
      choices.push_back(2);
    }
    if (choices.empty()) {
      return;
    }
    switch (choices[Pick(static_cast<int>(choices.size()))]) {
      case 0: {
        auto it = context.requests.begin();
        std::advance(it, Pick(static_cast<int>(context.requests.size())));
        Request& r = it->second;
        bool ok = true;
        if (r.stage == 0) {
          ok = !context.refuse_all;  // Proxy resolution.
        } else if (r.stage == 1) {
          ok = r.on_session || OpenStream(c);
          if (ok && !r.on_session) {
            context.has_session = true;
            context.session.draining = false;
          }
        } else if (r.stage == 2) {
          if (r.on_session) {
            context.session.frames.push_back(it->first);  // Queued.
          } else {
            ok = Write(c);
          }
        }
        if (!ok || ++r.stage > 3) {
          context.requests.erase(it);
        }
        return;
      }
      case 1:
        // The session's write loop: one frame.
        context.session.frames.pop_front();
        if (!Write(c)) {
          context.session.frames.clear();  // A write error closes it.
        }
        return;
      case 2: {
        auto it = context.websockets.begin();
        std::advance(it, Pick(static_cast<int>(context.websockets.size())));
        WebSocket& ws = it->second;
        bool ok = true;
        if (ws.stage == 0) {
          ok = !context.refuse_all;
        } else if (ws.stage == 1) {
          ok = OpenStream(c);
        } else if (ws.stage == 2) {
          ok = Write(c);
          ws.open = ok;
        }
        if (!ok) {
          context.websockets.erase(it);
        } else if (ws.stage < 3) {
          ++ws.stage;
        }
        return;
      }
    }
  }

  void NetworkReadsAPipe() {
    // Every (context, pipe) with something to read, the ProxyConfigClient
    // pipe of a hung context excepted.
    std::vector<std::pair<int, Pipe*>> ready;
    for (int c = 0; c < static_cast<int>(contexts_.size()); ++c) {
      Context& context = contexts_[c];
      auto add = [&](Pipe& pipe) {
        if (!pipe.closed && !pipe.queue.empty()) {
          ready.emplace_back(c, &pipe);
        }
      };
      for (Pipe& pipe : context.factories) {
        add(pipe);
      }
      add(context.context_pipe);
      if (!hung_.count(c)) {
        add(context.proxy_config);
      }
      for (Pipe& pipe : context.websocket_pipes) {
        add(pipe);
      }
    }
    if (ready.empty()) {
      return;
    }
    auto [c, pipe] = ready[Pick(static_cast<int>(ready.size()))];
    const Message m = pipe->queue.front();
    pipe->queue.pop_front();
    Handle(c, m);
  }

  void Step() {
    switch (Pick(5)) {
      case 0:
        if (pages_alive_) {
          PageSends();
        }
        return;
      case 1:
      case 2:
        NetworkReadsAPipe();
        return;
      case 3:
        Advance(Pick(static_cast<int>(contexts_.size())));
        return;
      case 4:
        // An answer reaches the browser, or time passes for its deadline.
        if (!answers_.empty()) {
          answers_.pop_front();
          BrowserAnswered();
        } else {
          MaybeBrowserGoesOn();
        }
        return;
    }
  }

  static constexpr int kDeadlineTicks = 400;

  std::mt19937 rng_;
  const Behaviour behaviour_;
  std::vector<Context> contexts_;
  std::set<int> hung_;
  std::deque<int> answers_;
  bool pages_alive_ = false;
  bool browser_ended_ = false;
  int acks_outstanding_ = 0;
  int end_sent_at_ = -1;
  int tick_ = 0;
};

constexpr int kRuns = 3000;
constexpr int kTicks = 900;

// Round 26: whatever order the pipes are read in, nothing of the identity
// starts or reaches Tor after its context has the end, and its pages are
// stopped only once every context has it.
TEST(MoltNetEndOrderingTest, NothingAfterTheEndInAnyOrder) {
  int ended_with_work_under_way = 0;
  for (uint32_t seed = 1; seed <= kRuns; ++seed) {
    Model model(seed, Behaviour{});
    model.Run(kTicks, /*end_at=*/100 + static_cast<int>(seed % 400));
    ASSERT_FALSE(model.AnythingAfterEnd()) << "seed " << seed;
    ASSERT_FALSE(model.browser_stepped_early) << "seed " << seed;
    ASSERT_EQ(model.context_ended_at.size(), 2u) << "seed " << seed;
    ASSERT_GE(model.network_had_end_at, 0) << "seed " << seed;
    for (int ended_at : model.context_ended_at) {
      ASSERT_LT(ended_at, model.network_had_end_at) << "seed " << seed;
    }
    ASSERT_GE(model.pages_stopped_at, model.network_had_end_at)
        << "seed " << seed;
    if (!model.sends.empty()) {
      ++ended_with_work_under_way;
    }
  }
  // The runs did have traffic for the end to stop.
  EXPECT_GT(ended_with_work_under_way, kRuns * 9 / 10);
}

// Round 25's behaviour: the same runs send after the end (an open
// WebSocket's frames, an HTTP/2 session's queued RST_STREAM), and the pages
// are stopped before the network has the end.
TEST(MoltNetEndOrderingTest, Round25SendsAfterTheEnd) {
  int leaked = 0;
  int stopped_early = 0;
  for (uint32_t seed = 1; seed <= kRuns; ++seed) {
    Model model(seed, Round25());
    model.Run(kTicks, /*end_at=*/100 + static_cast<int>(seed % 400));
    leaked += model.SentAfterEnd() ? 1 : 0;
    stopped_early += model.browser_stepped_early ? 1 : 0;
  }
  EXPECT_GT(leaked, kRuns / 10);
  EXPECT_EQ(stopped_early, kRuns);
}

size_t IndexOf(const EndSteps& steps, MoltEndStep step) {
  return static_cast<size_t>(std::find(steps.begin(), steps.end(), step) -
                             steps.begin());
}

// Any order of the end's steps that refuses writes to Tor only after the
// WebSockets are closed (whose closing handshakes are written at once) is
// caught by the checks. (MoltEndStepsInOrder() also wants it before the
// steps whose writes come in later tasks.)
TEST(MoltNetEndOrderingTest, RefusingWritesLateIsCaught) {
  EndSteps steps = kMoltEndSteps;
  std::sort(steps.begin() + 1, steps.end());
  int orders = 0;
  int caught = 0;
  do {
    if (IndexOf(steps, MoltEndStep::kRefuseTorWrites) <
        IndexOf(steps, MoltEndStep::kDropWebSockets)) {
      continue;
    }
    EXPECT_FALSE(network::MoltEndStepsInOrder(steps));
    ++orders;
    Behaviour behaviour;
    behaviour.steps = steps;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
      Model model(seed, behaviour);
      model.Run(kTicks, 100 + static_cast<int>(seed % 400));
      if (model.AnythingAfterEnd()) {
        ++caught;
        break;
      }
    }
  } while (std::next_permutation(steps.begin() + 1, steps.end()));
  EXPECT_GT(orders, 0);
  EXPECT_EQ(caught, orders);
}

// Every order that does refuse writes first is as safe as the one used.
TEST(MoltNetEndOrderingTest, EveryOrderThatRefusesWritesFirstHolds) {
  EndSteps steps = kMoltEndSteps;
  std::sort(steps.begin() + 1, steps.end());
  int orders = 0;
  do {
    if (!network::MoltEndStepsInOrder(steps)) {
      continue;
    }
    ++orders;
    Behaviour behaviour;
    behaviour.steps = steps;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
      Model model(seed, behaviour);
      model.Run(kTicks, 100 + static_cast<int>(seed % 400));
      ASSERT_FALSE(model.AnythingAfterEnd()) << "seed " << seed;
    }
  } while (std::next_permutation(steps.begin() + 1, steps.end()));
  EXPECT_GT(orders, 1);
}

// A context that never answers (a hung network service) holds the end up
// only until the deadline; the other context still has it before the pages
// are stopped, and nothing it handles after its end goes out.
TEST(MoltNetEndOrderingTest, AHungContextOnlyDelaysTheEnd) {
  for (uint32_t seed = 1; seed <= 500; ++seed) {
    Model model(seed, Behaviour{});
    model.Run(kTicks * 3, /*end_at=*/100, /*hung=*/{1});
    ASSERT_FALSE(model.browser_stepped_early) << "seed " << seed;
    ASSERT_GE(model.network_had_end_at, 0) << "seed " << seed;
    ASSERT_EQ(model.context_ended_at.size(), 1u) << "seed " << seed;
    ASSERT_LT(model.context_ended_at[0], model.network_had_end_at)
        << "seed " << seed;
    ASSERT_GE(model.pages_stopped_at, model.network_had_end_at);
    for (const Send& s : model.sends) {
      ASSERT_FALSE(s.context == 0 && s.after_end) << "seed " << seed;
    }
  }
}

// The policies themselves.
TEST(MoltNetEndOrderingTest, AnEndedContextAdmitsOnlyWhatSendsNothing) {
  for (MoltNetworkEntry entry :
       {MoltNetworkEntry::kURLLoader, MoltNetworkEntry::kWebSocket,
        MoltNetworkEntry::kWebTransport, MoltNetworkEntry::kPreconnect,
        MoltNetworkEntry::kPrefetch, MoltNetworkEntry::kHostResolution,
        MoltNetworkEntry::kTorStream, MoltNetworkEntry::kTorWrite}) {
    EXPECT_TRUE(MoltEndedContextAdmits(entry, false));
    EXPECT_FALSE(MoltEndedContextAdmits(entry, true));
  }
  EXPECT_TRUE(MoltEndedContextAdmits(MoltNetworkEntry::kProxyLookup, true));
  EXPECT_TRUE(MoltEndedContextAdmits(MoltNetworkEntry::kProxyLookup, false));
}

TEST(MoltNetEndOrderingTest, TheEndsStepsAreEachTakenOnce) {
  std::set<MoltEndStep> seen(kMoltEndSteps.begin(), kMoltEndSteps.end());
  EXPECT_EQ(seen.size(), kMoltEndSteps.size());
  EXPECT_EQ(kMoltEndSteps.front(), MoltEndStep::kMarkEnded);
  static_assert(network::MoltEndStepsInOrder(kMoltEndSteps));
  EndSteps late = kMoltEndSteps;
  std::swap(late[1], late[3]);  // Writes refused after WebSockets close.
  EXPECT_FALSE(network::MoltEndStepsInOrder(late));
  EndSteps unmarked = kMoltEndSteps;
  std::swap(unmarked[0], unmarked[1]);
  EXPECT_FALSE(network::MoltEndStepsInOrder(unmarked));
}

TEST(MoltNetEndOrderingTest, AContextStartsEndedOnlyWithTheRefusal) {
  EXPECT_TRUE(network::MoltStartsEnded(true, true));
  EXPECT_FALSE(network::MoltStartsEnded(true, false));
  EXPECT_FALSE(network::MoltStartsEnded(false, true));
}

TEST(MoltNetEndOrderingTest, TheBrowserTellsTheNetworkFirst) {
  EXPECT_EQ(kMoltNetEndSteps[0], MoltNetEndStep::kMarkEnded);
  EXPECT_EQ(kMoltNetEndSteps[1], MoltNetEndStep::kSendToNetwork);
  EXPECT_EQ(kMoltNetEndSteps[2], MoltNetEndStep::kWaitForNetwork);
  bool past_wait = false;
  for (MoltNetEndStep step : kMoltNetEndSteps) {
    if (step == MoltNetEndStep::kWaitForNetwork) {
      past_wait = true;
      continue;
    }
    EXPECT_EQ(MoltNetEndStepMayPrecedeNetwork(step), !past_wait);
  }
  EXPECT_TRUE(MoltNetEndReachedNetwork(0, false));
  EXPECT_FALSE(MoltNetEndReachedNetwork(1, false));
  EXPECT_TRUE(MoltNetEndReachedNetwork(1, true));
  EXPECT_TRUE(MoltNetEndReachedNetwork(-1, false));
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
