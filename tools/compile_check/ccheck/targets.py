# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Which files the check compiles by default."""
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[2] / "src"

# Priority 1: MoltBrowser's own code.
FIRST_PARTY_DIRS = [
    "chrome/browser/molt_ai",
    "chrome/browser/ui/webui/molt_ai",
    "chrome/services/molt_ai",
    "chrome/common/molt_ai",
    "chrome/renderer/molt_ai",
]
FIRST_PARTY_GLOBS = [
    ("components/omnibox/browser", "molt_"),
]

# Priority 2: patched upstream files with MoltBrowser hooks
# (docs/BUILD_MACHINE_CHECKLIST.md section 4).
UPSTREAM_HOOKS = [
    "net/socket/socks_connect_job.cc",
    "net/http/http_network_session.cc",
    "services/network/network_context.cc",
    "services/cert_verifier/cert_net_url_loader/cert_net_fetcher_url_loader.cc",
    "services/cert_verifier/cert_verifier_service_factory.cc",
    "chrome/browser/net/profile_network_context_service.cc",
    "chrome/browser/ssl/chrome_security_blocking_page_factory.cc",
    "chrome/browser/profiles/off_the_record_profile_impl.cc",
    "net/dns/host_resolver_manager.cc",
    "net/base/features.cc",
    "components/translate/core/browser/translate_manager.cc",
    "components/translate/core/browser/translate_language_list.cc",
    "chrome/browser/translate/translate_service.cc",
    "chrome/browser/translate/chrome_translate_client.cc",
    "chrome/browser/ui/views/translate/translate_bubble_view.cc",
    "chrome/browser/chrome_content_browser_client.cc",
    "chrome/browser/browsing_data/chrome_browsing_data_remover_delegate.cc",
    "chrome/browser/profile_resetter/profile_resetter.cc",
    "chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc",
    "components/omnibox/browser/autocomplete_controller.cc",
    "chrome/browser/ui/omnibox/chrome_omnibox_client.cc",
    "components/omnibox/browser/ai_prompt_provider.cc",
    "chrome/utility/services.cc",
    "content/utility/on_device_model/on_device_model_sandbox_init.cc",
    "chrome/browser/ui/views/frame/contents_web_view.cc",
    "chrome/browser/universal_web_contents_observers.cc",
    "chrome/browser/about_flags.cc",
]

SKIP_SUFFIXES = (".mm",)


def _cc_under(d):
    out = []
    for dirpath, _, files in os.walk(SRC / d):
        for f in files:
            if f.endswith((".cc", ".cpp")):
                out.append(os.path.relpath(os.path.join(dirpath, f), SRC))
    return sorted(out)


def first_party():
    out = []
    for d in FIRST_PARTY_DIRS:
        out += _cc_under(d)
    for d, prefix in FIRST_PARTY_GLOBS:
        out += [p for p in _cc_under(d)
                if os.path.basename(p).startswith(prefix)]
    return out


def upstream_hooks():
    return [p for p in UPSTREAM_HOOKS if (SRC / p).exists()]


def default_targets():
    return first_party() + upstream_hooks()


def expand(args):
    """Paths relative to src/ (files or directories) -> .cc files."""
    if not args:
        return default_targets()
    out = []
    for a in args:
        a = a[len("src/"):] if a.startswith("src/") else a
        if a == "@first-party":
            out += first_party()
        elif a == "@hooks":
            out += upstream_hooks()
        elif (SRC / a).is_dir():
            out += _cc_under(a)
        else:
            out.append(a)
    return list(dict.fromkeys(out))
