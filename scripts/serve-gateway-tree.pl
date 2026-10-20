#!/usr/bin/perl
# Serve a gateway tree on loopback, for browser runs with no relay and no TLS.
#
# The gateway page needs a secure context (WebCrypto) and same-origin
# /v1/relay/… fetches. Loopback gives the first for free, so a static tree made
# of the relay's real files and a fixture's sealed objects can be opened as an
# ordinary page:
#
#     perl scripts/serve-gateway-tree.pl /tmp/kelp-live/site 8080
#     # then open http://127.0.0.1:8080/b/<bundle>/#q=…&k=…
#
# GET only; one request per connection; every response carries the relay's own
# Content-Security-Policy (relay/relay.cpp) so the page runs under production
# policy. The fragment with #q=…&k=… holds a key: keep the URL in a file, never
# echo it, and remove the tree when the run is over.
#
# Core Perl only (IO::Socket::INET), so it runs on a Sailfish device, where
# python3, busybox httpd, openssl and socat are absent. This is a test tool,
# not part of the app or the relay; see META-INF/PROGRESS-20261016.md §3.40
# and §4.8.
#
# Layout it expects (built from relay/gateway/test/.build/fixture.json):
#
#     site/b/<bundle>/index.html                         loader.html
#     site/b/<bundle>/.kelp/{core,page,sw}.js            the shipped scripts
#     site/v1/relay/<token>/bundle/<bundle>/info         {"v":1}
#     site/v1/relay/<token>/bundle/<bundle>/object/<oid> sealed bytes

use strict;
use warnings;
use IO::Socket::INET;

my $root = shift or die "usage: serve-gateway-tree.pl SITE_ROOT [PORT]\n";
my $port = shift || 8080;
die "not a directory: $root\n" unless -d $root;

my $csp = q{default-src 'self'; img-src 'self' data: blob:; }
    . q{media-src 'self' blob:; style-src 'self' 'unsafe-inline'; }
    . q{connect-src 'self'};

$SIG{PIPE} = 'IGNORE';    # a reader that closes early must not stop the server

my $server = IO::Socket::INET->new(
    LocalAddr => '127.0.0.1',
    LocalPort => $port,
    Listen    => 32,
    ReuseAddr => 1,
) or die "listen on 127.0.0.1:$port: $!\n";
$| = 1;
print "serving $root on 127.0.0.1:$port\n";

while (my $client = $server->accept) {
    my $request = <$client>;
    next unless defined $request;
    my ($method, $target) = $request =~ m{^(\S+)\s+(\S+)};
    while (my $line = <$client>) { last if $line =~ /^\r?\n$/ }
    $method ||= 'GET';

    my ($code, $reason, $type, $body) =
        (200, 'OK', 'application/octet-stream', '');
    if ($method ne 'GET') {
        ($code, $reason, $type, $body) =
            (405, 'Method Not Allowed', 'text/plain', "GET only\n");
    }
    else {
        my $path = defined $target ? $target : '/';
        $path =~ s/\?.*//;
        $path =~ s/%([0-9A-Fa-f]{2})/chr(hex($1))/ge;
        my $escapes = grep { $_ eq '..' } split m{/}, $path;
        my $file = $root . $path;
        $file .= 'index.html' if !$escapes && -d $file;
        if ($escapes) {
            ($code, $reason, $type, $body) =
                (400, 'Bad Request', 'text/plain', "bad path\n");
        }
        elsif (-f $file && open(my $handle, '<:raw', $file)) {
            local $/;
            $body = <$handle>;
            close $handle;
            $type = 'text/html'        if $file =~ /\.html?$/;
            $type = 'application/javascript' if $file =~ /\.js$/;
            $type = 'application/json' if $file =~ m{(^|/)info$};
            $type = 'text/css'         if $file =~ /\.css$/;
            $type = 'image/svg+xml'    if $file =~ /\.svg$/;
            $type = 'image/png'        if $file =~ /\.png$/;
        }
        else {
            ($code, $reason, $type, $body) =
                (404, 'Not Found', 'text/plain', "not found\n");
        }
    }

    print $client "HTTP/1.1 $code $reason\r\n",
        "Content-Type: $type\r\n",
        'Content-Length: ' . length($body) . "\r\n",
        "Content-Security-Policy: $csp\r\n",
        "Connection: close\r\n\r\n",
        $body;
    close $client;
    print "$method " . (defined $target ? $target : '-') . " -> $code\n";
}
