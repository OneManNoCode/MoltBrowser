# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A minimal Chrome DevTools Protocol client over --remote-debugging-pipe.

The browser reads NUL-terminated JSON commands from fd 3 and writes
responses and events to fd 4. A pipe (unlike --remote-debugging-port) is
only reachable by the process that launched the browser: no other local
process or web page can talk to it, so the kit needs no new switch and
opens no port.
"""

import json
import os
import queue
import threading
import time


class CdpError(Exception):
    pass


class CdpClosed(CdpError):
    pass


class Cdp(object):
    def __init__(self, write_fd, read_fd):
        self._wfd = write_fd
        self._rfd = read_fd
        self._lock = threading.Lock()
        self._next = 0
        self._pending = {}
        self._events = []
        self._event_cv = threading.Condition()
        self._closed = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        buf = b''
        while True:
            try:
                chunk = os.read(self._rfd, 1 << 16)
            except OSError:
                chunk = b''
            if not chunk:
                break
            buf += chunk
            while b'\0' in buf:
                raw, buf = buf.split(b'\0', 1)
                try:
                    msg = json.loads(raw.decode('utf-8', 'replace'))
                except ValueError:
                    continue
                if 'id' in msg:
                    with self._lock:
                        q = self._pending.pop(msg['id'], None)
                    if q is not None:
                        q.put(msg)
                else:
                    with self._event_cv:
                        self._events.append(msg)
                        if len(self._events) > 20000:
                            del self._events[:5000]
                        self._event_cv.notify_all()
        self._closed = True
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for q in pending:
            q.put(None)
        with self._event_cv:
            self._event_cv.notify_all()

    @property
    def closed(self):
        return self._closed

    def call(self, method, params=None, session=None, timeout=30.0):
        if self._closed:
            raise CdpClosed('the browser closed the DevTools pipe')
        q = queue.Queue()
        with self._lock:
            self._next += 1
            msg_id = self._next
            self._pending[msg_id] = q
        msg = {'id': msg_id, 'method': method, 'params': params or {}}
        if session:
            msg['sessionId'] = session
        data = json.dumps(msg).encode('utf-8') + b'\0'
        try:
            while data:
                n = os.write(self._wfd, data)
                data = data[n:]
        except OSError:
            raise CdpClosed('the browser closed the DevTools pipe')
        try:
            reply = q.get(timeout=timeout)
        except queue.Empty:
            with self._lock:
                self._pending.pop(msg_id, None)
            raise CdpError('%s timed out after %ss' % (method, timeout))
        if reply is None:
            raise CdpClosed('the browser closed the DevTools pipe')
        if 'error' in reply:
            raise CdpError('%s: %s' % (method, reply['error'].get(
                'message', reply['error'])))
        return reply.get('result') or {}

    def mark(self):
        """A position in the event stream for wait_event(since=...)."""
        with self._event_cv:
            return len(self._events)

    def wait_event(self, predicate, timeout=30.0, since=0):
        deadline = time.time() + timeout
        with self._event_cv:
            pos = since
            while True:
                while pos < len(self._events):
                    ev = self._events[pos]
                    pos += 1
                    if predicate(ev):
                        return ev
                if self._closed:
                    return None
                left = deadline - time.time()
                if left <= 0:
                    return None
                self._event_cv.wait(min(left, 0.5))

    def events_since(self, since, method=None):
        with self._event_cv:
            return [e for e in self._events[since:]
                    if method is None or e.get('method') == method]

    # ---- helpers -----------------------------------------------------------

    def targets(self):
        return self.call('Target.getTargets').get('targetInfos', [])

    def attach(self, target_id):
        sid = self.call('Target.attachToTarget',
                        {'targetId': target_id, 'flatten': True})['sessionId']
        for domain in ('Page.enable', 'Runtime.enable'):
            try:
                self.call(domain, session=sid, timeout=10)
            except CdpError:
                pass
        return sid

    def evaluate(self, session, expression, timeout=30.0, await_promise=True,
                 user_gesture=False):
        res = self.call('Runtime.evaluate', {
            'expression': expression, 'returnByValue': True,
            'awaitPromise': await_promise, 'userGesture': user_gesture,
            'timeout': int(timeout * 1000)}, session=session,
            timeout=timeout + 5)
        if res.get('exceptionDetails'):
            details = res['exceptionDetails']
            text = (details.get('exception') or {}).get(
                'description') or details.get('text')
            raise CdpError('script error: %s' % str(text)[:300])
        return (res.get('result') or {}).get('value')

    def navigate(self, session, url, timeout=60.0):
        """Navigates and waits for the load event. Returns an error text
        (e.g. net::ERR_PROXY_CONNECTION_FAILED) or None."""
        since = self.mark()
        res = self.call('Page.navigate', {'url': url}, session=session,
                        timeout=timeout)
        if res.get('errorText'):
            return res['errorText']
        ev = self.wait_event(
            lambda e: e.get('sessionId') == session and
            e.get('method') == 'Page.loadEventFired',
            timeout=timeout, since=since)
        if ev is None:
            return 'timeout waiting for the load event'
        return None
