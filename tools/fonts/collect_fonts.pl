#!/usr/bin/perl
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Lists the fonts of a fresh macOS (or Linux) install for
# molt_font_inventory.py, using only what the OS ships: macOS has no Python,
# but has /usr/bin/perl with the core modules used here.
#
#   /usr/bin/perl collect_fonts.pl [--out FILE] [--root DIR=LOCATION]...
#
# Writes molt-fonts-raw.json (or FILE): per font file its path (the home
# folder as "~"), where it is (system, local, on-demand, user), size,
# SHA-256 and, per face, the raw name, OS/2, head and fvar tables. Copy it
# to a machine with Python 3 and run
#   python3 molt_font_inventory.py import molt-fonts-raw.json
# Nothing is installed or changed. --root replaces the default font folders
# (tests use it).

use strict;
use warnings;
use Digest::SHA ();
use Encode ();
use File::Find ();
use JSON::PP ();
use MIME::Base64 ();
use POSIX ();

my $MAX_FILE = 256 << 20;
my $MAX_FILES = 20000;
my $MAX_FACES = 256;
my $MAX_TABLES = 512;
my %WANT = map { $_ => 1 } ('name', 'OS/2', 'head', 'fvar');
my $OPENTYPE = qr/\.(?:ttf|otf|ttc|otc)\z/i;
my $OTHER = qr/\.(?:dfont|fon|fnt|pfb|pfm|pfa|woff2?|pcf|pcf\.gz|bdf|t1)\z/i;

my $out = 'molt-fonts-raw.json';
my @roots;
while (@ARGV) {
  my $arg = shift @ARGV;
  if ($arg eq '--out' && @ARGV) {
    $out = shift @ARGV;
  } elsif ($arg eq '--root' && @ARGV) {
    my ($dir, $location) = split /=/, shift(@ARGV), 2;
    push @roots, [$dir, $location || 'system'];
  } else {
    die "usage: collect_fonts.pl [--out FILE] [--root DIR=LOCATION]...\n";
  }
}

my $home = $ENV{HOME} || '';
$home =~ s{/+\z}{};

sub text {
  # File names are bytes; macOS and Linux spell them in UTF-8.
  return Encode::decode('UTF-8', $_[0], Encode::FB_DEFAULT);
}

sub redact {
  my ($path) = @_;
  if (length($home) > 1 && index($path, "$home/") == 0) {
    return '~' . substr($path, length($home));
  }
  return $path;
}

sub run {
  my ($tool, @args) = @_;
  return '' unless -x $tool;
  my $pid = open(my $fh, '-|');
  return '' unless defined $pid;
  if (!$pid) {
    open(STDERR, '>', '/dev/null');
    exec { $tool } $tool, @args or POSIX::_exit(127);
  }
  local $/;
  my $output = <$fh>;
  close $fh;
  $output = '' unless defined $output;
  $output =~ s/\s+\z//;
  return text($output);
}

sub os_info {
  my %info = (machine => run('/usr/bin/uname', '-m'));
  if ($^O eq 'darwin') {
    $info{platform} = 'mac';
    $info{version} = run('/usr/bin/sw_vers', '-productVersion');
    $info{build} = run('/usr/bin/sw_vers', '-buildVersion');
    $info{label} = 'macos-' . ($info{version} || 'unknown');
  } else {
    $info{platform} = 'linux';
    if (open(my $fh, '<', '/etc/os-release')) {
      while (my $line = <$fh>) {
        if ($line =~ /^(ID|VERSION_ID|PRETTY_NAME)=(.*)$/) {
          my ($key, $value) = (lc $1, $2);
          $value =~ s/^"(.*)"$/$1/;
          $info{$key} = text($value);
        }
      }
      close $fh;
    }
    $info{label} = ($info{id} || 'linux') . '-' . ($info{version_id} || 'unknown');
  }
  return \%info;
}

sub default_roots {
  if ($^O eq 'darwin') {
    my @r = (['/System/Library/Fonts', 'system'], ['/Library/Fonts', 'local'],
             ["$home/Library/Fonts", 'user']);
    my $assets = '/System/Library/AssetsV2';
    if (opendir(my $dh, $assets)) {
      for my $e (sort readdir $dh) {
        push @r, ["$assets/$e", 'on-demand'] if $e =~ /^com_apple_MobileAsset_Font/;
      }
      closedir $dh;
    }
    return @r;
  }
  return (['/usr/share/fonts', 'system'], ['/usr/local/share/fonts', 'local'],
          ["$home/.local/share/fonts", 'user'], ["$home/.fonts", 'user']);
}

sub u16 { my ($d, $o) = @_; die "truncated\n" if $o + 2 > length $$d; return unpack('n', substr($$d, $o, 2)); }
sub u32 { my ($d, $o) = @_; die "truncated\n" if $o + 4 > length $$d; return unpack('N', substr($$d, $o, 4)); }

# The faces of a font file: [{tag => hex, tables => {tag => base64}}].
sub faces {
  my ($d) = @_;
  die "truncated\n" if length $$d < 12;
  my @offsets = (0);
  if (substr($$d, 0, 4) eq 'ttcf') {
    my $n = u32($d, 8);
    die "$n faces\n" if $n < 1 || $n > $MAX_FACES;
    @offsets = map { u32($d, 12 + 4 * $_) } 0 .. $n - 1;
  }
  my @faces;
  for my $offset (@offsets) {
    die "truncated\n" if $offset + 12 > length $$d;
    my $tag = substr($$d, $offset, 4);
    die "not an sfnt face\n"
      unless $tag eq "\0\1\0\0" || $tag eq 'OTTO' || $tag eq 'true';
    my $count = u16($d, $offset + 4);
    die "$count tables\n" if $count > $MAX_TABLES;
    my %tables;
    for my $i (0 .. $count - 1) {
      my $record = $offset + 12 + 16 * $i;
      die "truncated\n" if $record + 16 > length $$d;
      my $name = substr($$d, $record, 4);
      next unless $WANT{$name};
      my ($start, $length) = (u32($d, $record + 8), u32($d, $record + 12));
      die "table $name outside the file\n" if $start + $length > length $$d;
      $tables{$name} = MIME::Base64::encode_base64(substr($$d, $start, $length), '');
    }
    push @faces, { tag => unpack('H*', $tag), tables => \%tables };
  }
  return \@faces;
}

sub describe {
  my ($path, $location) = @_;
  my %entry = (path => text(redact($path)), location => $location);
  my $size = -s $path;
  if (!defined $size) {
    $entry{error} = 'unreadable';
    return \%entry;
  }
  $entry{size} = $size + 0;
  if ($size > $MAX_FILE) {
    $entry{error} = "larger than $MAX_FILE bytes";
    return \%entry;
  }
  my $data;
  if (open(my $fh, '<:raw', $path)) {
    local $/;
    $data = <$fh>;
    close $fh;
  }
  if (!defined $data) {
    $entry{error} = 'unreadable';
    return \%entry;
  }
  $entry{sha256} = Digest::SHA::sha256_hex($data);
  if ($path !~ $OPENTYPE) {
    $entry{error} = 'not an OpenType font (not compared)';
    return \%entry;
  }
  my $faces = eval { faces(\$data) };
  if ($faces) {
    $entry{faces} = $faces;
  } else {
    my $why = $@ || 'malformed';
    chomp $why;
    $entry{error} = "unreadable font: $why";
  }
  return \%entry;
}

@roots = default_roots() unless @roots;
my %found;
for my $root (@roots) {
  my ($dir, $location) = @$root;
  next unless -d $dir;
  File::Find::find({ no_chdir => 1, wanted => sub {
    return unless -f $_ && ($_ =~ $OPENTYPE || $_ =~ $OTHER);
    $found{$_} //= $location;
    die "more than $MAX_FILES font files\n" if keys(%found) > $MAX_FILES;
  } }, $dir);
}

my @fonts = map { describe($_, $found{$_}) } sort keys %found;
my $doc = {
  format => 'molt-font-raw-1',
  collector => 'collect_fonts.pl 1',
  collected => POSIX::strftime('%Y-%m-%dT%H:%M:%SZ', gmtime),
  os => os_info(),
  fonts => \@fonts,
};
open(my $fh, '>:raw', $out) or die "can't write $out: $!\n";
print $fh JSON::PP->new->utf8->canonical->encode($doc), "\n";
close $fh or die "can't write $out: $!\n";
print "wrote $out (", scalar(@fonts), " font files)\n";
