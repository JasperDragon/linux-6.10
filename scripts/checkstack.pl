#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0

#	Check the stack usage of functions
#
#	Copyright Joern Engel <joern@lazybastard.org>
#	Inspired by Linus Torvalds
#	Original idea maybe from Keith Owens
#	 <arnd@bergmann-dalldorf.de>
#	Mips port by Juan Quintela <quintela@mandrakesoft.com>
#	Arm port by Holger Schurig
#	Random bits by Matt Mackall <mpm@selenic.com>
#	M68k port by Geert Uytterhoeven and Andreas Schwab
#	AArch64, PARISC ports by Kyle McMartin
#	 <errandir_news@mph.eclipse.co.uk>
#	ppc64le port by Breno Leitao <leitao@debian.org>
#	 <wafgo01@gmail.com>
#	 <tangyouling@kylinos.cn>
#
#	Usage:
#	objdump -d vmlinux | scripts/checkstack.pl [arch] [min_stack]
#
#	TODO :	Port to all architectures (one regex per arch)

use strict;

# check for arch
#
# $re is used for two matches:
# $& (whole re) matches the complete objdump line with the stack growth
# $1 (first bracket) matches the size of the stack growth
#
# $dre is similar, but for dynamic stack redutions:
# $& (whole re) matches the complete objdump line with the stack growth
# $1 (first bracket) matches the dynamic amount of the stack growth
#
# $sub: subroutine for special handling to check stack usage.
#
# use anything else and feel the pain ;)
my (@stack, $re, $dre, $sub, $x, $xs, $funcre, $min_stack);
{
	my $arch = shift;
	if ($arch eq "") {
		$arch = `uname -m`;
		chomp($arch);
	}

	$min_stack = shift;
	if ($min_stack eq "" || $min_stack !~ /^\d+$/) {
		$min_stack = 512;
	}

	$x	= "[0-9a-f]";	# hex character
	$xs	= "[0-9a-f ]";	# hex character or space
	$funcre = qr/^$x* <(.*)>:$/;
	if ($arch =~ '^(aarch|arm)64$') {
		#ffffffc0006325cc:       a9bb7bfd        stp     x29, x30, [sp, #-80]!
		#a110:       d11643ff        sub     sp, sp, #0x590
		$re = qr/^.*stp.*sp, ?\#-([0-9]{1,8})\]\!/o;
		$dre = qr/^.*sub.*sp, sp, #(0x$x{1,8})/o;
	} elsif ($arch eq 'arm') {
		#c0008ffc:	e24dd064	sub	sp, sp, #100	; 0x64
		$re = qr/.*sub.*sp, sp, #([0-9]{1,4})/o;
		$sub = \&arm_push_handling;
		print("wrong or unknown architecture \"$arch\"\n");
		exit
	}
}

#
# To count stack usage of push {*, fp, ip, lr, pc} instruction in ARM,
# if FRAME POINTER is enabled.
# e.g. c01f0d48: e92ddff0 push {r4, r5, r6, r7, r8, r9, sl, fp, ip, lr, pc}
#
sub arm_push_handling {
	my $regex = qr/.*push.*fp, ip, lr, pc}/o;
	my $size = 0;
	my $line_arg = shift;

	if ($line_arg =~ m/$regex/) {
		$size = $line_arg =~ tr/,//;
		$size = ($size + 1) * 4;
	}

	return $size;
}

#
# main()
#
my ($func, $file, $lastslash, $total_size, $addr, $intro);

$total_size = 0;

while (my $line = <STDIN>) {
	if ($line =~ m/$funcre/) {
		$func = $1;
		next if $line !~ m/^($x*)/;
		if ($total_size > $min_stack) {
			push @stack, "$intro$total_size\n";
		}
		$addr = "0x$1";
		$intro = "$addr $func [$file]:";
		my $padlen = 56 - length($intro);
		while ($padlen > 0) {
			$intro .= '	';
			$padlen -= 8;
		}

		$total_size = 0;
	}
	elsif ($line =~ m/(.*):\s*file format/) {
		$file = $1;
		$file =~ s/\.ko//;
		$lastslash = rindex($file, "/");
		if ($lastslash != -1) {
			$file = substr($file, $lastslash + 1);
		}
	}
	elsif ($line =~ m/$re/) {
		my $size = $1;
		$size = hex($size) if ($size =~ /^0x/);

		if ($size > 0xf0000000) {
			$size = - $size;
			$size += 0x80000000;
			$size += 0x80000000;
		}
		next if ($size > 0x10000000);

		$total_size += $size;
	}
	elsif (defined $dre && $line =~ m/$dre/) {
		my $size = $1;

		$size = hex($size) if ($size =~ /^0x/);
		$total_size += $size;
	}
	elsif (defined $sub) {
		my $size = &$sub($line);

		$total_size += $size;
	}
}
if ($total_size > $min_stack) {
	push @stack, "$intro$total_size\n";
}

# Sort output by size (last field) and function name if size is the same
sub sort_lines {
	my ($a, $b) = @_;

	my $num_a = $1 if $a =~ /:\t*(\d+)$/;
	my $num_b = $1 if $b =~ /:\t*(\d+)$/;
	my $func_a = $1 if $a =~ / (.*):/;
	my $func_b = $1 if $b =~ / (.*):/;

	if ($num_a != $num_b) {
		return $num_b <=> $num_a;
	} else {
		return $func_a cmp $func_b;
	}
}

print sort { sort_lines($a, $b) } @stack;
