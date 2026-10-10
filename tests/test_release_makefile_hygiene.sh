#!/bin/sh
# Release credentials never reach a process argv. /proc/<pid>/cmdline (and `ps`) is
# world-readable, so a token on a command line is visible to every user on the release host for
# the life of the process. make expands $(NAME) and ${NAME} in a recipe BEFORE it runs
# `sh -c '<line>'`, so a make-expanded secret lands on the shell's argv even when it is only
# tested or piped; a recipe reads a secret as $${NAME}, which the shell expands from the
# environment (the Makefile's global `export`). This pins, for the Makefile and every workflow:
#   1. no `docker run -e|--env NAME=<value>` for a secret (name only: docker copies the value)
#   2. no recipe line in which make expands a secret
#   3. run_release_with_devops hands the credentials to `$(MAKE) release` via the environment,
#      never as `make release $(info)` / `make release NAME=<value>`
#   4. no secret after a credential flag (--token, --api-key, --password, -k, -p) or in an
#      Authorization header on a command line
#   5. no `${{ secrets.X }}` inside a workflow `run:` script (it belongs in `env:`)
#
# Product-agnostic, so the file is copied verbatim between the ONDEWO C++ clients. Registered
# with CTest by tests/CMakeLists.txt; run by hand as:
#   sh tests/test_release_makefile_hygiene.sh <repository root>
set -eu

root="${1:-$(dirname "$0")/..}"
cd "$root"
[ -f Makefile ] || { echo "FAIL: no Makefile in $root" >&2; exit 1; }

workflows=""
if [ -d .github/workflows ]; then
  workflows="$(find .github/workflows -maxdepth 1 -type f \( -name '*.yml' -o -name '*.yaml' \) | sort)"
fi

# shellcheck disable=SC2086 # $workflows is a newline-separated list of paths without spaces
perl -e '
  use strict;
  use warnings;
  my $secret = qr/[A-Z0-9_]*(?:TOKEN|PASSWORD|PASSPHRASE|API_KEY|SECRET|USERNAME)[A-Z0-9_]*/;
  my @failures;
  sub fail { push @failures, join("", @_) }

  open(my $mk, "<", "Makefile") or die "Makefile: $!";
  my @lines = <$mk>;
  close $mk;
  my ($target, $in_devops) = ("", 0);
  for my $i (0 .. $#lines) {
    my $line = $lines[$i];
    chomp $line;
    my $at = "Makefile:" . ($i + 1);
    if ($line =~ /^([A-Za-z0-9_.\-]+)\s*:(?!=)/) { $target = $1; }
    fail("$at: docker gets a secret value on its argv (use -e NAME): $line")
      if $line =~ /(?:^|\s)(?:-e|--env)[\s=]+$secret=/;
    next unless $line =~ /^\t/;
    fail("$at: make expands a secret into a recipe line (use \$\${NAME}): $line")
      if $line =~ /(?<!\$)\$[({]$secret[)}]/;
    fail("$at: a secret follows a credential flag on a command line: $line")
      if $line =~ /(?:--token|--api-key|--password|\s-k|\s-p)[\s=]*["\x27]?\$+[({]?$secret/;
    fail("$at: a secret is put into an Authorization header on a command line: $line")
      if $line =~ /Authorization:.*\$+[({]?$secret/i;
    fail("$at: make release gets its credentials as arguments: $line")
      if $line =~ /(?:\$\(MAKE\)|\bmake)\s+release\b.*(?:\$\(info\)|\b$secret=)/;
    fail("$at: run_release_with_devops passes \$(info) to make: $line")
      if $target eq "run_release_with_devops" && $line =~ /\$\(info\)/;
  }
  my $devops = join("", @lines) =~ /^run_release_with_devops:[^\n]*\n((?:\t[^\n]*\n|#[^\n]*\n)*)/m ? $1 : undef;
  if (defined $devops) {
    fail("Makefile: run_release_with_devops does not export the credentials with set -a")
      unless $devops =~ /set -a/;
    fail("Makefile: run_release_with_devops does not end in a bare \$(MAKE) release")
      unless $devops =~ /\$\(MAKE\) release\s*$/;
  }

  for my $wf (@ARGV) {
    open(my $fh, "<", $wf) or die "$wf: $!";
    my ($run_indent, $n) = (-1, 0);
    while (my $line = <$fh>) {
      chomp $line;
      $n++;
      my ($indent) = $line =~ /^( *)/;
      $indent = length $indent;
      if ($run_indent >= 0 && $line =~ /\S/ && $indent <= $run_indent) { $run_indent = -1; }
      if ($line =~ /^( *)(?:- )?run:(.*)$/) {
        my ($key_indent, $rest) = (length $1, $2);
        fail("$wf:$n: a secret is interpolated into a run: script (move it to env:): $line")
          if $rest =~ /\$\{\{\s*secrets\./;
        $run_indent = $key_indent;
        next;
      }
      fail("$wf:$n: a secret is interpolated into a run: script (move it to env:): $line")
        if $run_indent >= 0 && $line =~ /\$\{\{\s*secrets\./;
    }
    close $fh;
  }

  if (@failures) { print STDERR "FAIL: $_\n" for @failures; exit 1; }
  print "release credentials stay off argv: Makefile and ", scalar(@ARGV), " workflow(s) checked\n";
' $workflows
