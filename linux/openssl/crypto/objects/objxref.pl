#! /usr/bin/env perl
# Copyright 1998-2019 The OpenSSL Project Authors. All Rights Reserved.
#
# Licensed under the OpenSSL license (the "License").  You may not use
# this file except in compliance with the License.  You can obtain a copy
# in the file LICENSE in the source distribution or at
# https://www.openssl.org/source/license.html


use strict;
use FindBin;
use lib "$FindBin::Bin/../../util/perl";
use OpenSSL::copyright;

my %xref_tbl;
my %oid_tbl;

my ($mac_file, $xref_file) = @ARGV;

# The year the output file is generated.
my $YEAR = OpenSSL::copyright::latest(($0, $mac_file, $xref_file));

open(IN, $mac_file) || die "Can't open $mac_file, $!\n";

# Read in OID nid values for a lookup table.

while (<IN>)
	{
	s|\R$||;                # Better chomp
	my ($name, $num) = /^(\S+)\s+(\S+)$/;
	$oid_tbl{$name} = $num;
	}
close IN;

open(IN, $xref_file) || die "Can't open $xref_file, $!\n";

my $ln = 1;

while (<IN>)
	{
	s|\R$||;                # Better chomp
	s/#.*$//;
	next if (/^\S*$/);
	my ($xr, $p1, $p2) = /^(\S+)\s+(\S+)\s+(\S+)/;
	check_oid($xr);
	check_oid($p1);
	check_oid($p2);
	$xref_tbl{$xr} = [$p1, $p2, $ln];
	}

my @xrkeys = keys %xref_tbl;

my @srt1 = sort { $oid_tbl{$a} <=> $oid_tbl{$b}} @xrkeys;

my $i;
for($i = 0; $i <= $#srt1; $i++)
	{
	$xref_tbl{$srt1[$i]}[2] = $i;
	}

my @srt2 = sort
	{
	my$ap1 = $oid_tbl{$xref_tbl{$a}[0]};
	my$bp1 = $oid_tbl{$xref_tbl{$b}[0]};
	return $ap1 - $bp1 if ($ap1 != $bp1);
	my$ap2 = $oid_tbl{$xref_tbl{$a}[1]};
	my$bp2 = $oid_tbl{$xref_tbl{$b}[1]};

	return $ap2 - $bp2;
	} @xrkeys;

my $pname = $0;
$pname =~ s|.*/||;

print <<EOF;
/*
 * WARNING: do not edit!
 * Generated by $pname
 *
 * Copyright 1998-$YEAR The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */


typedef struct {
    int sign_id;
    int hash_id;
    int pkey_id;
} nid_triple;

DEFINE_STACK_OF(nid_triple)

static const nid_triple sigoid_srt[] = {
EOF

foreach (@srt1)
	{
	my $xr = $_;
	my ($p1, $p2) = @{$xref_tbl{$_}};
	my $o1 = "    {NID_$xr, NID_$p1,";
	my $o2 = "NID_$p2},";
        if (length("$o1 $o2") < 78)
		{
		print "$o1 $o2\n";
		}
	else
		{
		print "$o1\n     $o2\n";
		}
        }

print "};";
print <<EOF;


static const nid_triple *const sigoid_srt_xref[] = {
EOF

foreach (@srt2)
	{
	my ($p1, $p2, $x) = @{$xref_tbl{$_}};
	# If digest or signature algorithm is "undef" then the algorithm
	# needs special handling and is excluded from the cross reference table.
	next if $p1 eq "undef" || $p2 eq "undef";
	print "    \&sigoid_srt\[$x\],\n";
	}

print "};\n";

sub check_oid
	{
	my ($chk) = @_;
	if (!exists $oid_tbl{$chk})
		{
		die "Can't find \"$chk\"\n";
		}
	}
