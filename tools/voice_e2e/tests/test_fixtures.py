# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The fixture sites: where each name and path goes, the server over TLS
(with byte ranges), and the browser's host rules."""

import os
import shutil
import socket
import ssl
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import fixtures  # noqa: E402


class ResolveTest(unittest.TestCase):
    def name(self, host, path):
        p, extra = fixtures.resolve(host, path)
        return (os.path.relpath(p, fixtures.FIXTURES) if p else None), extra

    def test_sites(self):
        self.assertEqual(self.name("www.youtube.com", "/")[0], "video/index.html")
        self.assertEqual(self.name("www.youtube.com", "/results?search_query=a")[0],
                         "video/results.html")
        self.assertEqual(self.name("www.youtube.com", "/watch?v=video4")[0],
                         "video/watch.html")
        for host in ("www.google.com", "duckduckgo.com", "www.bing.com", "search.brave.com"):
            self.assertEqual(self.name(host, "/search?q=x")[0], "search/index.html")
            self.assertEqual(self.name(host, "/?q=x")[0], "search/index.html")
        self.assertEqual(self.name("en.wikipedia.org", "/wiki/Lyon_tramway")[0],
                         "wiki/article.html")
        self.assertEqual(self.name("en.wikipedia.org", "/w/index.php?search=a")[0],
                         "wiki/article.html")
        self.assertEqual(self.name("news.test", "/story")[0], "news/story.html")
        self.assertEqual(self.name("shop.test", "/_kit/kit.js")[0], "_kit/kit.js")
        # Phase 2's pages: a reply, a deleted message.
        self.assertEqual(self.name("mail.test", "/compose?to=a&subject=b")[0],
                         "mail/compose.html")
        self.assertEqual(self.name("mail.test", "/deleted?id=5")[0], "mail/deleted.html")
        path, extra = self.name("files.test", "/report.txt")
        self.assertEqual(path, "files/report.txt")
        self.assertIn("attachment", extra["Content-Disposition"])

    def test_nothing_else(self):
        for host, path in (("evil.example", "/"), ("news.test", "/../video/index.html"),
                           ("news.test", "/%2e%2e/video/index.html"),
                           ("news.test", "/missing"), ("news.test", "/_kit/../news/story.html"),
                           ("news.test", "/story.html"), (None, "/")):
            self.assertIsNone(fixtures.resolve(host, path)[0], (host, path))

    def test_every_page_of_every_site_is_reachable(self):
        for site in sorted(set(fixtures.SITES.values())):
            self.assertTrue(os.path.isfile(os.path.join(fixtures.FIXTURES, site, "index.html"))
                            or site == "player", site)

    def test_host_rules(self):
        rules = fixtures.host_rules(4443)
        self.assertIn("MAP www.youtube.com 127.0.0.1:4443", rules)
        self.assertIn("MAP news.test 127.0.0.1:4443", rules)
        self.assertTrue(rules.index("MAP * ~NOTFOUND") > rules.index("MAP www.youtube.com"))


@unittest.skipUnless(shutil.which("openssl"), "needs openssl")
class ServerTest(unittest.TestCase):
    def test_serves_over_tls_with_ranges(self):
        with tempfile.TemporaryDirectory() as d:
            cert, key, spki = fixtures.make_cert(d)
            self.assertEqual(len(spki), 44)
            with open(os.path.join(d, "song.wav"), "wb") as f:
                f.write(b"RIFF" + bytes(range(256)) * 4)
            server = fixtures.FixtureServer(cert, key, generated_dir=d).start()
            try:
                ctx = ssl.create_default_context(cafile=cert)

                def get(host, path, headers=""):
                    with socket.create_connection(("127.0.0.1", server.port), 10) as raw:
                        with ctx.wrap_socket(raw, server_hostname=host) as s:
                            s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close"
                                       "\r\n\r\n" % (path, host, headers)).encode())
                            data = b""
                            while True:
                                chunk = s.recv(65536)
                                if not chunk:
                                    break
                                data += chunk
                    head, _, body = data.partition(b"\r\n\r\n")
                    return head.decode(), body

                head, body = get("news.test", "/story")
                self.assertIn("200", head.split("\r\n")[0])
                self.assertIn(b"Trams return to Lyon", body)
                head, body = get("www.youtube.com", "/_kit/song.wav", "Range: bytes=4-7\r\n")
                self.assertIn("206", head.split("\r\n")[0])
                self.assertIn("Content-Range: bytes 4-7/1028", head)
                self.assertEqual(body, bytes([0, 1, 2, 3]))
                head, _ = get("news.test", "/nothing")
                self.assertIn("404", head.split("\r\n")[0])
            finally:
                server.stop()


if __name__ == "__main__":
    unittest.main()
