# MoltBrowser package repositories for Linux

You can install MoltBrowser from an apt repository (Debian, Ubuntu and
derivatives) or a dnf/yum repository (Fedora, RHEL and derivatives). After
that, your system updates it like any other package.

> **Status:** the tooling is ready, but the repositories go live with the
> first release signed with the archive key, which hasn't been created yet.
> Until the key's fingerprint is listed below, there is no MoltBrowser
> repository to add. Don't trust any repository or key that claims to be
> one. Until then, install the `.deb` / `.rpm` from the
> [Releases page](https://github.com/OneManNoCode/MoltBrowser/releases).

## How it is protected

The repository metadata is signed with GenEye's **archive key**, an
OpenPGP key that is only ever used on an offline machine, like the key that
signs in-app updates ([SIGNED_UPDATES.md](SIGNED_UPDATES.md)). CI builds
the repository unsigned; the owner signs it offline. The servers that host
it (GitHub Pages) hold no keys and are not trusted.

- **apt:**
  - `InRelease` (clearsigned) and `Release.gpg` (detached) sign `Release`.
  - `Release` lists the SHA-256 of `Packages`.
  - `Packages` lists the SHA-256 of each `.deb`.
- **dnf/yum:**
  - `repomd.xml.asc` signs `repomd.xml` (`repo_gpgcheck=1`).
  - Each `.rpm` is signed too (`gpgcheck=1`).
- **The packages** are byte-for-byte the release's `.deb`. The `.rpm` is the
  release's `.rpm` plus its signature; its header and payload digests are
  unchanged.

The repository's packages are also covered by the release checksums and
by the offline-signed release manifest and build provenance
([SIGNED_UPDATES.md](SIGNED_UPDATES.md#build-provenance)).

## The archive key

| | |
|---|---|
| Fingerprint | *not created yet* (it will be listed here, in [SECURITY.md](../SECURITY.md), on the website and in every release's notes) |
| Key file (binary, for apt) | `https://onemannocode.github.io/MoltBrowser/linux/moltbrowser-archive-keyring.gpg` |
| Key file (armored, for dnf) | `https://onemannocode.github.io/MoltBrowser/linux/moltbrowser-archive-keyring.asc` |
| In this repository | `installer/linux/moltbrowser-archive-keyring.asc` |

**Always compare the fingerprint** of the key you downloaded with the one
published here and in at least one other place in the table above. Do it
before you trust the key. Each place is maintained separately, so an
attacker would have to change all of them.

```bash
gpg --show-keys --with-fingerprint moltbrowser-archive-keyring.gpg
```

The key has a primary key (it only certifies) and a signing subkey; both
expire, and GenEye extends them well before they do. If apt reports
`EXPKEYSIG` or dnf says the key expired, download the key file again,
check the fingerprint (it stays the same) and replace your copy.

## Debian, Ubuntu (apt)

Works with any apt that supports `signed-by` (apt 1.1 and later).
`/etc/apt/keyrings` is the standard place for keys you add yourself from
apt 2.4 (Debian 12, Ubuntu 22.04); the `install -D` below creates it on
older systems.

1. Download the key and check its fingerprint:
   ```bash
   curl -fsSLo /tmp/moltbrowser.gpg \
     https://onemannocode.github.io/MoltBrowser/linux/moltbrowser-archive-keyring.gpg
   gpg --show-keys --with-fingerprint /tmp/moltbrowser.gpg
   ```
   Stop here if the fingerprint is not the published one.
2. Install the key where only apt's `signed-by` will use it. Don't put it in
   `trusted.gpg.d`: there it would be trusted for every repository.
   ```bash
   sudo install -D -m 644 /tmp/moltbrowser.gpg /etc/apt/keyrings/moltbrowser.gpg
   ```
3. Add the repository. Either use the deb822 file:
   ```bash
   sudo curl -fsSLo /etc/apt/sources.list.d/moltbrowser.sources \
     https://onemannocode.github.io/MoltBrowser/linux/moltbrowser.sources
   ```
   Or write the one-line form:
   ```bash
   echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/moltbrowser.gpg] https://onemannocode.github.io/MoltBrowser/linux/apt stable main" \
     | sudo tee /etc/apt/sources.list.d/moltbrowser.list
   ```
   The deb822 file (`installer/linux/moltbrowser.sources` in this
   repository) contains:
   ```
   Types: deb
   URIs: https://onemannocode.github.io/MoltBrowser/linux/apt
   Suites: stable
   Components: main
   Architectures: amd64
   Signed-By: /etc/apt/keyrings/moltbrowser.gpg
   ```
4. Install:
   ```bash
   sudo apt update
   sudo apt install moltbrowser
   ```
   apt also installs the recommended `tor` package, which MoltNet uses.

## Fedora, RHEL (dnf/yum)

1. Add the repository file (`installer/linux/moltbrowser.repo`):
   ```bash
   sudo curl -fsSLo /etc/yum.repos.d/moltbrowser.repo \
     https://onemannocode.github.io/MoltBrowser/linux/moltbrowser.repo
   ```
   It contains:
   ```ini
   [moltbrowser]
   name=MoltBrowser
   baseurl=https://onemannocode.github.io/MoltBrowser/linux/rpm
   enabled=1
   gpgcheck=1
   repo_gpgcheck=1
   gpgkey=https://onemannocode.github.io/MoltBrowser/linux/moltbrowser-archive-keyring.asc
   ```
   Keep `gpgcheck=1` (each package's signature) and `repo_gpgcheck=1` (the
   metadata's signature).
2. Install:
   ```bash
   sudo dnf install moltbrowser
   ```
   On first use dnf shows the key's fingerprint and asks whether to import
   it. Answer yes only if it matches the published fingerprint.

The packages are signed with an RSA key, which every rpm version can check
(older ones, such as RHEL 8's, can't check Ed25519).

## Removing it

```bash
# apt
sudo apt remove moltbrowser
sudo rm /etc/apt/sources.list.d/moltbrowser.sources /etc/apt/keyrings/moltbrowser.gpg
# dnf
sudo dnf remove moltbrowser
sudo rm /etc/yum.repos.d/moltbrowser.repo
sudo rpm -e gpg-pubkey-<key id>   # the id `rpm -qa gpg-pubkey*` shows for MoltBrowser
```

## Troubleshooting

- **`NO_PUBKEY`** (apt): the key file is missing or isn't the one in
  `signed-by=`. Repeat steps 1 and 2.
- **`EXPKEYSIG` / key expired:** download the key again (same fingerprint,
  new expiry) and replace your copy.
- **`Hash Sum mismatch`:** usually a mirror or proxy in the middle of an
  update. Run `sudo apt update` again. If it persists, report it (see
  [SECURITY.md](../SECURITY.md)): the metadata is signed, so a lasting
  mismatch means someone served files that don't match it.
- **dnf `repomd.xml GPG signature verification error`:** same advice. Never
  set `repo_gpgcheck=0` or `gpgcheck=0` to get past it.

## Hosting

The repository is served by GitHub Pages from
`https://onemannocode.github.io/MoltBrowser/linux/`. It holds the current
release only. The tree is static and self-verifying, so any static host can
mirror it. `scripts/linux-repo-publish.sh --to-dir` writes a verified copy;
nothing about trust changes when it moves.
