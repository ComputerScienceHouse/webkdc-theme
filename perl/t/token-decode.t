#!/usr/bin/perl
#
# Test token decoding via the Perl API.
#
# Written by Russ Allbery <rra@stanford.edu>
# Copyright 2012
#     The Board of Trustees of the Leland Stanford Junior University
#
# See LICENSE for licensing terms.

use strict;
use warnings;

use lib ('t/lib', 'lib', 'blib/arch');
use RRA::TAP::Automake qw(test_file_path);
use Util qw(contents);

use Test::More tests => 210;

use MIME::Base64 qw(encode_base64);
use WebAuth ();
BEGIN {
    use_ok ('WebAuth::Token');
    use_ok ('WebAuth::Token::App');
    use_ok ('WebAuth::Token::Cred');
    use_ok ('WebAuth::Token::Error');
    use_ok ('WebAuth::Token::Id');
    use_ok ('WebAuth::Token::Login');
    use_ok ('WebAuth::Token::Proxy');
    use_ok ('WebAuth::Token::Request');
    use_ok ('WebAuth::Token::WebKDCProxy');
    use_ok ('WebAuth::Token::WebKDCService');
}

# These will be loaded from the configuration file.
our %TOKENS_GOOD;
our %TOKENS_ERROR;
our %TOKENS_BAD;

# Read a token from a test file and return it without the trailing newline.
sub read_token {
    my ($token) = @_;
    my $path = test_file_path ("data/tokens/$token")
        or BAIL_OUT ("cannot find data/tokens/$token");
    return contents ($path);
}

# General setup.
my $wa = WebAuth->new;
my $path = test_file_path ("data/keyring")
    or BAIL_OUT ('cannot find data/keyring');
my $keyring = $wa->keyring_read ($path);
$path = test_file_path ("data/tokens.conf");
require $path or BAIL_OUT ("cannot load data/tokens.conf");

# Loop through the good tokens, load the named token, and check its attributes
# against the expected attributes from the configuration file.
for my $name (sort keys %TOKENS_GOOD) {
    my $data = read_token ($name);
    my $object = WebAuth::Token->new ($wa, $data, $keyring);
    isa_ok ($object, $TOKENS_GOOD{$name}[0]);
    my $attrs = $TOKENS_GOOD{$name}[1];
    for my $attr (sort keys %$attrs) {
        is ($object->$attr, $attrs->{$attr}, "... $name $attr");
    }
}

# Check that a decoded token contains the WebAuth context.  Do this by poking
# around inside the hash, since there's no public accessor.
my $data = read_token ('app-ok');
my $object = $wa->token_decode ($data, $keyring);
isa_ok ($object, 'WebAuth::Token');
isa_ok ($object, 'WebAuth::Token::App');
ok (defined ($object->{ctx}), '... and has a context');
is (ref ($object->{ctx}), 'WebAuth', '... which is the correct type');

# Build a token manually and test that it decodes properly, and test
# token_encrypt in the process.
my $now = time;
my $creation = pack ('N', $now);
my $expiration = pack ('N', $now + 60);
my $attrs = "t=app;s=test;ct=$creation;et=$expiration;";
my $token = encode_base64($wa->token_encrypt ($attrs, $keyring), '');
ok ($token, 'Encrypting a token works');
my $app = WebAuth::Token->new ($wa, $token, $keyring);
isa_ok ($app, 'WebAuth::Token::App');
is ($app->subject, 'test', '... subject test');
is ($app->creation, $now, "... creation $now");
is ($app->expiration, $now + 60, '... expiration ' . ($now + 60));

# Decode the error and bad tokens and check that they throw exceptions.
for my $tokens_ref (\%TOKENS_ERROR, \%TOKENS_BAD) {
    for my $name (sort keys %{$tokens_ref}) {
        my $data = read_token ($name);
        my $object = eval { WebAuth::Token->new ($wa, $data, $keyring) };
        is ($object, undef, "Parsing token $name failed correctly");
        isa_ok ($@, 'WebAuth::Exception', 'thrown exception');
    }
}

# Do the same for the special error token.
$data = read_token ('app-bad-hmac');
$object = eval { WebAuth::Token->new ($wa, $data, $keyring) };
is ($object, undef, "Parsing token app-bad-hmac failed correctly");
isa_ok ($@, 'WebAuth::Exception', 'thrown exception');
